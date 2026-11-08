/* tfsd: the native torblesFS mount. It speaks the FUSE kernel protocol (7.31) over /dev/fuse (mounted through
 * fusermount3, no libfuse) and answers from a native index (TFSNIDX1 + its INDEX.side overlay):
 *   LOOKUP / GETATTR / READDIRPLUS are solved from the index; node ids are base row + 2 (dynamic ids for overlay paths)
 *   --links   sidecar mode: non-directories are symlinks to the real paths, every write is refused (EROFS)
 *   default   passthrough: file data and every change go to the backing tree, then into the index
 *   --watch   inotify in the same loop: changes made outside the mount reach the index (and the sidecar file)
 *   @ segments: /dir/@type=f/@name=*.py/@count  (views narrow, a final @count / @bytes / @newest / @list is a file)
 *   .q        saved queries (INDEX.queries): mkdir .q/NAME, write .q/NAME/.expression, read .count / .bytes / .newest
 * usage: tfsd -i INDEX [--links] [--watch] [--save-every S] MOUNTPOINT */
#include "tfscore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <fnmatch.h>
#include <ctype.h>
#include <locale.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <sys/wait.h>

/* ------------------------------------------------------------------ protocol */
struct in_hdr { uint32_t len, opcode; uint64_t unique, nodeid; uint32_t uid, gid, pid; uint16_t extlen, pad; };
struct out_hdr { uint32_t len; int32_t error; uint64_t unique; };
struct fattr { uint64_t ino, size, blocks, atime, mtime, ctime; uint32_t atimensec, mtimensec, ctimensec, mode, nlink, uid, gid, rdev, blksize, flags; };
struct entry_out { uint64_t nodeid, generation, entry_valid, attr_valid; uint32_t entry_valid_nsec, attr_valid_nsec; struct fattr attr; };
struct attr_out { uint64_t attr_valid; uint32_t attr_valid_nsec, dummy; struct fattr attr; };
struct dirent_h { uint64_t ino, off; uint32_t namelen, type; };
struct open_out { uint64_t fh; uint32_t open_flags, padding; };
struct read_in { uint64_t fh, offset; uint32_t size, read_flags; uint64_t lock_owner; uint32_t flags, padding; };
struct setattr_in { uint32_t valid, padding; uint64_t fh, size, lock_owner, atime, mtime, ctime; uint32_t atimensec, mtimensec, ctimensec, mode, unused4, uid, gid, unused5; };
enum { LOOKUP = 1, FORGET = 2, GETATTR = 3, SETATTR = 4, READLINK = 5, SYMLINK = 6, MKNOD = 8, MKDIR = 9, UNLINK = 10, RMDIR = 11,
       RENAME = 12, LINK = 13, OPEN = 14, READ = 15, WRITE = 16, STATFS = 17, RELEASE = 18, FSYNC = 20, SETXATTR = 21, GETXATTR = 22,
       LISTXATTR = 23, REMOVEXATTR = 24, FLUSH = 25, INIT = 26, OPENDIR = 27, READDIR = 28, RELEASEDIR = 29, FSYNCDIR = 30,
       ACCESS = 34, CREATE = 35, INTERRUPT = 36, DESTROY = 38, BATCH_FORGET = 42, READDIRPLUS = 44, RENAME2 = 45 };
#define FATTR_MODE 1
#define FATTR_UID 2
#define FATTR_GID 4
#define FATTR_SIZE 8
#define FATTR_ATIME 16
#define FATTR_MTIME 32
#define FATTR_FH 64
#define FATTR_ATIME_NOW 128
#define FATTR_MTIME_NOW 256
#define FOPEN_DIRECT_IO 1
#define FOPEN_KEEP_CACHE 2
#define ROW_NODE 2ull
#define DYN_NODE (1ull << 62)
#define QROOT (1ull << 61)
#define VNODE ((1ull << 61) + 1)
#define DELTA_COOKIE (1ull << 62)
#define Q_COOKIE (DELTA_COOKIE - 1)

static int fd = -1, links, watching, ttl = 60; static const char *root, *mp; static char mp_abs[8192]; static int64_t dev0;
static uint8_t outbuf[(1 << 21) + 4096];

static void reply(uint64_t unique, int err, const void *a, size_t al) {
    struct out_hdr h = { (uint32_t)(sizeof h + (err ? 0 : al)), -err, unique };
    struct iovec v[2] = { { &h, sizeof h }, { (void *)a, err ? 0 : al } };
    if (writev(fd, v, 2) < 0 && errno != ENOENT && errno != ENODEV) perror("tfsd: writev");
}

/* ------------------------------------------------------------------ string map (path -> node, key -> vnode) */
typedef struct { char **k; uint64_t *v; size_t cap, n; } smap_t;
static uint64_t fnv(const char *s) { uint64_t h = 1469598103934665603ull; for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull; return h; }
static int64_t sm_get(smap_t *m, const char *k) {
    if (!m->cap) return -1;
    for (size_t s = fnv(k) & (m->cap - 1); m->k[s]; s = (s + 1) & (m->cap - 1)) if (m->k[s] != (char *)1 && !strcmp(m->k[s], k)) return (int64_t)m->v[s];
    return -1;
}
static void sm_put(smap_t *m, const char *k, uint64_t v);
static void sm_grow(smap_t *m) {
    smap_t o = *m; m->cap = o.cap ? o.cap * 2 : 1024; m->n = 0; m->k = calloc(m->cap, sizeof *m->k); m->v = calloc(m->cap, sizeof *m->v);
    for (size_t i = 0; i < o.cap; i++) if (o.k[i] && o.k[i] != (char *)1) { sm_put(m, o.k[i], o.v[i]); free(o.k[i]); }
    free(o.k); free(o.v);
}
static void sm_put(smap_t *m, const char *k, uint64_t v) {
    if ((m->n + 1) * 2 > m->cap) sm_grow(m);
    size_t s = fnv(k) & (m->cap - 1);
    for (; m->k[s]; s = (s + 1) & (m->cap - 1)) if (m->k[s] != (char *)1 && !strcmp(m->k[s], k)) { m->v[s] = v; return; }
    m->k[s] = strdup(k); m->v[s] = v; m->n++;
}
static void sm_del(smap_t *m, const char *k) {
    if (!m->cap) return;
    for (size_t s = fnv(k) & (m->cap - 1); m->k[s]; s = (s + 1) & (m->cap - 1))
        if (m->k[s] != (char *)1 && !strcmp(m->k[s], k)) { free(m->k[s]); m->k[s] = (char *)1; return; }
}

/* ------------------------------------------------------------------ nodes */
static smap_t path_node; static char **dyn_path; static size_t ndyn, capdyn;
typedef struct { char *old, *new_; } ren_t; static ren_t *renames; static size_t nren;
static void join(const char *d, const char *n, char *out) {
    if (!strcmp(d, "/")) snprintf(out, 16384, "/%s", n); else snprintf(out, 16384, "%s/%s", d, n);
}
static const char *apply_renames(const char *p, char *buf) {
    if (!nren) return p;
    if (p != buf) { strcpy(buf, p); p = buf; }
    char t[16384];
    for (size_t i = 0; i < nren; i++) if (path_under(buf, renames[i].old)) { snprintf(t, sizeof t, "%s%s", renames[i].new_, buf + strlen(renames[i].old)); strcpy(buf, t); }
    return buf;
}
/* the path of a node (into buf), or NULL (stale) */
static const char *node_path(uint64_t node, char *buf) {
    if (node == 1) return root;
    if (node >= ROW_NODE && node < ROW_NODE + IX.H.n) {         /* a live row is at its path; a moved one follows the renames */
        row_path(node - ROW_NODE, buf); return ix_dead(node - ROW_NODE) ? apply_renames(buf, buf) : buf; }
    if (node >= DYN_NODE && node - DYN_NODE < ndyn) return dyn_path[node - DYN_NODE];
    return NULL;
}
static uint64_t node_of(const char *p) {
    int64_t n = sm_get(&path_node, p); if (n >= 0) return (uint64_t)n;
    if (!strcmp(p, root)) return 1;
    int64_t r = ix_row(p); if (r >= 0) return (uint64_t)r + ROW_NODE;
    if (ndyn == capdyn) { capdyn = capdyn ? 2 * capdyn : 1024; dyn_path = realloc(dyn_path, capdyn * sizeof *dyn_path); }
    dyn_path[ndyn] = strdup(p); sm_put(&path_node, p, DYN_NODE + ndyn); return DYN_NODE + ndyn++;
}

