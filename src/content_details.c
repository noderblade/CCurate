#include "content_details.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

void content_occurrences(Catalog *c, Buf *out, const char *hash, const char *empty_destination,
                         int page)
{
    /* Group before paging. The (hash,eid) index streams groups without sorting
     * every occurrence, and the extra row avoids a whole-group count. */
    const char *matches =
        empty_destination
            ? "SELECT v.eid,MAX(v.accepted) kept FROM empty_copies z JOIN versions v ON "
              "v.id=z.version WHERE z.dest=? GROUP BY v.eid ORDER BY v.eid LIMIT 257 OFFSET ?"
            : "SELECT v.eid,MAX(v.accepted) kept FROM versions v WHERE v.hash=? AND v.size>0 "
              "GROUP BY v.eid ORDER BY v.eid LIMIT 257 OFFSET ?";
    char *sql =
        fmt("WITH matches AS (%s) SELECT e.path,e.present,e.reviewed,e.discarded,m.kept,"
            "e.error,EXISTS(SELECT 1 FROM discoveries d WHERE d.eid=e.id),"
            "CASE WHEN ? THEN EXISTS(SELECT 1 FROM empty_copies z WHERE z.version=e.current "
            "AND z.dest=?) ELSE cv.hash=? AND cv.size>0 END,"
            "COALESCE(z.state,o.state),COALESCE(z.dest,o.dest) FROM matches m JOIN entries e "
            "ON e.id=m.eid LEFT JOIN versions cv ON cv.id=e.current "
            "LEFT JOIN objects o ON o.hash=? AND o.size>0 LEFT JOIN empty_copies z "
            "ON z.version=(SELECT MIN(version) FROM empty_copies WHERE dest=?) ORDER BY m.eid",
            matches);
    sqlite3_stmt *q = sql_prepare(c, sql);
    free(sql);
    badd(out, "\n%s (page %lld; Ctrl+Alt+PgUp/PgDn):\n",
         empty_destination ? "ORIGINAL EMPTY-FILE PATHS" : "CONTENT OCCURRENCES",
         page < 0 ? 1LL : (long long)page + 1);
    badd(out, "One row per recorded source path.\n");
    if (!q) {
        badd(out, "Cannot read occurrences: %s\n", sqlite3_errmsg(c->db));
        return;
    }
    bind_text(q, 1, empty_destination ? empty_destination : hash);
    sqlite3_bind_int64(q, 2, (int64_t)(page < 0 ? 0 : page) * 256);
    sqlite3_bind_int(q, 3, empty_destination != NULL);
    bind_text(q, 4, empty_destination);
    bind_text(q, 5, hash);
    bind_text(q, 6, hash);
    bind_text(q, 7, empty_destination);
    int count = 0, step;
    while ((step = sqlite3_step(q)) == SQLITE_ROW) {
        if (++count > 256) {
            badd(out, "More occurrences on the next page.\n");
            break;
        }
        const char *state = (const char *)sqlite3_column_text(q, 8);
        int retained = sqlite3_column_int(q, 4);
        int unverified = sqlite3_column_bytes(q, 5) || sqlite3_column_int(q, 6);
        int current = sqlite3_column_int(q, 7);
        char *path = display_path((const char *)sqlite3_column_text(q, 0));
        badd(out, "%s %s%s%s%s%s%s\n",
             retained && state && !strcmp(state, "verified") && sqlite3_column_text(q, 9) ? "[OK]"
                                                                                          : "[ ]",
             path, sqlite3_column_int(q, 2) ? " [reviewed]" : "",
             sqlite3_column_int(q, 3) ? " [rejected]" : "",
             !sqlite3_column_int(q, 1) ? " [absent at last scan]" : "",
             unverified ? " [current content unverified]" : "",
             !current ? " [historical content only]" : "");
        free(path);
    }
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        badd(out, "Cannot read occurrences: %s\n", sqlite3_errmsg(c->db));
    else if (!count)
        badd(out, "No occurrences on this page.\n");
    sqlite3_finalize(q);
    badd(out, "Paths and current-content status reflect the saved catalog.\n");
}

enum {
    VF_MODE,
    VF_SIZE,
    VF_UID,
    VF_GID,
    VF_MTIME,
    VF_CTIME,
    VF_ATIME,
    VF_BTIME,
    VF_FILESYSTEM,
    VF_INO,
    VF_HASH,
    VF_LINK,
    VF_ATTRS,
    VF_META_ERROR,
    VF_FIELD_COUNT,
    HISTORY_PAGE_SIZE = 16
};
static const char *fields[VF_FIELD_COUNT] = {"mode",  "size",  "uid",   "gid",        "mtime",
                                             "ctime", "atime", "btime", "filesystem", "ino",
                                             "hash",  "link",  "attrs", "meta_error"};
