/* tfs content: file contents as columns next to the index (INDEX.content).
 *
 * One row per regular file path, valid while the file's identity in the index (device, inode, size, mtime_ns) is
 * unchanged: GNU wc counts in the C locale (lines, words, bytes, maximum display width), the git blob id (SHA-1 of
 * "blob <len>\0" + content), and the file's distinct byte trigrams in a files x trigrams matrix stored
 * column-sparse (for each trigram, the sorted rows of the files that contain it). A fixed-string search ANDs the
 * columns of the pattern's trigrams into candidates and reads only those to confirm. Binary files (a NUL byte) stay
 * out of the matrix unless the store was created with --index-binary, and are then always candidates.
 * A refresh compares each path's identity with the index (metadata only) and reads just the new or changed files;
 * replaced rows are marked dead, new trigram entries go to a delta that compaction merges into the matrix.
 *
 *   tfs -i INDEX content [--no-refresh] [--index-binary] refresh ROOT | wc [-s] PATH... | hash PATH...
 *                                                       | grep [-c] [--trace] PATTERN ROOT | compact | info */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "tfscore.h"
#include "tfsadmin.h"
#include "tfsext.h"

#define MAGIC "TFSCONT1"
enum { F_DEAD = 1, F_UNINDEXED = 2 };
typedef struct {
    uint64_t n, cap; char **path; int64_t (*ident)[4], (*cols)[4]; uint8_t (*sha)[20]; uint8_t *fl;
    uint64_t nk, ne; uint32_t *keys, *rows; uint64_t *ptr;                 /* the matrix (CSR by trigram) */
    uint64_t nd, capd; uint32_t *dtri, *drow;                              /* delta: (trigram, row) */
    int index_binary, rows_mapped, delta_mapped, matrix_mapped;             /* arrays still pointing into the mapped file */
    int64_t *hmap; uint64_t hcap;                                          /* path -> row (live rows) */
} cstore_t;
static cstore_t S;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void *xrealloc(void *p, size_t n) { void *q = realloc(p, n ? n : 1); if (!q) { perror("tfs content"); exit(2); } return q; }

/* ------------------------------------------------------------------ scanning one file */
static void wc_counts(const uint8_t *b, size_t n, int64_t *out) {          /* GNU wc, C locale (wc.c single-byte loop) */
    int64_t lines = 0, words = 0, maxlen = 0, pos = 0; int in_word = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        switch (c) {
        case '\n': lines++; /* fallthrough */
        case '\r': case '\f': if (pos > maxlen) maxlen = pos; pos = 0; in_word = 0; break;
        case '\t': pos += 8 - (pos % 8); in_word = 0; break;
        case ' ': pos++; in_word = 0; break;
        case '\v': in_word = 0; break;
        default: {
            pos += (c >= 0x20 && c < 0x7f);
            int w2 = c != 0xA0;                                             /* gnulib: 0xA0 is U+00A0, a no-break space */
            words += (!in_word) & w2; in_word = w2;
        } }
    }
    if (pos > maxlen) maxlen = pos;
    out[0] = lines; out[1] = words; out[2] = (int64_t)n; out[3] = maxlen;
}
static uint64_t SEEN[1 << 18];                                              /* 2^24 bits */
static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static uint32_t *TB; static uint64_t TBCAP;
static uint64_t trigrams(const uint8_t *b, size_t n) {                     /* distinct trigrams of b, sorted, into TB */
    uint64_t k = 0;
    if (n < 3) return 0;
    uint32_t t = ((uint32_t)b[0] << 8) | b[1];
    for (size_t i = 2; i < n; i++) {
        t = ((t << 8) | b[i]) & 0xFFFFFF;
        uint64_t m = 1ull << (t & 63);
        if (!(SEEN[t >> 6] & m)) {
            SEEN[t >> 6] |= m;
            if (k == TBCAP) { TBCAP = TBCAP ? 2 * TBCAP : 65536; TB = xrealloc(TB, TBCAP * 4); }
            TB[k++] = t;
        }
    }
    for (uint64_t i = 0; i < k; i++) SEEN[TB[i] >> 6] = 0;
    qsort(TB, k, 4, cmp_u32);
    return k;
}
/* one read: counts, blob id, trigrams; returns -1 if unreadable, else the number of trigrams (into TB), with
 * *binary set when the file holds a NUL byte */