/* ------------------------------------------------------------------ attributes */
static int ent_of(const char *p, ent_t *e) {
    if (ix_lookup(p, e)) return 1;
    if (!strcmp(p, root) && ix_put_path(p)) return ix_lookup(p, e);
    return 0;
}
static void fill_attr(const char *p, const ent_t *e, struct fattr *a) {
    memset(a, 0, sizeof *a);
    int64_t mt = ix_attr(e, A_MTIME), t = ix_attr(e, A_TYPE), dev = ix_attr(e, A_DEV);
    static const uint32_t ifmt[128] = { ['f'] = S_IFREG, ['d'] = S_IFDIR, ['l'] = S_IFLNK, ['s'] = S_IFSOCK, ['p'] = S_IFIFO, ['c'] = S_IFCHR, ['b'] = S_IFBLK };
    int64_t sec = mt / 1000000000, ns = mt % 1000000000; if (ns < 0) { ns += 1000000000; sec--; }
    a->ino = (uint64_t)ix_attr(e, A_INODE); a->size = (uint64_t)ix_attr(e, A_SIZE);
    a->blocks = (dev == dev0 || !dev) ? (uint64_t)ix_attr(e, A_BLOCKS) : 0;        /* a mount point shows as an empty directory */
    a->atime = a->mtime = a->ctime = (uint64_t)sec; a->atimensec = a->mtimensec = a->ctimensec = (uint32_t)ns;
    a->mode = ifmt[t & 127] | (uint32_t)ix_attr(e, A_MODE); a->nlink = (uint32_t)ix_attr(e, A_NLINK);
    a->uid = (uint32_t)ix_attr(e, A_UID); a->gid = (uint32_t)ix_attr(e, A_GID); a->blksize = 4096;
    if (links && t != 'd') { a->mode = S_IFLNK | 0777; a->size = strlen(p); a->blocks = 0; }  /* sidecar: a symlink to the real path */
}
static uint32_t dtype(int64_t t, int virt_dir) {
    if (virt_dir) return 4;
    if (links && t != 'd') return 10;
    switch (t) { case 'f': return 8; case 'd': return 4; case 'l': return 10; case 's': return 12; case 'p': return 1; case 'c': return 2; case 'b': return 6; }
    return 0;
}
static void entry_for(const char *p, const ent_t *e, struct entry_out *o) {
    memset(o, 0, sizeof *o); o->nodeid = node_of(p); o->entry_valid = o->attr_valid = (uint64_t)ttl; fill_attr(p, e, &o->attr);
}

/* ------------------------------------------------------------------ expressions (stored queries, URI views) */
typedef struct {
    char *path; int has_type; char types[16]; char **globs; int nglobs;
    int has_min, has_max, has_newer, has_older; int64_t min_size, max_size, newer, older;
} expr_t;
static expr_t *ex_new(const char *path) { expr_t *e = calloc(1, sizeof *e); e->path = strdup(path); return e; }
static expr_t *ex_copy(const expr_t *s) {
    expr_t *e = malloc(sizeof *e); *e = *s; e->path = strdup(s->path); e->globs = malloc((s->nglobs + 1) * sizeof *e->globs);
    for (int i = 0; i < s->nglobs; i++) e->globs[i] = strdup(s->globs[i]);
    return e;
}
static void ex_free(expr_t *e) { if (!e) return; for (int i = 0; i < e->nglobs; i++) free(e->globs[i]); free(e->globs); free(e->path); free(e); }
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void ex_add_glob(expr_t *e, const char *g) {
    for (int i = 0; i < e->nglobs; i++) if (!strcmp(e->globs[i], g)) return;
    e->globs = realloc(e->globs, (e->nglobs + 1) * sizeof *e->globs); e->globs[e->nglobs++] = strdup(g);
    qsort(e->globs, e->nglobs, sizeof *e->globs, cmp_str);
}
static void sort_types(char *t) {
    char seen[256] = {0}, out[16]; int k = 0;
    for (const char *p = t; *p; p++) seen[(uint8_t)*p] = 1;
    for (int c = 1; c < 256 && k < 15; c++) if (seen[c]) out[k++] = (char)c;
    out[k] = 0; strcpy(t, out);
}
/* AND one conjunct in (same key twice: both must hold) */
static void conjoin(expr_t *e, const char *field, const char *sval, int64_t ival) {
    if (!strcmp(field, "name")) { ex_add_glob(e, sval); return; }
    if (!strcmp(field, "type")) {
        if (!e->has_type) { e->has_type = 1; snprintf(e->types, sizeof e->types, "%s", sval); sort_types(e->types); return; }
        char t[16]; int k = 0; for (const char *p = e->types; *p; p++) if (strchr(sval, *p) && k < 15) t[k++] = *p;
        t[k] = 0; strcpy(e->types, t); return;                             /* empty: nothing matches */
    }
    if (!strcmp(field, "min_size")) { e->min_size = e->has_min && e->min_size > ival ? e->min_size : ival; e->has_min = 1; }
    else if (!strcmp(field, "max_size")) { e->max_size = e->has_max && e->max_size < ival ? e->max_size : ival; e->has_max = 1; }
    else if (!strcmp(field, "newer")) { e->newer = e->has_newer && e->newer > ival ? e->newer : ival; e->has_newer = 1; }
    else if (!strcmp(field, "older")) { e->older = e->has_older && e->older < ival ? e->older : ival; e->has_older = 1; }
}
/* `NNNns` exact, epoch seconds, or an ISO date / date-time (local time): nanoseconds; returns -1 on failure */
static int parse_ts(const char *v, int64_t *out) {
    size_t l = strlen(v); char *end;
    if (l > 2 && !strcmp(v + l - 2, "ns")) { int ok = 1; for (size_t i = 0; i < l - 2; i++) if (!isdigit((unsigned char)v[i])) ok = 0;
                                              if (ok) { *out = strtoll(v, NULL, 10); return 0; } }
    double d = strtod(v, &end); if (*v && !*end) { *out = (int64_t)(d * 1e9); return 0; }
    struct tm tm; memset(&tm, 0, sizeof tm); int Y, M, D, h = 0, m = 0; double s = 0; char sep;
    int k = sscanf(v, "%d-%d-%d%c%d:%d:%lf", &Y, &M, &D, &sep, &h, &m, &s);
    if (k != 3 && k < 6) return -1;
    if (k >= 4 && sep != 'T' && sep != ' ') return -1;
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D; tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = (int)s; tm.tm_isdst = -1;
    time_t t = mktime(&tm); *out = (int64_t)t * 1000000000 + (int64_t)((s - (int)s) * 1e9 + 0.5); return 0;
}
static int is_int(const char *v) { if (*v == '-' || *v == '+') v++; if (!*v) return 0; for (; *v; v++) if (!isdigit((unsigned char)*v)) return 0; return 1; }
/* one path segment: 0 literal name (*lit set), 1 result (*res set), 2 conjunct applied to e */
static int parse_segment(const char *seg, const char **lit, const char **res, char *field, char *sval, int64_t *ival) {
    static const char *R[] = { "count", "bytes", "newest", "list" };
    if (!strncmp(seg, "@@", 2)) { *lit = seg + 1; return 0; }
    if (seg[0] != '@') { *lit = seg; return 0; }
    const char *b = seg + 1;
    for (int i = 0; i < 4; i++) if (!strcmp(b, R[i])) { *res = R[i]; return 1; }
    if (!strncmp(b, "size>", 5) || !strncmp(b, "size<", 5)) {
        const char *v = b + 5; int ok = *v != 0; for (const char *p = v; *p; p++) if (!isdigit((unsigned char)*p)) ok = 0;
        if (ok) { strcpy(field, b[4] == '>' ? "min_size" : "max_size"); *ival = strtoll(v, NULL, 10); return 2; }
        *lit = seg; return 0;
    }
    const char *eq = strchr(b, '='); *lit = seg;
    if (!eq) return 0;
    size_t kl = (size_t)(eq - b); char key[32]; if (kl >= sizeof key) return 0; memcpy(key, b, kl); key[kl] = 0; const char *v = eq + 1;
    if (!strcmp(key, "min_size") || !strcmp(key, "max_size")) { if (!is_int(v)) return 0; strcpy(field, key); *ival = strtoll(v, NULL, 10); return 2; }
    if (!strcmp(key, "newer") || !strcmp(key, "older")) { if (parse_ts(v, ival)) return 0; strcpy(field, key); return 2; }
    if (!strcmp(key, "type")) { if (!*v) return 0; strcpy(field, key); snprintf(sval, 4096, "%s", v); return 2; }
    if (!strcmp(key, "name")) { strcpy(field, key); snprintf(sval, 4096, "%s", v); return 2; }
    return 0;
}
/* shlex.quote / shlex.split (POSIX) for the .expression text */
static void shq(const char *s, char *out) {
    int safe = *s != 0; for (const char *p = s; *p; p++) if (!(isalnum((unsigned char)*p) || strchr("@%+=:,./-_", *p))) safe = 0;
    if (safe) { strcpy(out, s); return; }
    size_t j = 0; out[j++] = '\'';
    for (const char *p = s; *p; p++) { if (*p == '\'') { memcpy(out + j, "'\"'\"'", 5); j += 5; } else out[j++] = *p; }
    out[j++] = '\''; out[j] = 0;
}
static int shsplit(const char *s, char **tok, int max) {
    int n = 0; char cur[8192]; size_t j = 0; int in = 0;
    for (const char *p = s;; p++) {
        if (!*p || isspace((unsigned char)*p)) { if (in) { cur[j] = 0; if (n < max) tok[n++] = strdup(cur); j = 0; in = 0; } if (!*p) break; continue; }
        in = 1;
        if (*p == '\'') { for (p++; *p && *p != '\''; p++) cur[j++] = *p; if (!*p) return -1; }
        else if (*p == '"') { for (p++; *p && *p != '"'; p++) { if (*p == '\\' && strchr("\\\"$`\n", p[1]) && p[1]) p++; cur[j++] = *p; } if (!*p) return -1; }
        else if (*p == '\\' && p[1]) { p++; cur[j++] = *p; }
        else cur[j++] = *p;
        if (j > sizeof cur - 8) return -1;
    }
    return n;
}
static const char *rel(const char *p) {                     /* path relative to the mount root ("" for the root) */
    size_t l = strlen(root); if (!strcmp(p, root)) return "";
    if (l == 1) return p + 1;
    return (!strncmp(p, root, l) && p[l] == '/') ? p + l + 1 : p;
}
/* the expression as text; with_root: the path as given (absolute), else relative to the mount root */
static void ex_text(const expr_t *e, int relative, char *out, size_t cap) {
    char q[16384]; size_t j = 0; out[0] = 0;
#define ADD(...) do { j += (size_t)snprintf(out + j, j < cap ? cap - j : 0, __VA_ARGS__); } while (0)
    const char *p = relative ? rel(e->path) : e->path;
    if (*p && strcmp(p, "/")) { shq(p, q); ADD("path=%s ", q); }
    if (e->has_type) { shq(e->types, q); ADD("type=%s ", *e->types ? q : "''"); }
    for (int i = 0; i < e->nglobs; i++) { shq(e->globs[i], q); ADD("name=%s ", q); }
    if (e->has_min) ADD("min_size=%lld ", (long long)e->min_size);
    if (e->has_max) ADD("max_size=%lld ", (long long)e->max_size);
    if (e->has_newer) ADD("newer=%lldns ", (long long)e->newer);
    if (e->has_older) ADD("older=%lldns ", (long long)e->older);
    while (j && j <= cap && out[j - 1] == ' ') out[--j] = 0;
#undef ADD
}
/* `path=sub type=f name='*.py' min_size=N newer=...` (relative paths resolve under the root); NULL if invalid */
static expr_t *ex_parse(const char *text) {
    char *tok[256]; int n = shsplit(text, tok, 256); if (n < 0) return NULL;
    expr_t *e = ex_new(root); int bad = 0;
    for (int i = 0; i < n; i++) {
        char *eq = strchr(tok[i], '='); if (!eq) { bad = 1; break; }
        *eq = 0; const char *k = tok[i], *v = eq + 1; int64_t iv;
        if (!strcmp(k, "path")) {
            char p[16384], q[16384];
            if (v[0] == '/') snprintf(p, sizeof p, "%s", v); else if (!*v) snprintf(p, sizeof p, "%s", root); else join(root, v, p);
            norm_path(p, q); free(e->path); e->path = strdup(q);
        }
        else if (!strcmp(k, "type")) { e->has_type = 1; snprintf(e->types, sizeof e->types, "%s", v); sort_types(e->types); }
        else if (!strcmp(k, "name")) ex_add_glob(e, v);
        else if (!strcmp(k, "min_size") || !strcmp(k, "size>")) { if (!is_int(v)) { bad = 1; break; } e->has_min = 1; e->min_size = strtoll(v, NULL, 10); }
        else if (!strcmp(k, "max_size") || !strcmp(k, "size<")) { if (!is_int(v)) { bad = 1; break; } e->has_max = 1; e->max_size = strtoll(v, NULL, 10); }
        else if (!strcmp(k, "newer") || !strcmp(k, "newer_ns")) { if (!strcmp(k, "newer_ns") && is_int(v)) iv = strtoll(v, NULL, 10); else if (parse_ts(v, &iv)) { bad = 1; break; } e->has_newer = 1; e->newer = iv; }
        else if (!strcmp(k, "older") || !strcmp(k, "older_ns")) { if (!strcmp(k, "older_ns") && is_int(v)) iv = strtoll(v, NULL, 10); else if (parse_ts(v, &iv)) { bad = 1; break; } e->has_older = 1; e->older = iv; }
        else { bad = 1; break; }
    }
    for (int i = 0; i < n; i++) free(tok[i]);
    if (bad) { ex_free(e); return NULL; }
    return e;
}

