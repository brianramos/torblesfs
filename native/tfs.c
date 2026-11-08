/* tfs: the torblesFS tool. It maps an index file (TFSNIDX1, written by `tfs build`) and answers stat / ls / du /
 * find / locate without parsing or rebuilding anything; content and dupes answer content questions next to it.
 *
 *   tfs -i INDEX stat PATH
 *   tfs -i INDEX ls DIR
 *   tfs -i INDEX du PATH
 *   tfs -i INDEX find ROOT [EXPRESSION] [--count | --bytes | --newest | --group-by ext]
 *   tfs -i INDEX locate [-b] [-i] [-r] [-A] [-c] [-0] [-l N] [--root DIR] PATTERN...
 *
 * find: a compatible subset of find(1)'s expression language, evaluated per row with find's short-circuit semantics
 * (a path is printed where evaluation reaches -print; with no action, the whole expression is wrapped in -print;
 * -prune cuts the subtree of a directory where it is reached; -maxdepth / -mindepth as find). Name and path tests
 * use glibc fnmatch(3) and GNU regex (RE_SYNTAX_EMACS) in the user's locale, as find does; name tests are evaluated
 * once per distinct name, path-substring tests once per directory and per name.
 * locate: findutils' rules: a pattern without wildcards is a substring of the whole path, with wildcards it is
 * fnmatch(pattern, path, 0); -b tests the basename; -r is a GNU regex search; patterns are OR-ed (-A: AND-ed). */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <fnmatch.h>
#include <regex.h>
#include <locale.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "tfscore.h"
#include "tfsadmin.h"
#include "tfsext.h"

/* ------------------------------------------------------------------ the mapped index: the shared core (tfscore.c) */
static hdr_t H;
static const uint64_t *K, *T, *dstart, *doff, *noff, *roff; static const uint8_t *TY, *dblob, *nblob, *rblob;
static const int64_t *SZ, *BL, *MT, *MO, *UID, *GID, *NL, *INO, *DEV;
static uint64_t NMASK;
static dent_t *DX; static uint64_t ND;                  /* the live delta entries (index file + sidecar) */
static void die(const char *m) { fprintf(stderr, "tfs: %s\n", m); exit(2); }
static void open_index(const char *path) {
    ix_open(path); H = IX.H;
    K = IX.K; T = IX.T; dstart = IX.dstart; doff = IX.doff; noff = IX.noff; roff = IX.roff;
    TY = IX.TY; dblob = IX.dblob; nblob = IX.nblob; rblob = IX.rblob;
    SZ = IX.col[A_SIZE]; BL = IX.col[A_BLOCKS]; MT = IX.col[A_MTIME]; MO = IX.col[A_MODE]; UID = IX.col[A_UID];
    GID = IX.col[A_GID]; NL = IX.col[A_NLINK]; INO = IX.col[A_INODE]; DEV = IX.col[A_DEV];
    NMASK = IX.nmask;
    DX = malloc((IX.ndx ? IX.ndx : 1) * sizeof *DX); ND = 0;
    for (size_t i = 0; i < IX.ndx; i++) if (IX.dx[i].live) DX[ND++] = IX.dx[i];
}
#define DSTR(i) ((const char *)(dblob + doff[i]))
#define DLEN(i) (doff[(i) + 1] - doff[i] - 1)
#define NSTR(i) ((const char *)(nblob + noff[i]))
#define NLEN(i) (noff[(i) + 1] - noff[i] - 1)
static void load_strings(void) {}
static void load_delta(void) {}
static int bcmp_str(const uint8_t *a, uint64_t al, const char *b, size_t bl) {        /* al: stored length incl. NUL */
    al--; int c = memcmp(a, b, al < bl ? al : bl); return c ? c : (al < bl ? -1 : al > bl);
}
#define lower dict_lower
#define find_str dict_find
#define norm norm_path
#define under path_under
#define subtree ix_subtree
static inline int dead(uint64_t r) { return ix_dead(r); }
static inline uint64_t dcode(uint64_t r) { return ix_dc(r); }
static inline uint64_t ncode(uint64_t r) { return ix_nc(r); }
static inline char e_type(const ent_t *e) { return e->d ? (char)e->d->at.a[A_TYPE] : (char)TY[e->row]; }
static inline int64_t e_size(const ent_t *e) { return e->d ? e->d->at.a[A_SIZE] : SZ[e->row]; }
static inline int64_t e_mtime(const ent_t *e) { return e->d ? e->d->at.a[A_MTIME] : MT[e->row]; }
static inline int64_t e_mode(const ent_t *e) { return e->d ? e->d->at.a[A_MODE] : MO[e->row]; }
static inline int64_t e_uid(const ent_t *e) { return e->d ? e->d->at.a[A_UID] : UID[e->row]; }
static inline int64_t e_gid(const ent_t *e) { return e->d ? e->d->at.a[A_GID] : GID[e->row]; }
static inline int64_t e_nlink(const ent_t *e) { return e->d ? e->d->at.a[A_NLINK] : NL[e->row]; }
static inline int64_t e_inode(const ent_t *e) { return e->d ? e->d->at.a[A_INODE] : INO[e->row]; }
static inline int64_t e_dev(const ent_t *e) { return e->d ? e->d->at.a[A_DEV] : DEV[e->row]; }
static int lookup(const char *path, ent_t *e) { return ix_lookup(path, e); }
#define row_of ix_row

