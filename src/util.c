#include "curator.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void *xmalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fputs("Out of memory\n", stderr);
        exit(2);
    }
    return p;
}
char *xstrdup(const char *s)
{
    return strdup(s ? s : "");
}
char *fmt(const char *s, ...)
{
    va_list a;
    va_start(a, s);
    char *r = NULL;
    if (vasprintf(&r, s, a) < 0)
        abort();
    va_end(a);
    return r;
}
void badd(Buf *b, const char *s, ...)
{
    va_list a;
    va_start(a, s);
    char *t = NULL;
    int n = vasprintf(&t, s, a);
    va_end(a);
    if (n < 0)
        abort();
    if (b->n + (size_t)n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
        if (!b->s)
            abort();
    }
    memcpy(b->s + b->n, t, n + 1);
    b->n += n;
    free(t);
}
void bfree(Buf *b)
{
    free(b->s);
    memset(b, 0, sizeof(*b));
}
char *b64(const void *p, size_t n)
{
    char *s = xmalloc(4 * ((n + 2) / 3) + 1);
    EVP_EncodeBlock((unsigned char *)s, p, (int)n);
    return s;
}
unsigned char *unb64(const char *s, size_t *len)
{
    size_t n = strlen(s);
    unsigned char *r = xmalloc(n + 1);
    int k = EVP_DecodeBlock(r, (const unsigned char *)s, (int)n);
    if (k < 0 || n % 4) {
        free(r);
        return NULL;
    }
    if (n && s[n - 1] == '=')
        k--;
    if (n > 1 && s[n - 2] == '=')
        k--;
    r[k] = 0;
    *len = k;
    return r;
}
/* Human-readable spelling plus a separate base64 field preserve arbitrary POSIX bytes. */
char *display_path(const char *s)
{
    Buf b = {0};
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned n = 1;
        if (*p >= 0xc2 && *p <= 0xdf)
            n = 2;
        else if (*p >= 0xe0 && *p <= 0xef)
            n = 3;
        else if (*p >= 0xf0 && *p <= 0xf4)
            n = 4;
        int ok = *p < 0x80;
        if (n > 1) {
            ok = 1;
            for (unsigned j = 1; j < n; j++)
                if (!p[j] || (p[j] & 0xc0) != 0x80) {
                    ok = 0;
                    break;
                }
            if (ok && ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] >= 0xa0) ||
                       (p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] >= 0x90)))
                ok = 0;
        }
        if (*p < 32 || *p == 127 || !ok) {
            badd(&b, "\\x%02x", *p++);
        } else {
            badd(&b, "%.*s", n, p);
            p += n;
        }
    }
    return b.s ? b.s : xstrdup("");
}
char *json_string(const char *s)
{
    char *d = display_path(s ? s : "");
    Buf b = {0};
    badd(&b, "\"");
    for (const unsigned char *p = (unsigned char *)d; *p; p++) {
        if (*p == '"' || *p == '\\')
            badd(&b, "\\%c", *p);
        else
            badd(&b, "%c", *p);
    }
    badd(&b, "\"");
    free(d);
    return b.s;
}
char *join_path(const char *a, const char *b)
{
    if (!*a)
        return xstrdup(b);
    if (!*b)
        return xstrdup(a);
    return fmt("%s/%s", a, b);
}
char *parent_path(const char *s)
{
    char *r = xstrdup(s), *p = strrchr(r, '/');
    if (p)
        *p = 0;
    else
        *r = 0;
    return r;
}
const char *base_name(const char *s)
{
    const char *p = strrchr(s, '/');
    return p ? p + 1 : s;
}
int valid_relative(const char *s, int empty)
{
    if (!s || *s == '/' || (!*s && !empty))
        return 0;
    const char *p = s;
    while (*p) {
        const char *q = strchr(p, '/');
        size_t n = q ? (size_t)(q - p) : strlen(p);
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return 0;
        if (!q)
            break;
        p = q + 1;
        if (!*p)
            return 0;
    }
    return 1;
}
int descendant(const char *p, const char *root)
{
#ifdef _WIN32
    return cc_win_descendant(p, root);
#else
    size_t n = strlen(root);
    return !n || (!memcmp(p, root, strlen(p) < n ? strlen(p) : n) && strlen(p) >= n &&
                  (p[n] == 0 || p[n] == '/'));
#endif
}
int parent_beneath(int root, const char *path, int create, char **leaf)
{
    if (!valid_relative(path, 0)) {
        errno = EINVAL;
        return -1;
    }
    char *p = parent_path(path);
    *leaf = xstrdup(base_name(path));
    int fd = dup(root);
    char *save = NULL;
    for (char *part = strtok_r(p, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (create && mkdirat(fd, part, 0755) < 0 && errno != EEXIST) {
            close(fd);
            fd = -1;
            break;
        }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        fd = next;
        if (fd < 0)
            break;
    }
    free(p);
    return fd;
}
int open_beneath(int root, const char *path, int flags, mode_t mode)
{
    if (!*path)
        return openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char *leaf = NULL;
    int p = parent_beneath(root, path, 0, &leaf);
    if (p < 0) {
        free(leaf);
        return -1;
    }
    int fd = openat(p, leaf, flags | O_NOFOLLOW | O_CLOEXEC, mode);
    int e = errno;
    close(p);
    free(leaf);
    errno = e;
    return fd;
}
int mkdir_beneath(int root, const char *path)
{
    if (!*path)
        return 0;
    char *leaf = NULL;
    int p = parent_beneath(root, path, 1, &leaf);
    if (p < 0) {
        free(leaf);
        return -1;
    }
    int rc = mkdirat(p, leaf, 0755);
    if (rc < 0 && errno == EEXIST) {
        int t = openat(p, leaf, O_DIRECTORY | O_NOFOLLOW | O_RDONLY | O_CLOEXEC);
        if (t >= 0) {
            close(t);
            rc = 0;
        }
    }
    if (!rc)
        fsync(p);
    close(p);
    free(leaf);
    return rc;
}
int64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
int64_t monotonic_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
void duration_text(double seconds, char out[40])
{
    if (seconds < 0 || seconds > 315360000) {
        strcpy(out, "--");
        return;
    }
    uint64_t n = (uint64_t)seconds;
    if (n >= 86400)
        snprintf(out, 40, "%llud %02llu:%02llu", (unsigned long long)(n / 86400),
                 (unsigned long long)(n / 3600 % 24), (unsigned long long)(n / 60 % 60));
    else
        snprintf(out, 40, "%02llu:%02llu:%02llu", (unsigned long long)(n / 3600),
                 (unsigned long long)(n / 60 % 60), (unsigned long long)(n % 60));
}
void scan_status(Catalog *c, ScanStatus *s)
{
    memset(s, 0, sizeof(*s));
    s->phase = atomic_load(&c->scan.phase);
    s->found = atomic_load(&c->scan.found);
    s->done = atomic_load(&c->scan.done);
    s->planned_bytes = atomic_load(&c->scan.planned_bytes);
    s->resolved_bytes = atomic_load(&c->scan.resolved_bytes);
    uint64_t current = atomic_load(&c->scan.current_bytes),
             plan = atomic_load(&c->scan.current_plan);
    s->current_bytes = current < plan ? current : plan;
    s->read_bytes = atomic_load(&c->bytes);
    s->hash_files = atomic_load(&c->scan.hash_files);
    s->hashed_files = atomic_load(&c->scan.hashed_files);
    s->errors = atomic_load(&c->scan.errors);
    int64_t now = atomic_load(&c->scan.finished);
    if (!now)
        now = monotonic_ns();
    int64_t start = atomic_load(&c->scan.started), phase = atomic_load(&c->scan.phase_started);
    s->elapsed = start ? (now - start) / 1e9 : 0;
    s->phase_elapsed = phase ? (now - phase) / 1e9 : 0;
    s->fraction = -1;
    s->eta = -1;
    if (s->phase == SCAN_HASH) {
        s->fraction = s->planned_bytes
                          ? (double)(s->resolved_bytes + s->current_bytes) / s->planned_bytes
                      : s->found ? (double)s->done / s->found
                                 : 0;
        if (s->fraction > 1)
            s->fraction = 1;
        s->bytes_per_second = s->phase_elapsed > 0 ? s->read_bytes / s->phase_elapsed : 0;
        if (s->phase_elapsed >= 1 && (s->done || s->current_bytes)) {
            double remain = (double)s->planned_bytes - s->resolved_bytes - s->current_bytes;
            double by_bytes =
                remain > 0 && s->bytes_per_second > 0 ? remain / s->bytes_per_second : 0;
            double by_entries =
                s->done ? (double)(s->found - s->done) * s->phase_elapsed / s->done : 0;
            s->eta = by_bytes > by_entries ? by_bytes : by_entries;
            if (remain > 0 && s->bytes_per_second == 0)
                s->eta = -1;
        }
    } else if (s->phase == SCAN_EXPORT || s->phase == SCAN_DONE)
        s->fraction = 1;
}
int64_t mtime_ns(const struct stat *s)
{
    return (int64_t)s->st_mtim.tv_sec * 1000000000 + s->st_mtim.tv_nsec;
}
int64_t ctime_ns(const struct stat *s)
{
    return (int64_t)s->st_ctim.tv_sec * 1000000000 + s->st_ctim.tv_nsec;
}
int stat_same(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mode == b->st_mode && mtime_ns(a) == mtime_ns(b) && ctime_ns(a) == ctime_ns(b);
}
int write_all(int fd, const void *v, size_t n)
{
    const char *p = v;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        p += k;
        n -= k;
    }
    return 0;
}
int hash_fd(Catalog *c, int fd, char hash[129], struct stat *stable)
{
    hash[0] = 0;
    struct stat a, b;
    if (fstat(fd, &a) || !S_ISREG(a.st_mode))
        return fail(c, "Not a regular file");
    if (lseek(fd, 0, SEEK_SET) < 0)
        return fail(c, "seek: %s", strerror(errno));
    EVP_MD_CTX *m = EVP_MD_CTX_new();
    EVP_DigestInit_ex(m, EVP_sha512(), NULL);
    unsigned char *buf = xmalloc(1024 * 1024), digest[64];
    uint64_t total = 0;
    int rc = 0;
    /* Never chase an EOF that moves as another process appends data. Check the
     * original snapshot after every chunk, including truncation and rewrites. */
    while (total < (uint64_t)a.st_size) {
        if (c && atomic_load(&c->cancel)) {
            rc = fail(c, "Cancelled");
            break;
        }
        size_t want = (uint64_t)a.st_size - total > 1024 * 1024
                          ? 1024 * 1024
                          : (size_t)((uint64_t)a.st_size - total);
        ssize_t n = read(fd, buf, want);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            rc = fail(c,
                      errno == EBUSY || errno == EAGAIN
                          ? "UNSCANNED: file is busy; retry on the next scan (%s)"
                          : "read: %s",
                      strerror(errno));
            break;
        }
        if (!n)
            break;
        EVP_DigestUpdate(m, buf, n);
        total += n;
        if (c)
            atomic_fetch_add(&c->bytes, n);
        if (c && atomic_load(&c->scan.phase) == SCAN_HASH)
            atomic_fetch_add(&c->scan.current_bytes, n);
        if (fstat(fd, &b) || !stat_same(&a, &b)) {
            rc = fail(c, "UNSCANNED: file changed while reading; retry on the next scan");
            break;
        }
    }
    unsigned dl = 0;
    EVP_DigestFinal_ex(m, digest, &dl);
    EVP_MD_CTX_free(m);
    free(buf);
    if (!rc && (fstat(fd, &b) || !stat_same(&a, &b) || total != (uint64_t)a.st_size))
        rc = fail(c, "UNSCANNED: file changed while reading; retry on the next scan");
    if (!rc) {
        for (int i = 0; i < 64; i++)
            sprintf(hash + 2 * i, "%02x", digest[i]);
        if (stable)
            *stable = b;
    }
    return rc;
}
void human_bytes(uint64_t n, char out[40])
{
    const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = n;
    int i = 0;
    while (v >= 1024 && i < 5) {
        v /= 1024;
        i++;
    }
    snprintf(out, 40, i ? "%.2f %s" : "%.0f %s", v, u[i]);
}
void date_text(int64_t n, char out[40])
{
    if (!n) {
        strcpy(out, "n/a");
        return;
    }
    time_t t = n / 1000000000;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, 40, "%Y-%m-%d %H:%M", &tm);
}
void progress(Catalog *c, const char *s, ...)
{
    if (!c)
        return;
    pthread_mutex_lock(&c->progress_mutex);
    va_list a;
    va_start(a, s);
    vsnprintf(c->progress, sizeof(c->progress), s, a);
    va_end(a);
    pthread_mutex_unlock(&c->progress_mutex);
}
int fail(Catalog *c, const char *s, ...)
{
    if (c) {
        va_list a;
        va_start(a, s);
        vsnprintf(c->error, sizeof(c->error), s, a);
        va_end(a);
        activity_log(c,
                     strstr(c->error, "ancel") || !strncmp(c->error, "UNSCANNED:", 10) ? LOG_WARNING
                                                                                       : LOG_ERROR,
                     "application.error", NULL, NULL, "%s", c->error);
    }
    return -1;
}
int sql_error(Catalog *c, int rc, const char *operation)
{
    int primary = rc & 255, extended = sqlite3_extended_errcode(c->db);
    if ((extended & 255) != primary)
        extended = rc;
    return fail(c, "SQLite %s [%d/%d] during %.160s: %s%s",
                primary == SQLITE_BUSY     ? "busy"
                : primary == SQLITE_LOCKED ? "locked"
                                           : "error",
                primary, extended, operation, sqlite3_errmsg(c->db),
                primary == SQLITE_BUSY || primary == SQLITE_LOCKED
                    ? ". Another connection may hold the catalog. Close other CCurate instances "
                      "or database tools and retry. Do not delete the database, WAL or SHM files."
                    : "");
}
int sql_exec(Catalog *c, const char *sql)
{
    int rc = sqlite3_exec(c->db, sql, NULL, NULL, NULL);
    return rc == SQLITE_OK ? 0 : sql_error(c, rc, sql);
}
int sql_rollback(Catalog *c)
{
    if (!c->db || sqlite3_get_autocommit(c->db))
        return 0;
    sqlite3_progress_handler(c->db, 0, NULL, NULL);
    return sql_exec(c, "ROLLBACK");
}
int sql_end(Catalog *c, int result)
{
    if (!result)
        result = sql_exec(c, "COMMIT");
    /* A failed COMMIT can leave a write lock owned by this connection. */
    if (result)
        sql_rollback(c);
    return result;
}
sqlite3_stmt *sql_prepare(Catalog *c, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(c->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) {
        sql_error(c, rc, sql);
        return NULL;
    }
    return s;
}
void bind_text(sqlite3_stmt *s, int i, const char *v)
{
    if (v)
        sqlite3_bind_text(s, i, v, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, i);
}
char *setting(Catalog *c, const char *key, const char *fallback)
{
    sqlite3_stmt *s = sql_prepare(c, "SELECT value FROM settings WHERE key=?");
    bind_text(s, 1, key);
    char *r = sqlite3_step(s) == SQLITE_ROW ? xstrdup((char *)sqlite3_column_text(s, 0))
                                            : xstrdup(fallback);
    sqlite3_finalize(s);
    return r;
}
int set_setting(Catalog *c, const char *key, const char *value)
{
    sqlite3_stmt *s = sql_prepare(
        c, "INSERT INTO settings VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    if (!s)
        return -1;
    bind_text(s, 1, key);
    bind_text(s, 2, value);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : sql_error(c, rc, "save setting");
}
void event_log(Catalog *c, const char *op, const char *from, const char *to, const char *hash)
{
    activity_log(c, LOG_INFO, op, from, to, "%s%s", hash ? "SHA-512: " : "Recorded operation",
                 hash ? hash : "");
    sqlite3_stmt *s = sql_prepare(c, "INSERT INTO events(time,op,src,dst,hash) VALUES(?,?,?,?,?)");
    sqlite3_bind_int64(s, 1, now_ns());
    bind_text(s, 2, op);
    bind_text(s, 3, from);
    bind_text(s, 4, to);
    bind_text(s, 5, hash);
    sqlite3_step(s);
    sqlite3_finalize(s);
}
