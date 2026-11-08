/* torblesFS native core: see tfscore.h */
#include "tfscore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/inotify.h>
#include <sys/random.h>
#include <poll.h>

tfsix_t IX;
static void die(const char *m) { fprintf(stderr, "tfs: %s\n", m); exit(2); }

/* ------------------------------------------------------------------ strings and dictionaries */
const char *dstr(uint64_t i) { return (const char *)(IX.dblob + IX.doff[i]); }
uint64_t dlen(uint64_t i) { return IX.doff[i + 1] - IX.doff[i] - 1; }
const char *nstr(uint64_t i) { return (const char *)(IX.nblob + IX.noff[i]); }
uint64_t nlen(uint64_t i) { return IX.noff[i + 1] - IX.noff[i] - 1; }
const char *root_str(uint64_t i) { return (const char *)(IX.rblob + IX.roff[i]); }
static int bcmp_str(const uint8_t *a, uint64_t al, const char *b, size_t bl) {    /* al includes the NUL */
    al--; int c = memcmp(a, b, al < bl ? al : bl); return c ? c : (al < bl ? -1 : al > bl);
}
uint64_t dict_lower(const uint64_t *off, const uint8_t *blob, uint64_t count, const char *s, size_t sl) {
    uint64_t lo = 0, hi = count;
    while (lo < hi) { uint64_t m = (lo + hi) / 2; if (bcmp_str(blob + off[m], off[m + 1] - off[m], s, sl) < 0) lo = m + 1; else hi = m; }
    return lo;
}
int64_t dict_find(const uint64_t *off, const uint8_t *blob, uint64_t count, const char *s) {
    size_t sl = strlen(s); uint64_t i = dict_lower(off, blob, count, s, sl);
    return (i < count && bcmp_str(blob + off[i], off[i + 1] - off[i], s, sl) == 0) ? (int64_t)i : -1;
}
void split_path(const char *p, char *dir, char *name) {
    const char *s = strrchr(p, '/');
    if (!s) { strcpy(dir, "."); strcpy(name, p); return; }
    if (s == p) strcpy(dir, "/"); else { memcpy(dir, p, s - p); dir[s - p] = 0; }
    strcpy(name, s + 1);
}
void norm_path(const char *in, char *out) {
    size_t j = 0;
    for (size_t i = 0; in[i]; i++) { if (in[i] == '/' && j && out[j - 1] == '/') continue; out[j++] = in[i]; }
    while (j > 1 && out[j - 1] == '/') j--;
    out[j] = 0;
}
int path_under(const char *p, const char *root) {
    size_t l = strlen(root); if (l == 1 && root[0] == '/') return p[0] == '/';
    return !strncmp(p, root, l) && (p[l] == '/' || p[l] == 0);
}
const char *row_path(uint64_t r, char *buf) {
    uint64_t d = ix_dc(r), m = ix_nc(r), dl = dlen(d), nl = nlen(m);
    if (dl == 1 && dstr(d)[0] == '/') { buf[0] = '/'; memcpy(buf + 1, nstr(m), nl + 1); }
    else { memcpy(buf, dstr(d), dl); buf[dl] = '/'; memcpy(buf + dl + 1, nstr(m), nl + 1); }
    return buf;
}

/* ------------------------------------------------------------------ delta hash (path -> index in dx) */
static uint64_t hstr(const char *s) { uint64_t h = 1469598103934665603ull; for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull; return h; }
static void ht_rebuild(size_t cap) {
    free(IX.ht); IX.hcap = cap; IX.ht = malloc(cap * sizeof *IX.ht); memset(IX.ht, 0xff, cap * sizeof *IX.ht); IX.hused = 0;
    for (size_t i = 0; i < IX.ndx; i++) {
        if (!IX.dx[i].live) continue;
        size_t s = hstr(IX.dx[i].path) & (cap - 1); while (IX.ht[s] >= 0) s = (s + 1) & (cap - 1);
        IX.ht[s] = (int64_t)i; IX.hused++;
    }
}
static int64_t dx_find(const char *path) {
    if (!IX.hcap) return -1;
    size_t s = hstr(path) & (IX.hcap - 1);
    while (IX.ht[s] >= 0) { dent_t *d = &IX.dx[IX.ht[s]]; if (d->live && !strcmp(d->path, path)) return IX.ht[s]; s = (s + 1) & (IX.hcap - 1); }
    return -1;
}
static void dx_set(const char *path, const attrs_t *at) {
    int64_t i = dx_find(path);
    if (i >= 0) { IX.dx[i].at = *at; return; }
    if (IX.ndx == IX.capx) { IX.capx = IX.capx ? 2 * IX.capx : 1024; IX.dx = realloc(IX.dx, IX.capx * sizeof *IX.dx); }
    IX.dx[IX.ndx] = (dent_t){ strdup(path), *at, 1 }; IX.ndx++;
    if ((IX.hused + 1) * 2 > IX.hcap) ht_rebuild(IX.hcap ? IX.hcap * 2 : 4096);
    else { size_t s = hstr(path) & (IX.hcap - 1); while (IX.ht[s] >= 0) s = (s + 1) & (IX.hcap - 1); IX.ht[s] = (int64_t)(IX.ndx - 1); IX.hused++; }
}
static int dx_remove(const char *path) {
    int64_t i = dx_find(path); if (i < 0) return 0;
    IX.dx[i].live = 0; return 1;                     /* its slot stays as a tombstone: probing continues past it */
}
size_t ix_live_delta(void) { size_t n = 0; for (size_t i = 0; i < IX.ndx; i++) n += IX.dx[i].live; return n; }