/* ------------------------------------------------------------------ output helpers */
static int cmp_cstr(const void *a, const void *b) {
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    size_t lx = strlen(x), ly = strlen(y); int c = memcmp(x, y, lx < ly ? lx : ly); return c ? c : (lx < ly ? -1 : lx > ly);
}
typedef struct { char **v; size_t n, cap; } svec_t;
static void sv_push(svec_t *s, const char *p) {
    if (s->n == s->cap) { s->cap = s->cap ? 2 * s->cap : 1024; s->v = realloc(s->v, s->cap * sizeof *s->v); }
    s->v[s->n++] = strdup(p);
}
static void sv_print(svec_t *s, int nul, size_t limit) {
    qsort(s->v, s->n, sizeof *s->v, cmp_cstr);
    size_t m = s->n < limit ? s->n : limit;
    for (size_t i = 0; i < m; i++) { if (i && !strcmp(s->v[i], s->v[i - 1])) continue; fputs(s->v[i], stdout); putchar(nul ? 0 : '\n'); }
}
static void commas(char *out, int64_t v) {
    char t[32]; int n = snprintf(t, sizeof t, "%lld", (long long)v), j = 0, neg = t[0] == '-';
    for (int i = 0; i < n; i++) { if (i > neg && (n - i) % 3 == 0) out[j++] = ','; out[j++] = t[i]; }
    out[j] = 0;
}

/* ------------------------------------------------------------------ stat / ls / du */
static int cmd_stat(const char *p) {
    char path[8192]; norm(p, path); ent_t e;
    if (!lookup(path, &e)) { fprintf(stderr, "%s: not in index\n", path); return 1; }
    time_t t = (time_t)(e_mtime(&e) / 1000000000); struct tm tm; localtime_r(&t, &tm); char ts[32], sz[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm); commas(sz, e_size(&e));
    printf("%c %04llo %lld:%lld %14s %s %s\n", e_type(&e), (long long)e_mode(&e), (long long)e_uid(&e), (long long)e_gid(&e), sz, ts, path);
    return 0;
}
static int cmd_ls(const char *p) {
    char path[8192]; norm(p, path); load_strings(); load_delta(); svec_t s = {0};
    int64_t d = find_str(doff, dblob, H.ndirs, path); ent_t e;
    int isdir = (lookup(path, &e) && e_type(&e) == 'd');
    for (uint64_t i = 0; i < H.nroots; i++) if (bcmp_str(rblob + roff[i], roff[i + 1] - roff[i], path, strlen(path)) == 0) isdir = 1;
    if (!isdir && strcmp(path, "/")) { fprintf(stderr, "%s: not a directory in the index\n", path); return 1; }
    if (d >= 0) for (uint64_t r = dstart[d]; r < dstart[d + 1]; r++) if (!dead(r)) sv_push(&s, NSTR(ncode(r)));
    char dir[8192], name[4096];
    for (uint64_t i = 0; i < ND; i++) { split_path(DX[i].path, dir, name); if (!strcmp(dir, path)) sv_push(&s, name); }
    sv_print(&s, 0, (size_t)-1); return 0;
}
static int cmd_du(const char *p) {
    char path[8192]; norm(p, path); span_t sp[2]; int k = subtree(path, sp); int64_t cnt = 0, tot = 0;
    for (int j = 0; j < k; j++) for (uint64_t r = sp[j].a; r < sp[j].b; r++) if (!dead(r)) { cnt++; tot += SZ[r]; }
    load_delta(); char b[8192];
    for (uint64_t i = 0; i < ND; i++) if (strcmp(DX[i].path, path) && under(DX[i].path, path)) { cnt++; tot += DX[i].at.a[A_SIZE]; }
    (void)b; char c1[32], c2[32]; commas(c1, tot); commas(c2, cnt); printf("%s bytes in %s entries below %s\n", c1, c2, path); return 0;
}

/* ------------------------------------------------------------------ find: expression tree */
enum { N_AND, N_OR, N_NOT, N_COMMA, N_PRINT, N_PRUNE, N_TRUE, N_FALSE, N_NAME, N_PATH, N_REGEX, N_TYPE, N_SIZE, N_MTIME,
       N_NEWER, N_COL, N_NOOWNER, N_PERM, N_EMPTY };
typedef struct node { int k; struct node *a, *b; char *pat; int icase; uint8_t *nmask; regex_t *rx; struct re_pattern_buffer *re;
                      char types[16]; char op; int64_t v, unit; int col; int perm_kind; int path_kind; uint8_t *dmask; } node_t;
static node_t *nn(int k) { node_t *x = calloc(1, sizeof *x); x->k = k; return x; }
static char **tok; static int ntok, ti, has_action, maxdepth = -1, mindepth = -1, print0;
static double now_s;

