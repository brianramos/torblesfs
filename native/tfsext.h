/* torblesFS native: regular files below a path (from the index, no walk), SHA-1, and the content / dupes commands. */
#ifndef TFSEXT_H
#define TFSEXT_H
#include <stdint.h>
#include <stddef.h>

typedef struct { char *path; int64_t size, mtime, dev, ino; } fent_t;
/* the live regular files at `root` (if it is one) or below it (recurse) / directly in it (!recurse): base + delta */
size_t ix_files(const char *root, int recurse, fent_t **out);
void   fents_free(fent_t *f, size_t n);

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t nbuf; } sha1_t;
void sha1_init(sha1_t *c);
void sha1_update(sha1_t *c, const void *data, size_t n);
void sha1_final(sha1_t *c, uint8_t out[20]);
void hex20(const uint8_t d[20], char out[41]);

int cmd_content(int argc, char **argv, const char *index_path);
int cmd_dupes(int argc, char **argv);
#endif