/* ------------------------------------------------------------------ open: index + sidecar */
static char *side_path(void) { static char p[8192]; snprintf(p, sizeof p, "%s.side", IX.path); return p; }
static void load_delta_section(const uint64_t *off, const uint8_t *blob, const int64_t *attrs, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        attrs_t at; memcpy(at.a, attrs + i * NATTR, sizeof at.a); dx_set((const char *)(blob + off[i]), &at);
    }
}
int ix_open(const char *path) {
    memset(&IX, 0, sizeof IX); IX.path = strdup(path);
    int fd = open(path, O_RDONLY); if (fd < 0) { perror(path); exit(2); }
    struct stat st; fstat(fd, &st);
    IX.base = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd); IX.map_size = st.st_size;
    if (IX.base == MAP_FAILED || st.st_size < 512 || memcmp(IX.base, "TFSNIDX1", 8)) die("not a native torblesFS index (TFSNIDX1)");
    memcpy(&IX.H, IX.base + 8, sizeof IX.H);
    if (IX.H.version != 3 || IX.H.file_size != (uint64_t)st.st_size) die("unsupported or truncated native index (rebuild it)");
#define P(t, f) ((const t *)(IX.base + IX.H.f))
    IX.K = P(uint64_t, off_K); IX.TY = P(uint8_t, off_type);
    IX.col[A_SIZE] = P(int64_t, off_size); IX.col[A_BLOCKS] = P(int64_t, off_blocks); IX.col[A_MTIME] = P(int64_t, off_mtime);
    IX.col[A_MODE] = P(int64_t, off_mode); IX.col[A_UID] = P(int64_t, off_uid); IX.col[A_GID] = P(int64_t, off_gid);
    IX.col[A_NLINK] = P(int64_t, off_nlink); IX.col[A_INODE] = P(int64_t, off_inode); IX.col[A_DEV] = P(int64_t, off_dev);
    IX.dstart = P(uint64_t, off_dstart); IX.doff = P(uint64_t, off_doff); IX.dblob = P(uint8_t, off_dblob);
    IX.noff = P(uint64_t, off_noff); IX.nblob = P(uint8_t, off_nblob); IX.roff = P(uint64_t, off_roff); IX.rblob = P(uint8_t, off_rblob);
    IX.xoff = P(uint64_t, off_xoff); IX.xblob = P(uint8_t, off_xblob);
    IX.nmask = (1ull << IX.H.wn) - 1;
    IX.T = malloc((IX.H.nw ? IX.H.nw : 1) * 8); memcpy(IX.T, IX.base + IX.H.off_T, IX.H.nw * 8);
    load_delta_section(IX.xoff, IX.xblob, P(int64_t, off_xattr), IX.H.ndelta);
    /* the sidecar, if it belongs to this index, replaces the file's tombstones and delta */
    int sfd = open(side_path(), O_RDONLY);
    if (sfd >= 0) {
        struct stat ss; fstat(sfd, &ss); uint8_t *s = mmap(0, ss.st_size, PROT_READ, MAP_PRIVATE, sfd, 0); close(sfd);
        if (s != MAP_FAILED && ss.st_size >= 64 && !memcmp(s, "TFSSIDE1", 8)) {
            uint64_t h[6]; memcpy(h, s + 8, sizeof h);        /* index_id, nw, ndelta, off_xoff, off_xblob, off_xattr */
            if (h[0] == IX.H.index_id && h[1] == IX.H.nw) {
                memcpy(IX.T, s + 64, IX.H.nw * 8);
                for (size_t i = 0; i < IX.ndx; i++) IX.dx[i].live = 0;
                ht_rebuild(IX.hcap ? IX.hcap : 4096);
                load_delta_section((const uint64_t *)(s + h[3]), s + h[4], (const int64_t *)(s + h[5]), h[2]);
            }
            munmap(s, ss.st_size);
        }
    }
    IX.gen = 1;
    return 0;
}
void ix_close(void) { if (IX.base) munmap((void *)IX.base, IX.map_size); IX.base = NULL; }