static node_t *expr(void);
static const char *take(void) { if (ti >= ntok) die("find: missing argument"); return tok[ti++]; }
static void num(const char *s, char *op, int64_t *v, int64_t *unit, int sized) {
    *op = (s[0] == '+' || s[0] == '-') ? s[0] : '='; const char *q = *op == '=' ? s : s + 1; char *end;
    *v = strtoll(q, &end, 10); *unit = 1;
    if (sized) {
        switch (*end) { case 'c': *unit = 1; break; case 'w': *unit = 2; break; case 'k': *unit = 1024; break;
                        case 'M': *unit = 1 << 20; break; case 'G': *unit = 1 << 30; break; default: *unit = 512; }
    }
}
static node_t *primary(void) {
    const char *t = take(); node_t *x;
    if (!strcmp(t, "!") || !strcmp(t, "-not")) { x = nn(N_NOT); x->a = primary(); return x; }
    if (!strcmp(t, "(")) { x = expr(); if (ti >= ntok || strcmp(tok[ti++], ")")) die("find: missing )"); return x; }
    if (!strcmp(t, "-name") || !strcmp(t, "-iname")) { x = nn(N_NAME); x->pat = strdup(take()); x->icase = t[1] == 'i'; return x; }
    if (!strcmp(t, "-path") || !strcmp(t, "-ipath") || !strcmp(t, "-wholename") || !strcmp(t, "-iwholename")) {
        x = nn(N_PATH); x->pat = strdup(take()); x->icase = t[1] == 'i'; return x; }
    if (!strcmp(t, "-regex") || !strcmp(t, "-iregex")) {
        x = nn(N_REGEX); x->pat = strdup(take()); x->re = calloc(1, sizeof *x->re);
        re_set_syntax(RE_SYNTAX_EMACS | (t[1] == 'i' ? RE_ICASE : 0));
        const char *err = re_compile_pattern(x->pat, strlen(x->pat), x->re); if (err) die(err); return x; }
    if (!strcmp(t, "-type")) { x = nn(N_TYPE); const char *s = take(); int j = 0; for (; *s && j < 15; s++) if (*s != ',') x->types[j++] = *s; return x; }
    if (!strcmp(t, "-size")) { x = nn(N_SIZE); num(take(), &x->op, &x->v, &x->unit, 1); return x; }
    if (!strcmp(t, "-mtime") || !strcmp(t, "-mmin")) { x = nn(N_MTIME); num(take(), &x->op, &x->v, &x->unit, 0); x->unit = t[2] == 't' ? 86400 : 60; return x; }
    if (!strcmp(t, "-newer")) { x = nn(N_NEWER); struct stat st; const char *f = take(); if (stat(f, &st)) { perror(f); exit(1); }
        x->v = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec; return x; }
    if (!strcmp(t, "-uid") || !strcmp(t, "-gid") || !strcmp(t, "-links") || !strcmp(t, "-inum")) {
        x = nn(N_COL); x->col = t[1] == 'u' ? 0 : t[1] == 'g' ? 1 : t[1] == 'l' ? 2 : 3; num(take(), &x->op, &x->v, &x->unit, 0); return x; }
    if (!strcmp(t, "-user") || !strcmp(t, "-group")) {
        x = nn(N_COL); x->col = t[1] == 'u' ? 0 : 1; x->op = '='; const char *v = take();
        if (isdigit((unsigned char)v[0])) x->v = atoll(v);
        else if (x->col == 0) { struct passwd *pw = getpwnam(v); if (!pw) die("find: unknown user"); x->v = pw->pw_uid; }
        else { struct group *gr = getgrnam(v); if (!gr) die("find: unknown group"); x->v = gr->gr_gid; }
        return x; }
    if (!strcmp(t, "-nouser") || !strcmp(t, "-nogroup")) { x = nn(N_NOOWNER); x->col = t[3] == 'u' ? 0 : 1; return x; }
    if (!strcmp(t, "-perm")) { x = nn(N_PERM); const char *v = take(); x->perm_kind = (v[0] == '-' || v[0] == '/') ? v[0] : '=';
        x->v = strtoll(v + (x->perm_kind != '='), NULL, 8); return x; }
    if (!strcmp(t, "-empty")) return nn(N_EMPTY);
    if (!strcmp(t, "-true")) return nn(N_TRUE);
    if (!strcmp(t, "-false")) return nn(N_FALSE);
    if (!strcmp(t, "-print") || !strcmp(t, "-print0")) { has_action = 1; if (t[6]) print0 = 1; return nn(N_PRINT); }
    if (!strcmp(t, "-prune")) return nn(N_PRUNE);
    if (!strcmp(t, "-maxdepth")) { maxdepth = atoi(take()); return nn(N_TRUE); }
    if (!strcmp(t, "-mindepth")) { mindepth = atoi(take()); return nn(N_TRUE); }
    fprintf(stderr, "tfs find: unsupported primary %s (side-effecting actions are not part of the recomposed program)\n", t); exit(2);
}
static int is_sep(const char *t) { return !strcmp(t, ")") || !strcmp(t, "-o") || !strcmp(t, "-or") || !strcmp(t, ","); }
static node_t *term(void) {
    node_t *e = primary();
    while (ti < ntok && !is_sep(tok[ti])) {
        if (!strcmp(tok[ti], "-a") || !strcmp(tok[ti], "-and")) ti++;
        node_t *x = nn(N_AND); x->a = e; x->b = primary(); e = x;
    }
    return e;
}
static node_t *expr(void) {
    node_t *e = term();
    while (ti < ntok && (!strcmp(tok[ti], "-o") || !strcmp(tok[ti], "-or") || !strcmp(tok[ti], ","))) {
        node_t *x = nn(!strcmp(tok[ti], ",") ? N_COMMA : N_OR); ti++; x->a = e; x->b = term(); e = x;
    }
    return e;
}