static const char *labels[VF_FIELD_COUNT] = {"Type / permissions",
                                             "Size (bytes)",
                                             "UID",
                                             "GID",
                                             "mtime",
                                             "ctime",
                                             "atime",
                                             "btime",
                                             "Filesystem reference",
                                             "Inode",
                                             "SHA-512",
                                             "Symlink target",
                                             "xattrs / ACLs",
                                             "Metadata error"};

static void timestamp(int64_t ns, char out[80], int birth)
{
    if (birth && !ns) {
        strcpy(out, "unavailable");
        return;
    }
    int64_t seconds = ns / 1000000000, remainder = ns % 1000000000;
    if (remainder < 0) {
        seconds--;
        remainder += 1000000000;
    }
    time_t t = (time_t)seconds;
    struct tm tm;
    char date[40], zone[12] = {0};
    if (!localtime_r(&t, &tm) || !strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &tm)) {
        snprintf(out, 80, "%lld ns since Unix epoch", (long long)ns);
        return;
    }
    strftime(zone, sizeof(zone), "%z", &tm);
    snprintf(out, 80, "%s.%09lld %s", date, (long long)remainder, zone);
}

static int equal_value(sqlite3_stmt *q, int a, int b)
{
    int na = sqlite3_column_type(q, a) == SQLITE_NULL;
    int nb = sqlite3_column_type(q, b) == SQLITE_NULL;
    if (na || nb)
        return na == nb;
    const void *av = sqlite3_column_blob(q, a), *bv = sqlite3_column_blob(q, b);
    int n = sqlite3_column_bytes(q, a);
    return n == sqlite3_column_bytes(q, b) && (!n || !memcmp(av, bv, n));
}

static int equal_field(sqlite3_stmt *q, int field)
{
    int a = 5 + field * 2, b = a + 1;
    /* Filling previously unknown provenance is not a file change. */
    if (field == VF_FILESYSTEM &&
        (sqlite3_column_type(q, a) == SQLITE_NULL || sqlite3_column_type(q, b) == SQLITE_NULL))
        return 1;
    return equal_value(q, a, b);
}

static void value_text(Buf *out, sqlite3_stmt *q, int column, int field)
{
    if (sqlite3_column_type(q, column) == SQLITE_NULL) {
        badd(out, "unavailable");
    } else if (field >= VF_MTIME && field <= VF_BTIME) {
        char value[80];
        timestamp(sqlite3_column_int64(q, column), value, field == VF_BTIME);
        badd(out, "%s", value);
    } else if (field == VF_MODE) {
        badd(out, "%06llo", (unsigned long long)sqlite3_column_int64(q, column));
    } else if (field < VF_HASH) {
        badd(out, "%lld", (long long)sqlite3_column_int64(q, column));
    } else {
        char *value = display_path((const char *)sqlite3_column_text(q, column));
        /* Hashes/errors are short; avoid expanding a very long symlink into an
         * unbounded history page. Original bytes remain in the catalog. */
        if (strlen(value) > 1024)
            badd(out, "[value too long to display; %d stored bytes]",
                 sqlite3_column_bytes(q, column));
        else
            badd(out, "%s", value);
        free(value);
    }
}