/* name masks: one fnmatch per distinct name, per glob, cached for the whole mount (the dictionary never changes) */
typedef struct { char *glob; uint8_t *m; } gmask_t; static gmask_t *gm; static size_t ngm;
static const uint8_t *glob_mask(const char *g) {
    for (size_t i = 0; i < ngm; i++) if (!strcmp(gm[i].glob, g)) return gm[i].m;
    uint8_t *m = malloc(IX.H.nnames + 1);
    for (uint64_t c = 0; c < IX.H.nnames; c++) m[c] = fnmatch(g, nstr(c), 0) == 0;
    gm = realloc(gm, (ngm + 1) * sizeof *gm); gm[ngm++] = (gmask_t){ strdup(g), m }; return m;
}
static int ex_test(const expr_t *x, const ent_t *e, const char *path) {
    int64_t t = ix_attr(e, A_TYPE);
    if (x->has_type && !(t && strchr(x->types, (int)t))) return 0;
    if (x->nglobs) {
        if (!e->d) { uint64_t nc = ix_nc((uint64_t)e->row); for (int i = 0; i < x->nglobs; i++) if (!glob_mask(x->globs[i])[nc]) return 0; }
        else { const char *b = strrchr(path, '/'); b = b ? b + 1 : path; for (int i = 0; i < x->nglobs; i++) if (fnmatch(x->globs[i], b, 0)) return 0; }
    }
    int64_t s = ix_attr(e, A_SIZE), m = ix_attr(e, A_MTIME);
    if (x->has_min && !(s > x->min_size)) return 0;
    if (x->has_max && !(s < x->max_size)) return 0;
    if (x->has_newer && !(m > x->newer)) return 0;
    if (x->has_older && !(m < x->older)) return 0;
    return 1;
}
/* every match of x: the path itself, base rows below it, overlay entries below it */
typedef void (*visit_fn)(const ent_t *e, const char *path, void *ctx);
static void ex_select(const expr_t *x, visit_fn f, void *ctx) {
    char buf[16384]; ent_t e;
    if (ent_of(x->path, &e) && ex_test(x, &e, x->path)) f(&e, x->path, ctx);
    span_t sp[2]; int k = ix_subtree(x->path, sp);
    for (int j = 0; j < k; j++) for (uint64_t r = sp[j].a; r < sp[j].b; r++) {
        if (ix_dead(r)) continue;
        ent_t er = { (int64_t)r, NULL }; if (!x->nglobs && x->has_type && !strchr(x->types, IX.TY[r])) continue;
        if (ex_test(x, &er, NULL)) f(&er, row_path(r, buf), ctx);
    }
    for (size_t i = 0; i < IX.ndx; i++) {
        dent_t *d = &IX.dx[i];
        if (!d->live || !strcmp(d->path, x->path) || !path_under(d->path, x->path)) continue;
        ent_t ed = { -1, d }; if (ex_test(x, &ed, d->path)) f(&ed, d->path, ctx);
    }
}
typedef struct { int64_t count, bytes, best_mt; char best[16384]; int have; char *list; size_t ll, lc; } agg_t;
static void agg_visit(const ent_t *e, const char *path, void *ctx) {
    agg_t *a = ctx; a->count++; a->bytes += ix_attr(e, A_SIZE);
    int64_t mt = ix_attr(e, A_MTIME); if (!a->have || mt > a->best_mt) { a->have = 1; a->best_mt = mt; strcpy(a->best, path); }
    if (a->list) {
        size_t need = strlen(mp_abs) + strlen(path) + 4;
        if (a->ll + need >= a->lc) { a->lc = (a->lc + need) * 2; a->list = realloc(a->list, a->lc); }
        a->ll += (size_t)sprintf(a->list + a->ll, "%s/%s\n", strcmp(mp_abs, "/") ? mp_abs : "", rel(path));
    }
}

/* directories holding a match strictly below them: dictionary-code bitmap + overlay paths, per (query, generation) */
typedef struct { char *key; uint64_t gen; uint8_t *codes; smap_t extra; } holders_t; static holders_t *H; static size_t nH;
typedef struct { holders_t *h; const char *top; } hctx_t;
static void mark_up(holders_t *h, const char *top, const char *dir) {
    char d[16384]; snprintf(d, sizeof d, "%s", dir);
    for (;;) {
        int64_t c = dict_find(IX.doff, IX.dblob, IX.H.ndirs, d);
        if (c >= 0) { if (h->codes[c]) return; h->codes[c] = 1; }
        else { if (sm_get(&h->extra, d) >= 0) return; sm_put(&h->extra, d, 1); }
        if (!strcmp(d, top) || !strcmp(d, "/") || !path_under(d, top)) return;
        char *s = strrchr(d, '/'); if (!s) return; if (s == d) s[1] = 0; else *s = 0;
    }
}
static void holder_visit(const ent_t *e, const char *path, void *ctx) {
    hctx_t *c = ctx;
    if (!strcmp(path, c->top)) return;
    if (!e->d) { uint64_t dc = ix_dc((uint64_t)e->row); if (c->h->codes[dc]) return; mark_up(c->h, c->top, dstr(dc)); return; }
    char dir[16384], nm[4096]; split_path(path, dir, nm); mark_up(c->h, c->top, dir);
}
static holders_t *holders(const expr_t *x) {
    char key[20000]; ex_text(x, 0, key, sizeof key); char k2[20100]; snprintf(k2, sizeof k2, "%s\x01%s", x->path, key);
    holders_t *h = NULL; for (size_t i = 0; i < nH; i++) if (!strcmp(H[i].key, k2)) { h = &H[i]; break; }
    if (h && h->gen == IX.gen) return h;
    if (!h) { H = realloc(H, (nH + 1) * sizeof *H); h = &H[nH++]; memset(h, 0, sizeof *h); h->key = strdup(k2); h->codes = calloc(IX.H.ndirs + 1, 1); }
    else { memset(h->codes, 0, IX.H.ndirs + 1); for (size_t i = 0; i < h->extra.cap; i++) if (h->extra.k[i] && h->extra.k[i] != (char *)1) free(h->extra.k[i]);
           free(h->extra.k); free(h->extra.v); memset(&h->extra, 0, sizeof h->extra); }
    hctx_t c = { h, x->path }; ex_select(x, holder_visit, &c); h->gen = IX.gen; return h;
}
static int is_holder(holders_t *h, const char *p) {
    int64_t c = dict_find(IX.doff, IX.dblob, IX.H.ndirs, p);
    if (c >= 0 && h->codes[c]) return 1;
    return sm_get(&h->extra, p) >= 0;
}