/* per-distinct-name precomputation of -name / -iname (fnmatch once per name, not per row) */
static void prepare(node_t *x) {
    if (!x) return;
    prepare(x->a); prepare(x->b);
    if (x->k == N_NAME) {
        x->nmask = malloc(H.nnames); const char *p = x->pat; size_t pl = strlen(p);
        int suffix = !x->icase && p[0] == '*' && pl > 1 && !strpbrk(p + 1, "*?[]\\");      /* '*SUFFIX': memcmp, as fnmatch decides it */
        int prefix = !x->icase && pl > 1 && p[pl - 1] == '*' && !strpbrk(p, "?[]\\") && !memchr(p, '*', pl - 1);
        int exact = !x->icase && !strpbrk(p, "*?[]\\");
        for (uint64_t i = 0; i < H.nnames; i++) {
            const char *n = NSTR(i); size_t l = NLEN(i);
            x->nmask[i] = suffix ? (l >= pl - 1 && !memcmp(n + l - (pl - 1), p + 1, pl - 1))
                        : prefix ? !strncmp(n, p, pl - 1)
                        : exact ? (l == pl && !memcmp(n, p, pl))
                        : fnmatch(p, n, x->icase ? FNM_CASEFOLD : 0) == 0;
        }
    }
    if (x->k == N_PATH) {
        const char *p = x->pat;
        if (!x->icase && !strpbrk(p, "*?[]\\")) x->path_kind = 1;                       /* a plain path: equality */
        else if (p[0] == '*' && p[1] && !strpbrk(p + 1, "*?[]\\/")) {                   /* '*SUFFIX': the name's suffix */
            x->path_kind = 2; x->nmask = malloc(H.nnames); size_t sl = strlen(p + 1);
            for (uint64_t i = 0; i < H.nnames; i++)
                x->nmask[i] = NLEN(i) >= sl && (x->icase ? !strcasecmp(NSTR(i) + NLEN(i) - sl, p + 1) : !memcmp(NSTR(i) + NLEN(i) - sl, p + 1, sl));
        } else x->path_kind = 3;                                                         /* general: fnmatch on the path */
    }
}

static uint64_t root_dev; static const char *cur_root;
static int dir_empty(const char *path, int64_t dev) {
    if (dev != (int64_t)root_dev) return 0;                 /* a mount point: find sees the mounted root, never empty here */
    int64_t d = find_str(doff, dblob, H.ndirs, path);
    if (d >= 0) for (uint64_t r = dstart[d]; r < dstart[d + 1]; r++) if (!dead(r)) return 0;
    char dir[8192], name[4096]; load_delta();
    for (uint64_t i = 0; i < ND; i++) { split_path(DX[i].path, dir, name); if (!strcmp(dir, path)) return 0; }
    return 1;
}
static int owner_known(int col, int64_t id) {
    static int64_t cache_id[2][64]; static int cache_v[2][64], nc[2];
    for (int i = 0; i < nc[col]; i++) if (cache_id[col][i] == id) return cache_v[col][i];
    int v = col == 0 ? getpwuid((uid_t)id) != NULL : getgrgid((gid_t)id) != NULL;
    if (nc[col] < 64) { cache_id[col][nc[col]] = id; cache_v[col][nc[col]++] = v; }
    return v;
}
static int cmp_op(char op, int64_t v, int64_t n) { return op == '+' ? v > n : op == '-' ? v < n : v == n; }

