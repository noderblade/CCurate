#include "curator.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/random.h>
#endif
#include <unistd.h>

static int done(sqlite3_stmt *s)
{
    int r = sqlite3_step(s);
    sqlite3_finalize(s);
    return r == SQLITE_DONE ? 0 : -1;
}
static int no_replace(int a, const char *src, int b, const char *dst)
{
    return renameat2(a, src, b, dst, RENAME_NOREPLACE);
}
void preserve_parents(Catalog *c, const char *path)
{
    char *p = parent_path(path);
    for (;;) {
        sqlite3_stmt *s = sql_prepare(c, "UPDATE versions SET accepted=1 WHERE id=(SELECT current "
                                         "FROM entries WHERE path=? AND kind='directory')");
        bind_text(s, 1, p);
        done(s);
        if (!*p)
            break;
        char *next = parent_path(p);
        free(p);
        p = next;
    }
    free(p);
}
static void accept_hash(Catalog *c, const char *hash)
{
    sqlite3_stmt *s = sql_prepare(c, "UPDATE versions SET accepted=1 WHERE hash=? AND eid "
                                     "IN(SELECT id FROM entries WHERE discarded=0)");
    bind_text(s, 1, hash);
    done(s);
    s = sql_prepare(c, "SELECT DISTINCT e.path FROM versions v JOIN entries e ON e.id=v.eid WHERE "
                       "v.hash=? AND v.accepted=1");
    bind_text(s, 1, hash);
    while (sqlite3_step(s) == SQLITE_ROW)
        preserve_parents(c, (char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
}
static int finalize_copy(Catalog *c, int64_t op, const char *src, const char *dst, const char *hash)
{
    if (sql_exec(c, "BEGIN IMMEDIATE"))
        return -1;
    sqlite3_stmt *s =
        sql_prepare(c, "UPDATE objects SET dest=?,state='verified',verified=?,copied_from=(SELECT "
                       "id FROM entries WHERE path=?) WHERE hash=?");
    bind_text(s, 1, dst);
    sqlite3_bind_int64(s, 2, now_ns());
    bind_text(s, 3, src);
    bind_text(s, 4, hash);
    int rc = done(s);
    accept_hash(c, hash);
    s = sql_prepare(c, "UPDATE operations SET state='done' WHERE id=?");
    sqlite3_bind_int64(s, 1, op);
    rc |= done(s);
    event_log(c, "copy", src, dst, hash);
    if (rc) {
        sql_exec(c, "ROLLBACK");
        return fail(c, "Cannot commit copy metadata");
    }
    return sql_exec(c, "COMMIT");
}
static int move_mapping(Catalog *c, int64_t op, const char *src, const char *dst)
{
    if (sql_exec(c, "BEGIN IMMEDIATE"))
        return -1;
    sqlite3_stmt *s = sql_prepare(c, "UPDATE objects SET dest=?||substr(dest,length(?)+1) WHERE "
                                     "dest=? OR substr(dest,1,length(?)+1)=?||'/'");
    bind_text(s, 1, dst);
    for (int i = 2; i <= 5; i++)
        bind_text(s, i, src);
    int rc = done(s);
    s = sql_prepare(c, "UPDATE operations SET state='done' WHERE id=?");
    sqlite3_bind_int64(s, 1, op);
    rc |= done(s);
    event_log(c, "move", src, dst, NULL);
    if (rc) {
        sql_exec(c, "ROLLBACK");
        return -1;
    }
    return sql_exec(c, "COMMIT");
}
static int verify_at(Catalog *c, const char *path, const char *expected)
{
    int fd = open_beneath(c->dfd, path, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    char hash[129];
    int rc = hash_fd(c, fd, hash, NULL);
    close(fd);
    return !rc && !strcmp(hash, expected) ? 0 : -1;
}
int recover_operations(Catalog *c)
{
    sqlite3_stmt *s = sql_prepare(
        c, "SELECT id,kind,src,dst,temp,hash FROM operations WHERE state='pending' ORDER BY id");
    if (!s)
        return -1;
    typedef struct {
        int64_t id;
        char *kind, *src, *dst, *temp, *hash;
    } Op;
    Op *ops = NULL;
    int n = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        ops = realloc(ops, (n + 1) * sizeof(*ops));
        ops[n].id = sqlite3_column_int64(s, 0);
        ops[n].kind = xstrdup((char *)sqlite3_column_text(s, 1));
        ops[n].src = xstrdup((char *)sqlite3_column_text(s, 2));
        ops[n].dst = xstrdup((char *)sqlite3_column_text(s, 3));
        ops[n].temp = xstrdup((char *)sqlite3_column_text(s, 4));
        ops[n].hash = xstrdup((char *)sqlite3_column_text(s, 5));
        n++;
    }
    sqlite3_finalize(s);
    for (int i = 0; i < n; i++) {
        Op *o = ops + i;
        int recovered = 0;
        if (!strcmp(o->kind, "copy")) {
            if (!verify_at(c, o->dst, o->hash)) {
                finalize_copy(c, o->id, o->src, o->dst, o->hash);
                recovered = 1;
            } else if (*o->temp && !verify_at(c, o->temp, o->hash)) {
                char *a = NULL, *b = NULL;
                int af = parent_beneath(c->dfd, o->temp, 0, &a),
                    bf = parent_beneath(c->dfd, o->dst, 0, &b);
                if (af >= 0 && bf >= 0 && !no_replace(af, a, bf, b) && !fsync(bf)) {
                    finalize_copy(c, o->id, o->src, o->dst, o->hash);
                    recovered = 1;
                }
                if (af >= 0)
                    close(af);
                if (bf >= 0)
                    close(bf);
                free(a);
                free(b);
            }
            if (*o->temp) {
                char *leaf = NULL;
                int pf = parent_beneath(c->dfd, o->temp, 0, &leaf);
                if (pf >= 0) {
                    unlinkat(pf, leaf, 0);
                    fsync(pf);
                    close(pf);
                }
                free(leaf);
            }
        } else if (!strcmp(o->kind, "move")) {
            int fd = open_beneath(c->dfd, o->dst, O_PATH, 0);
            struct stat st;
            if (fd >= 0 && !fstat(fd, &st)) {
                char *sig =
                    fmt("%llu:%llu", (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
                if (!strcmp(sig, o->temp)) {
                    move_mapping(c, o->id, o->src, o->dst);
                    recovered = 1;
                }
                free(sig);
            }
            if (fd >= 0)
                close(fd);
        }
        if (!recovered) {
            s = sql_prepare(c, "UPDATE operations SET state='interrupted' WHERE id=?");
            sqlite3_bind_int64(s, 1, o->id);
            done(s);
        }
        free(o->kind);
        free(o->src);
        free(o->dst);
        free(o->temp);
        free(o->hash);
    }
    free(ops);
    return 0;
}

int mark_entries(Catalog *c, const char *path, const char *action)
{
    const char *expr = !strcmp(action, "review")     ? "reviewed=1"
                       : !strcmp(action, "unreview") ? "reviewed=0"
                       : !strcmp(action, "reject")   ? "discarded=1"
                       : !strcmp(action, "restore")  ? "discarded=0"
                                                     : NULL;
    if (!expr)
        return fail(c, "Unknown marking action");
    if (sql_exec(c, "BEGIN IMMEDIATE"))
        return -1;
    char *q = fmt("UPDATE entries SET %s WHERE ?='' OR path=? OR substr(path,1,length(?)+1)=?||'/'",
                  expr);
    sqlite3_stmt *s = sql_prepare(c, q);
    free(q);
    for (int i = 1; i <= 4; i++)
        bind_text(s, i, path);
    int rc = done(s);
    if (!strcmp(action, "reject")) {
        rc |= sql_exec(
            c, "INSERT OR IGNORE INTO rejected_retention(version) SELECT v.id FROM "
               "versions v JOIN entries e ON e.id=v.eid WHERE e.discarded=1 AND v.accepted=1");
        s = sql_prepare(c, "UPDATE versions SET accepted=0 WHERE eid IN(SELECT id FROM entries "
                           "WHERE discarded=1)");
        rc |= done(s);
    }
    if (!strcmp(action, "restore")) {
        rc |= sql_exec(c,
                       "UPDATE versions SET accepted=1 WHERE id IN(SELECT version FROM "
                       "rejected_retention) AND eid IN(SELECT id FROM entries WHERE discarded=0);");
        rc |= sql_exec(c, "DELETE FROM rejected_retention WHERE version IN(SELECT id FROM "
                          "versions WHERE accepted=1);");
        s = sql_prepare(
            c, "UPDATE versions SET accepted=1 WHERE eid IN(SELECT id FROM entries WHERE "
               "discarded=0) AND hash IN(SELECT hash FROM objects WHERE state='verified')");
        rc |= done(s);
    }
    event_log(c, action, path, NULL, NULL);
    if (rc) {
        sql_exec(c, "ROLLBACK");
        return fail(c, "Cannot save decision");
    }
    sql_exec(c, "COMMIT");
    return export_archive(c);
}
static int64_t intent(Catalog *c, const char *kind, const char *src, const char *dst,
                      const char *temp, const char *hash)
{
    sqlite3_stmt *s =
        sql_prepare(c, "INSERT INTO operations(kind,src,dst,temp,hash) VALUES(?,?,?,?,?)");
    bind_text(s, 1, kind);
    bind_text(s, 2, src);
    bind_text(s, 3, dst);
    bind_text(s, 4, temp);
    bind_text(s, 5, hash);
    if (done(s))
        return -1;
    return sqlite3_last_insert_rowid(c->db);
}
static int batch_checked(Catalog *c, const char *hash)
{
    sqlite3_stmt *s = sql_prepare(c, "SELECT 1 FROM temp.copy_checked WHERE hash=?");
    bind_text(s, 1, hash);
    int found = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    return found;
}
static void batch_check(Catalog *c, const char *hash)
{
    sqlite3_stmt *s = sql_prepare(c, "INSERT OR IGNORE INTO temp.copy_checked VALUES(?)");
    bind_text(s, 1, hash);
    done(s);
}

static int copy_regular(Catalog *c, Entry *e, const char *dst)
{
    if (!*e->hash || *e->error)
        return fail(c, "File needs a successful scan: %s", e->path);
    /* Reusing a previously copied hash must also validate the live source entry. */
    int check = open_beneath(c->sfd, e->path, O_RDONLY | O_NONBLOCK, 0);
    struct stat current;
    if (check < 0)
        return fail(c, "Source unavailable: %s", e->path);
    int same = !fstat(check, &current) && S_ISREG(current.st_mode) && current.st_size == e->size &&
               current.st_dev == (dev_t)e->dev && current.st_ino == (ino_t)e->ino &&
               mtime_ns(&current) == e->mtime;
    close(check);
    sqlite3_stmt *check_stmt = sql_prepare(c, "SELECT ctime FROM versions WHERE id=?");
    sqlite3_bind_int64(check_stmt, 1, e->version);
    same = same && sqlite3_step(check_stmt) == SQLITE_ROW &&
           sqlite3_column_int64(check_stmt, 0) == ctime_ns(&current);
    sqlite3_finalize(check_stmt);
    if (!same)
        return fail(c, "Source changed; rescan before copying: %s", e->path);
    if (*e->dest && !strcmp(e->state, "verified")) {
        if (!batch_checked(c, e->hash)) {
            progress(c, "Checking existing copy: %s", e->dest);
            if (verify_at(c, e->dest, e->hash)) {
                sqlite3_stmt *s =
                    sql_prepare(c, "UPDATE objects SET state='unchecked' WHERE hash=?");
                bind_text(s, 1, e->hash);
                done(s);
                return fail(
                    c, "Existing destination is missing or changed; audit/reconcile required: %s",
                    e->dest);
            }
            batch_check(c, e->hash);
        }
        accept_hash(c, e->hash);
        event_log(c, "reuse", e->path, e->dest, e->hash);
        return 0;
    }
    if (!valid_relative(dst, 0) || descendant(dst, META_DIR))
        return fail(c, "Invalid destination path");
    int src = open_beneath(c->sfd, e->path, O_RDONLY | O_NONBLOCK | O_NOATIME, 0);
    if (src < 0 && errno == EPERM)
        src = open_beneath(c->sfd, e->path, O_RDONLY | O_NONBLOCK, 0);
    if (src < 0)
        return fail(c, "Source open: %s", strerror(errno));
    struct stat st;
    if (fstat(src, &st) || !S_ISREG(st.st_mode) || st.st_size != e->size ||
        st.st_dev != (dev_t)e->dev || st.st_ino != (ino_t)e->ino || mtime_ns(&st) != e->mtime) {
        close(src);
        return fail(c, "Source changed; rescan before copying: %s", e->path);
    }
    sqlite3_stmt *s = sql_prepare(c, "SELECT ctime FROM versions WHERE id=?");
    sqlite3_bind_int64(s, 1, e->version);
    int stable = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int64(s, 0) == ctime_ns(&st);
    sqlite3_finalize(s);
    if (!stable) {
        close(src);
        return fail(c, "Source metadata changed; rescan");
    }
    char *leaf = NULL;
    int dir = parent_beneath(c->dfd, dst, 1, &leaf);
    if (dir < 0) {
        close(src);
        free(leaf);
        return fail(c, "Destination directory: %s", strerror(errno));
    }
    struct stat existing;
    if (!fstatat(dir, leaf, &existing, AT_SYMLINK_NOFOLLOW)) {
        if (S_ISREG(existing.st_mode) && !verify_at(c, dst, e->hash)) {
            int64_t op = intent(c, "copy", e->path, dst, NULL, e->hash);
            close(src);
            close(dir);
            free(leaf);
            if (op < 0)
                return -1;
            batch_check(c, e->hash);
            return finalize_copy(c, op, e->path, dst, e->hash);
        }
        close(src);
        close(dir);
        free(leaf);
        return fail(c, "Name collision (nothing overwritten): %s", dst);
    }
    unsigned long long random = 0;
    if (getrandom(&random, sizeof(random), 0) != (ssize_t)sizeof(random))
        random = (unsigned long long)now_ns();
    char *tmpname = fmt(".rc-copy-%016llx.part", random), *par = parent_path(dst),
         *tmppath = join_path(par, tmpname);
    free(par);
    int64_t op = intent(c, "copy", e->path, dst, tmppath, e->hash);
    if (op < 0) {
        close(src);
        close(dir);
        free(leaf);
        free(tmpname);
        free(tmppath);
        return -1;
    }
    int out = openat(dir, tmpname, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    int rc = 0;
    if (out < 0)
        rc = fail(c, "Temporary file: %s", strerror(errno));
    unsigned char *buf = xmalloc(1024 * 1024), *zeros = xmalloc(1024 * 1024);
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md, EVP_sha512(), NULL);
    uint64_t size = 0;
    progress(c, "Copying: %s", e->path);
    while (!rc) {
        if (atomic_load(&c->cancel)) {
            rc = fail(c, "Copy cancelled");
            break;
        }
        ssize_t n = read(src, buf, 1024 * 1024);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            rc = fail(c, "Source read: %s", strerror(errno));
            break;
        }
        if (!n)
            break;
        EVP_DigestUpdate(md, buf, n);
        if (!memcmp(buf, zeros, n)) {
            if (lseek(out, n, SEEK_CUR) < 0)
                rc = fail(c, "Sparse seek: %s", strerror(errno));
        } else if (write_all(out, buf, n))
            rc = fail(c, "Destination write: %s", strerror(errno));
        size += n;
        atomic_fetch_add(&c->bytes, n);
    }
    struct stat after;
    char hash[129];
    unsigned char digest[64];
    unsigned dl;
    EVP_DigestFinal_ex(md, digest, &dl);
    EVP_MD_CTX_free(md);
    for (int i = 0; i < 64; i++)
        sprintf(hash + i * 2, "%02x", digest[i]);
    free(buf);
    free(zeros);
    if (!rc && (fstat(src, &after) || !stat_same(&st, &after) || size != (uint64_t)e->size ||
                strcmp(hash, e->hash)))
        rc = fail(c, "Source changed while copying; unfinished copy removed");
    close(src);
    if (!rc && ftruncate(out, (off_t)size))
        rc = fail(c, "ftruncate: %s", strerror(errno));
    if (!rc) { /* Exact original ownership and privileged bits stay in the manifest. */
        if (fchmod(out, st.st_mode & 0777))
            rc = fail(c, "chmod destination: %s", strerror(errno));
        struct timespec ts[2] = {st.st_atim, st.st_mtim};
        if (!rc && futimens(out, ts))
            rc = fail(c, "timestamps destination: %s", strerror(errno));
        if (!rc && fsync(out))
            rc = fail(c, "fsync destination: %s", strerror(errno));
    }
    if (out >= 0 && close(out) && !rc)
        rc = fail(c, "close destination: %s", strerror(errno));
    if (!rc) {
        progress(c, "Verifying destination SHA-512: %s", dst);
        if (verify_at(c, tmppath, e->hash))
            rc = fail(c, "Destination verification failed");
    }
    if (!rc && no_replace(dir, tmpname, dir, leaf))
        rc = fail(c, "Publish copy: %s", strerror(errno));
    int published = !rc;
    if (!rc && fsync(dir))
        rc = fail(c, "Directory sync failed; restart will reconcile");
    if (!rc) {
        rc = finalize_copy(c, op, e->path, dst, e->hash);
        batch_check(c, e->hash);
        atomic_fetch_add(&c->files, 1);
    }
    if (rc && !published) {
        unlinkat(dir, tmpname, 0);
        fsync(dir);
        s = sql_prepare(c, "UPDATE operations SET state='failed' WHERE id=?");
        sqlite3_bind_int64(s, 1, op);
        done(s);
    }
    close(dir);
    free(tmpname);
    free(tmppath);
    free(leaf);
    return rc;
}
int copy_entry(Catalog *c, const char *source, const char *target_dir, int complete)
{
    if (c->sfd < 0)
        return fail(c, "Source unavailable");
    if (!valid_relative(source, 1) || !valid_relative(target_dir, 1) ||
        descendant(target_dir, META_DIR))
        return fail(c, "Invalid path");
    sql_exec(c, "CREATE TEMP TABLE IF NOT EXISTS copy_checked(hash TEXT PRIMARY KEY);DELETE FROM "
                "copy_checked;");
    Entry e = {0};
    if (load_entry(c, source, &e, 0))
        return -1;
    if (e.pending) {
        entry_free(&e);
        return fail(
            c,
            "This entry is pending scan verification; finish or restart the scan before copying");
    }
    if (e.discarded) {
        entry_free(&e);
        return fail(c, "Restore the rejected entry before copying");
    }
    int directory = !strcmp(e.kind, "directory");
    entry_free(&e);
    sqlite3_stmt *s = sql_prepare(
        c, directory ? "SELECT path FROM entries WHERE (?='' OR path=? OR "
                       "substr(path,1,length(?)+1)=?||'/') AND discarded=0 AND present=1 ORDER BY "
                       "(kind='directory') DESC,path"
                     : "SELECT path FROM entries WHERE path=? AND discarded=0 AND present=1");
    if (directory)
        for (int i = 1; i <= 4; i++)
            bind_text(s, i, source);
    else
        bind_text(s, 1, source);
    char **paths = NULL;
    int n = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        paths = realloc(paths, (n + 1) * sizeof(*paths));
        paths[n++] = xstrdup((char *)sqlite3_column_text(s, 0));
    }
    sqlite3_finalize(s);
    char *parent = parent_path(source);
    int rc = 0;
    for (int i = 0; i < n; i++) {
        if (atomic_load(&c->cancel)) {
            rc = fail(c, "Cancelled");
            break;
        }
        if (load_entry(c, paths[i], &e, 0)) {
            rc = -1;
            break;
        }
        if (e.pending) {
            rc = fail(c, "Pending scan verification: %s", e.path);
            entry_free(&e);
            break;
        }
        const char *tail = *parent ? paths[i] + strlen(parent) + 1 : paths[i];
        char *dst = join_path(target_dir, directory ? tail : base_name(paths[i]));
        if (!strcmp(e.kind, "directory")) {
            if (complete) {
                if (mkdir_beneath(c->dfd, dst))
                    rc = fail(c, "mkdir: %s", strerror(errno));
                else {
                    s = sql_prepare(c, "UPDATE versions SET accepted=1 WHERE id=?");
                    sqlite3_bind_int64(s, 1, e.version);
                    done(s);
                    preserve_parents(c, e.path);
                }
            }
        } else if (!strcmp(e.kind, "file"))
            rc = copy_regular(c, &e, dst);
        else if (*e.error)
            rc = fail(c, "Unreadable metadata: %s", e.path);
        else {
            s = sql_prepare(c, "UPDATE versions SET accepted=1 WHERE id=?");
            sqlite3_bind_int64(s, 1, e.version);
            done(s);
            preserve_parents(c, e.path);
            event_log(c, "keep_metadata", e.path, NULL, NULL);
        }
        free(dst);
        entry_free(&e);
        if (rc)
            break;
    }
    for (int i = 0; i < n; i++)
        free(paths[i]);
    free(paths);
    free(parent);
    char err[1024];
    snprintf(err, sizeof(err), "%s", c->error);
    int er = export_archive(c);
    if (rc)
        snprintf(c->error, sizeof(c->error), "%s", err);
    progress(c, rc ? "Operation interrupted: %s" : "Copy complete", rc ? c->error : "");
    return rc ? rc : er;
}
int move_target(Catalog *c, const char *from, const char *to)
{
    if (!valid_relative(from, 0) || !valid_relative(to, 0) || descendant(from, META_DIR) ||
        descendant(to, META_DIR) || descendant(to, from))
        return fail(c, "Invalid move");
    char *a = NULL, *b = NULL;
    int af = parent_beneath(c->dfd, from, 0, &a), bf = parent_beneath(c->dfd, to, 1, &b);
    struct stat st;
    int rc = 0;
    if (af < 0 || bf < 0 || fstatat(af, a, &st, AT_SYMLINK_NOFOLLOW))
        rc = fail(c, "Move: %s", strerror(errno));
    int64_t op = -1;
    if (!rc) {
        char *sig = fmt("%llu:%llu", (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
        op = intent(c, "move", from, to, sig, NULL);
        free(sig);
        if (op < 0)
            rc = -1;
    }
    if (!rc && no_replace(af, a, bf, b)) {
        rc = fail(c, "Move refused: %s", strerror(errno));
        sqlite3_stmt *s = sql_prepare(c, "UPDATE operations SET state='failed' WHERE id=?");
        sqlite3_bind_int64(s, 1, op);
        done(s);
    }
    if (!rc && (fsync(af) || fsync(bf)))
        rc = fail(c, "Move needs recovery after directory sync error");
    if (!rc)
        rc = move_mapping(c, op, from, to);
    if (af >= 0)
        close(af);
    if (bf >= 0)
        close(bf);
    free(a);
    free(b);
    if (!rc)
        rc = export_archive(c);
    return rc;
}
int audit_target(Catalog *c)
{
    sqlite3_stmt *s = sql_prepare(c, "SELECT hash,dest FROM objects WHERE dest IS NOT NULL");
    char **hashes = NULL, **paths = NULL;
    int n = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        hashes = realloc(hashes, (n + 1) * sizeof(*hashes));
        paths = realloc(paths, (n + 1) * sizeof(*paths));
        hashes[n] = xstrdup((char *)sqlite3_column_text(s, 0));
        paths[n++] = xstrdup((char *)sqlite3_column_text(s, 1));
    }
    sqlite3_finalize(s);
    int rc = 0;
    for (int i = 0; i < n; i++) {
        if (atomic_load(&c->cancel)) {
            rc = -1;
            break;
        }
        progress(c, "SHA-512 audit: %s", paths[i]);
        int fd = open_beneath(c->dfd, paths[i], O_RDONLY | O_NONBLOCK, 0);
        char hash[129] = {0};
        int ok = fd >= 0 && !hash_fd(c, fd, hash, NULL) && !strcmp(hash, hashes[i]);
        const char *state = ok ? "verified" : fd < 0 ? "missing" : "corrupt";
        if (fd >= 0)
            close(fd);
        s = sql_prepare(c, "UPDATE objects SET state=?,verified=? WHERE hash=?");
        bind_text(s, 1, state);
        sqlite3_bind_int64(s, 2, ok ? now_ns() : 0);
        bind_text(s, 3, hashes[i]);
        done(s);
        if (ok)
            accept_hash(c, hashes[i]);
        else {
            rc = 1;
            activity_log(c, LOG_ERROR, "audit.content", paths[i], NULL, "Destination content is %s",
                         state);
        }
        atomic_fetch_add(&c->files, 1);
    }
    for (int i = 0; i < n; i++) {
        free(hashes[i]);
        free(paths[i]);
    }
    free(hashes);
    free(paths);
    event_log(c, "audit", NULL, NULL, NULL);
    if (export_archive(c))
        return -1;
    progress(c, rc ? "Audit found missing / changed data" : "Audit complete: SHA-512 matches");
    return rc;
}
static int reconcile_walk(Catalog *c, const char *path, int depth)
{
    if (depth > 512 || atomic_load(&c->cancel))
        return -1;
    if (descendant(path, META_DIR))
        return 0;
    int fd = open_beneath(c->dfd, path, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0)
        return 0;
    struct stat st;
    if (fstat(fd, &st)) {
        close(fd);
        return 0;
    }
    if (S_ISREG(st.st_mode)) {
        char hash[129];
        progress(c, "Finding relocated content: %s", path);
        if (!hash_fd(c, fd, hash, NULL)) {
            sqlite3_stmt *s = sql_prepare(
                c,
                "UPDATE objects SET dest=?,state='verified',verified=? WHERE hash=? AND "
                "state<>'verified' AND NOT EXISTS(SELECT 1 FROM objects WHERE dest=? AND hash<>?)");
            bind_text(s, 1, path);
            sqlite3_bind_int64(s, 2, now_ns());
            bind_text(s, 3, hash);
            bind_text(s, 4, path);
            bind_text(s, 5, hash);
            done(s);
            if (sqlite3_changes(c->db)) {
                accept_hash(c, hash);
                event_log(c, "recover", NULL, path, hash);
            }
        }
        close(fd);
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        close(fd);
        return 0;
    }
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char *p = join_path(path, e->d_name);
        reconcile_walk(c, p, depth + 1);
        free(p);
    }
    closedir(d);
    return 0;
}
int reconcile_target(Catalog *c)
{
    int rc = audit_target(c);
    if (rc < 0)
        return rc;
    reconcile_walk(c, "", 0);
    return export_archive(c);
}
