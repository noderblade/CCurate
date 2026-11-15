#include "source_filesystem.h"
#include <stdlib.h>
#include <unistd.h>

typedef struct SourceFilesystem {
    struct SourceFilesystem *next;
    uint64_t device;
    int fd;
    int64_t id;
    unsigned epoch;
    char *identity, *name;
} SourceFilesystem;

int64_t source_filesystem(Catalog *c, int parent, const char *leaf, const struct stat *st)
{
    SourceFilesystem *f;
    for (f = c->filesystems; f; f = f->next)
        if (f->device == (uint64_t)st->st_dev)
            break;
    if (!f) {
        char *identity = NULL, *name = NULL;
        int fd = source_filesystem_probe(parent, leaf, st, &identity, &name);
        if (fd < 0) {
            free(identity);
            free(name);
            return 0;
        }
        f = xmalloc(sizeof(*f));
        f->device = st->st_dev;
        f->fd = fd;
        f->identity = identity;
        f->name = name ? name : xstrdup("Unknown filesystem");
        f->next = c->filesystems;
        c->filesystems = f;
        if (!identity)
            activity_log(c, LOG_WARNING, "source.identity", f->name, NULL,
                         "Persistent filesystem identity unavailable; content will be verified "
                         "before trusting saved hashes. For ZFS, install zfs/zpool utilities "
                         "and allow reading the guid properties.");
    }
    if (!f->identity)
        return 0;
    if (f->id && f->epoch == c->filesystem_epoch)
        return f->id;
    sqlite3_stmt *s = sql_prepare(c, "INSERT INTO source_filesystems(identity,name) VALUES(?,?) "
                                     "ON CONFLICT(identity) DO UPDATE SET name=excluded.name "
                                     "WHERE name<>excluded.name");
    if (!s)
        return -1;
    bind_text(s, 1, f->identity);
    bind_text(s, 2, f->name);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE)
        return sql_error(c, rc, "register source filesystem");
    s = sql_prepare(c, "SELECT id FROM source_filesystems WHERE identity=?");
    if (!s)
        return -1;
    bind_text(s, 1, f->identity);
    rc = sqlite3_step(s);
    f->id = rc == SQLITE_ROW ? sqlite3_column_int64(s, 0) : 0;
    sqlite3_finalize(s);
    f->epoch = c->filesystem_epoch;
    return f->id ? f->id : sql_error(c, rc, "resolve source filesystem");
}

void source_filesystems_close(Catalog *c)
{
    SourceFilesystem *f = c->filesystems;
    while (f) {
        SourceFilesystem *next = f->next;
        close(f->fd);
        free(f->identity);
        free(f->name);
        free(f);
        f = next;
    }
    c->filesystems = NULL;
}

void source_filesystem_details(Catalog *c, Buf *out, int64_t id)
{
    if (!id) {
        badd(out, "Filesystem identity: not recorded / unavailable\n");
        return;
    }
    sqlite3_stmt *s = sql_prepare(c, "SELECT name,identity FROM source_filesystems WHERE id=?");
    if (!s)
        return;
    sqlite3_bind_int64(s, 1, id);
    if (sqlite3_step(s) == SQLITE_ROW) {
        char *name = display_path((const char *)sqlite3_column_text(s, 0));
        char *identity = display_path((const char *)sqlite3_column_text(s, 1));
        badd(out, "Filesystem #%lld: %s\nIdentity: %s\n", (long long)id, name, identity);
        free(name);
        free(identity);
    }
    sqlite3_finalize(s);
}