/* the path of the entry being evaluated, built on first use */
static char pbuf[16384]; static const char *cur_path; static const ent_t *cur_e;
static const char *get_path(void) { if (!cur_path) cur_path = row_path(cur_e->row, pbuf); return cur_path; }
#define path get_path()
/* evaluate with find's short-circuit semantics: returns the value; sets *printed / *pruned when reached */
static int eval(node_t *x, const ent_t *e, const char *unused_path, int *printed, int *pruned) {
    switch (x->k) {
    case N_AND: return eval(x->a, e, 0, printed, pruned) && eval(x->b, e, 0, printed, pruned);
    case N_OR: return eval(x->a, e, 0, printed, pruned) || eval(x->b, e, 0, printed, pruned);
    case N_NOT: return !eval(x->a, e, 0, printed, pruned);
    case N_COMMA: eval(x->a, e, 0, printed, pruned); return eval(x->b, e, 0, printed, pruned);
    case N_PRINT: *printed = 1; return 1;
    case N_PRUNE: *pruned = 1; return 1;
    case N_TRUE: return 1;
    case N_FALSE: return 0;
    case N_NAME: {
        if (!e->d) return x->nmask[ncode(e->row)];
        const char *b = strrchr(path, '/'); return fnmatch(x->pat, b ? b + 1 : path, x->icase ? FNM_CASEFOLD : 0) == 0; }
    case N_PATH:
        if (x->path_kind == 1) return !strcmp(path, x->pat);
        if (x->path_kind == 2 && !e->d) return x->nmask[ncode(e->row)];
        return fnmatch(x->pat, path, x->icase ? FNM_CASEFOLD : 0) == 0;
    case N_REGEX: { int l = (int)strlen(path); return re_match(x->re, path, l, 0, NULL) == l; }
    case N_TYPE: return strchr(x->types, e_type(e)) != NULL;
    case N_SIZE: { int64_t s = e_size(e); return cmp_op(x->op, (s + x->unit - 1) / x->unit, x->v); }
    case N_MTIME: { double age = now_s - (double)e_mtime(e) / 1e9; return cmp_op(x->op, (int64_t)(age / x->unit >= 0 ? age / x->unit : age / x->unit - 1), x->v); }
    case N_NEWER: return e_mtime(e) > x->v;
    case N_COL: { int64_t v = x->col == 0 ? e_uid(e) : x->col == 1 ? e_gid(e) : x->col == 2 ? e_nlink(e) : e_inode(e); return cmp_op(x->op, v, x->v); }
    case N_NOOWNER: return !owner_known(x->col, x->col == 0 ? e_uid(e) : e_gid(e));
    case N_PERM: { int64_t m = e_mode(e) & 07777;
        return x->perm_kind == '=' ? m == x->v : x->perm_kind == '-' ? (m & x->v) == x->v : (x->v == 0 || (m & x->v) != 0); }
    case N_EMPTY: { char t = e_type(e); if (t == 'f') return e_size(e) == 0; if (t == 'd') return dir_empty(path, e_dev(e)); return 0; }
    }
    return 0;
}
#undef path
static int32_t *ddepth; static int root_depth;
static int slashes(const char *p) { int d = 0; for (; *p; p++) d += *p == '/'; return d; }
static int row_depth(uint64_t r) {
    if (!ddepth) { ddepth = malloc(H.ndirs * sizeof *ddepth); for (uint64_t i = 0; i < H.ndirs; i++) ddepth[i] = DLEN(i) == 1 ? 0 : slashes(DSTR(i)); }
    return ddepth[dcode(r)] + 1 - root_depth;
}
static int depth_of(const char *path, const char *root) {
    if (!strcmp(path, root)) return 0;
    size_t l = strlen(root); const char *p = path + (l == 1 ? 1 : l + 1); int d = 1;
    for (; *p; p++) if (*p == '/') d++;
    return d;
}

/* pruned directories: mark their directory codes; delta entries are checked by path prefix */
static uint8_t *pruned_codes; static svec_t pruned_paths;
static void prune_dir(const char *path) {
    sv_push(&pruned_paths, path);
    int64_t d = find_str(doff, dblob, H.ndirs, path); if (d >= 0) pruned_codes[d] = 1;
    char pre[8192]; size_t l = strlen(path); if (l == 1) l = 0; memcpy(pre, path, l); pre[l] = '/';
    uint64_t lo = lower(doff, dblob, H.ndirs, pre, l + 1); pre[l] = '0'; uint64_t hi = lower(doff, dblob, H.ndirs, pre, l + 1);
    memset(pruned_codes + lo, 1, hi > lo ? hi - lo : 0);
}
static int is_pruned_path(const char *p) {
    for (size_t i = 0; i < pruned_paths.n; i++) if (strcmp(p, pruned_paths.v[i]) && under(p, pruned_paths.v[i])) return 1;
    return 0;
}