static int64_t scan_file(const char *path, int64_t *cols, uint8_t *sha, int *binary) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); if (fd < 0) return -1;
    struct stat st; if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    size_t n = (size_t)st.st_size; const uint8_t *b = (const uint8_t *)""; void *map = NULL;
    if (n) { map = mmap(NULL, n, PROT_READ, MAP_PRIVATE, fd, 0); if (map == MAP_FAILED) { close(fd); return -1; } b = map; madvise(map, n, MADV_SEQUENTIAL); }
    close(fd);
    wc_counts(b, n, cols);
    char hdr[32]; int hl = snprintf(hdr, sizeof hdr, "blob %zu", n) + 1;
    sha1_t c; sha1_init(&c); sha1_update(&c, hdr, (size_t)hl); sha1_update(&c, b, n); sha1_final(&c, sha);
    *binary = n && memchr(b, 0, n) != NULL;
    int64_t k = (*binary && !S.index_binary) ? 0 : (int64_t)trigrams(b, n);
    if (map) munmap(map, n);
    return k;
}

/* ------------------------------------------------------------------ the store: rows, path map, load / save */
static uint64_t hstr(const char *s) { uint64_t h = 1469598103934665603ull; for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull; return h; }
static void hmap_put(uint64_t r) {
    if (2 * (S.n + 1) > S.hcap) {                                           /* grow and rebuild from live rows */
        free(S.hmap); S.hcap = 1024; while (S.hcap < 4 * (S.n + 1)) S.hcap *= 2;
        S.hmap = malloc(S.hcap * 8); memset(S.hmap, 0xff, S.hcap * 8);
        for (uint64_t i = 0; i < S.n; i++) if (!(S.fl[i] & F_DEAD) && i != r) {
            uint64_t s = hstr(S.path[i]) & (S.hcap - 1); while (S.hmap[s] >= 0) s = (s + 1) & (S.hcap - 1); S.hmap[s] = (int64_t)i; }
    }
    uint64_t s = hstr(S.path[r]) & (S.hcap - 1); while (S.hmap[s] >= 0) s = (s + 1) & (S.hcap - 1); S.hmap[s] = (int64_t)r;
}
static int64_t hmap_get(const char *p) {
    if (!S.hcap) return -1;
    for (uint64_t s = hstr(p) & (S.hcap - 1); S.hmap[s] != -1; s = (s + 1) & (S.hcap - 1))
        if (S.hmap[s] >= 0 && !(S.fl[S.hmap[s]] & F_DEAD) && !strcmp(S.path[S.hmap[s]], p)) return S.hmap[s];
    return -1;
}
static void hmap_rebuild(void) {
    free(S.hmap); S.hmap = NULL; S.hcap = 0;
    for (uint64_t i = 0; i < S.n; i++) if (!(S.fl[i] & F_DEAD)) hmap_put(i);
}
static void *own(void *p, size_t used, size_t cap, int mapped) {          /* grow; a mapped array is copied out */
    if (!mapped) return xrealloc(p, cap);
    void *q = xrealloc(NULL, cap); if (used) memcpy(q, p, used); return q;
}
static uint64_t add_row(const char *path) {
    if (S.n == S.cap) {
        uint64_t c = S.cap ? 2 * S.cap : 4096; int m = S.rows_mapped;
        S.ident = own(S.ident, S.n * 32, c * 32, m); S.cols = own(S.cols, S.n * 32, c * 32, m);
        S.sha = own(S.sha, S.n * 20, c * 20, m); S.fl = own(S.fl, S.n, c, m);
        S.path = xrealloc(S.path, c * sizeof *S.path); S.cap = c; S.rows_mapped = 0;
    }
    uint64_t r = S.n++; S.path[r] = strdup(path); S.fl[r] = 0; return r;
}
static void add_delta(uint32_t t, uint32_t r) {
    if (S.nd == S.capd || S.delta_mapped) {
        uint64_t c = S.nd == S.capd ? (S.capd ? 2 * S.capd : 1 << 20) : S.capd;
        S.dtri = own(S.dtri, S.nd * 4, c * 4, S.delta_mapped); S.drow = own(S.drow, S.nd * 4, c * 4, S.delta_mapped);
        S.capd = c; S.delta_mapped = 0;
    }
    S.dtri[S.nd] = t; S.drow[S.nd++] = r;
}

