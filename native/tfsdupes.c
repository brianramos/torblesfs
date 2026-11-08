/* tfs dupes: duplicate files, fdupes-compatible options and output, answered from the index.
 *
 * Each stage consumes only what the previous one kept:
 *   0 candidates   regular files below the given directories, from the index (-r, -n, -A, -G, -L); hard links
 *                  collapsed by (device, inode) unless -H                                            [no file access]
 *   1 size groups  sizes held by at least two files: a file with a unique size is never opened       [no file access]
 *   2 partial      SHA-1 of the first 4096 bytes, grouped by (size, digest)
 *   3 full         SHA-1 of the whole file, only for partial collisions larger than 4096 bytes
 *   4 confirm      byte comparison against the set's first file (skipped with -Q)
 *   5 order        within a set by mtime, then path; sets by their first path
 * With --store DIR, digests are cached by identity (device, inode, size, mtime): a rerun hashes only changed files.
 *
 *   tfs -i INDEX dupes [-r] [-n] [-A] [-H] [-G BYTES] [-L BYTES] [-Q] [-S] [-m] [-1] [-f] [--store DIR] [--trace] DIR... */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include "tfscore.h"
#include "tfsadmin.h"
#include "tfsext.h"

#define PARTIAL 4096
#define CHUNK 65536
typedef struct { int64_t dev, ino, size, mtime; uint8_t has_p, has_f, p[20], f[20]; } crec_t;   /* the cache record */
static crec_t *C; static uint64_t NC, CCAP, CHCAP; static int64_t *CH;
static uint64_t opened, nread;

static uint64_t hkey(int64_t a, int64_t b, int64_t c, int64_t d) {
    uint64_t h = 1469598103934665603ull; int64_t v[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) { h ^= (uint64_t)v[i]; h *= 1099511628211ull; h ^= h >> 29; }
    return h;
}
static void cache_index(void) {
    CHCAP = 1024; while (CHCAP < 4 * (NC + 1)) CHCAP *= 2;
    free(CH); CH = malloc(CHCAP * 8); memset(CH, 0xff, CHCAP * 8);
    for (uint64_t i = 0; i < NC; i++) {
        uint64_t s = hkey(C[i].dev, C[i].ino, C[i].size, C[i].mtime) & (CHCAP - 1);
        while (CH[s] >= 0) s = (s + 1) & (CHCAP - 1);
        CH[s] = (int64_t)i;
    }
}
static crec_t *cache_get(const fent_t *f) {
    for (uint64_t s = hkey(f->dev, f->ino, f->size, f->mtime) & (CHCAP - 1); CH[s] >= 0; s = (s + 1) & (CHCAP - 1)) {
        crec_t *c = &C[CH[s]];
        if (c->dev == f->dev && c->ino == f->ino && c->size == f->size && c->mtime == f->mtime) return c;
    }
    if (NC == CCAP) { CCAP = CCAP ? 2 * CCAP : 4096; C = realloc(C, CCAP * sizeof *C); }
    C[NC] = (crec_t){ f->dev, f->ino, f->size, f->mtime, 0, 0, {0}, {0} }; NC++;
    if (2 * NC > CHCAP) cache_index();
    else { uint64_t s = hkey(f->dev, f->ino, f->size, f->mtime) & (CHCAP - 1); while (CH[s] >= 0) s = (s + 1) & (CHCAP - 1); CH[s] = (int64_t)(NC - 1); }
    return &C[NC - 1];
}
static void cache_load(const char *dir) {
    char p[8300]; snprintf(p, sizeof p, "%s/.tfs-dupes-cache", dir);
    FILE *f = fopen(p, "rb"); uint64_t n = 0; char m[8];
    if (f && fread(m, 1, 8, f) == 8 && !memcmp(m, "TFSDUPC1", 8) && fread(&n, 8, 1, f) == 1) {
        C = malloc((n ? n : 1) * sizeof *C); CCAP = n ? n : 1;
        NC = fread(C, sizeof *C, n, f);
    }
    if (f) fclose(f);
    cache_index();
}
static void cache_save(const char *dir) {
    mkdir(dir, 0755);
    char p[8300], t[8310]; snprintf(p, sizeof p, "%s/.tfs-dupes-cache", dir); snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "wb"); if (!f) { perror(t); return; }
    int ok = fwrite("TFSDUPC1", 1, 8, f) == 8 && fwrite(&NC, 8, 1, f) == 1 && fwrite(C, sizeof *C, NC, f) == NC;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(t, p)) { perror("tfs dupes: cache"); unlink(t); }
}