typedef struct { int mode; const char *key; } out_t;     /* mode: 0 paths, 1 count, 2 bytes, 3 newest, 4 group */
static int64_t g_count, g_bytes, g_best_mt = INT64_MIN; static char g_best[8192]; static svec_t g_out;
typedef struct { char *ext; int64_t c, b; } grp_t; static grp_t *G; static size_t NG, CG;
static const char *ext_of(const char *name) {               /* as os.path.splitext: leading dots are not an extension */
    const char *dot = strrchr(name, '.'); if (!dot) return "";
    for (const char *p = name; p < dot; p++) if (*p != '.') return dot;
    return "";
}
static int32_t *extcode; static char **extstr; static int64_t *extc, *extb; static size_t next_;
static void ext_prepare(void) {
    extcode = malloc(H.nnames * sizeof *extcode); size_t cap = 1024; extstr = malloc(cap * sizeof *extstr);
    size_t hcap = 1 << 16; int32_t *ht = malloc(hcap * sizeof *ht); memset(ht, 0xff, hcap * sizeof *ht);
    for (uint64_t i = 0; i < H.nnames; i++) {
        const char *x = ext_of(NSTR(i)); uint64_t h = 1469598103934665603ull; for (const char *p = x; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
        size_t s = h & (hcap - 1);
        while (ht[s] >= 0 && strcmp(extstr[ht[s]], x)) s = (s + 1) & (hcap - 1);
        if (ht[s] < 0) { if (next_ == cap) { cap *= 2; extstr = realloc(extstr, cap * sizeof *extstr); } extstr[next_] = (char *)x; ht[s] = (int32_t)next_++;
                         if (next_ * 2 > hcap) die("too many distinct extensions"); }
        extcode[i] = ht[s];
    }
    extc = calloc(next_, sizeof *extc); extb = calloc(next_, sizeof *extb);
}
static void emit(const ent_t *e, const char *path, out_t *o) {
    if (o->mode == 4 && !e->d) { int32_t c = extcode[ncode(e->row)]; extc[c]++; extb[c] += e_size(e); return; }
    if (o->mode == 1) g_count++;
    else if (o->mode == 2) g_bytes += e_size(e);
    else if (o->mode == 3) { if (e_mtime(e) > g_best_mt) { g_best_mt = e_mtime(e); strcpy(g_best, path); } }
    else if (o->mode == 4) {                                   /* a delta entry: through its own extension string */
        const char *b = strrchr(path, '/'); const char *x = ext_of(b ? b + 1 : path); size_t i;
        for (i = 0; i < next_; i++) if (!strcmp(extstr[i], x)) break;
        if (i == next_) { extstr = realloc(extstr, (next_ + 1) * sizeof *extstr); extc = realloc(extc, (next_ + 1) * sizeof *extc);
                          extb = realloc(extb, (next_ + 1) * sizeof *extb); extstr[i] = strdup(x); extc[i] = extb[i] = 0; next_++; }
        extc[i]++; extb[i] += e_size(e);
    }
    else if (o->mode == 4) {
        const char *b = strrchr(path, '/'); const char *x = ext_of(b ? b + 1 : path);
        size_t i; for (i = 0; i < NG; i++) if (!strcmp(G[i].ext, x)) break;
        if (i == NG) { if (NG == CG) { CG = CG ? 2 * CG : 256; G = realloc(G, CG * sizeof *G); } G[NG++] = (grp_t){ strdup(x), 0, 0 }; }
        G[i].c++; G[i].b += e_size(e);
    } else sv_push(&g_out, path);
}
static int cmp_grp(const void *a, const void *b) {
    const grp_t *x = a, *y = b; if (x->c != y->c) return x->c > y->c ? -1 : 1; return cmp_cstr(&x->ext, &y->ext);
}

static void visit(const ent_t *e, const char *given_path, node_t *ast, out_t *o) {
    cur_e = e; cur_path = given_path;
    if (maxdepth >= 0 || mindepth >= 0) {
        int d = given_path ? depth_of(given_path, cur_root) : row_depth((uint64_t)e->row);
        if (maxdepth >= 0 && d > maxdepth) return;
        if (mindepth >= 0 && d < mindepth) return;          /* visited, not evaluated */
    }
    int printed = 0, pruned = 0, v = eval(ast, e, 0, &printed, &pruned);
    if (pruned && e_type(e) == 'd') prune_dir(get_path());
    if (has_action ? printed : v) emit(e, (o->mode == 0 || o->mode == 3 || (o->mode == 4 && e->d)) ? get_path() : "", o);
}

static int cmd_find(int argc, char **argv) {
    if (argc < 1) die("find: ROOT required");
    char root[8192]; norm(argv[0], root); cur_root = root;
    out_t o = { 0, NULL }; char **ex = malloc(argc * sizeof *ex); int ne = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--count")) o.mode = 1; else if (!strcmp(argv[i], "--bytes")) o.mode = 2;
        else if (!strcmp(argv[i], "--newest")) o.mode = 3; else if (!strcmp(argv[i], "--group-by")) { o.mode = 4; o.key = argv[++i]; }
        else ex[ne++] = argv[i];
    }
    tok = ex; ntok = ne; ti = 0; node_t *ast = ne ? expr() : nn(N_TRUE);
    if (ti != ntok) die("find: unexpected token");
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); now_s = ts.tv_sec + ts.tv_nsec / 1e9;
    prepare(ast); load_delta(); pruned_codes = calloc(H.ndirs + 1, 1);
    if (o.mode == 4) ext_prepare();
    root_depth = strcmp(root, "/") ? slashes(root) : 0;
    ent_t start;
    if (lookup(root, &start)) { root_dev = (uint64_t)e_dev(&start); visit(&start, root, ast, &o); }
    else if (strcmp(root, "/")) { fprintf(stderr, "tfs find: %s: not in index\n", root); return 1; }
    span_t sp[2]; int k = subtree(root, sp);
    for (int j = 0; j < k; j++)
        for (uint64_t r = sp[j].a; r < sp[j].b; r++) {
            if (dead(r) || pruned_codes[dcode(r)]) continue;
            ent_t e = { (int64_t)r, NULL }; visit(&e, NULL, ast, &o);
        }
    for (uint64_t i = 0; i < ND; i++)
        if (strcmp(DX[i].path, root) && under(DX[i].path, root) && !is_pruned_path(DX[i].path)) {
            ent_t e = { -1, &DX[i] }; visit(&e, DX[i].path, ast, &o);
        }
    if (o.mode == 1) printf("%lld\n", (long long)g_count);
    else if (o.mode == 2) printf("%lld\n", (long long)g_bytes);
    else if (o.mode == 3) printf("%s\n", g_best_mt == INT64_MIN ? "" : g_best);
    else if (o.mode == 4) {
        for (size_t i = 0; i < next_; i++) if (extc[i]) { if (NG == CG) { CG = CG ? 2 * CG : 256; G = realloc(G, CG * sizeof *G); } G[NG++] = (grp_t){ extstr[i], extc[i], extb[i] }; }
        qsort(G, NG, sizeof *G, cmp_grp); for (size_t i = 0; i < NG; i++) printf("%9lld %15lld %s\n", (long long)G[i].c, (long long)G[i].b, G[i].ext); }
    else sv_print(&g_out, print0, (size_t)-1);
    return 0;
}