/* file: MAGIC, u64 header[8] (flags, rows, path bytes, trigrams, matrix entries, delta entries), then 8-byte aligned
 * sections: path offsets u64[rows+1] | paths (NUL-terminated) | identity i64[rows][4] | wc i64[rows][4] |
 * blob id u8[rows][20] | flags u8[rows] | trigrams u32[nk] | starts u64[nk+1] | rows u32[ne] | delta trigram u32[nd] | delta row u32[nd].
 * It is mapped copy-on-write: queries touch only the sections and posting lists they need. */
#define AL(x) (((x) + 7) & ~(uint64_t)7)
static int load(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    struct stat st; if (fstat(fd, &st) || st.st_size < 72) { fprintf(stderr, "tfs content: %s: not a content store\n", path); exit(2); }
    uint8_t *b = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0); close(fd);
    if (b == MAP_FAILED || memcmp(b, MAGIC, 8)) { fprintf(stderr, "tfs content: %s: not a content store\n", path); exit(2); }
    uint64_t h[8]; memcpy(h, b + 8, sizeof h);
    S.index_binary = (int)(h[0] & 1); S.n = S.cap = h[1]; uint64_t pb = h[2]; S.nk = h[3]; S.ne = h[4]; S.nd = S.capd = h[5];
    uint64_t o = 72, end;
    const uint64_t *off = (const uint64_t *)(b + o); o = AL(o + (S.n + 1) * 8);
    const char *blob = (const char *)(b + o); o = AL(o + pb);
    S.ident = (void *)(b + o); o = AL(o + S.n * 32); S.cols = (void *)(b + o); o = AL(o + S.n * 32);
    S.sha = (void *)(b + o); o = AL(o + S.n * 20); S.fl = b + o; o = AL(o + S.n);
    S.keys = (uint32_t *)(b + o); o = AL(o + S.nk * 4); S.ptr = (uint64_t *)(b + o); o = AL(o + (S.nk + 1) * 8);
    S.rows = (uint32_t *)(b + o); o = AL(o + S.ne * 4); S.dtri = (uint32_t *)(b + o); o = AL(o + S.nd * 4);
    S.drow = (uint32_t *)(b + o); end = AL(o + S.nd * 4);
    if (end > (uint64_t)st.st_size) { fprintf(stderr, "tfs content: %s: truncated\n", path); exit(2); }
    S.path = malloc((S.n ? S.n : 1) * sizeof *S.path);
    for (uint64_t i = 0; i < S.n; i++) S.path[i] = (char *)blob + off[i];
    S.rows_mapped = S.delta_mapped = S.matrix_mapped = 1;
    hmap_rebuild(); return 1;
}
static int wr(FILE *f, const void *p, size_t n) {                         /* write, then pad to 8 bytes */
    static const uint8_t z[8] = { 0 };
    return (n == 0 || fwrite(p, 1, n, f) == n) && (AL(n) == n || fwrite(z, 1, AL(n) - n, f) == AL(n) - n);
}
static void save(const char *path) {
    char tmp[8320]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) { perror(tmp); exit(2); }
    uint64_t *off = malloc((S.n + 1) * 8); off[0] = 0;
    for (uint64_t i = 0; i < S.n; i++) off[i + 1] = off[i] + strlen(S.path[i]) + 1;
    uint64_t h[8] = { (uint64_t)S.index_binary, S.n, off[S.n], S.nk, S.ne, S.nd, 0, 0 };
    uint64_t zero = 0; const uint64_t *ptr = S.ptr ? S.ptr : &zero;
    int ok = fwrite(MAGIC, 1, 8, f) == 8 && fwrite(h, 1, sizeof h, f) == sizeof h && wr(f, off, (S.n + 1) * 8);
    for (uint64_t i = 0; ok && i < S.n; i++) ok = fwrite(S.path[i], 1, strlen(S.path[i]) + 1, f) == strlen(S.path[i]) + 1;
    static const uint8_t z[8] = { 0 }; if (ok && AL(off[S.n]) != off[S.n]) ok = fwrite(z, 1, AL(off[S.n]) - off[S.n], f) == AL(off[S.n]) - off[S.n];
    ok = ok && wr(f, S.ident, S.n * 32) && wr(f, S.cols, S.n * 32) && wr(f, S.sha, S.n * 20) && wr(f, S.fl, S.n)
            && wr(f, S.keys, S.nk * 4) && wr(f, ptr, (S.nk + 1) * 8) && wr(f, S.rows, S.ne * 4) && wr(f, S.dtri, S.nd * 4) && wr(f, S.drow, S.nd * 4);
    ok = (fflush(f) == 0) && ok && fsync(fileno(f)) == 0; ok = (fclose(f) == 0) && ok;
    free(off);
    if (!ok || rename(tmp, path)) { perror("tfs content: save"); unlink(tmp); exit(2); }
}