/* ------------------------------------------------------------------ saved queries (INDEX.queries) */
typedef struct { char *name; expr_t *e; } query_t; static query_t *Q; static size_t nq;
static char qfile[8300];
static query_t *q_find(const char *n) { for (size_t i = 0; i < nq; i++) if (!strcmp(Q[i].name, n)) return &Q[i]; return NULL; }
static void q_save(void) {
    char tmp[8400]; snprintf(tmp, sizeof tmp, "%s.tmp", qfile); FILE *f = fopen(tmp, "w"); if (!f) return;
    char t[20000];
    for (size_t i = 0; i < nq; i++) {
        for (const char *p = Q[i].name; *p; p++) { if (*p == '\t' || *p == '\n' || *p == '%') fprintf(f, "%%%02X", (uint8_t)*p); else fputc(*p, f); }
        ex_text(Q[i].e, 0, t, sizeof t); fprintf(f, "\t%s\n", t);
    }
    fflush(f); fsync(fileno(f)); fclose(f); rename(tmp, qfile);
}
static void q_load(void) {
    FILE *f = fopen(qfile, "r"); if (!f) return; char line[20000];
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t'); if (!tab) continue; *tab = 0; line[strcspn(tab + 1, "\n") + (tab + 1 - line)] = 0;
        char name[4096]; size_t j = 0;
        for (const char *p = line; *p && j < sizeof name - 1; p++) { if (*p == '%' && p[1] && p[2]) { unsigned v; sscanf(p + 1, "%2X", &v); name[j++] = (char)v; p += 2; } else name[j++] = *p; }
        name[j] = 0; expr_t *e = ex_parse(tab + 1); if (!e) continue;
        Q = realloc(Q, (nq + 1) * sizeof *Q); Q[nq++] = (query_t){ strdup(name), e };
    }
    fclose(f);
}

/* ------------------------------------------------------------------ virtual nodes */
enum { V_QROOT, V_VIEW, V_AGG };
typedef struct { int kind; char *qname; expr_t *ex; char *x; } vnode_t;    /* query: a saved name, or its own expression */
static vnode_t **V; static size_t nV; static smap_t vkey;       /* stable pointers: lookups hold them across inserts */
/* a view reached by an @ segment inside another view is keyed by that parent too: a redundant condition (@type=fl
 * below @type=f) gives the parent's own expression, and answering a lookup with the parent's node is a loop the
 * kernel refuses (ELOOP) */
static uint64_t vsalt;
static void vkey_of(int kind, const char *qname, const expr_t *ex, const char *x, char *out) {
    char t[20000] = ""; if (ex) { ex_text(ex, 0, t, sizeof t); }
    snprintf(out, 40000, "%d\x01%s\x01%s\x01%s\x01%s\x01%llu", kind, qname ? qname : "", ex ? ex->path : "", t, x ? x : "", (unsigned long long)vsalt);
}
static uint64_t vnode(int kind, const char *qname, const expr_t *ex, const char *x) {
    if (kind == V_QROOT) return QROOT;
    static char k[40000]; vkey_of(kind, qname, ex, x, k);
    int64_t n = sm_get(&vkey, k); if (n >= 0) return (uint64_t)n;
    V = realloc(V, (nV + 1) * sizeof *V); V[nV] = malloc(sizeof **V);
    *V[nV] = (vnode_t){ kind, qname ? strdup(qname) : NULL, ex ? ex_copy(ex) : NULL, x ? strdup(x) : NULL };
    sm_put(&vkey, k, VNODE + nV); return VNODE + nV++;
}
static vnode_t qroot_v = { V_QROOT, NULL, NULL, NULL };
static vnode_t *vget(uint64_t node, int *err) {
    if (node == QROOT) return &qroot_v;
    if (node < VNODE || node - VNODE >= nV) { *err = ESTALE; return NULL; }
    vnode_t *v = V[node - VNODE];
    if (v->qname && !q_find(v->qname)) { *err = ENOENT; return NULL; }
    return v;
}
static const expr_t *vexpr(const vnode_t *v) { return v->qname ? q_find(v->qname)->e : v->ex; }
static const char *AGG[] = { ".expression", ".count", ".bytes", ".newest" };
/* the content of a result file */
static char *agg_content(const vnode_t *v, size_t *len) {
    const expr_t *e = vexpr(v); const char *k = v->x + 1; char *out = malloc(64);
    if (!strcmp(k, "expression")) {
        char t[20000]; ex_text(e, 1, t, sizeof t); out = realloc(out, strlen(t) + 2); sprintf(out, "%s\n", t); *len = strlen(out); return out;
    }
    agg_t a; memset(&a, 0, sizeof a);
    if (!strcmp(k, "list")) { a.lc = 4096; a.list = malloc(a.lc); a.list[0] = 0; free(out); ex_select(e, agg_visit, &a); *len = a.ll; return a.list; }
    ex_select(e, agg_visit, &a);
    if (!strcmp(k, "count")) sprintf(out, "%lld\n", (long long)a.count);
    else if (!strcmp(k, "bytes")) sprintf(out, "%lld\n", (long long)a.bytes);
    else if (!a.have) out[0] = 0;
    else {
        out = realloc(out, strlen(mp_abs) + strlen(a.best) + 4);
        sprintf(out, "%s/%s", strcmp(mp_abs, "/") ? mp_abs : "", rel(a.best));
        size_t l = strlen(out); while (l > 1 && out[l - 1] == '/') out[--l] = 0;
        strcat(out, "\n");
    }
    *len = strlen(out); return out;
}
static int vattr(uint64_t node, struct fattr *a, int64_t open_size) {
    int err = 0; vnode_t *v = vget(node, &err); if (!v) return err;
    ent_t re; int64_t mt = ent_of(root, &re) ? ix_attr(&re, A_MTIME) : 0;
    memset(a, 0, sizeof *a); a->ino = node; a->uid = getuid(); a->gid = getgid(); a->blksize = 4096;
    if (v->kind == V_VIEW) { ent_t e; if (!ent_of(v->x, &e)) return ENOENT; mt = ix_attr(&e, A_MTIME); }
    a->atime = a->mtime = a->ctime = (uint64_t)(mt / 1000000000); a->atimensec = a->mtimensec = a->ctimensec = (uint32_t)(mt % 1000000000);
    if (v->kind == V_QROOT) { a->mode = S_IFDIR | 0755; a->nlink = 2; return 0; }
    if (v->kind == V_VIEW) { a->mode = S_IFDIR | 0555; a->nlink = 2; return 0; }
    size_t len = 0; if (open_size >= 0) len = (size_t)open_size; else { char *c = agg_content(v, &len); free(c); }
    a->size = len; a->nlink = 1; a->mode = S_IFREG | ((!strcmp(v->x, ".expression") && v->qname) ? 0644 : 0444);
    return 0;
}
static int ventry(uint64_t node, struct entry_out *o) {
    memset(o, 0, sizeof *o); o->nodeid = node; return vattr(node, &o->attr, -1);      /* TTL 0: always re-evaluated */
}

