#ifndef CCURATE_H
#define CCURATE_H
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include "platform/windows/fs_windows.h"
#else
#include <sys/stat.h>
#endif

/* CCurate session state lives on the destination. */
#define META_DIR ".ccurate"
#define SCHEMA_VERSION 1
#define HASH_LEN 128
enum { SCAN_IDLE, SCAN_ENUMERATE, SCAN_HASH, SCAN_EXPORT, SCAN_DONE, SCAN_STOPPED };
typedef struct {
    atomic_int phase;
    atomic_ullong found, done, planned_bytes, resolved_bytes, current_bytes, current_plan;
    atomic_ullong hash_files, hashed_files, errors;
    atomic_llong started, phase_started, finished;
} ScanProgress;
typedef struct {
    int phase;
    uint64_t found, done, planned_bytes, resolved_bytes, current_bytes, read_bytes;
    uint64_t hash_files, hashed_files, errors;
    double elapsed, phase_elapsed, fraction, bytes_per_second, eta;
} ScanStatus;
typedef struct {
    atomic_ullong database, wal, shm, jsonl, free_bytes;
} StorageStatus;
typedef struct {
    char *s;
    size_t n, cap;
} Buf;
enum { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERROR };
typedef struct ActivityLog ActivityLog;
typedef struct {
    int level;
    char *text;
} ActivityLine;
typedef struct {
    ActivityLog *log;
    sqlite3 *db;
    char *source, *destination, *state_dir;
    int sfd, dfd, mfd, lockfd, readonly;
    atomic_int cancel;
    atomic_ullong files, bytes;
    ScanProgress scan;
    StorageStatus storage;
    pthread_mutex_t progress_mutex;
    char progress[1024], error[1024];
} Catalog;
typedef struct {
    int64_t id, version, size, mtime, btime, mode, uid, gid, dev, ino;
    char *path, *parent, *name, *kind, *hash, *dest, *state, *error;
    int present, reviewed, discarded, accepted, copies, original, pending;
    int64_t children, secured;
} Entry;
typedef struct {
    uint64_t paths, secured, rejected, reviewed, errors, missing, special;
    uint64_t unique, unique_bytes, needed_bytes, free_bytes, total_bytes;
} Summary;
typedef struct {
    uint8_t *rgba;
    int width, height, pages, error;
    char *text;
} Preview;
#define VIEW_PAGE_SIZE 1000
enum { SORT_NAME, SORT_STATUS, SORT_SIZE, SORT_MTIME, SORT_BTIME, SORT_MODE, SORT_COUNT };
enum { SORT_DESC = 256, SORT_COLUMN_MASK = 255 };

enum {
    VIEW_LEFT = 1,
    VIEW_RIGHT = 2,
    VIEW_DETAILS = 4,
    VIEW_SUMMARY = 8,
    VIEW_BALANCE = 16,
    VIEW_CYCLE = 32
};
typedef struct {
    unsigned flags;
    char *path[2], *search[2], *anchor[2], *detail_path;
    int page[2], sort[2], historical, unsecured, fast, target_refresh, occurrence_page;
    int64_t detail_version;
} ViewRequest;
typedef struct {
    uint64_t serial;
    unsigned flags;
    Entry *rows[2];
    int count[2], page[2];
    int64_t total[2];
    Summary summary;
    char *details, *balance;
    Entry cycle;
} ViewResult;
typedef struct ViewService ViewService;
ViewService *view_start(Catalog *c);
uint64_t view_submit(ViewService *s, const ViewRequest *request);
ViewResult *view_take(ViewService *s);
int view_busy(ViewService *s);
void view_result_free(ViewResult *r);
void view_save(ViewService *s, const char *left, const char *right, int selected, float scale,
               int filter, int page, float split_top, float split_bottom, float split_rows,
               int theme, int log_visible, int log_level, float split_log, const char *font_file,
               const char *font_family, const char *font_style, int font_index, int sort_left,
               int sort_right, int created_left, int created_right);
void view_stop(ViewService *s);

