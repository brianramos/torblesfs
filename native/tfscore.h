/* torblesFS native core: the mapped index (TFSNIDX1), its live overlay (INDEX.side: tombstones + changed entries),
 * the scanner and builder, change application, the inotify watcher and the btrfs send-stream parser. */
#ifndef TFSCORE_H
#define TFSCORE_H
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>

typedef struct {
    uint64_t version, n, ndirs, nnames, wn, nw, nroots, ndelta;
    uint64_t off_K, off_type, off_size, off_blocks, off_mtime, off_mode, off_uid, off_gid, off_nlink, off_inode, off_dev;
    uint64_t off_T, off_dstart, off_doff, off_dblob, off_noff, off_nblob, off_roff, off_rblob, off_xoff, off_xblob, off_xattr, file_size;
    uint64_t index_id, flags;                       /* format 3: identity (pairs the sidecar), bit 0 = one filesystem */
} hdr_t;

enum { A_TYPE, A_SIZE, A_BLOCKS, A_MTIME, A_MODE, A_UID, A_GID, A_NLINK, A_INODE, A_DEV, NATTR };
typedef struct { int64_t a[NATTR]; } attrs_t;
typedef struct { char *path; attrs_t at; int live; } dent_t;

/* the index: base (mapped, read-only) + overlay (tombstone bitmap copy + delta entries keyed by path) */
typedef struct {
    const uint8_t *base; size_t map_size; hdr_t H;
    const uint64_t *K, *dstart, *doff, *noff, *roff, *xoff; const uint8_t *TY, *dblob, *nblob, *rblob, *xblob;
    const int64_t *col[NATTR];                      /* A_TYPE column is TY (uint8): col[A_TYPE] unused */
    uint64_t *T;                                    /* mutable tombstones */
    dent_t *dx; size_t ndx, capx;                   /* delta entries (live == 0: removed) */
    int64_t *ht; size_t hcap, hused;                /* path -> delta index */
    uint64_t nmask; char *path; uint64_t gen;       /* gen: bumped on every change */
    int dirty;
} tfsix_t;

typedef struct { int64_t row; dent_t *d; } ent_t;  /* a live entry: a base row or a delta entry */
typedef struct { uint64_t a, b; } span_t;

extern tfsix_t IX;
int  ix_open(const char *path);                     /* map + load the sidecar if it matches; returns 0 */
void ix_close(void);
const char *dstr(uint64_t i); uint64_t dlen(uint64_t i); const char *nstr(uint64_t i); uint64_t nlen(uint64_t i);
const char *root_str(uint64_t i);
uint64_t dict_lower(const uint64_t *off, const uint8_t *blob, uint64_t count, const char *s, size_t sl);
int64_t dict_find(const uint64_t *off, const uint8_t *blob, uint64_t count, const char *s);
static inline int ix_dead(uint64_t r) { return (IX.T[r >> 6] >> (r & 63)) & 1; }
static inline uint64_t ix_dc(uint64_t r) { return IX.K[r] >> IX.H.wn; }
static inline uint64_t ix_nc(uint64_t r) { return IX.K[r] & IX.nmask; }
int64_t ix_attr(const ent_t *e, int k);
const char *row_path(uint64_t r, char *buf);
int64_t ix_row(const char *path);                   /* live base row or -1 */
int  ix_lookup(const char *path, ent_t *e);         /* delta first, then base */
int  ix_subtree(const char *path, span_t *out);     /* rows strictly below path: up to 2 spans */
int  path_under(const char *p, const char *root);   /* p == root or below it */
void split_path(const char *p, char *dir, char *name);
void norm_path(const char *in, char *out);

/* changes (overlay) */
void ix_put(const char *path, const attrs_t *at);
int  ix_delete(const char *path);
size_t ix_delete_subtree(const char *path);
int  ix_put_path(const char *path);                 /* lstat; stores or deletes; re-reads other hard-link names */
void attrs_from_stat(const struct stat *st, attrs_t *at);
void ix_removed(const char *path);                /* deleted / renamed away (subtree, hard links) */
void ix_put_tree(const char *path);               /* created / renamed in (a directory with everything below) */
int  ix_save_sidecar(void);
size_t ix_live_delta(void);

/* scanning and building */
typedef struct { char **parent; char **name; attrs_t *at; size_t n, cap; } rows_t;
void rows_add(rows_t *r, const char *parent, const char *name, const attrs_t *at);
void rows_free(rows_t *r);
int  scan_roots(char **roots, int nroots, int one_fs, rows_t *out);
int  build_index(rows_t *rows, char **roots, int nroots, int one_fs, const char *out_path);
int  ix_rows(rows_t *out);                          /* every live entry of base + overlay */
int  ix_refresh_overlay(size_t *created, size_t *modified, size_t *deleted);   /* rescan, apply the differences */

/* watcher */
int  watch_open(void);                              /* inotify fd watching every indexed directory */
int  watch_poll(int fd);                            /* read and apply pending events; returns how many */
size_t watch_count(void);

/* btrfs send-stream (receive --dump) */
size_t btrfs_apply_dump(const char *text, size_t len, const char *subvol, size_t *nops);
#endif