static int digest(const char *path, int64_t limit, uint8_t out[20]) {      /* SHA-1 of the first `limit` bytes (-1: all) */
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    static uint8_t buf[CHUNK]; sha1_t c; sha1_init(&c); opened++;
    for (int64_t left = limit; limit < 0 || left > 0;) {
        size_t want = limit < 0 || left > CHUNK ? CHUNK : (size_t)left;
        ssize_t r = read(fd, buf, want); if (r < 0) { close(fd); return -1; } if (r == 0) break;
        sha1_update(&c, buf, (size_t)r); nread += (uint64_t)r; if (limit >= 0) left -= r;
    }
    close(fd); sha1_final(&c, out); return 0;
}
static int same(const char *a, const char *b) {                             /* byte comparison; -1 on error */
    int fa = open(a, O_RDONLY | O_CLOEXEC), fb = open(b, O_RDONLY | O_CLOEXEC); opened += 2;
    if (fa < 0 || fb < 0) { if (fa >= 0) close(fa); if (fb >= 0) close(fb); return -1; }
    static uint8_t x[CHUNK], y[CHUNK]; int res = 1;
    for (;;) {
        ssize_t r = read(fa, x, CHUNK), s = r > 0 ? read(fb, y, (size_t)r) : read(fb, y, 1);
        if (r < 0 || s < 0) { res = -1; break; }
        nread += (uint64_t)(r + s);
        if (r != s || memcmp(x, y, (size_t)r)) { res = 0; break; }
        if (r == 0) break;
    }
    close(fa); close(fb); return res;
}