ActivityLog *activity_open(int state_fd);
void activity_close(ActivityLog *log);
uint64_t activity_status(ActivityLog *log, char error[256]);
void activity_log(Catalog *c, int level, const char *event, const char *path, const char *target,
                  const char *format, ...);
ActivityLine *activity_snapshot(ActivityLog *log, int level, int *count, uint64_t *revision,
                                char error[256]);
void activity_lines_free(ActivityLine *lines, int count);

void *xmalloc(size_t n);
char *xstrdup(const char *s);
char *fmt(const char *s, ...);
void badd(Buf *b, const char *s, ...);
void bfree(Buf *b);
char *b64(const void *p, size_t n);
unsigned char *unb64(const char *s, size_t *len);
char *json_string(const char *s);
char *display_path(const char *s);
char *join_path(const char *a, const char *b);
char *parent_path(const char *s);
const char *base_name(const char *s);
int valid_relative(const char *s, int allow_empty);
int descendant(const char *path, const char *parent);
int open_beneath(int root, const char *path, int flags, mode_t mode);
int parent_beneath(int root, const char *path, int create, char **leaf);
int mkdir_beneath(int root, const char *path);
int64_t now_ns(void);
int64_t monotonic_ns(void);
void scan_status(Catalog *c, ScanStatus *s);
void duration_text(double seconds, char out[40]);
int64_t mtime_ns(const struct stat *s);
int64_t ctime_ns(const struct stat *s);
int stat_same(const struct stat *a, const struct stat *b);
int write_all(int fd, const void *p, size_t n);
int hash_fd(Catalog *c, int fd, char hash[129], struct stat *stable);
void human_bytes(uint64_t n, char out[40]);
void date_text(int64_t ns, char out[40]);
void progress(Catalog *c, const char *f, ...);
int fail(Catalog *c, const char *f, ...);
int sql_exec(Catalog *c, const char *sql);
sqlite3_stmt *sql_prepare(Catalog *c, const char *sql);
void bind_text(sqlite3_stmt *s, int i, const char *v);
char *setting(Catalog *c, const char *key, const char *fallback);
int set_setting(Catalog *c, const char *key, const char *value);
void event_log(Catalog *c, const char *op, const char *from, const char *to, const char *hash);
int catalog_open(Catalog *c, const char *source, const char *destination, int browse);
void catalog_close(Catalog *c);
int scan_source(Catalog *c, int full);
int export_archive(Catalog *c);
int import_archive(Catalog *c, const char *filename);
int recover_operations(Catalog *c);
int scan_one(Catalog *c, const char *path, int full, int64_t generation);
int load_entry(Catalog *c, const char *path, Entry *out, int historical);
int load_version(Catalog *c, const char *path, int64_t version, Entry *out);
void preserve_parents(Catalog *c, const char *path);
int list_entries(Catalog *c, const char *path, Entry **out, int historical, int unsecured,
                 const char *search);
int list_entries_page(Catalog *c, const char *path, Entry **out, int historical, int unsecured,
                      const char *search, int page, int limit, int fast, int sort, int64_t *total);
int64_t count_entries(Catalog *c, const char *path, int historical, int unsecured,
                      const char *search, int fast);
int64_t entry_position(Catalog *c, const char *parent, const char *anchor, int historical,
                       int unsecured, const char *search, int fast, int sort);
void entry_free(Entry *e);
void entries_free(Entry *e, int n);
char *entry_details(Catalog *c, const Entry *e);
char *entry_details_page(Catalog *c, const Entry *e, int page);
int prepare_hash_groups(Catalog *c);
void catalog_summary(Catalog *c, Summary *s);
char *summary_text(Catalog *c);
int mark_entries(Catalog *c, const char *path, const char *action);
int copy_entry(Catalog *c, const char *source, const char *target_dir, int complete);
int move_target(Catalog *c, const char *from, const char *to);
int audit_target(Catalog *c);
int reconcile_target(Catalog *c);
int preview_read(int fd, int page, int hex_only, Preview *p);
int preview_worker_main(int fd, int page, int hex_only);
int preview_spawn(int fd, int page, int hex_only, Preview *p, atomic_int *cancel);
void preview_free(Preview *p);
#endif