/* a view's listing: (name, node, entry) */
typedef struct { char *name; uint64_t node; int is_dir; int64_t ino; struct entry_out ent; } vitem_t;
typedef struct { vitem_t *v; size_t n, cap; } vlist_t;
static void vl_add(vlist_t *l, const char *name, uint64_t node, int is_dir, int64_t ino, const struct entry_out *ent) {
    if (l->n == l->cap) { l->cap = l->cap ? 2 * l->cap : 64; l->v = realloc(l->v, l->cap * sizeof *l->v); }
    l->v[l->n++] = (vitem_t){ strdup(name), node, is_dir, ino, *ent };
}
static void vl_free(vlist_t *l) { for (size_t i = 0; i < l->n; i++) free(l->v[i].name); free(l->v); memset(l, 0, sizeof *l); }
static int cmp_vitem(const void *a, const void *b) { return strcmp(((const vitem_t *)a)->name, ((const vitem_t *)b)->name); }
static void view_children(const vnode_t *v, const char *d, vlist_t *out) {
    const expr_t *e = vexpr(v); holders_t *h = holders(e); char path[16384]; struct entry_out ent;
    int64_t dc = dict_find(IX.doff, IX.dblob, IX.H.ndirs, d);
    vlist_t tmp = {0};
    if (dc >= 0) for (uint64_t r = IX.dstart[dc]; r < IX.dstart[dc + 1]; r++) {
        if (ix_dead(r)) continue;
        ent_t er = { (int64_t)r, NULL }; row_path(r, path); const char *nm = nstr(ix_nc(r));
        if (IX.TY[r] == 'd') {
            if (!path_under(path, e->path) && !path_under(e->path, path)) continue;
            if (is_holder(h, path) || (path_under(path, e->path) && ex_test(e, &er, path))) {
                expr_t *sub = ex_copy(e); uint64_t n = v->qname ? vnode(V_VIEW, v->qname, NULL, path) : vnode(V_VIEW, NULL, sub, path); ex_free(sub);
                ventry(n, &ent); vl_add(&tmp, nm, n, 1, ix_attr(&er, A_INODE), &ent);
            }
        } else if (path_under(path, e->path) && ex_test(e, &er, path)) { entry_for(path, &er, &ent); vl_add(&tmp, nm, ent.nodeid, 0, ix_attr(&er, A_INODE), &ent); }
    }
    char dir[16384], nm[4096];
    for (size_t i = 0; i < IX.ndx; i++) {
        dent_t *x = &IX.dx[i]; if (!x->live) continue;
        split_path(x->path, dir, nm); if (strcmp(dir, d)) continue;
        ent_t ed = { -1, x };
        if (x->at.a[A_TYPE] == 'd') {
            if (is_holder(h, x->path) || (path_under(x->path, e->path) && ex_test(e, &ed, x->path))) {
                uint64_t n = v->qname ? vnode(V_VIEW, v->qname, NULL, x->path) : vnode(V_VIEW, NULL, e, x->path);
                ventry(n, &ent); vl_add(&tmp, nm, n, 1, x->at.a[A_INODE], &ent);
            }
        } else if (path_under(x->path, e->path) && ex_test(e, &ed, x->path)) { entry_for(x->path, &ed, &ent); vl_add(&tmp, nm, ent.nodeid, 0, x->at.a[A_INODE], &ent); }
    }
    qsort(tmp.v, tmp.n, sizeof *tmp.v, cmp_vitem);
    for (size_t i = 0; i < tmp.n; i++) if (!i || strcmp(tmp.v[i].name, tmp.v[i - 1].name)) vl_add(out, tmp.v[i].name, tmp.v[i].node, tmp.v[i].is_dir, tmp.v[i].ino, &tmp.v[i].ent);
    vl_free(&tmp);
}
static int vlist(uint64_t node, vlist_t *out) {
    int err = 0; vnode_t *v = vget(node, &err); if (!v) return err; struct entry_out ent;
    if (v->kind == V_QROOT) {
        for (size_t i = 0; i < nq; i++) { uint64_t n = vnode(V_VIEW, Q[i].name, NULL, Q[i].e->path); ventry(n, &ent); vl_add(out, Q[i].name, n, 1, 0, &ent); }
        qsort(out->v, out->n, sizeof *out->v, cmp_vitem); return 0;
    }
    if (v->kind != V_VIEW) return ENOTDIR;
    if (v->qname && !strcmp(v->x, q_find(v->qname)->e->path))
        for (int i = 0; i < 4; i++) { uint64_t n = vnode(V_AGG, v->qname, NULL, AGG[i]); ventry(n, &ent); vl_add(out, AGG[i], n, 0, 0, &ent); }
    view_children(v, v->x, out); return 0;
}

/* ------------------------------------------------------------------ open virtual files */
typedef struct { int used; uint64_t node; char *buf; size_t len, cap; int writing; } vfh_t; static vfh_t *VF; static size_t nVF;
static uint64_t vfh_new(uint64_t node, char *buf, size_t len, int writing) {
    size_t i; for (i = 0; i < nVF; i++) if (!VF[i].used) break;
    if (i == nVF) { VF = realloc(VF, (nVF + 1) * sizeof *VF); nVF++; }
    VF[i] = (vfh_t){ 1, node, buf, len, len, writing }; return i + 1;
}
static vfh_t *vfh_get(uint64_t fh) { return (fh >= 1 && fh <= nVF && VF[fh - 1].used) ? &VF[fh - 1] : NULL; }
static int set_query(const char *qname, const char *text) {
    expr_t *e = ex_parse(text); if (!e) return EINVAL;
    if (!path_under(e->path, root)) { ex_free(e); return EINVAL; }
    query_t *q = q_find(qname); if (!q) { ex_free(e); return ENOENT; }
    ex_free(q->e); q->e = e; q_save(); return 0;
}

/* ------------------------------------------------------------------ request handlers: virtual */
static int uri_step(uint64_t parent, const expr_t *base, const char *field, const char *sval, int64_t ival, const char *res, struct entry_out *o) {
    if (res) {
        char x[32]; snprintf(x, sizeof x, "@%s", res);
        uint64_t n = vnode(V_AGG, NULL, base, x); return ventry(n, o);
    }
    expr_t *e = ex_copy(base); conjoin(e, field, sval, ival); if (e->has_type) sort_types(e->types);
    vsalt = parent; uint64_t n = vnode(V_VIEW, NULL, e, e->path); vsalt = 0; ex_free(e); return ventry(n, o);
}
static void v_lookup(uint64_t unique, uint64_t node, const char *name) {
    int err = 0; vnode_t *v = vget(node, &err); struct entry_out o;
    if (!v) { reply(unique, err, 0, 0); return; }
    if (v->kind == V_QROOT) {
        query_t *q = q_find(name); if (!q) { reply(unique, ENOENT, 0, 0); return; }
        err = ventry(vnode(V_VIEW, name, NULL, q->e->path), &o); reply(unique, err, &o, sizeof o); return;
    }
    if (v->kind != V_VIEW) { reply(unique, ENOTDIR, 0, 0); return; }
    const expr_t *e = vexpr(v);
    if (v->qname && !strcmp(v->x, e->path)) for (int i = 0; i < 4; i++) if (!strcmp(name, AGG[i])) {
        err = ventry(vnode(V_AGG, v->qname, NULL, AGG[i]), &o); reply(unique, err, &o, sizeof o); return; }
    if (name[0] == '@') {
        const char *lit = NULL, *res = NULL; char field[32], sval[4096]; int64_t ival = 0;
        int k = parse_segment(name, &lit, &res, field, sval, &ival);
        if (k) { expr_t *b = ex_copy(e); free(b->path); b->path = strdup(v->x); err = uri_step(node, b, field, sval, ival, res, &o); ex_free(b); reply(unique, err, &o, sizeof o); return; }
        name = lit;
    }
    vlist_t l = {0}; view_children(v, v->x, &l);
    for (size_t i = 0; i < l.n; i++) if (!strcmp(l.v[i].name, name)) { o = l.v[i].ent; vl_free(&l); reply(unique, 0, &o, sizeof o); return; }
    vl_free(&l);
    char p[16384]; join(v->x, name, p); ent_t re;                       /* a subdirectory without matches: reachable */
    if (ent_of(p, &re) && ix_attr(&re, A_TYPE) == 'd') {
        uint64_t n = v->qname ? vnode(V_VIEW, v->qname, NULL, p) : vnode(V_VIEW, NULL, v->ex, p);
        err = ventry(n, &o); reply(unique, err, &o, sizeof o); return;
    }
    reply(unique, ENOENT, 0, 0);
}
static size_t put_dirent(uint8_t *buf, size_t used, size_t size, int plus, const struct entry_out *eo, uint64_t ino, uint64_t cookie, const char *name, uint32_t type) {
    size_t nl = strlen(name), hl = plus ? sizeof(struct entry_out) : 0, rl = (hl + sizeof(struct dirent_h) + nl + 7) & ~(size_t)7;
    if (used + rl > size) return 0;
    uint8_t *p = buf + used; memset(p, 0, rl);
    if (plus) memcpy(p, eo, sizeof *eo);
    struct dirent_h dh = { ino, cookie, (uint32_t)nl, type }; memcpy(p + hl, &dh, sizeof dh); memcpy(p + hl + sizeof dh, name, nl);
    return rl;
}
static void v_readdir(uint64_t unique, uint64_t node, const struct read_in *ri, int plus) {
    vlist_t l = {0}; int err = vlist(node, &l); if (err) { reply(unique, err, 0, 0); return; }
    size_t size = ri->size < sizeof outbuf ? ri->size : sizeof outbuf, used = 0; struct entry_out z; memset(&z, 0, sizeof z);
    for (uint64_t i = ri->offset; i < l.n + 2; i++) {
        size_t k;
        if (i < 2) k = put_dirent(outbuf, used, size, plus, &z, node, i + 1, i ? ".." : ".", 4);
        else { vitem_t *it = &l.v[i - 2]; k = put_dirent(outbuf, used, size, plus, &it->ent, it->ino ? (uint64_t)it->ino : it->node, i + 1, it->name, it->is_dir ? 4 : dtype('f', 0)); }
        if (!k) break; used += k;
    }
    vl_free(&l); reply(unique, 0, outbuf, used);
}