int64_t ix_attr(const ent_t *e, int k) {
    if (e->d) return e->d->at.a[k];
    return k == A_TYPE ? (int64_t)IX.TY[e->row] : IX.col[k][e->row];
}
int64_t ix_row(const char *path) {
    char dir[8192], name[4096]; split_path(path, dir, name);
    int64_t d = dict_find(IX.doff, IX.dblob, IX.H.ndirs, dir), m = dict_find(IX.noff, IX.nblob, IX.H.nnames, name);
    if (d < 0 || m < 0) return -1;
    uint64_t key = ((uint64_t)d << IX.H.wn) | (uint64_t)m, lo = IX.dstart[d], hi = IX.dstart[d + 1];
    while (lo < hi) { uint64_t mid = (lo + hi) / 2; if (IX.K[mid] < key) lo = mid + 1; else hi = mid; }
    return (lo < IX.dstart[d + 1] && IX.K[lo] == key && !ix_dead(lo)) ? (int64_t)lo : -1;
}
int ix_lookup(const char *path, ent_t *e) {
    int64_t i = dx_find(path); if (i >= 0) { e->d = &IX.dx[i]; e->row = -1; return 1; }
    int64_t r = ix_row(path); if (r < 0) return 0; e->d = NULL; e->row = r; return 1;
}
int ix_subtree(const char *path, span_t *out) {
    int k = 0; int64_t d = dict_find(IX.doff, IX.dblob, IX.H.ndirs, path);
    if (d >= 0 && IX.dstart[d + 1] > IX.dstart[d]) out[k++] = (span_t){ IX.dstart[d], IX.dstart[d + 1] };
    char pre[8192]; size_t l = strlen(path); if (l == 1 && path[0] == '/') l = 0;
    memcpy(pre, path, l); pre[l] = '/';
    uint64_t lo = dict_lower(IX.doff, IX.dblob, IX.H.ndirs, pre, l + 1); pre[l] = '0';
    uint64_t hi = dict_lower(IX.doff, IX.dblob, IX.H.ndirs, pre, l + 1);
    if (lo < hi && bcmp_str(IX.dblob + IX.doff[lo], IX.doff[lo + 1] - IX.doff[lo], path, strlen(path)) == 0) lo++;
    if (hi > lo && IX.dstart[hi] > IX.dstart[lo]) out[k++] = (span_t){ IX.dstart[lo], IX.dstart[hi] };
    return k;
}

/* ------------------------------------------------------------------ changes */
static void kill_row(int64_t r) { if (r >= 0) IX.T[r >> 6] |= 1ull << (r & 63); }
void ix_put(const char *path, const attrs_t *at) { kill_row(ix_row(path)); dx_set(path, at); IX.gen++; IX.dirty = 1; }
int ix_delete(const char *path) {
    int64_t r = ix_row(path); kill_row(r); int hit = dx_remove(path); IX.gen++; IX.dirty = 1; return r >= 0 || hit;
}
size_t ix_delete_subtree(const char *path) {
    size_t n = 0; span_t sp[2]; int k = ix_subtree(path, sp);
    for (int j = 0; j < k; j++) for (uint64_t r = sp[j].a; r < sp[j].b; r++) if (!ix_dead(r)) { kill_row((int64_t)r); n++; }
    int any = 0;
    for (size_t i = 0; i < IX.ndx; i++) if (IX.dx[i].live && path_under(IX.dx[i].path, path)) { IX.dx[i].live = 0; n++; any = 1; }
    (void)any;
    n += (size_t)ix_delete(path); IX.gen++; IX.dirty = 1;
    return n;
}
void attrs_from_stat(const struct stat *st, attrs_t *at) {
    mode_t m = st->st_mode; char t = S_ISDIR(m) ? 'd' : S_ISREG(m) ? 'f' : S_ISLNK(m) ? 'l' : S_ISSOCK(m) ? 's' : S_ISFIFO(m) ? 'p' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' : '?';
    at->a[A_TYPE] = t; at->a[A_SIZE] = st->st_size; at->a[A_BLOCKS] = st->st_blocks;
    at->a[A_MTIME] = (int64_t)st->st_mtim.tv_sec * 1000000000 + st->st_mtim.tv_nsec; at->a[A_MODE] = m & 07777;
    at->a[A_UID] = st->st_uid; at->a[A_GID] = st->st_gid; at->a[A_NLINK] = st->st_nlink; at->a[A_INODE] = (int64_t)st->st_ino; at->a[A_DEV] = (int64_t)st->st_dev;
}
static void relink(int64_t inode, const char *skip) {               /* other names of a hard-linked inode changed too */
    char buf[16384];
    for (uint64_t r = 0; r < IX.H.n; r++)
        if (IX.col[A_INODE][r] == inode && !ix_dead(r)) {
            row_path(r, buf); if (strcmp(buf, skip)) { struct stat st; attrs_t at; if (!lstat(buf, &st)) { attrs_from_stat(&st, &at); ix_put(buf, &at); } else ix_delete(buf); }
        }
    for (size_t i = 0; i < IX.ndx; i++)
        if (IX.dx[i].live && IX.dx[i].at.a[A_INODE] == inode && strcmp(IX.dx[i].path, skip)) {
            struct stat st; attrs_t at; char *p = strdup(IX.dx[i].path);
            if (!lstat(p, &st)) { attrs_from_stat(&st, &at); ix_put(p, &at); } else ix_delete(p);
            free(p);
        }
}
int ix_put_path(const char *path) {
    struct stat st; if (lstat(path, &st)) { ix_delete(path); return 0; }
    attrs_t at; attrs_from_stat(&st, &at); ix_put(path, &at);
    if (!S_ISDIR(st.st_mode) && st.st_nlink > 1) relink((int64_t)st.st_ino, path);
    return 1;
}