/* ------------------------------------------------------------------ compaction: drop dead rows, merge the delta */
static double compact(void) {
    double t0 = now();
    uint64_t *remap = malloc((S.n ? S.n : 1) * 8), m = 0;
    for (uint64_t i = 0; i < S.n; i++) remap[i] = (S.fl[i] & F_DEAD) ? UINT64_MAX : m++;
    uint32_t *cnt = calloc((1u << 24) + 1, 4);                              /* counting sort by trigram; stable, and every */
    for (uint64_t i = 0; i < S.nk; i++)                                    /* delta row is newer than every base row */
        for (uint64_t j = S.ptr[i]; j < S.ptr[i + 1]; j++) if (remap[S.rows[j]] != UINT64_MAX) cnt[S.keys[i] + 1]++;
    for (uint64_t j = 0; j < S.nd; j++) if (remap[S.drow[j]] != UINT64_MAX) cnt[S.dtri[j] + 1]++;
    uint64_t nk = 0, ne = 0;
    for (uint32_t t = 0; t < (1u << 24); t++) { nk += cnt[t + 1] != 0; ne += cnt[t + 1]; }
    if (ne >= UINT32_MAX) { fprintf(stderr, "tfs content: matrix too large\n"); exit(2); }
    uint32_t *keys = malloc((nk ? nk : 1) * 4), *rows = malloc((ne ? ne : 1) * 4); uint64_t *ptr = malloc((nk + 1) * 8), k = 0;
    for (uint32_t t = 0; t < (1u << 24); t++) cnt[t + 1] += cnt[t];       /* cnt[t]: first slot of trigram t */
    for (uint32_t t = 0; t < (1u << 24); t++) if (cnt[t + 1] > cnt[t]) { keys[k] = t; ptr[k++] = cnt[t]; }
    ptr[nk] = ne;
    for (uint64_t i = 0; i < S.nk; i++)
        for (uint64_t j = S.ptr[i]; j < S.ptr[i + 1]; j++) if (remap[S.rows[j]] != UINT64_MAX) rows[cnt[S.keys[i]]++] = (uint32_t)remap[S.rows[j]];
    for (uint64_t j = 0; j < S.nd; j++) if (remap[S.drow[j]] != UINT64_MAX) rows[cnt[S.dtri[j]]++] = (uint32_t)remap[S.drow[j]];
    free(cnt);
    if (!S.matrix_mapped) { free(S.keys); free(S.rows); free(S.ptr); }
    S.keys = keys; S.rows = rows; S.ptr = ptr; S.nk = nk; S.ne = ne; S.nd = 0; S.matrix_mapped = 0;
    for (uint64_t i = 0, w = 0; i < S.n; i++) {
        if (remap[i] == UINT64_MAX) continue;
        S.path[w] = S.path[i]; memcpy(S.ident[w], S.ident[i], 32); memcpy(S.cols[w], S.cols[i], 32);
        memcpy(S.sha[w], S.sha[i], 20); S.fl[w] = S.fl[i]; w++;
    }
    S.n = m; free(remap); hmap_rebuild();
    return now() - t0;
}