void content_version_history(Catalog *c, Buf *out, int64_t entry_id, int64_t selected_version,
                             DetailHistory *history)
{
    history->available = history->more = 0;
    sqlite3_stmt *q =
        sql_prepare(c, "SELECT e.path,e.current,(SELECT COUNT(*) FROM versions WHERE eid=e.id) "
                       "FROM entries e WHERE e.id=?");
    if (!q) {
        badd(out, "\nCannot read version history: %s\n", sqlite3_errmsg(c->db));
        return;
    }
    sqlite3_bind_int64(q, 1, entry_id);
    int step = sqlite3_step(q);
    if (step != SQLITE_ROW) {
        if (step != SQLITE_DONE)
            badd(out, "\nCannot read version history: %s\n", sqlite3_errmsg(c->db));
        sqlite3_finalize(q);
        return;
    }
    int64_t current = sqlite3_column_int64(q, 1), count = sqlite3_column_int64(q, 2);
    char *path = display_path((const char *)sqlite3_column_text(q, 0));
    badd(out, "\nVERSION HISTORY: %s\n%lld catalog record%s; %lld earlier record%s.\n",
         *path ? path : "/", (long long)count, count == 1 ? "" : "s",
         (long long)(count ? count - 1 : 0), count == 2 ? "" : "s");
    free(path);
    sqlite3_finalize(q);
    if (!count)
        return;
    history->available = 1;
    if (!history->expanded) {
        badd(out, "Show changes: Version history button / Ctrl+Shift+V.\n");
        return;
    }
    if (history->page < 0)
        history->page = 0;
    if ((int64_t)history->page * HISTORY_PAGE_SIZE >= count)
        history->page = (int)((count - 1) / HISTORY_PAGE_SIZE);
    badd(out, "Page %d; Ctrl+Shift+PgUp/PgDn. Times include nanoseconds and local UTC offset.\n",
         history->page + 1);
    Buf sql = {0};
    badd(&sql, "SELECT v.id,old.id,v.accepted,COALESCE(z.dest,o.dest),COALESCE(z.state,o.state)");
    for (int i = 0; i < VF_FIELD_COUNT; i++)
        badd(&sql, ",v.%s,old.%s", fields[i], fields[i]);
    badd(&sql, " FROM versions v LEFT JOIN versions old ON old.id=(SELECT p.id FROM versions p "
               "WHERE p.eid=v.eid AND p.id<v.id ORDER BY p.id DESC LIMIT 1) "
               "LEFT JOIN objects o ON o.hash=v.hash AND v.size>0 "
               "LEFT JOIN empty_copies z ON z.version=v.id WHERE v.eid=? "
               "ORDER BY v.id DESC LIMIT 17 OFFSET ?");
    q = sql_prepare(c, sql.s);
    bfree(&sql);
    if (!q) {
        badd(out, "Cannot read version history: %s\n", sqlite3_errmsg(c->db));
        return;
    }
    sqlite3_bind_int64(q, 1, entry_id);
    sqlite3_bind_int64(q, 2, (int64_t)history->page * HISTORY_PAGE_SIZE);
    int rows = 0;
    while ((step = sqlite3_step(q)) == SQLITE_ROW) {
        if (++rows > HISTORY_PAGE_SIZE) {
            history->more = 1;
            break;
        }
        int64_t id = sqlite3_column_int64(q, 0), previous = sqlite3_column_int64(q, 1);
        if (previous)
            badd(out, "\nv%lld -> v%lld", (long long)previous, (long long)id);
        else
            badd(out, "\nv%lld", (long long)id);
        badd(out, "%s%s [%s]\n", id == current ? " [current record]" : "",
             selected_version && id == selected_version ? " [selected]" : "",
             sqlite3_column_int(q, 2) ? "retained" : "not retained");
        if (sqlite3_column_text(q, 3)) {
            char *dest = display_path((const char *)sqlite3_column_text(q, 3));
            badd(out, "  Destination [%s]: %s\n", sqlite3_column_text(q, 4), dest);
            free(dest);
        } else if (sqlite3_column_int(q, 2))
            badd(out, "  Retained metadata; no destination copy.\n");
        if (!previous) {
            badd(out, "  First catalog observation; no previous record to compare.\n");
            continue;
        }
        int changed = 0;
        for (int i = 0; i < VF_FIELD_COUNT; i++)
            changed += !equal_field(q, i);
        int hash_column = 5 + VF_HASH * 2;
        int comparable = sqlite3_column_bytes(q, hash_column) == 128 &&
                         sqlite3_column_bytes(q, hash_column + 1) == 128;
        if (comparable && equal_value(q, hash_column, hash_column + 1))
            badd(out, "  %s; content unchanged (same SHA-512).\n",
                 changed ? "Metadata changed" : "No recorded metadata changes");
        else if (comparable)
            badd(out, "  Content changed (different SHA-512).\n");
        else
            badd(out, "  Content comparison unavailable (no pair of recorded SHA-512 hashes).\n");
        for (int i = 0; i < VF_FIELD_COUNT; i++) {
            int newer = 5 + i * 2, older = newer + 1;
            if (equal_field(q, i))
                continue;
            if (i == VF_ATTRS) {
                badd(out, "  xattrs / ACLs: changed (full values retained in catalog).\n");
                continue;
            }
            badd(out, "  %s:\n    previous: ", labels[i]);
            value_text(out, q, older, i);
            badd(out, "\n    new:      ");
            value_text(out, q, newer, i);
            badd(out, "\n");
        }
    }
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        badd(out, "Cannot read version history: %s\n", sqlite3_errmsg(c->db));
    else if (history->more)
        badd(out, "\nMore records on the next history page.\n");
    sqlite3_finalize(q);
}