/* a path left the tree (deleted or renamed away): drop it (and its subtree); other names of a hard link are re-read */
void ix_removed(const char *path) {
    ent_t e; int64_t ino = -1, nl = 0; int dir = 0;
    if (ix_lookup(path, &e)) { ino = ix_attr(&e, A_INODE); nl = ix_attr(&e, A_NLINK); dir = ix_attr(&e, A_TYPE) == 'd'; }
    if (dir) ix_delete_subtree(path); else { ix_delete(path); if (nl > 1) relink(ino, path); }
}
static void scan_dir(const char *dir, dev_t dev, int one_fs, rows_t *out);
/* a path appeared (created or renamed in): store it and, for a directory, everything below it */
void ix_put_tree(const char *path) {
    struct stat st; if (!ix_put_path(path) || lstat(path, &st) || !S_ISDIR(st.st_mode)) return;
    rows_t r = {0}; scan_dir(path, st.st_dev, 0, &r); char p[16384];
    for (size_t i = 0; i < r.n; i++) { snprintf(p, sizeof p, strcmp(r.parent[i], "/") ? "%s/%s" : "%s%s", r.parent[i], r.name[i]); ix_put(p, &r.at[i]); }
    rows_free(&r);
}

/* ------------------------------------------------------------------ sidecar */
static int write_all(int fd, const void *p, size_t n) { const uint8_t *b = p; while (n) { ssize_t k = write(fd, b, n); if (k <= 0) return -1; b += k; n -= (size_t)k; } return 0; }
int ix_save_sidecar(void) {
    char tmp[8300]; snprintf(tmp, sizeof tmp, "%s.tmp%d", side_path(), (int)getpid());
    size_t nd = 0, blob = 0; for (size_t i = 0; i < IX.ndx; i++) if (IX.dx[i].live) { nd++; blob += strlen(IX.dx[i].path) + 1; }
    uint64_t off_x = 64 + IX.H.nw * 8, off_b = off_x + (nd + 1) * 8, off_a = (off_b + blob + 7) / 8 * 8;
    uint8_t head[64] = "TFSSIDE1"; uint64_t h[6] = { IX.H.index_id, IX.H.nw, nd, off_x, off_b, off_a }; memcpy(head + 8, h, sizeof h);
    uint64_t *xo = malloc((nd + 1) * 8); char *bb = malloc(blob + 1); int64_t *aa = malloc((nd ? nd : 1) * NATTR * 8);
    size_t j = 0, pos = 0; xo[0] = 0;
    for (size_t i = 0; i < IX.ndx; i++) if (IX.dx[i].live) {
        size_t l = strlen(IX.dx[i].path) + 1; memcpy(bb + pos, IX.dx[i].path, l); pos += l; xo[j + 1] = pos;
        memcpy(aa + j * NATTR, IX.dx[i].at.a, sizeof IX.dx[i].at.a); j++;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return -1;
    uint8_t zero[8] = {0};
    int bad = write_all(fd, head, 64) || write_all(fd, IX.T, IX.H.nw * 8) || write_all(fd, xo, (nd + 1) * 8) || write_all(fd, bb, blob)
              || write_all(fd, zero, off_a - off_b - blob) || write_all(fd, aa, nd * NATTR * 8) || fsync(fd);
    close(fd); free(xo); free(bb); free(aa);
    if (bad || rename(tmp, side_path())) { unlink(tmp); return -1; }
    IX.dirty = 0; return 0;
}

/* ------------------------------------------------------------------ rows: scanning, building */
void rows_add(rows_t *r, const char *parent, const char *name, const attrs_t *at) {
    if (r->n == r->cap) { r->cap = r->cap ? 2 * r->cap : 65536; r->parent = realloc(r->parent, r->cap * sizeof *r->parent);
                          r->name = realloc(r->name, r->cap * sizeof *r->name); r->at = realloc(r->at, r->cap * sizeof *r->at); }
    r->parent[r->n] = (char *)parent; r->name[r->n] = strdup(name); r->at[r->n] = *at; r->n++;
}
void rows_free(rows_t *r) { for (size_t i = 0; i < r->n; i++) free(r->name[i]); free(r->parent); free(r->name); free(r->at); memset(r, 0, sizeof *r); }

/* interned directory strings: every row of one directory shares its parent pointer */
static char *intern_dir(const char *s) { return strdup(s); }

static void scan_dir(const char *dir, dev_t dev, int one_fs, rows_t *out) {
    /* iterative: a stack of directory paths */
    size_t cap = 1024, n = 0; char **stack = malloc(cap * sizeof *stack); stack[n++] = strdup(dir);
    while (n) {
        char *d = stack[--n]; char *parent = intern_dir(d);
        int fd = open(d, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) { free(d); continue; }
        DIR *dp = fdopendir(fd); if (!dp) { close(fd); free(d); continue; }
        struct dirent *de;
        while ((de = readdir(dp))) {
            if (de->d_name[0] == '.' && (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2]))) continue;
            struct stat st; if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
            attrs_t at; attrs_from_stat(&st, &at); rows_add(out, parent, de->d_name, &at);
            if (S_ISDIR(st.st_mode) && (!one_fs || st.st_dev == dev)) {
                size_t l = strlen(d); char *c = malloc(l + strlen(de->d_name) + 2);
                if (l == 1 && d[0] == '/') sprintf(c, "/%s", de->d_name); else sprintf(c, "%s/%s", d, de->d_name);
                if (n == cap) { cap *= 2; stack = realloc(stack, cap * sizeof *stack); }
                stack[n++] = c;
            }
        }
        closedir(dp); free(d);
    }
    free(stack);
}
int scan_roots(char **roots, int nroots, int one_fs, rows_t *out) {
    for (int i = 0; i < nroots; i++) {
        struct stat st; if (lstat(roots[i], &st)) { perror(roots[i]); continue; }
        char dir[8192], name[4096]; split_path(roots[i], dir, name);
        attrs_t at; attrs_from_stat(&st, &at);
        if (strcmp(roots[i], "/")) rows_add(out, intern_dir(dir), name, &at); else rows_add(out, intern_dir("/"), "/", &at);   /* as find prints it */
        if (S_ISDIR(st.st_mode)) scan_dir(roots[i], st.st_dev, one_fs, out);
    }
    return 0;
}

