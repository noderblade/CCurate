#include "target_details.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void source_occurrences(Catalog *c, Buf *b, const char *key, int empty, int page)
{
    /* One extra row determines whether a next page exists, without a global count.
     * Empty files are linked by destination/version, never by the empty digest. */
    const char *columns =
        "SELECT e.path,v.id,e.present,e.reviewed,e.discarded,v.accepted,v.id=e.current ";
    char *sql = fmt("%s%s", columns,
                    empty ? "FROM empty_copies z JOIN versions v ON v.id=z.version "
                            "JOIN entries e ON e.id=v.eid WHERE z.dest=? "
                            "ORDER BY z.version LIMIT 257 OFFSET ?"
                          : "FROM versions v JOIN entries e ON e.id=v.eid WHERE v.hash=? "
                            "ORDER BY v.eid,v.id LIMIT 257 OFFSET ?");
    sqlite3_stmt *q = sql_prepare(c, sql);
    free(sql);
    badd(b, "\n%s (page %d; Ctrl+Alt+PgUp/PgDn):\n",
         empty ? "ORIGINAL EMPTY-FILE PATHS" : "SOURCE CONTENT OCCURRENCES", page + 1);
    if (!q) {
        badd(b, "Cannot read source links: %s\n", sqlite3_errmsg(c->db));
        return;
    }
    bind_text(q, 1, key);
    sqlite3_bind_int64(q, 2, (int64_t)page * 256);
    int count = 0, step;
    while ((step = sqlite3_step(q)) == SQLITE_ROW) {
        if (++count > 256) {
            badd(b, "More occurrences on the next page.\n");
            break;
        }
        char *path = display_path((const char *)sqlite3_column_text(q, 0));
        badd(b, "%s  (v%lld)%s%s%s%s%s\n", path, sqlite3_column_int64(q, 1),
             sqlite3_column_int(q, 5) ? " [retained]" : " [not retained]",
             sqlite3_column_int(q, 3) ? " [reviewed]" : "",
             sqlite3_column_int(q, 4) ? " [rejected]" : "",
             sqlite3_column_int(q, 2) ? "" : " [absent at last scan]",
             sqlite3_column_int(q, 6) ? "" : " [older version]");
        free(path);
    }
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        badd(b, "Cannot read source links: %s\n", sqlite3_errmsg(c->db));
    else if (!count)
        badd(b, "No occurrences on this page.\n");
    sqlite3_finalize(q);
    badd(b, "Source flags reflect the catalog snapshot; paths are relative to the source root.\n");
}

char *target_details_page(Catalog *c, const char *path, int page)
{
    Buf b = {0};
    if (!valid_relative(path, 0))
        return xstrdup("Select an entry in the destination pane.");
    if (page < 0)
        page = 0;
    char *absolute = join_path(c->destination, path), *safe = display_path(absolute);
    badd(&b, "DESTINATION\n%s\n", safe);
    free(absolute);
    free(safe);

    char *leaf = NULL;
    int parent = parent_beneath(c->dfd, path, 0, &leaf);
    struct stat st;
    int found = parent >= 0 && !fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
    if (!found)
        badd(&b, "\nCannot read destination metadata: %s\n", strerror(errno));
    else {
        const char *kind = S_ISREG(st.st_mode)   ? "file"
                           : S_ISDIR(st.st_mode) ? "directory"
                           : S_ISLNK(st.st_mode) ? "symlink"
                                                 : "special";
        char size[40], modified[40], created[40];
        struct statx sx;
        int64_t born = 0;
        if (!statx(parent, leaf, AT_SYMLINK_NOFOLLOW, STATX_BTIME, &sx) &&
            (sx.stx_mask & STATX_BTIME))
            born = (int64_t)sx.stx_btime.tv_sec * 1000000000 + sx.stx_btime.tv_nsec;
        human_bytes(st.st_size, size);
        date_text(mtime_ns(&st), modified);
        date_text(born, created);
        badd(&b,
             "\n%s | %s (%lld B) | mode %04llo\nModified: %s\nCreated: %s\n"
             "UID:GID %lld:%lld\nMetadata read from the destination.\n",
             kind, size, (long long)st.st_size, (unsigned long long)st.st_mode & 07777, modified,
             created, (long long)st.st_uid, (long long)st.st_gid);
        if (S_ISDIR(st.st_mode))
            badd(&b, "Directory size is filesystem metadata, not the recursive content size.\n"
                     "Open the directory and select a file to inspect its source links.\n");
        if (S_ISLNK(st.st_mode))
            badd(&b, "Symlink: content is not followed or previewed.\n");
    }
    if (parent >= 0)
        close(parent);
    free(leaf);

    if (descendant(path, META_DIR)) {
        badd(&b, "\nSESSION DATA: CCurate's index, settings or activity log.\n");
        return b.s;
    }
    sqlite3_stmt *q = sql_prepare(c, "SELECT o.hash,o.state,o.verified,o.size,e.path FROM objects o "
                                   "LEFT JOIN entries e ON e.id=o.copied_from WHERE o.dest=?");
    if (!q)
        goto database_error;
    bind_text(q, 1, path);
    int step = sqlite3_step(q), empty = 0;
    if (step == SQLITE_DONE) {
        sqlite3_finalize(q);
        empty = 1;
        q = sql_prepare(c, "SELECT v.hash,z.state,z.verified,v.size,e.path FROM empty_copies z "
                           "JOIN versions v ON v.id=z.version JOIN entries e ON e.id=v.eid "
                           "WHERE z.dest=? ORDER BY z.version LIMIT 1");
        if (!q)
            goto database_error;
        bind_text(q, 1, path);
        step = sqlite3_step(q);
    }
    if (step == SQLITE_ROW) {
        char *hash = xstrdup((const char *)sqlite3_column_text(q, 0));
        char verified[40];
        date_text(sqlite3_column_int64(q, 2), verified);
        safe = display_path((const char *)sqlite3_column_text(q, 4)
                                ? (const char *)sqlite3_column_text(q, 4)
                                : "Unknown");
        badd(&b, "\nINDEXED COPY [%s]\nLast verification: %s\nCopied from: %s\n",
             sqlite3_column_text(q, 1), verified, safe);
        free(safe);
        if (found && (!S_ISREG(st.st_mode) || st.st_size != sqlite3_column_int64(q, 3)))
            badd(&b, "WARNING: destination type or size differs from the indexed copy.\n");
        sqlite3_finalize(q);
        if (empty)
            badd(&b, "EMPTY FILE: tracked individually; not a content duplicate group.\n");
        else
            badd(&b, "Indexed SHA-512:\n%.64s\n%s\n", hash, strlen(hash) > 64 ? hash + 64 : "");
        badd(&b, "Content is not rehashed by this view. Use Audit (Ctrl+A) to verify it.\n");
        source_occurrences(c, &b, empty ? path : hash, empty, page);
        free(hash);
    } else {
        sqlite3_finalize(q);
        if (step != SQLITE_DONE)
            goto database_error;
        if (!found || !S_ISDIR(st.st_mode))
            badd(&b, "\nNOT INDEXED: no stored source-to-destination copy mapping.\n"
                     "Select Reconcile if this is a copied file moved outside CCurate.\n");
    }
    return b.s;

database_error:
    badd(&b, "\nCannot read destination index: %s\n", sqlite3_errmsg(c->db));
    return b.s;
}