/* ------------------------------------------------------------------ request handlers: real tree */
static int lookup_real(const char *parent, const char *name, struct entry_out *o) {
    char p[16384]; join(parent, name, p); ent_t e;
    if (!ix_lookup(p, &e)) return ENOENT;
    entry_for(p, &e, o); return 0;
}
static void do_lookup(uint64_t unique, uint64_t node, const char *name) {
    struct entry_out o; char buf[16384];
    if (node == 1 && !strcmp(name, ".q")) { int err = ventry(QROOT, &o); reply(unique, err, &o, sizeof o); return; }
    if (node >= QROOT && node < DYN_NODE) { v_lookup(unique, node, name); return; }
    const char *d = node_path(node, buf); if (!d) { reply(unique, ESTALE, 0, 0); return; }
    if (name[0] == '@') {
        const char *lit = NULL, *res = NULL; char field[32], sval[4096]; int64_t ival = 0;
        int k = parse_segment(name, &lit, &res, field, sval, &ival);
        if (k) { expr_t *b = ex_new(d); int err = uri_step(0, b, field, sval, ival, res, &o); ex_free(b); reply(unique, err, &o, sizeof o); return; }
        name = lit;
    }
    int err = lookup_real(d, name, &o); reply(unique, err, &o, sizeof o);
}
static void do_getattr(uint64_t unique, uint64_t node, const uint8_t *body) {
    struct attr_out o; memset(&o, 0, sizeof o);
    if (node >= QROOT && node < DYN_NODE) {
        int64_t open_size = -1; uint32_t flags; uint64_t fh; memcpy(&flags, body, 4); memcpy(&fh, body + 8, 8);
        if ((flags & 1) && vfh_get(fh)) open_size = (int64_t)vfh_get(fh)->len;
        int err = vattr(node, &o.attr, open_size); reply(unique, err, &o, sizeof o); return;
    }
    char buf[16384]; const char *p = node_path(node, buf); ent_t e;
    if (!p) { reply(unique, ESTALE, 0, 0); return; }
    if (node >= ROW_NODE && node < ROW_NODE + IX.H.n && !ix_dead(node - ROW_NODE)) { e.row = (int64_t)(node - ROW_NODE); e.d = NULL; }
    else if (!ent_of(p, &e)) { reply(unique, ENOENT, 0, 0); return; }
    o.attr_valid = (uint64_t)ttl; fill_attr(p, &e, &o.attr); reply(unique, 0, &o, sizeof o);
}
static void do_readdir(uint64_t unique, uint64_t node, const struct read_in *ri, int plus) {
    if (node >= QROOT && node < DYN_NODE) { v_readdir(unique, node, ri, plus); return; }
    char buf[16384]; const char *p = node_path(node, buf); ent_t me;
    if (!p) { reply(unique, ESTALE, 0, 0); return; }
    if (!ent_of(p, &me)) { reply(unique, ENOENT, 0, 0); return; }
    if (ix_attr(&me, A_TYPE) != 'd') { reply(unique, ENOTDIR, 0, 0); return; }
    size_t size = ri->size < sizeof outbuf ? ri->size : sizeof outbuf, used = 0, k; uint64_t off = ri->offset;
    struct entry_out z, eo; memset(&z, 0, sizeof z); char path[16384];
    if (off < 2) {
        char par[16384], nm[4096]; split_path(p, par, nm); ent_t pe; int64_t pino = ix_attr(&me, A_INODE);
        if (strcmp(p, "/") && ent_of(par, &pe)) pino = ix_attr(&pe, A_INODE);
        for (uint64_t i = off; i < 2; i++) {
            if (!(k = put_dirent(outbuf, used, size, plus, &z, i ? (uint64_t)pino : (uint64_t)ix_attr(&me, A_INODE), i + 1, i ? ".." : ".", 4))) goto done;
            used += k;
        }
        off = 2;
    }
    int64_t dc = dict_find(IX.doff, IX.dblob, IX.H.ndirs, p);
    if (dc >= 0 && off < Q_COOKIE) {
        uint64_t a = IX.dstart[dc], b = IX.dstart[dc + 1], r0 = off == 2 ? a : off - 2; if (r0 < a) r0 = a;
        for (uint64_t r = r0; r < b; r++) {
            if (ix_dead(r)) continue;
            ent_t e = { (int64_t)r, NULL }; row_path(r, path);
            memset(&eo, 0, sizeof eo); eo.nodeid = r + ROW_NODE; eo.entry_valid = eo.attr_valid = (uint64_t)ttl; fill_attr(path, &e, &eo.attr);
            if (!(k = put_dirent(outbuf, used, size, plus, &eo, (uint64_t)IX.col[A_INODE][r], r + 3, nstr(ix_nc(r)), dtype(IX.TY[r], 0)))) goto done;
            used += k;
        }
    }
    if (node == 1 && off < Q_COOKIE) {
        struct entry_out qo; ventry(QROOT, &qo);
        if (!(k = put_dirent(outbuf, used, size, plus, &qo, QROOT, Q_COOKIE, ".q", 4))) goto done;
        used += k; off = Q_COOKIE;
    }
    {
        uint64_t kk = 0, start = off >= DELTA_COOKIE ? off - DELTA_COOKIE + 1 : 0; char dir[16384], nm[4096];
        for (size_t i = 0; i < IX.ndx; i++) {
            dent_t *x = &IX.dx[i]; if (!x->live) continue;
            split_path(x->path, dir, nm); if (strcmp(dir, p)) continue;
            if (kk++ < start) continue;
            ent_t e = { -1, x }; entry_for(x->path, &e, &eo);
            if (!(k = put_dirent(outbuf, used, size, plus, &eo, (uint64_t)x->at.a[A_INODE], DELTA_COOKIE + kk - 1, nm, dtype(x->at.a[A_TYPE], 0)))) goto done;
            used += k;
        }
    }
done:
    reply(unique, 0, outbuf, used);
}

/* passthrough writes: the backing tree first, then the index */
static int wpath(uint64_t node, const char *name, char *parent, char *out) {
    char buf[16384]; const char *d = node_path(node, buf); if (!d) return ESTALE;
    if (node >= QROOT && node < DYN_NODE) return EROFS;
    strcpy(parent, d); join(d, name, out); return 0;
}
static void entry_reply(uint64_t unique, const char *p) {
    struct entry_out o; ent_t e; if (!ix_lookup(p, &e)) { reply(unique, EIO, 0, 0); return; }
    entry_for(p, &e, &o); reply(unique, 0, &o, sizeof o);
}
static void do_rename(uint64_t unique, uint64_t node, uint64_t newdir, const char *old, const char *new_, uint32_t flags) {
    char od[16384], po[16384], nd[16384], pn[16384]; int err;
    if ((err = wpath(node, old, od, po)) || (err = wpath(newdir, new_, nd, pn))) { reply(unique, err, 0, 0); return; }
    struct stat st;
    if ((flags & 1) && !lstat(pn, &st)) { reply(unique, EEXIST, 0, 0); return; }
    if (flags & ~1u) { reply(unique, EINVAL, 0, 0); return; }
    uint64_t moved = node_of(po);
    if (rename(po, pn)) { reply(unique, errno, 0, 0); return; }
    ix_removed(pn); ix_removed(po); ix_put_tree(pn); ix_put_path(od); ix_put_path(nd);
    renames = realloc(renames, (nren + 1) * sizeof *renames); renames[nren++] = (ren_t){ strdup(po), strdup(pn) };
    char t[16384]; char **mv = NULL; uint64_t *mvn = NULL; size_t nmv = 0;   /* paths below the old name move with it */
    for (size_t i = 0; i < path_node.cap; i++) {
        char *k = path_node.k[i]; if (!k || k == (char *)1 || !path_under(k, po)) continue;
        mv = realloc(mv, (nmv + 1) * sizeof *mv); mvn = realloc(mvn, (nmv + 1) * sizeof *mvn); mv[nmv] = strdup(k); mvn[nmv++] = path_node.v[i];
    }
    for (size_t i = 0; i < nmv; i++) {
        snprintf(t, sizeof t, "%s%s", pn, mv[i] + strlen(po)); sm_del(&path_node, mv[i]); sm_put(&path_node, t, mvn[i]);
        if (mvn[i] >= DYN_NODE) { free(dyn_path[mvn[i] - DYN_NODE]); dyn_path[mvn[i] - DYN_NODE] = strdup(t); }
        free(mv[i]);
    }
    free(mv); free(mvn);
    sm_put(&path_node, pn, moved); if (moved >= DYN_NODE) { free(dyn_path[moved - DYN_NODE]); dyn_path[moved - DYN_NODE] = strdup(pn); }
    reply(unique, 0, 0, 0);
}
static int names2(const uint8_t *body, size_t blen, const char **a, const char **b) {
    *a = (const char *)body; size_t l = strnlen(*a, blen); if (l >= blen) return -1;
    *b = *a + l + 1; return strnlen(*b, blen - l - 1) < blen - l - 1 ? 0 : -1;
}