/* builder: dictionaries in byte order, rows by (dir code, name code), the TFSNIDX1 format 3 sections */
static int cmp_pstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }   /* strcmp: byte order */
static char **sorted_unique(char **v, size_t n, size_t *out_n) {
    char **s = malloc((n ? n : 1) * sizeof *s); memcpy(s, v, n * sizeof *s); qsort(s, n, sizeof *s, cmp_pstr);
    size_t k = 0; for (size_t i = 0; i < n; i++) if (!k || strcmp(s[k - 1], s[i])) s[k++] = s[i];
    *out_n = k; return s;
}
static int64_t bfind(char **v, size_t n, const char *s) {
    size_t lo = 0, hi = n; while (lo < hi) { size_t m = (lo + hi) / 2; if (strcmp(v[m], s) < 0) lo = m + 1; else hi = m; }
    return (lo < n && !strcmp(v[lo], s)) ? (int64_t)lo : -1;
}
typedef struct { uint64_t k; uint64_t i; } kv_t;
static int cmp_kv(const void *a, const void *b) { uint64_t x = ((const kv_t *)a)->k, y = ((const kv_t *)b)->k; return x < y ? -1 : x > y; }
static void pad64(FILE *f) { long p = ftell(f); static const uint8_t z[64]; if (p % 64) fwrite(z, 1, 64 - p % 64, f); }
int build_index(rows_t *rows, char **roots, int nroots, int one_fs, const char *out_path) {
    size_t n = rows->n;
    /* directories: every parent, and the full path of every directory entry */
    char **dall = malloc((2 * n + 1) * sizeof *dall); size_t nd = 0;
    for (size_t i = 0; i < n; i++) {
        dall[nd++] = rows->parent[i];
        if (rows->at[i].a[A_TYPE] == 'd') {
            const char *p = rows->parent[i]; size_t l = strlen(p); char *c = malloc(l + strlen(rows->name[i]) + 2);
            if (l == 1 && p[0] == '/') sprintf(c, "/%s", rows->name[i]); else sprintf(c, "%s/%s", p, rows->name[i]);
            dall[nd++] = c;
        }
    }
    size_t ndirs, nnames; char **dirs = sorted_unique(dall, nd, &ndirs); char **names = sorted_unique(rows->name, n, &nnames);
    uint64_t wn = 1; while ((1ull << wn) < (nnames > 1 ? nnames : 2)) wn++;
    kv_t *kv = malloc((n ? n : 1) * sizeof *kv);
    /* the parent of consecutive rows is usually the same pointer: cache its code */
    const char *lastp = NULL; int64_t lastd = -1;
    for (size_t i = 0; i < n; i++) {
        if (rows->parent[i] != lastp) { lastp = rows->parent[i]; lastd = bfind(dirs, ndirs, lastp); }
        int64_t nc = bfind(names, nnames, rows->name[i]);
        kv[i].k = ((uint64_t)lastd << wn) | (uint64_t)nc; kv[i].i = i;
    }
    qsort(kv, n, sizeof *kv, cmp_kv);
    for (size_t i = 1; i < n; i++) if (kv[i].k == kv[i - 1].k) { fprintf(stderr, "tfs: duplicate entry %s/%s\n", rows->parent[kv[i].i], rows->name[kv[i].i]); }
    char tmp[8300]; snprintf(tmp, sizeof tmp, "%s.tmp%d", out_path, (int)getpid());
    FILE *f = fopen(tmp, "wb"); if (!f) { perror(tmp); return -1; }
    hdr_t H; memset(&H, 0, sizeof H); H.version = 3; H.n = n; H.ndirs = ndirs; H.nnames = nnames; H.wn = wn; H.nw = (n + 63) / 64;
    H.nroots = (uint64_t)nroots; H.ndelta = 0; H.flags = one_fs ? 1 : 0; if (getrandom(&H.index_id, sizeof H.index_id, 0) != sizeof H.index_id) H.index_id = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
    uint8_t head[512] = {0}; fwrite(head, 1, 512, f);
#define SEC(field) do { pad64(f); H.field = (uint64_t)ftell(f); } while (0)
    SEC(off_K); for (size_t i = 0; i < n; i++) fwrite(&kv[i].k, 8, 1, f);
    SEC(off_type); for (size_t i = 0; i < n; i++) { uint8_t t = (uint8_t)rows->at[kv[i].i].a[A_TYPE]; fwrite(&t, 1, 1, f); }
    static const int cols[] = { A_SIZE, A_BLOCKS, A_MTIME, A_MODE, A_UID, A_GID, A_NLINK, A_INODE, A_DEV };
    uint64_t *offs[] = { &H.off_size, &H.off_blocks, &H.off_mtime, &H.off_mode, &H.off_uid, &H.off_gid, &H.off_nlink, &H.off_inode, &H.off_dev };
    for (int c = 0; c < 9; c++) { pad64(f); *offs[c] = (uint64_t)ftell(f); for (size_t i = 0; i < n; i++) fwrite(&rows->at[kv[i].i].a[cols[c]], 8, 1, f); }
    SEC(off_T); { uint64_t z = 0; for (uint64_t i = 0; i < H.nw; i++) fwrite(&z, 8, 1, f); }
    SEC(off_dstart); { size_t r = 0; for (uint64_t d = 0; d <= ndirs; d++) { while (r < n && (kv[r].k >> wn) < d) r++; uint64_t v = r; fwrite(&v, 8, 1, f); } }
#define BLOB(offf, blobf, arr, cnt) do { SEC(offf); uint64_t o = 0; fwrite(&o, 8, 1, f); \
        for (size_t i = 0; i < (cnt); i++) { o += strlen(arr[i]) + 1; fwrite(&o, 8, 1, f); } \
        SEC(blobf); for (size_t i = 0; i < (cnt); i++) fwrite(arr[i], 1, strlen(arr[i]) + 1, f); } while (0)
    BLOB(off_doff, off_dblob, dirs, ndirs);
    BLOB(off_noff, off_nblob, names, nnames);
    BLOB(off_roff, off_rblob, roots, (size_t)nroots);
    SEC(off_xoff); { uint64_t o = 0; fwrite(&o, 8, 1, f); } SEC(off_xblob); SEC(off_xattr);
    pad64(f); H.file_size = (uint64_t)ftell(f);
    fseek(f, 0, SEEK_SET); fwrite("TFSNIDX1", 1, 8, f); fwrite(&H, sizeof H, 1, f);
    if (fflush(f) || fsync(fileno(f))) { fclose(f); return -1; }
    fclose(f);
    if (rename(tmp, out_path)) { perror(out_path); return -1; }
    char side[8300]; snprintf(side, sizeof side, "%s.side", out_path); unlink(side);   /* a new index: its overlay is empty */
    free(kv); free(dirs); free(names); free(dall);
    return 0;
}
int ix_rows(rows_t *out) {                                   /* every live entry: base rows + delta */
    char buf[16384], dir[8192], name[4096]; char *lastd = NULL; uint64_t lastc = UINT64_MAX;
    for (uint64_t r = 0; r < IX.H.n; r++) {
        if (ix_dead(r)) continue;
        uint64_t d = ix_dc(r); if (d != lastc) { lastc = d; lastd = strdup(dstr(d)); }
        attrs_t at; ent_t e = { (int64_t)r, NULL }; for (int k = 0; k < NATTR; k++) at.a[k] = ix_attr(&e, k);
        rows_add(out, lastd, nstr(ix_nc(r)), &at);
    }
    for (size_t i = 0; i < IX.ndx; i++) if (IX.dx[i].live) { split_path(IX.dx[i].path, dir, name); rows_add(out, strdup(dir), name, &IX.dx[i].at); }
    (void)buf; return 0;
}
int ix_refresh_overlay(size_t *created, size_t *modified, size_t *deleted) {
    rows_t s = {0}; char **roots = malloc(IX.H.nroots * sizeof *roots);
    for (uint64_t i = 0; i < IX.H.nroots; i++) roots[i] = (char *)root_str(i);
    scan_roots(roots, (int)IX.H.nroots, (int)(IX.H.flags & 1), &s);
    size_t ndx0 = IX.ndx; uint8_t *seen = calloc(IX.H.n + 1, 1); uint8_t *dseen = calloc(ndx0 + 1, 1); size_t c = 0, m = 0, d = 0; char path[16384];
    for (size_t i = 0; i < s.n; i++) {
        const char *p = s.parent[i]; size_t l = strlen(p);
        if (l == 1 && p[0] == '/') snprintf(path, sizeof path, "/%s", s.name[i]); else snprintf(path, sizeof path, "%s/%s", p, s.name[i]);
        ent_t e;
        if (!ix_lookup(path, &e)) { ix_put(path, &s.at[i]); c++; continue; }
        if (e.d) { if ((size_t)(e.d - IX.dx) < ndx0) dseen[e.d - IX.dx] = 1; } else seen[e.row] = 1;
        int same = 1; for (int k = 0; k < NATTR; k++) if (ix_attr(&e, k) != s.at[i].a[k]) same = 0;
        if (!same) { ix_put(path, &s.at[i]); m++; }
    }
    for (uint64_t r = 0; r < IX.H.n; r++) if (!ix_dead(r) && !seen[r]) { kill_row((int64_t)r); d++; }
    for (size_t i = 0; i < ndx0; i++) if (IX.dx[i].live && !dseen[i]) { dx_remove(IX.dx[i].path); d++; }
    IX.gen++; IX.dirty = 1; free(seen); free(dseen); free(roots); rows_free(&s);
    if (created) *created = c;
    if (modified) *modified = m;
    if (deleted) *deleted = d;
    return 0;
}