/* ------------------------------------------------------------------ locate */
typedef struct { int kind; const char *pat; uint8_t *nmask, *dmask, *dend, *nstart; struct re_pattern_buffer *re; } lpat_t;
static int has_meta(const char *s) { return strpbrk(s, "*?[]\\") != NULL; }
static char *lower_dup(const char *s) { char *t = strdup(s); for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p); return t; }
static int contains(const char *hay, const char *needle, int icase) { return (icase ? strcasestr(hay, needle) : strstr(hay, needle)) != NULL; }

static int cmd_locate(int argc, char **argv) {
    int bname = 0, icase = 0, rx = 0, all = 0, count = 0, nul = 0; long limit = -1; const char *roots[64]; int nr = 0;
    const char *pats[256]; int np = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-b") || !strcmp(a, "--basename")) bname = 1;
        else if (!strcmp(a, "-i") || !strcmp(a, "--ignore-case")) icase = 1;
        else if (!strcmp(a, "-r") || !strcmp(a, "--regex")) rx = 1;
        else if (!strcmp(a, "-A") || !strcmp(a, "--all")) all = 1;
        else if (!strcmp(a, "-c") || !strcmp(a, "--count")) count = 1;
        else if (!strcmp(a, "-0") || !strcmp(a, "--null")) nul = 1;
        else if (!strcmp(a, "-e") || !strcmp(a, "--existing")) ;          /* the index is live: nothing to re-stat */
        else if (!strcmp(a, "-l") || !strcmp(a, "--limit")) limit = atol(argv[++i]);
        else if (!strcmp(a, "--root")) roots[nr++] = argv[++i];
        else pats[np++] = a;
    }
    if (!np) die("locate: no pattern");
    load_strings(); load_delta();
    lpat_t *L = calloc(np, sizeof *L);
    for (int j = 0; j < np; j++) {                       /* compile each pattern into dictionary masks */
        lpat_t *p = &L[j]; p->pat = icase && !rx ? lower_dup(pats[j]) : pats[j];
        if (rx) { p->kind = 4; p->re = calloc(1, sizeof *p->re); re_set_syntax(RE_SYNTAX_EMACS | (icase ? RE_ICASE : 0));
                  const char *err = re_compile_pattern(p->pat, strlen(p->pat), p->re); if (err) die(err); continue; }
        if (bname) {
            p->kind = 1; p->nmask = malloc(H.nnames);
            for (uint64_t i = 0; i < H.nnames; i++)
                p->nmask[i] = has_meta(p->pat) ? fnmatch(p->pat, NSTR(i), icase ? FNM_CASEFOLD : 0) == 0 : contains(NSTR(i), p->pat, icase);
        } else if (!has_meta(p->pat)) {                  /* whole-path substring: dir OR name OR across the last '/' */
            p->kind = 2; p->dmask = malloc(H.ndirs); p->nmask = malloc(H.nnames);
            for (uint64_t i = 0; i < H.ndirs; i++) p->dmask[i] = contains(DSTR(i), p->pat, icase);
            const char *sl = strrchr(p->pat, '/');
            if (!sl) for (uint64_t i = 0; i < H.nnames; i++) p->nmask[i] = contains(NSTR(i), p->pat, icase);
            else {
                size_t hl = sl - p->pat; const char *tail = sl + 1; size_t tl = strlen(tail);
                p->dend = malloc(H.ndirs); p->nstart = malloc(H.nnames); memset(p->nmask, 0, H.nnames);
                for (uint64_t i = 0; i < H.ndirs; i++) {
                    const char *d = DSTR(i); size_t dl = DLEN(i); if (dl == 1 && d[0] == '/') dl = 0;
                    p->dend[i] = dl >= hl && (icase ? !strncasecmp(d + dl - hl, p->pat, hl) : !memcmp(d + dl - hl, p->pat, hl));
                }
                for (uint64_t i = 0; i < H.nnames; i++) p->nstart[i] = icase ? !strncasecmp(NSTR(i), tail, tl) : !strncmp(NSTR(i), tail, tl);
            }
        } else p->kind = 3;                              /* whole-path glob: fnmatch on the path */
    }
    svec_t out = {0}; int64_t n = 0; char buf[16384];
    const char *rlist[64]; int nl = 0;
    if (nr) for (int i = 0; i < nr; i++) { char *t = malloc(8192); norm(roots[i], t); rlist[nl++] = t; }
    else for (uint64_t i = 0; i < H.nroots; i++) rlist[nl++] = (const char *)(rblob + roff[i]);
    for (int q = 0; q < nl; q++) {
        const char *root = rlist[q]; span_t sp[3]; int k = subtree(root, sp);
        int64_t me = strcmp(root, "/") ? row_of(root) : -1; if (me >= 0) sp[k++] = (span_t){ (uint64_t)me, (uint64_t)me + 1 };
        for (int j = 0; j < k; j++)
            for (uint64_t r = sp[j].a; r < sp[j].b; r++) {
                if (dead(r)) continue;
                uint64_t d = dcode(r), m = ncode(r); int hit = all; const char *path = NULL;
                for (int t = 0; t < np; t++) {
                    lpat_t *p = &L[t]; int h;
                    if (p->kind == 1) h = p->nmask[m];
                    else if (p->kind == 2) h = p->dmask[d] || (p->dend ? (p->dend[d] && p->nstart[m]) : p->nmask[m]);
                    else {
                        if (!path) path = row_path(r, buf);
                        if (p->kind == 3) h = fnmatch(p->pat, path, icase ? FNM_CASEFOLD : 0) == 0;
                        else h = re_search(p->re, path, (int)strlen(path), 0, (int)strlen(path), NULL) >= 0;
                    }
                    if (all ? !h : h) { hit = !all; break; }
                }
                if (!hit) continue;
                if (count) n++; else sv_push(&out, path ? path : row_path(r, buf));
            }
        for (uint64_t i = 0; i < ND; i++) {
            if (!under(DX[i].path, root)) continue;
            const char *subj = DX[i].path; if (bname) { const char *b = strrchr(subj, '/'); subj = b ? b + 1 : subj; }
            int hit = all;
            for (int t = 0; t < np; t++) {
                const char *pt = L[t].pat; int h;
                if (rx) h = re_search(L[t].re, subj, (int)strlen(subj), 0, (int)strlen(subj), NULL) >= 0;
                else if (has_meta(pt)) h = fnmatch(pt, subj, icase ? FNM_CASEFOLD : 0) == 0;
                else h = contains(subj, pt, icase);
                if (all ? !h : h) { hit = !all; break; }
            }
            if (hit) { if (count) n++; else sv_push(&out, DX[i].path); }
        }
    }
    if (count) { printf("%lld\n", (long long)n); return n ? 0 : 1; }
    sv_print(&out, nul, limit < 0 ? (size_t)-1 : (size_t)limit);
    return out.n ? 0 : 1;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    const char *idx = getenv("TFS_INDEX"); int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-i")) { idx = argv[i + 1]; i += 2; }
    if (i < argc && !strcmp(argv[i], "build")) return cmd_build(argc - i - 1, argv + i + 1);
    if (!idx || i >= argc) {
        fprintf(stderr, "usage: tfs build -o INDEX [--cross-filesystems] ROOT...\n"
                        "       tfs -i INDEX {stat PATH | ls DIR | du PATH | find ROOT [EXPR] | locate [opts] PATTERN...}\n"
                        "       tfs -i INDEX {refresh | compact | watch [--save-every S] | btrfs {init SUBVOL SNAPDIR | catchup | mark}}\n"
                        "       tfs -i INDEX content [--no-refresh] [--index-binary] {refresh ROOT | wc [-s] PATH... | hash PATH...\n"
                        "                                                           | grep [-c] [--trace] PATTERN ROOT | compact | info}\n"
                        "       tfs -i INDEX dupes [-r] [-n] [-A] [-H] [-G BYTES] [-L BYTES] [-Q] [-S] [-m] [-1] [-f] [--store DIR] DIR...\n");
        return 2;
    }
    const char *cmd = argv[i++];
    if (!strcmp(cmd, "btrfs")) return cmd_btrfs(argc - i, argv + i, idx);
    open_index(idx);
    if (!strcmp(cmd, "stat") && i < argc) return cmd_stat(argv[i]);
    if (!strcmp(cmd, "ls") && i < argc) return cmd_ls(argv[i]);
    if (!strcmp(cmd, "du") && i < argc) return cmd_du(argv[i]);
    if (!strcmp(cmd, "find")) return cmd_find(argc - i, argv + i);
    if (!strcmp(cmd, "locate")) return cmd_locate(argc - i, argv + i);
    if (!strcmp(cmd, "refresh")) return cmd_refresh(argc - i, argv + i);
    if (!strcmp(cmd, "compact")) return cmd_compact(argc - i, argv + i);
    if (!strcmp(cmd, "dump")) return cmd_dump(argc - i, argv + i);
    if (!strcmp(cmd, "watch")) return cmd_watch(argc - i, argv + i);
    if (!strcmp(cmd, "content")) return cmd_content(argc - i, argv + i, idx);
    if (!strcmp(cmd, "dupes")) return cmd_dupes(argc - i, argv + i);
    fprintf(stderr, "tfs: unknown command %s\n", cmd); return 2;
}