/* ------------------------------------------------------------------ dispatch */
static int running = 1;
static void handle(const uint8_t *req, size_t len) {
    const struct in_hdr *h = (const struct in_hdr *)req; const uint8_t *body = req + sizeof *h; size_t blen = len - sizeof *h;
    uint64_t u = h->unique, node = h->nodeid; int virt = node >= QROOT && node < DYN_NODE; char buf[16384], par[16384], p[16384];
    switch (h->opcode) {
    case INIT: {
        uint32_t in[4]; memcpy(in, body, 16); uint32_t out[16] = {0};
        out[0] = 7; out[1] = in[1] < 31 ? in[1] : 31; out[2] = in[2];
        out[3] = in[3] & ((1u << 0) | (1u << 5) | (1u << 13) | (1u << 22));   /* ASYNC_READ BIG_WRITES DO_READDIRPLUS MAX_PAGES */
        out[4] = 16 | (12u << 16); out[5] = 1 << 20; out[6] = 1; out[7] = 256;
        reply(u, 0, out, 64); return; }
    case FORGET: case BATCH_FORGET: case INTERRUPT: return;
    case DESTROY: reply(u, 0, 0, 0); running = 0; return;
    case OPENDIR: reply(u, ENOSYS, 0, 0); return;                         /* no_opendir: listings are keyed by node */
    case RELEASEDIR: case FSYNCDIR: case FLUSH: reply(u, 0, 0, 0); return;
    case LOOKUP: do_lookup(u, node, (const char *)body); return;
    case GETATTR: do_getattr(u, node, body); return;
    case READDIR: case READDIRPLUS: do_readdir(u, node, (const struct read_in *)body, h->opcode == READDIRPLUS); return;
    case STATFS: {
        struct statvfs v; if (statvfs(root, &v)) { reply(u, errno, 0, 0); return; }
        uint64_t o[5] = { v.f_blocks, v.f_bfree, v.f_bavail, v.f_files, v.f_ffree }; uint8_t z[80] = {0}; memcpy(z, o, 40);
        uint32_t b[3] = { (uint32_t)v.f_bsize, (uint32_t)v.f_namemax, (uint32_t)v.f_frsize }; memcpy(z + 40, b, 12); reply(u, 0, z, 80); return; }
    case ACCESS: {
        if (virt) { uint32_t mask; memcpy(&mask, body, 4); reply(u, 0, 0, 0); return; }
        uint32_t mask; memcpy(&mask, body, 4); const char *q = node_path(node, buf); if (!q) { reply(u, ESTALE, 0, 0); return; }
        reply(u, faccessat(AT_FDCWD, q, (int)mask, AT_SYMLINK_NOFOLLOW) ? errno : 0, 0, 0); return; }
    case READLINK: {
        const char *q = node_path(node, buf); ent_t e; if (!q || virt) { reply(u, virt ? EINVAL : ESTALE, 0, 0); return; }
        if (links && ix_lookup(q, &e) && ix_attr(&e, A_TYPE) != 'd') { reply(u, 0, q, strlen(q)); return; }
        ssize_t k = readlink(q, (char *)outbuf, 16384); if (k < 0) { reply(u, errno, 0, 0); return; }
        reply(u, 0, outbuf, (size_t)k); return; }
    case OPEN: {
        uint32_t flags; memcpy(&flags, body, 4);
        if (virt) {
            int err = 0; vnode_t *v = vget(node, &err); if (!v) { reply(u, err, 0, 0); return; }
            if (v->kind != V_AGG) { reply(u, EISDIR, 0, 0); return; }
            int writing = (flags & O_ACCMODE) != O_RDONLY;
            if (writing && strcmp(v->x, ".expression")) { reply(u, EACCES, 0, 0); return; }
            if (writing && !v->qname) { reply(u, EACCES, 0, 0); return; }
            size_t len = 0; char *c = (writing && (flags & O_TRUNC)) ? calloc(1, 1) : agg_content(v, &len);
            struct open_out o = { vfh_new(node, c, len, writing), FOPEN_DIRECT_IO, 0 }; reply(u, 0, &o, sizeof o); return;
        }
        const char *q = node_path(node, buf); if (!q) { reply(u, ESTALE, 0, 0); return; }
        if (links && (flags & O_ACCMODE) != O_RDONLY) { reply(u, EROFS, 0, 0); return; }
        int f = open(q, (int)(flags & ~(uint32_t)(O_CREAT | O_EXCL | O_NOCTTY)));
        if (f < 0) { reply(u, errno, 0, 0); return; }
        if (flags & O_TRUNC) ix_put_path(q);
        struct open_out o = { (uint64_t)f, watching ? 0 : FOPEN_KEEP_CACHE, 0 }; reply(u, 0, &o, sizeof o); return; }
    case READ: {
        const struct read_in *ri = (const struct read_in *)body; size_t n = ri->size < sizeof outbuf ? ri->size : sizeof outbuf;
        if (virt) { vfh_t *f = vfh_get(ri->fh); if (!f) { reply(u, EBADF, 0, 0); return; }
                    size_t a = ri->offset < f->len ? ri->offset : f->len, b = a + n < f->len ? a + n : f->len; reply(u, 0, f->buf + a, b - a); return; }
        ssize_t k = pread((int)ri->fh, outbuf, n, (off_t)ri->offset); reply(u, k < 0 ? errno : 0, outbuf, k < 0 ? 0 : (size_t)k); return; }
    case WRITE: {
        const struct read_in *wi = (const struct read_in *)body; const uint8_t *data = body + 40; uint32_t out[2] = { wi->size, 0 };
        if (virt) { vfh_t *f = vfh_get(wi->fh); if (!f || !f->writing) { reply(u, EBADF, 0, 0); return; }
                    size_t end = wi->offset + wi->size; if (end > f->cap) { f->cap = end * 2 + 64; f->buf = realloc(f->buf, f->cap); }
                    if (wi->offset > f->len) memset(f->buf + f->len, 0, wi->offset - f->len);
                    memcpy(f->buf + wi->offset, data, wi->size); if (end > f->len) f->len = end; reply(u, 0, out, 8); return; }
        if (links) { reply(u, EROFS, 0, 0); return; }
        ssize_t k = pwrite((int)wi->fh, data, wi->size, (off_t)wi->offset); if (k < 0) { reply(u, errno, 0, 0); return; }
        const char *q = node_path(node, buf); if (q) ix_put_path(q);
        out[0] = (uint32_t)k; reply(u, 0, out, 8); return; }
    case RELEASE: {
        uint64_t fh; memcpy(&fh, body, 8);
        if (virt) {
            vfh_t *f = vfh_get(fh); int err = 0;
            if (f) { if (f->writing) { int e2 = 0; vnode_t *v = vget(f->node, &e2); char *t = malloc(f->len + 1); memcpy(t, f->buf, f->len); t[f->len] = 0;
                                       err = v ? set_query(v->qname, t) : e2; free(t); }
                     free(f->buf); f->used = 0; }
            reply(u, err, 0, 0); return;
        }
        close((int)fh); const char *q = node_path(node, buf); struct stat st; if (q && !lstat(q, &st)) ix_put_path(q);
        reply(u, 0, 0, 0); return; }
    case FSYNC: { uint64_t fh; uint32_t fl; memcpy(&fh, body, 8); memcpy(&fl, body + 8, 4);
        if (virt) { reply(u, 0, 0, 0); return; }
        reply(u, ((fl & 1) ? fdatasync((int)fh) : fsync((int)fh)) ? errno : 0, 0, 0); return; }
    case GETXATTR: case LISTXATTR: reply(u, ENOSYS, 0, 0); return;
    }
    /* ---- changes: in views only .q's own operations and .expression edits; in sidecar mode none */
    int op = (int)h->opcode;
    if (virt) {
        int err = 0; vnode_t *v = vget(node, &err);
        if (!v) { reply(u, err, 0, 0); return; }
        if (op == SETATTR) {
            const struct setattr_in *s = (const struct setattr_in *)body; struct attr_out o; memset(&o, 0, sizeof o);
            if (v->kind == V_AGG && !strcmp(v->x, ".expression") && v->qname && (s->valid & FATTR_SIZE)) {
                vfh_t *f = (s->valid & FATTR_FH) ? vfh_get(s->fh) : NULL;
                if (f) { if (s->size < f->len) f->len = s->size; } else if (s->size == 0) set_query(v->qname, "");
            }
            err = vattr(node, &o.attr, -1); reply(u, err, &o, sizeof o); return;
        }
        if (v->kind == V_QROOT && op == MKDIR) {
            const char *name = (const char *)body + 8; struct entry_out o;
            if (q_find(name)) { reply(u, EEXIST, 0, 0); return; }
            Q = realloc(Q, (nq + 1) * sizeof *Q); Q[nq++] = (query_t){ strdup(name), ex_new(root) }; q_save();
            err = ventry(vnode(V_VIEW, name, NULL, root), &o); reply(u, err, &o, sizeof o); return;
        }
        if (v->kind == V_QROOT && op == RMDIR) {
            query_t *q = q_find((const char *)body); if (!q) { reply(u, ENOENT, 0, 0); return; }
            free(q->name); ex_free(q->e); *q = Q[--nq]; q_save(); reply(u, 0, 0, 0); return;
        }
        if (v->kind == V_QROOT && (op == RENAME || op == RENAME2)) {
            uint64_t nd; memcpy(&nd, body, 8); size_t hl = op == RENAME ? 8 : 16; const char *a, *b;
            if (nd != QROOT) { reply(u, EXDEV, 0, 0); return; }
            if (names2(body + hl, blen - hl, &a, &b)) { reply(u, EINVAL, 0, 0); return; }
            query_t *q = q_find(a); if (!q) { reply(u, ENOENT, 0, 0); return; }
            query_t *t = q_find(b); if (t && t != q) { free(t->name); ex_free(t->e); *t = Q[--nq]; q = q_find(a); }
            free(q->name); q->name = strdup(b); q_save(); reply(u, 0, 0, 0); return;
        }
        reply(u, (op == SETXATTR || op == REMOVEXATTR || op == SYMLINK || op == MKNOD || op == MKDIR || op == UNLINK || op == RMDIR ||
                  op == RENAME || op == RENAME2 || op == LINK || op == CREATE) ? EROFS : ENOSYS, 0, 0);
        return;
    }
    if (links) { reply(u, (op == SETATTR || op == SYMLINK || op == MKNOD || op == MKDIR || op == UNLINK || op == RMDIR || op == RENAME ||
                           op == RENAME2 || op == LINK || op == CREATE || op == SETXATTR || op == REMOVEXATTR) ? EROFS : ENOSYS, 0, 0); return; }
    int err = 0;
    switch (op) {
    case SETATTR: {
        const struct setattr_in *s = (const struct setattr_in *)body; const char *q = node_path(node, buf); if (!q) { reply(u, ESTALE, 0, 0); return; }
        if ((s->valid & FATTR_MODE) && fchmodat(AT_FDCWD, q, s->mode & 07777, 0)) err = errno;
        if (!err && (s->valid & (FATTR_UID | FATTR_GID)) && lchown(q, (s->valid & FATTR_UID) ? s->uid : (uid_t)-1, (s->valid & FATTR_GID) ? s->gid : (gid_t)-1)) err = errno;
        if (!err && (s->valid & FATTR_SIZE) && ((s->valid & FATTR_FH) ? ftruncate((int)s->fh, (off_t)s->size) : truncate(q, (off_t)s->size))) err = errno;
        if (!err && (s->valid & (FATTR_ATIME | FATTR_MTIME | FATTR_ATIME_NOW | FATTR_MTIME_NOW))) {
            struct timespec ts[2] = { { 0, UTIME_OMIT }, { 0, UTIME_OMIT } };
            if (s->valid & FATTR_ATIME_NOW) ts[0].tv_nsec = UTIME_NOW; else if (s->valid & FATTR_ATIME) { ts[0].tv_sec = (time_t)s->atime; ts[0].tv_nsec = s->atimensec; }
            if (s->valid & FATTR_MTIME_NOW) ts[1].tv_nsec = UTIME_NOW; else if (s->valid & FATTR_MTIME) { ts[1].tv_sec = (time_t)s->mtime; ts[1].tv_nsec = s->mtimensec; }
            if (utimensat(AT_FDCWD, q, ts, AT_SYMLINK_NOFOLLOW)) err = errno;
        }
        if (err) { reply(u, err, 0, 0); return; }
        ix_put_path(q); ent_t e; struct attr_out o; memset(&o, 0, sizeof o);
        if (!ix_lookup(q, &e)) { reply(u, ENOENT, 0, 0); return; }
        o.attr_valid = (uint64_t)ttl; fill_attr(q, &e, &o.attr); reply(u, 0, &o, sizeof o); return; }
    case SYMLINK: { const char *name, *target; if (names2(body, blen, &name, &target)) { reply(u, EINVAL, 0, 0); return; }
        if ((err = wpath(node, name, par, p))) break; if (symlink(target, p)) { err = errno; break; }
        ix_put_path(p); ix_put_path(par); entry_reply(u, p); return; }
    case MKNOD: { uint32_t m[4]; memcpy(m, body, 16); if ((err = wpath(node, (const char *)body + 16, par, p))) break;
        if (mknod(p, m[0] & ~m[2], m[1])) { err = errno; break; } ix_put_path(p); ix_put_path(par); entry_reply(u, p); return; }
    case MKDIR: { uint32_t m[2]; memcpy(m, body, 8); if ((err = wpath(node, (const char *)body + 8, par, p))) break;
        if (mkdir(p, m[0] & 07777)) { err = errno; break; } ix_put_path(p); ix_put_path(par); entry_reply(u, p); return; }
    case UNLINK: { if ((err = wpath(node, (const char *)body, par, p))) break;
        if (unlink(p)) { err = errno; break; } ix_removed(p); ix_put_path(par); reply(u, 0, 0, 0); return; }
    case RMDIR: { if ((err = wpath(node, (const char *)body, par, p))) break;
        if (rmdir(p)) { err = errno; break; } ix_delete_subtree(p); ix_put_path(par); reply(u, 0, 0, 0); return; }
    case RENAME: case RENAME2: {
        uint64_t nd; memcpy(&nd, body, 8); uint32_t fl = 0; size_t hl = 8; if (op == RENAME2) { memcpy(&fl, body + 8, 4); hl = 16; }
        const char *a, *b; if (names2(body + hl, blen - hl, &a, &b)) { err = EINVAL; break; }
        do_rename(u, node, nd, a, b, fl); return; }
    case LINK: { uint64_t on; memcpy(&on, body, 8); char sb[16384]; const char *src = node_path(on, sb);
        if (!src) { err = ESTALE; break; } if ((err = wpath(node, (const char *)body + 8, par, p))) break;
        if (link(src, p)) { err = errno; break; } ix_put_path(p); ix_put_path(par); entry_reply(u, p); return; }
    case CREATE: { uint32_t m[4]; memcpy(m, body, 16); if ((err = wpath(node, (const char *)body + 16, par, p))) break;
        int f = open(p, (int)(m[0] | O_CREAT), m[1] & 07777); if (f < 0) { err = errno; break; }
        ix_put_path(p); ix_put_path(par); ent_t e; ix_lookup(p, &e);
        struct { struct entry_out e; struct open_out o; } r; entry_for(p, &e, &r.e); r.o = (struct open_out){ (uint64_t)f, 0, 0 };
        reply(u, 0, &r, sizeof r); return; }
    default: err = ENOSYS;
    }
    reply(u, err, 0, 0);
}