/* ------------------------------------------------------------------ inotify watcher */
static char **wpath; static size_t wcap; static int watched;
#define WMASK (IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF)
static void wadd(int fd, const char *d) {
    int wd = inotify_add_watch(fd, d, WMASK | IN_ONLYDIR | IN_DONT_FOLLOW); if (wd < 0) return;
    if ((size_t)wd >= wcap) { size_t nc = (size_t)wd * 2 + 1024; wpath = realloc(wpath, nc * sizeof *wpath); memset(wpath + wcap, 0, (nc - wcap) * sizeof *wpath); wcap = nc; }
    if (!wpath[wd]) watched++;
    free(wpath[wd]); wpath[wd] = strdup(d);
}
size_t watch_count(void) { return (size_t)watched; }
int watch_open(void) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC); if (fd < 0) { perror("inotify_init1"); return -1; }
    for (uint64_t i = 0; i < IX.H.nroots; i++) wadd(fd, root_str(i));
    for (uint64_t d = 0; d < IX.H.ndirs; d++)
        for (uint64_t i = 0; i < IX.H.nroots; i++) if (path_under(dstr(d), root_str(i))) { wadd(fd, dstr(d)); break; }
    for (size_t i = 0; i < IX.ndx; i++) if (IX.dx[i].live && IX.dx[i].at.a[A_TYPE] == 'd') wadd(fd, IX.dx[i].path);
    return fd;
}
/* a directory appeared: for it and every directory below it, first the watch, then its own stat, then its listing,
 * so anything that changes after a directory was read raises an event (scanning first and watching after loses
 * changes made in between) */