/* ------------------------------------------------------------------ refresh */
typedef struct { uint64_t files, read, bytes, dropped; double compare_s, scan_s; } rstat_t;
static rstat_t refresh(const char *root) {
    rstat_t st = { 0 }; double t0 = now();
    fent_t *F; size_t nf = ix_files(root, 1, &F); st.files = nf;
    uint64_t cap = 1024; while (cap < 4 * (nf + 1)) cap *= 2;              /* the index's files: path -> position */
    int64_t *fh = malloc(cap * 8); memset(fh, 0xff, cap * 8);
    for (size_t i = 0; i < nf; i++) { uint64_t s = hstr(F[i].path) & (cap - 1); while (fh[s] >= 0) s = (s + 1) & (cap - 1); fh[s] = (int64_t)i; }
    for (uint64_t r = 0; r < S.n; r++) {                                   /* rows gone or changed in the index */
        if ((S.fl[r] & F_DEAD) || !path_under(S.path[r], root)) continue;
        int64_t hit = -1;
        for (uint64_t s = hstr(S.path[r]) & (cap - 1); fh[s] >= 0; s = (s + 1) & (cap - 1)) if (!strcmp(S.path[r], F[fh[s]].path)) { hit = fh[s]; break; }
        if (hit < 0 || S.ident[r][0] != F[hit].dev || S.ident[r][1] != F[hit].ino || S.ident[r][2] != F[hit].size || S.ident[r][3] != F[hit].mtime) {
            S.fl[r] |= F_DEAD; st.dropped++;
        }
    }
    free(fh); if (st.dropped) hmap_rebuild();
    double t1 = now(); st.compare_s = t1 - t0;
    for (size_t i = 0; i < nf; i++) {
        if (hmap_get(F[i].path) >= 0) continue;
        int64_t cols[4]; uint8_t sha[20]; int bin = 0;
        int64_t k = scan_file(F[i].path, cols, sha, &bin);
        if (k < 0) continue;                                               /* unreadable: no row (answers skip it) */
        uint64_t r = add_row(F[i].path);
        S.ident[r][0] = F[i].dev; S.ident[r][1] = F[i].ino; S.ident[r][2] = F[i].size; S.ident[r][3] = F[i].mtime;
        memcpy(S.cols[r], cols, 32); memcpy(S.sha[r], sha, 20);
        if (bin && !S.index_binary) S.fl[r] |= F_UNINDEXED;
        for (int64_t j = 0; j < k; j++) add_delta(TB[j], (uint32_t)r);
        hmap_put(r); st.read++; st.bytes += (uint64_t)cols[2];
    }
    st.scan_s = now() - t1;
    fents_free(F, nf);
    uint64_t dead = 0; for (uint64_t r = 0; r < S.n; r++) dead += S.fl[r] & F_DEAD;
    if (S.nd > (S.ne / 4 > (1u << 20) ? S.ne / 4 : (1u << 20)) || dead > (S.n / 4 > 1024 ? S.n / 4 : 1024)) compact();
    return st;
}

