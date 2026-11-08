/* torblesFS native: regular files below a path, answered from the index (base rows + live delta), and SHA-1. */
#include <stdlib.h>
#include <string.h>
#include "tfscore.h"
#include "tfsext.h"

static void push(fent_t **v, size_t *n, size_t *cap, const char *path, int64_t size, int64_t mt, int64_t dev, int64_t ino) {
    if (*n == *cap) { *cap = *cap ? 2 * *cap : 1024; *v = realloc(*v, *cap * sizeof **v); }
    (*v)[(*n)++] = (fent_t){ strdup(path), size, mt, dev, ino };
}

size_t ix_files(const char *root, int recurse, fent_t **out) {
    fent_t *v = NULL; size_t n = 0, cap = 0; char buf[8192];
    ent_t e;
    if (ix_lookup(root, &e) && ix_attr(&e, A_TYPE) == 'f') {
        push(&v, &n, &cap, root, ix_attr(&e, A_SIZE), ix_attr(&e, A_MTIME), ix_attr(&e, A_DEV), ix_attr(&e, A_INODE));
        *out = v; return n;
    }
    span_t sp[2]; int k = 0;
    if (recurse) k = ix_subtree(root, sp);
    else {
        int64_t d = dict_find(IX.doff, IX.dblob, IX.H.ndirs, root);
        if (d >= 0 && IX.dstart[d + 1] > IX.dstart[d]) sp[k++] = (span_t){ IX.dstart[d], IX.dstart[d + 1] };
    }
    for (int j = 0; j < k; j++)
        for (uint64_t r = sp[j].a; r < sp[j].b; r++) {
            if (ix_dead(r) || IX.TY[r] != 'f') continue;
            push(&v, &n, &cap, row_path(r, buf), IX.col[A_SIZE][r], IX.col[A_MTIME][r], IX.col[A_DEV][r], IX.col[A_INODE][r]);
        }
    for (size_t i = 0; i < IX.ndx; i++) {
        dent_t *d = &IX.dx[i];
        if (!d->live || d->at.a[A_TYPE] != 'f' || !strcmp(d->path, root) || !path_under(d->path, root)) continue;
        if (!recurse) { char dir[8192], name[4096]; split_path(d->path, dir, name); if (strcmp(dir, root)) continue; }
        push(&v, &n, &cap, d->path, d->at.a[A_SIZE], d->at.a[A_MTIME], d->at.a[A_DEV], d->at.a[A_INODE]);
    }
    *out = v; return n;
}

void fents_free(fent_t *f, size_t n) { for (size_t i = 0; i < n; i++) free(f[i].path); free(f); }

/* ------------------------------------------------------------------ SHA-1 (FIPS 180-4) */
#define ROL(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i]; e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
void sha1_init(sha1_t *c) {
    static const uint32_t iv[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->nbuf = 0;
}
void sha1_update(sha1_t *c, const void *data, size_t n) {
    const uint8_t *p = data; c->len += n;
    if (c->nbuf) {
        size_t t = 64 - c->nbuf < n ? 64 - c->nbuf : n; memcpy(c->buf + c->nbuf, p, t); c->nbuf += t; p += t; n -= t;
        if (c->nbuf == 64) { sha1_block(c->h, c->buf); c->nbuf = 0; }
    }
    for (; n >= 64; p += 64, n -= 64) sha1_block(c->h, p);
    if (n) { memcpy(c->buf, p, n); c->nbuf = n; }
}
void sha1_final(sha1_t *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8; uint8_t pad = 0x80, z = 0, L[8];
    sha1_update(c, &pad, 1); while (c->nbuf != 56) sha1_update(c, &z, 1);
    for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, L, 8);
    for (int i = 0; i < 5; i++) { out[4 * i] = c->h[i] >> 24; out[4 * i + 1] = c->h[i] >> 16; out[4 * i + 2] = c->h[i] >> 8; out[4 * i + 3] = c->h[i]; }
}
void hex20(const uint8_t d[20], char out[41]) {
    static const char *x = "0123456789abcdef";
    for (int i = 0; i < 20; i++) { out[2 * i] = x[d[i] >> 4]; out[2 * i + 1] = x[d[i] & 15]; }
    out[40] = 0;
}