static void walk_in(int fd, const char *top) {
    size_t cap = 64, n = 0; char **stack = malloc(cap * sizeof *stack); stack[n++] = strdup(top); char path[16384];
    while (n) {
        char *d = stack[--n];
        wadd(fd, d);
        if (!ix_put_path(d)) { free(d); continue; }
        int dfd = open(d, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); DIR *dp = dfd >= 0 ? fdopendir(dfd) : NULL;
        if (!dp) { if (dfd >= 0) close(dfd); free(d); continue; }
        struct dirent *de;
        while ((de = readdir(dp))) {
            if (de->d_name[0] == '.' && (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2]))) continue;
            struct stat st; if (fstatat(dfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
            snprintf(path, sizeof path, strcmp(d, "/") ? "%s/%s" : "%s%s", d, de->d_name);
            attrs_t at; attrs_from_stat(&st, &at); ix_put(path, &at);
            if (S_ISDIR(st.st_mode)) { if (n == cap) { cap *= 2; stack = realloc(stack, cap * sizeof *stack); } stack[n++] = strdup(path); }
        }
        closedir(dp); free(d);
    }
    free(stack);
}
int watch_poll(int fd) {
    static char buf[1 << 20] __attribute__((aligned(8))); int n = 0;
    for (;;) {
        ssize_t k = read(fd, buf, sizeof buf); if (k <= 0) break;
        for (char *p = buf; p < buf + k; ) {
            struct inotify_event *ev = (struct inotify_event *)p; p += sizeof *ev + ev->len; n++;
            if (ev->mask & IN_Q_OVERFLOW) { ix_refresh_overlay(NULL, NULL, NULL); continue; }
            if (ev->wd < 0 || (size_t)ev->wd >= wcap || !wpath[ev->wd] || (ev->mask & IN_IGNORED)) continue;
            const char *d = wpath[ev->wd];
            if (!ev->len) { if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) ix_put_path(d); continue; }
            char path[16384]; snprintf(path, sizeof path, strcmp(d, "/") ? "%s/%s" : "%s%s", d, ev->name);
            if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
                if (ev->mask & IN_ISDIR) ix_delete_subtree(path);
                else { ent_t e; int64_t ino = -1, nl = 0; if (ix_lookup(path, &e)) { ino = ix_attr(&e, A_INODE); nl = ix_attr(&e, A_NLINK); }
                       ix_delete(path); if (nl > 1) relink(ino, path); }
            } else if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) && (ev->mask & IN_ISDIR)) walk_in(fd, path);
            else ix_put_path(path);
            ix_put_path(d);
        }
    }
    return n;
}