/* ------------------------------------------------------------------ queries */
static int cmp_rowpath(const void *a, const void *b) { return strcmp(S.path[*(const uint64_t *)a], S.path[*(const uint64_t *)b]); }
static uint64_t rows_under(const char *root, uint64_t **out) {             /* live rows at or below root, in path order */
    uint64_t *v = malloc((S.n ? S.n : 1) * 8), k = 0;
    for (uint64_t r = 0; r < S.n; r++) if (!(S.fl[r] & F_DEAD) && path_under(S.path[r], root)) v[k++] = r;
    qsort(v, k, 8, cmp_rowpath); *out = v; return k;
}
static int has_tri(const uint32_t *t, uint64_t k, uint32_t x) {
    uint64_t lo = 0, hi = k; while (lo < hi) { uint64_t m = (lo + hi) / 2; if (t[m] < x) lo = m + 1; else hi = m; }
    return lo < k && t[lo] == x;
}
static int grep_rows(const char *pat, const char *root, int count, int trace) {
    size_t pl = strlen(pat); uint64_t *R, nr = rows_under(root, &R);
    uint64_t k = trigrams((const uint8_t *)pat, pl); uint32_t *pt = malloc((k ? k : 1) * 4); memcpy(pt, TB, k * 4);
    uint32_t *hits = NULL;
    if (k) {
        hits = calloc(S.n ? S.n : 1, 4);
        for (uint64_t j = 0; j < k; j++) {
            uint64_t lo = 0, hi = S.nk; while (lo < hi) { uint64_t m = (lo + hi) / 2; if (S.keys[m] < pt[j]) lo = m + 1; else hi = m; }
            if (lo < S.nk && S.keys[lo] == pt[j]) for (uint64_t e = S.ptr[lo]; e < S.ptr[lo + 1]; e++) hits[S.rows[e]]++;
        }
        for (uint64_t e = 0; e < S.nd; e++) if (has_tri(pt, k, S.dtri[e])) hits[S.drow[e]]++;
    }
    uint64_t ncand = 0, nhit = 0;
    for (uint64_t i = 0; i < nr; i++) {
        uint64_t r = R[i];
        if (k && hits[r] != k && !(S.fl[r] & F_UNINDEXED)) continue;
        ncand++;
        int fd = open(S.path[r], O_RDONLY | O_CLOEXEC); if (fd < 0) continue;
        struct stat st; if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); continue; }
        int found = 0;
        if (st.st_size >= (off_t)pl && st.st_size > 0) {
            void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (m != MAP_FAILED) { found = memmem(m, (size_t)st.st_size, pat, pl) != NULL; munmap(m, (size_t)st.st_size); }
        } else if (pl == 0) found = 1;
        close(fd);
        if (found) { nhit++; if (!count) { fputs(S.path[r], stdout); fputc('\n', stdout); } }
    }
    if (count) printf("%llu\n", (unsigned long long)nhit);
    if (trace) fprintf(stderr, "candidates read: %llu of %llu files\n", (unsigned long long)ncand, (unsigned long long)nr);
    free(R); free(pt); free(hits);
    return nhit ? 0 : 1;
}