static fent_t *F; static size_t NF;
static uint8_t (*PD)[20], (*FD)[20]; static uint8_t *ok;
static int by_path(const void *a, const void *b) { return strcmp(F[*(const size_t *)a].path, F[*(const size_t *)b].path); }
static int by_size(const void *a, const void *b) { int64_t x = F[*(const size_t *)a].size, y = F[*(const size_t *)b].size; return x < y ? -1 : x > y; }
static int by_part(const void *a, const void *b) {
    size_t i = *(const size_t *)a, j = *(const size_t *)b;
    if (F[i].size != F[j].size) return F[i].size < F[j].size ? -1 : 1;
    int c = memcmp(PD[i], PD[j], 20); return c ? c : (i > j) - (i < j);
}
static int by_full(const void *a, const void *b) {
    size_t i = *(const size_t *)a, j = *(const size_t *)b;
    if (F[i].size != F[j].size) return F[i].size < F[j].size ? -1 : 1;
    int c = memcmp(FD[i], FD[j], 20); return c ? c : (i > j) - (i < j);
}
static int by_mtime(const void *a, const void *b) {
    size_t i = *(const size_t *)a, j = *(const size_t *)b;
    if (F[i].mtime != F[j].mtime) return F[i].mtime < F[j].mtime ? -1 : 1;
    return strcmp(F[i].path, F[j].path);
}
typedef struct { size_t *m; size_t n; } set_t;
static int by_first(const void *a, const void *b) { return strcmp(F[((const set_t *)a)->m[0]].path, F[((const set_t *)b)->m[0]].path); }
static int hidden_below(const char *path, const char *root) {               /* a name or a directory below root starts with '.' */
    const char *p = path + strlen(root);
    for (; *p; p++) if (*p == '/' && p[1] == '.') return 1;
    return 0;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int cmd_dupes(int argc, char **argv) {
    int recurse = 0, noempty = 0, nohidden = 0, hardlinks = 0, quick = 0, showsize = 0, summarize = 0, sameline = 0, omitfirst = 0, trace = 0;
    int64_t minsize = -1, maxsize = -1; const char *store = NULL; char **dirs = malloc((argc + 1) * sizeof *dirs); int nd = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--store") && i + 1 < argc) store = argv[++i];
        else if (!strcmp(a, "--trace")) trace = 1;
        else if ((!strcmp(a, "-G") || !strcmp(a, "--minsize")) && i + 1 < argc) minsize = atoll(argv[++i]);
        else if ((!strcmp(a, "-L") || !strcmp(a, "--maxsize")) && i + 1 < argc) maxsize = atoll(argv[++i]);
        else if (!strcmp(a, "--recurse")) recurse = 1; else if (!strcmp(a, "--noempty")) noempty = 1;
        else if (!strcmp(a, "--nohidden")) nohidden = 1; else if (!strcmp(a, "--hardlinks")) hardlinks = 1;
        else if (!strcmp(a, "--quick")) quick = 1; else if (!strcmp(a, "--size")) showsize = 1;
        else if (!strcmp(a, "--summarize")) summarize = 1; else if (!strcmp(a, "--sameline")) sameline = 1;
        else if (!strcmp(a, "--omitfirst")) omitfirst = 1;
        else if (a[0] == '-' && a[1] && a[1] != '-') {
            for (const char *c = a + 1; *c; c++)
                switch (*c) {
                case 'r': recurse = 1; break; case 'n': noempty = 1; break; case 'A': nohidden = 1; break;
                case 'H': hardlinks = 1; break; case 'Q': quick = 1; break; case 'S': showsize = 1; break;
                case 'm': summarize = 1; break; case '1': sameline = 1; break; case 'f': omitfirst = 1; break;
                default: fprintf(stderr, "tfs dupes: unknown option -%c\n", *c); return 2;
                }
        } else { char *d = malloc(8192); abspath(a, d); dirs[nd++] = d; }
    }
    if (!nd) { fprintf(stderr, "usage: tfs -i INDEX dupes [-r] [-n] [-A] [-H] [-G BYTES] [-L BYTES] [-Q] [-S] [-m] [-1] [-f] [--store DIR] [--trace] DIR...\n"); return 2; }
    if (store) cache_load(store); else cache_index();
    double t0 = now(), t = t0;
    /* stage 0: candidates */
    size_t cap = 0;
    for (int j = 0; j < nd; j++) {
        fent_t *v; size_t n = ix_files(dirs[j], recurse, &v);
        for (size_t i = 0; i < n; i++) {
            fent_t *e = &v[i];
            if ((noempty && e->size == 0) || (minsize >= 0 && e->size < minsize) || (maxsize >= 0 && e->size > maxsize)
                || (nohidden && hidden_below(e->path, dirs[j]))) { free(e->path); continue; }
            if (NF == cap) { cap = cap ? 2 * cap : 4096; F = realloc(F, cap * sizeof *F); }
            F[NF++] = *e;
        }
        free(v);
    }
    size_t *ix = malloc((NF ? NF : 1) * sizeof *ix), n0 = NF, m = 0;
    for (size_t i = 0; i < NF; i++) ix[i] = i;
    qsort(ix, NF, sizeof *ix, by_path);                                   /* one entry per path (overlapping dirs) */
    uint8_t *keep = calloc(NF ? NF : 1, 1);
    for (size_t i = 0; i < NF; i++) if (i == 0 || strcmp(F[ix[i]].path, F[ix[i - 1]].path)) keep[ix[i]] = 1;
    if (!hardlinks) {                                                     /* one name per inode: the smallest path */
        uint64_t hc = 1024; while (hc < 4 * (NF + 1)) hc *= 2;
        int64_t *h = malloc(hc * 8); memset(h, 0xff, hc * 8);
        for (size_t q = 0; q < NF; q++) {
            size_t i = ix[q]; if (!keep[i]) continue;
            uint64_t s = hkey(F[i].dev, F[i].ino, 0, 0) & (hc - 1); int dup = 0;
            while (h[s] >= 0) { if (F[h[s]].dev == F[i].dev && F[h[s]].ino == F[i].ino) { dup = 1; break; } s = (s + 1) & (hc - 1); }
            if (dup) keep[i] = 0; else h[s] = (int64_t)i;
        }
        free(h);
    }
    for (size_t i = 0; i < NF; i++) if (keep[i]) ix[m++] = i;
    if (trace) fprintf(stderr, "{'stage': 'candidates', 'files': %zu, 'after_hardlinks': %zu, 'files_opened': 0, 'bytes_read': 0, 'seconds': %.4f}\n", n0, m, now() - t);
    /* stage 1: sizes held by at least two files */
    t = now(); qsort(ix, m, sizeof *ix, by_size);
    size_t *c1 = malloc((m ? m : 1) * sizeof *c1), n1 = 0, nsizes = 0;
    for (size_t a = 0, b; a < m; a = b) {
        for (b = a + 1; b < m && F[ix[b]].size == F[ix[a]].size; b++) {}
        if (b - a >= 2) { nsizes++; for (size_t q = a; q < b; q++) c1[n1++] = ix[q]; }
    }
    if (trace) fprintf(stderr, "{'stage': 'size groups', 'files': %zu, 'sizes': %zu, 'files_opened': 0, 'bytes_read': 0, 'never_opened': %zu, 'seconds': %.4f}\n", n1, nsizes, m - n1, now() - t);
    /* stage 2: partial digests */
    t = now(); opened = nread = 0;
    PD = malloc((NF ? NF : 1) * 20); FD = malloc((NF ? NF : 1) * 20); ok = calloc(NF ? NF : 1, 1);
    size_t *c2 = malloc((n1 ? n1 : 1) * sizeof *c2), n2 = 0;
    for (size_t q = 0; q < n1; q++) {
        size_t i = c1[q]; crec_t *c = cache_get(&F[i]);
        if (!c->has_p) { if (digest(F[i].path, PARTIAL, c->p)) continue; c->has_p = 1; if (F[i].size <= PARTIAL) { memcpy(c->f, c->p, 20); c->has_f = 1; } }
        memcpy(PD[i], c->p, 20); c2[n2++] = i;
    }
    qsort(c2, n2, sizeof *c2, by_part);
    size_t *c3 = malloc((n2 ? n2 : 1) * sizeof *c3), n3 = 0;
    for (size_t a = 0, b; a < n2; a = b) {
        for (b = a + 1; b < n2 && F[c2[b]].size == F[c2[a]].size && !memcmp(PD[c2[b]], PD[c2[a]], 20); b++) {}
        if (b - a >= 2) for (size_t q = a; q < b; q++) c3[n3++] = c2[q];
    }
    if (trace) fprintf(stderr, "{'stage': 'partial digest', 'files': %zu, 'files_opened': %llu, 'bytes_read': %llu, 'seconds': %.4f}\n", n3, (unsigned long long)opened, (unsigned long long)nread, now() - t);
    /* stage 3: full digests (files up to 4096 bytes were hashed whole in stage 2) */
    t = now(); opened = nread = 0; size_t small = 0, n4 = 0;
    for (size_t q = 0; q < n3; q++) {
        size_t i = c3[q]; crec_t *c = cache_get(&F[i]); small += F[i].size <= PARTIAL;
        if (!c->has_f) { if (digest(F[i].path, -1, c->f)) continue; c->has_f = 1; }
        memcpy(FD[i], c->f, 20); c3[n4++] = i;
    }
    qsort(c3, n4, sizeof *c3, by_full);
    set_t *sets = NULL; size_t ns = 0, scap = 0, nfull = 0;
    for (size_t a = 0, b; a < n4; a = b) {
        for (b = a + 1; b < n4 && F[c3[b]].size == F[c3[a]].size && !memcmp(FD[c3[b]], FD[c3[a]], 20); b++) {}
        if (b - a < 2) continue;
        nfull += b - a;
        if (ns == scap) { scap = scap ? 2 * scap : 256; sets = realloc(sets, scap * sizeof *sets); }
        sets[ns].m = malloc((b - a) * sizeof(size_t)); sets[ns].n = b - a; memcpy(sets[ns].m, c3 + a, (b - a) * sizeof(size_t));
        qsort(sets[ns].m, sets[ns].n, sizeof(size_t), by_mtime); ns++;
    }
    if (trace) fprintf(stderr, "{'stage': 'full digest', 'files': %zu, 'files_opened': %llu, 'bytes_read': %llu, 'skipped_small': %zu, 'seconds': %.4f}\n", nfull, (unsigned long long)opened, (unsigned long long)nread, small, now() - t);
    /* stage 4: confirm byte by byte against the set's first file; mismatches form new sets */
    t = now(); opened = nread = 0;
    set_t *out = NULL; size_t no = 0, ocap = 0;
    for (size_t s = 0; s < ns; s++) {
        size_t *rest = sets[s].m, nr = sets[s].n;
        while (nr >= 2) {
            size_t *yes = malloc(nr * sizeof *yes), *no_ = malloc(nr * sizeof *no_), ny = 0, nn = 0;
            yes[ny++] = rest[0];
            for (size_t q = 1; q < nr; q++) {
                int r = quick ? 1 : same(F[rest[0]].path, F[rest[q]].path);
                if (r == 1) yes[ny++] = rest[q]; else if (r == 0) no_[nn++] = rest[q];
            }
            if (ny >= 2) { if (no == ocap) { ocap = ocap ? 2 * ocap : 256; out = realloc(out, ocap * sizeof *out); } out[no++] = (set_t){ yes, ny }; }
            else free(yes);
            if (rest != sets[s].m) free(rest);
            rest = no_; nr = nn;
        }
        if (rest != sets[s].m) free(rest);
    }
    if (trace) fprintf(stderr, "{'stage': '%s', 'files_opened': %llu, 'bytes_read': %llu, 'seconds': %.4f}\n", quick ? "confirm (skipped, -Q)" : "confirm", (unsigned long long)opened, (unsigned long long)nread, now() - t);
    qsort(out, no, sizeof *out, by_first);
    if (summarize) {
        long long files = 0, bytes = 0; for (size_t s = 0; s < no; s++) { files += (long long)out[s].n - 1; bytes += F[out[s].m[0]].size * ((long long)out[s].n - 1); }
        if (bytes < 1000) printf("%lld duplicate files (in %zu sets), occupying %lld bytes.\n", files, no, bytes);
        else if (bytes <= 1000 * 1000) printf("%lld duplicate files (in %zu sets), occupying %.1f kilobytes\n", files, no, bytes / 1000.0);
        else printf("%lld duplicate files (in %zu sets), occupying %.1f megabytes\n", files, no, bytes / 1e6);
    } else {
        for (size_t s = 0; s < no; s++) {
            int64_t sz = F[out[s].m[0]].size;
            if (showsize) printf("%lld byte%s each:\n", (long long)sz, sz == 1 ? "" : "s");
            for (size_t q = omitfirst ? 1 : 0; q < out[s].n; q++) {
                const char *p = F[out[s].m[q]].path;
                if (sameline) { if (q > (size_t)(omitfirst ? 1 : 0)) putchar(' '); for (; *p; p++) { if (*p == ' ') putchar('\\'); putchar(*p); } }
                else { fputs(p, stdout); putchar('\n'); }
            }
            if (sameline) putchar('\n');
            putchar('\n');
        }
    }
    if (trace) fprintf(stderr, "{'stage': 'total', 'seconds': %.4f}\n", now() - t0);
    if (store) cache_save(store);
    return 0;
}