/* ------------------------------------------------------------------ btrfs receive --dump */
static size_t unescape_tok(const char **pp, char *out) {        /* one whitespace-separated token, btrfs-progs escapes undone */
    const char *p = *pp; size_t j = 0;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
        if (*p == '\\' && p[1]) {
            if (p[1] >= '0' && p[1] <= '7' && p[2] >= '0' && p[2] <= '7' && p[3] >= '0' && p[3] <= '7') { out[j++] = (char)((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0')); p += 4; continue; }
            char c = p[1]; out[j++] = c == 'a' ? '\a' : c == 'b' ? '\b' : c == 'e' ? 27 : c == 'f' ? '\f' : c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c == 'v' ? '\v' : c;
            p += 2; continue;
        }
        out[j++] = *p++;
    }
    out[j] = 0; *pp = p; return j;
}
typedef struct { char **v; size_t n, cap; } pset_t;
static void ps_add(pset_t *s, const char *p) { if (s->n == s->cap) { s->cap = s->cap ? 2 * s->cap : 1024; s->v = realloc(s->v, s->cap * sizeof *s->v); } s->v[s->n++] = strdup(p); }
static int cmp_pp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
size_t btrfs_apply_dump(const char *text, size_t len, const char *subvol, size_t *nops) {
    char prefix[8192] = "", op[64], a[16384], kv[16384], live[16384], dest[16384]; pset_t reread = {0}, rewalk = {0}; size_t ops = 0, deleted = 0;
    const char *p = text, *end = text + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', end - p); if (!eol) eol = end;
        const char *q = p; if (unescape_tok(&q, op) == 0) { p = eol + 1; continue; }
        unescape_tok(&q, a); dest[0] = 0;
        while (q < eol) { if (!unescape_tok(&q, kv)) break; if (!strncmp(kv, "dest=", 5)) strcpy(dest, kv + 5); }
        if (!strcmp(op, "snapshot") || !strcmp(op, "subvol")) { strcpy(prefix, a); p = eol + 1; continue; }
        ops++;
        const char *rel = a; size_t pl = strlen(prefix); if (pl && !strncmp(a, prefix, pl)) rel = a + pl; while (*rel == '/') rel++;
        snprintf(live, sizeof live, *rel ? "%s/%s" : "%s", subvol, rel);
        char parent[8192], nm[4096]; split_path(live, parent, nm);
        if (!strcmp(op, "unlink")) { deleted += (size_t)ix_delete(live); ps_add(&reread, parent); }
        else if (!strcmp(op, "rmdir")) { deleted += ix_delete_subtree(live); ps_add(&reread, parent); }
        else if (!strcmp(op, "rename")) {
            const char *dr = dest; if (pl && !strncmp(dr, prefix, pl)) dr += pl; while (*dr == '/') dr++;
            char to[16384]; snprintf(to, sizeof to, "%s/%s", subvol, dr); ent_t e; int was_dir = ix_lookup(live, &e) && ix_attr(&e, A_TYPE) == 'd';
            ix_delete_subtree(live); ps_add(&reread, parent); char tp[8192], tn[4096]; split_path(to, tp, tn); ps_add(&reread, tp);
            struct stat st; if (was_dir || (!lstat(to, &st) && S_ISDIR(st.st_mode))) ps_add(&rewalk, to); else ps_add(&reread, to);
        } else if (!strcmp(op, "mkdir")) { ps_add(&rewalk, live); ps_add(&reread, parent); }
        else if (!strcmp(op, "link")) { ps_add(&reread, live); if (dest[0]) { char t[16384]; snprintf(t, sizeof t, "%s/%s", subvol, dest); ps_add(&reread, t); } }
        else { ps_add(&reread, live); if (!strcmp(op, "mkfile") || !strcmp(op, "symlink") || !strcmp(op, "mknod") || !strcmp(op, "mkfifo") || !strcmp(op, "mksock")) ps_add(&reread, parent); }
        p = eol + 1;
    }
    qsort(rewalk.v, rewalk.n, sizeof *rewalk.v, cmp_pp);
    for (size_t i = 0; i < rewalk.n; i++) {
        if (i && !strcmp(rewalk.v[i], rewalk.v[i - 1])) continue;
        struct stat st; if (lstat(rewalk.v[i], &st) || !S_ISDIR(st.st_mode)) { ps_add(&reread, rewalk.v[i]); continue; }
        ix_put_path(rewalk.v[i]); rows_t r = {0}; scan_dir(rewalk.v[i], st.st_dev, 0, &r); char path[16384];
        for (size_t k = 0; k < r.n; k++) { const char *pp2 = r.parent[k]; snprintf(path, sizeof path, strcmp(pp2, "/") ? "%s/%s" : "%s%s", pp2, r.name[k]); ix_put(path, &r.at[k]); }
        rows_free(&r);
    }
    qsort(reread.v, reread.n, sizeof *reread.v, cmp_pp);
    for (size_t i = 0; i < reread.n; i++) if ((!i || strcmp(reread.v[i], reread.v[i - 1])) && path_under(reread.v[i], subvol)) ix_put_path(reread.v[i]);
    if (nops) *nops = ops;
    return deleted;
}