int cmd_content(int argc, char **argv, const char *index_path) {
    int no_refresh = 0, index_binary = 0, i = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--no-refresh")) no_refresh = 1;
        else if (!strcmp(argv[i], "--index-binary")) index_binary = 1;
        else { fprintf(stderr, "tfs content: unknown option %s\n", argv[i]); return 2; }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: tfs -i INDEX content [--no-refresh] [--index-binary] {refresh ROOT | wc [-s] PATH... | hash PATH...\n"
                        "                                                       | grep [-c] [--trace] PATTERN ROOT | compact | info}\n");
        return 2;
    }
    const char *act = argv[i++]; char store[8300]; snprintf(store, sizeof store, "%s.content", index_path);
    if (!load(store)) S.index_binary = index_binary;
    int changed = 0, total_only = 0, count = 0, trace = 0;
    /* options of the action, then its paths */
    char **paths = malloc((argc + 1) * sizeof *paths); int np = 0;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--total")) total_only = 1;
        else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--count")) count = 1;
        else if (!strcmp(argv[i], "--trace")) trace = 1;
        else { char *a = malloc(8192); if (np == 0 && !strcmp(act, "grep")) strcpy(a, argv[i]); else abspath(argv[i], a); paths[np++] = a; }
    }
    int first_root = !strcmp(act, "grep") ? 1 : 0;
    if (!strcmp(act, "refresh") || ((!strcmp(act, "wc") || !strcmp(act, "hash") || !strcmp(act, "grep")) && !no_refresh)) {
        if (np <= first_root) { fprintf(stderr, "tfs content %s: a path is required\n", act); return 2; }
        for (int j = first_root; j < np; j++) {
            rstat_t st = refresh(paths[j]); changed |= st.read || st.dropped;
            if (!strcmp(act, "refresh"))
                printf("{\"files\": %llu, \"read\": %llu, \"bytes_read\": %llu, \"dropped\": %llu, \"compare_s\": %.4f, \"scan_s\": %.4f}\n",
                       (unsigned long long)st.files, (unsigned long long)st.read, (unsigned long long)st.bytes, (unsigned long long)st.dropped, st.compare_s, st.scan_s);
        }
    }
    int rc = 0;
    if (!strcmp(act, "wc")) {
        int64_t tot[4] = { 0, 0, 0, 0 };
        for (int j = 0; j < np; j++) {
            uint64_t *R, nr = rows_under(paths[j], &R);
            for (uint64_t q = 0; q < nr; q++) {
                int64_t *c = S.cols[R[q]];
                tot[0] += c[0]; tot[1] += c[1]; tot[2] += c[2]; if (c[3] > tot[3]) tot[3] = c[3];
                if (!total_only) printf("%lld %lld %lld %lld %s\n", (long long)c[0], (long long)c[1], (long long)c[2], (long long)c[3], S.path[R[q]]);
            }
            free(R);
        }
        printf("%lld %lld %lld %lld total\n", (long long)tot[0], (long long)tot[1], (long long)tot[2], (long long)tot[3]);
    } else if (!strcmp(act, "hash")) {
        for (int j = 0; j < np; j++) {
            uint64_t *R, nr = rows_under(paths[j], &R); char hx[41];
            for (uint64_t q = 0; q < nr; q++) { hex20(S.sha[R[q]], hx); printf("%s  %s\n", hx, S.path[R[q]]); }
            free(R);
        }
    } else if (!strcmp(act, "grep")) {
        if (np != 2) { fprintf(stderr, "usage: tfs -i INDEX content grep [-c] [--trace] PATTERN ROOT\n"); return 2; }
        fflush(stdout); rc = grep_rows(paths[0], paths[1], count, trace);
    } else if (!strcmp(act, "compact")) {
        printf("compacted in %.3f s\n", compact()); changed = 1;
    } else if (!strcmp(act, "info")) {
        uint64_t live = 0, unidx = 0; for (uint64_t r = 0; r < S.n; r++) if (!(S.fl[r] & F_DEAD)) { live++; unidx += (S.fl[r] & F_UNINDEXED) != 0; }
        printf("{\n \"store\": \"%s\",\n \"rows\": %llu,\n \"live\": %llu,\n \"binary_unindexed\": %llu,\n \"index_binary\": %s,\n"
               " \"trigrams\": %llu,\n \"matrix_entries\": %llu,\n \"delta_entries\": %llu\n}\n", store, (unsigned long long)S.n,
               (unsigned long long)live, (unsigned long long)unidx, S.index_binary ? "true" : "false", (unsigned long long)S.nk,
               (unsigned long long)S.ne, (unsigned long long)S.nd);
    } else if (strcmp(act, "refresh")) { fprintf(stderr, "tfs content: unknown action %s\n", act); return 2; }
    fflush(stdout);
    if (changed) save(store);
    return rc;
}