/* ------------------------------------------------------------------ mount, loop */
static int do_mount(const char *mountpoint) {
    int sv[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { perror("socketpair"); return -1; }
    char opts[9000]; snprintf(opts, sizeof opts, "rw,nosuid,nodev,default_permissions,fsname=torblesfs:%s,subtype=torblesfs", root);
    pid_t pid = fork();
    if (!pid) {
        close(sv[0]); char env[32]; snprintf(env, sizeof env, "%d", sv[1]); setenv("_FUSE_COMMFD", env, 1);
        execlp("fusermount3", "fusermount3", "-o", opts, "--", mountpoint, (char *)NULL); _exit(127);
    }
    close(sv[1]);
    char c; struct iovec iov = { &c, 1 }; char cbuf[CMSG_SPACE(sizeof(int))]; struct msghdr m; memset(&m, 0, sizeof m);
    m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = cbuf; m.msg_controllen = sizeof cbuf;
    ssize_t k = recvmsg(sv[0], &m, 0); int st; waitpid(pid, &st, 0); close(sv[0]);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
    if (k <= 0 || !cm || cm->cmsg_type != SCM_RIGHTS) { fprintf(stderr, "tfsd: fusermount3 failed\n"); return -1; }
    int f; memcpy(&f, CMSG_DATA(cm), sizeof f); return f;
}
static void unmount_mp(void) {
    pid_t pid = fork(); if (!pid) { int z = open("/dev/null", O_WRONLY); dup2(z, 1); dup2(z, 2); execlp("fusermount3", "fusermount3", "-u", "-z", mp_abs, (char *)NULL); _exit(127); }
    int st; waitpid(pid, &st, 0);
}
static volatile sig_atomic_t stop_req;
static void on_sig(int s) { (void)s; stop_req = 1; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

void abspath(const char *in, char *out);
int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    const char *idx = getenv("TFS_INDEX"); double save_every = 1.0; const char *mpt = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) idx = argv[++i];
        else if (!strcmp(argv[i], "--links")) links = 1;
        else if (!strcmp(argv[i], "--watch")) watching = 1;
        else if (!strcmp(argv[i], "--save-every") && i + 1 < argc) save_every = atof(argv[++i]);
        else mpt = argv[i];
    }
    if (!idx || !mpt) { fprintf(stderr, "usage: tfsd -i INDEX [--links] [--watch] [--save-every S] MOUNTPOINT\n"); return 2; }
    ix_open(idx);
    if (IX.H.nroots != 1) { fprintf(stderr, "tfsd: mount needs an index with exactly one root\n"); return 2; }
    root = strdup(root_str(0)); abspath(mpt, mp_abs); mp = mp_abs;
    { char rr[8192], rm[8192]; if (realpath(root, rr) && realpath(mp_abs, rm) && path_under(rm, rr)) { fprintf(stderr, "tfsd: the mountpoint must be outside the indexed tree\n"); return 2; } }
    snprintf(qfile, sizeof qfile, "%s.queries", idx); q_load();
    ent_t re; if (!ent_of(root, &re)) { fprintf(stderr, "tfsd: root %s is not in the index\n", root); return 2; }
    dev0 = ix_attr(&re, A_DEV);
    if (watching) ttl = 1;
    int wfd = watching ? watch_open() : -1;
    fd = do_mount(mp_abs); if (fd < 0) return 1;
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN);
    size_t live = ix_live_delta(); for (uint64_t r = 0; r < IX.H.n; r++) live += !ix_dead(r);
    fprintf(stderr, "tfsd: %s -> %s (%s%s, %zu entries, %zu in the overlay)\n", root, mp_abs, links ? "links" : "passthrough",
            watching ? ", watching" : "", live, ix_live_delta());
    fflush(stderr);
    static uint8_t req[(1 << 20) + 8192]; double last = now(); int unmounting = 0;
    while (running) {
        if (stop_req && !unmounting) { unmounting = 1; unmount_mp(); }
        struct pollfd p[2] = { { fd, POLLIN, 0 }, { wfd, POLLIN, 0 } };
        int k = poll(p, wfd >= 0 ? 2 : 1, 500);
        if (k < 0 && errno != EINTR) break;
        if (k > 0 && (p[0].revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (k > 0 && (p[0].revents & POLLIN)) {
            ssize_t n = read(fd, req, sizeof req);
            if (n < 0) { if (errno == ENODEV) break; if (errno != EINTR && errno != ENOENT && errno != EAGAIN) { perror("tfsd: read"); break; } }
            else if (n >= (ssize_t)sizeof(struct in_hdr)) handle(req, (size_t)n);
        }
        if (wfd >= 0 && k > 0 && (p[1].revents & POLLIN)) watch_poll(wfd);
        if (IX.dirty && now() - last >= save_every) { ix_save_sidecar(); last = now(); }   /* never compacted while mounted: rows are node ids */
    }
    if (IX.dirty) ix_save_sidecar();
    if (!unmounting && running) unmount_mp();
    return 0;
}
