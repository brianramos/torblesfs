/* torblesFS native maintenance commands: build, refresh, compact, watch, btrfs (init | catchup | mark).
 * The index file is immutable once written; live changes go to INDEX.side (tombstones + changed entries), which
 * every reader (tfs, tfsd) loads with the index. compact folds the sidecar into a new index file. */
#include "tfscore.h"
#include "tfsadmin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/wait.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* os.path.abspath + normpath: lexical, symlinks are not resolved */
void abspath(const char *in, char *out) {
    char buf[16384];
    if (in[0] == '/') snprintf(buf, sizeof buf, "%s", in); else { char cwd[8192]; if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/"); snprintf(buf, sizeof buf, "%s/%s", cwd, in); }
    char *parts[2048]; int n = 0; char *save = NULL;
    for (char *t = strtok_r(buf, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (n) n--; continue; }
        if (n < 2048) parts[n++] = t;
    }
    size_t j = 0; out[0] = 0;
    for (int i = 0; i < n; i++) { out[j++] = '/'; size_t l = strlen(parts[i]); memcpy(out + j, parts[i], l); j += l; }
    if (!j) out[j++] = '/';
    out[j] = 0;
}

int cmd_build(int argc, char **argv) {
    const char *out = NULL; int one_fs = 1; char *roots[256]; int nr = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--cross-filesystems")) one_fs = 0;
        else if (nr < 256) { char *p = malloc(16384); abspath(argv[i], p); roots[nr++] = p; }
    }
    if (!out || !nr) { fprintf(stderr, "usage: tfs build -o INDEX [--cross-filesystems] ROOT...\n"); return 2; }
    double t0 = now(); rows_t r = {0}; scan_roots(roots, nr, one_fs, &r); double t1 = now();
    if (build_index(&r, roots, nr, one_fs, out)) return 1;
    fprintf(stderr, "indexed %zu entries in %.2f s (scan %.2f s, build %.2f s) -> %s\n", r.n, now() - t0, t1 - t0, now() - t1, out);
    return 0;
}

static int compact_into(const char *path) {          /* fold base + overlay into a new index at path */
    rows_t r = {0}; ix_rows(&r);
    char **roots = malloc((IX.H.nroots + 1) * sizeof *roots);
    for (uint64_t i = 0; i < IX.H.nroots; i++) roots[i] = strdup(root_str(i));
    int rc = build_index(&r, roots, (int)IX.H.nroots, (int)(IX.H.flags & 1), path);
    rows_free(&r); return rc;
}

int cmd_refresh(int argc, char **argv) {
    (void)argc; (void)argv; double t0 = now(); size_t c, m, d;
    ix_refresh_overlay(&c, &m, &d);
    if (ix_save_sidecar()) { perror("sidecar"); return 1; }
    printf("created %zu, modified %zu, deleted %zu in %.2f s (overlay: %zu entries)\n", c, m, d, now() - t0, ix_live_delta());
    return 0;
}

int cmd_compact(int argc, char **argv) {
    (void)argc; (void)argv; double t0 = now(); char *p = strdup(IX.path);
    if (compact_into(p)) return 1;
    printf("compacted in %.2f s\n", now() - t0); return 0;
}

/* ------------------------------------------------------------------ watch */
static volatile sig_atomic_t stop_flag;
static void on_sig(int s) { (void)s; stop_flag = 1; }
/* the overlay is folded into a new index file once it grows past this (readers keep their mapping of the old one) */
static size_t compact_threshold(void) { size_t t = IX.H.n / 8; return t > 200000 ? t : 200000; }

int watch_loop(int (*extra_fd)(void), void (*extra)(void), double save_every) {
    int fd = watch_open(); if (fd < 0) return 1;
    fprintf(stderr, "watching %zu directories\n", watch_count());
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    double last_save = now();
    while (!stop_flag) {
        struct pollfd p[2] = { { fd, POLLIN, 0 }, { extra_fd ? extra_fd() : -1, POLLIN, 0 } };
        int k = poll(p, extra_fd ? 2 : 1, 250);
        if (k > 0 && (p[0].revents & POLLIN)) watch_poll(fd);
        if (extra_fd && k > 0 && (p[1].revents & POLLIN)) extra();
        if (IX.dirty && now() - last_save >= save_every) {
            if (ix_live_delta() > compact_threshold()) {
                char *path = strdup(IX.path);
                if (!compact_into(path)) { ix_close(); ix_open(path); }
                free(path);
            } else ix_save_sidecar();
            last_save = now();
        }
    }
    if (IX.dirty) ix_save_sidecar();
    close(fd); return 0;
}
int cmd_watch(int argc, char **argv) {
    double every = 1.0;
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], "--save-every") && i + 1 < argc) every = atof(argv[++i]);
    return watch_loop(NULL, NULL, every);
}

/* ------------------------------------------------------------------ btrfs */
#define SEND "/usr/local/sbin/tfs-btrfs-send"
typedef struct { char subvol[8192], snapdir[8192], current[256]; } bstate_t;
static void state_path(char *out) { snprintf(out, 8300, "%s.btrfs.json", IX.path); }
static int json_str(const char *js, const char *key, char *out, size_t cap) {      /* {"key": "value", ...}: our own files */
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key); const char *p = strstr(js, pat); if (!p) return -1;
    p = strchr(p + strlen(pat), '"'); if (!p) return -1; p++;
    size_t j = 0;
    for (; *p && *p != '"' && j + 1 < cap; p++) {
        if (*p == '\\' && p[1]) { p++; if (*p == 'u') { unsigned v; sscanf(p + 1, "%4x", &v); out[j++] = (char)v; p += 4; continue; } }
        out[j++] = *p;
    }
    out[j] = 0; return 0;
}
static void json_put(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p); else if (*p < 0x20) fprintf(f, "\\u%04x", *p); else fputc(*p, f);
    }
    fputc('"', f);
}
static int load_state(bstate_t *s) {
    char p[8300]; state_path(p); FILE *f = fopen(p, "r"); if (!f) return -1;
    char buf[65536]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    return json_str(buf, "subvol", s->subvol, sizeof s->subvol) || json_str(buf, "snapdir", s->snapdir, sizeof s->snapdir)
           || json_str(buf, "current", s->current, sizeof s->current) ? -1 : 0;
}
static int save_state(const bstate_t *s) {
    char p[8300], t[8400]; state_path(p); snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w"); if (!f) return -1;
    fputs("{\"subvol\": ", f); json_put(f, s->subvol); fputs(", \"snapdir\": ", f); json_put(f, s->snapdir);
    fputs(", \"current\": ", f); json_put(f, s->current); fputs("}", f);
    fflush(f); fsync(fileno(f)); fclose(f); return rename(t, p);
}
static int run(char *const argv[], int quiet) {                  /* fork + exec, wait; returns the exit status */
    pid_t pid = fork(); if (pid < 0) return -1;
    if (!pid) { if (quiet) { int z = open("/dev/null", O_WRONLY); dup2(z, 1); dup2(z, 2); } execvp(argv[0], argv); _exit(127); }
    int st; waitpid(pid, &st, 0); return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static void next_name(const char *snapdir, char *out) {
    int mx = 0; DIR *d = opendir(snapdir); struct dirent *e;
    if (d) { while ((e = readdir(d))) if (!strncmp(e->d_name, "tfs-", 4)) { char *end; long v = strtol(e->d_name + 4, &end, 10); if (!*end && e->d_name[4] && v > mx) mx = (int)v; } closedir(d); }
    sprintf(out, "tfs-%d", mx + 1);
}
static int snapshot(const char *subvol, const char *snapdir, const char *name) {
    mkdir(snapdir, 0755); char dst[16384]; snprintf(dst, sizeof dst, "%s/%s", snapdir, name);
    char *a[] = { "btrfs", "subvolume", "snapshot", "-r", (char *)subvol, dst, NULL };
    return run(a, 1);
}
static int delete_snapshot(const char *snapdir, const char *name) {
    char dst[16384]; snprintf(dst, sizeof dst, "%s/%s", snapdir, name);
    char *a[] = { "btrfs", "subvolume", "delete", dst, NULL };
    if (!run(a, 1)) return 0;
    if (access(dst, F_OK)) return 0;
    char *b[] = { "btrfs", "property", "set", "-f", "-ts", dst, "ro", "false", NULL }; run(b, 1);
    return run(a, 1);
}
/* `sudo -n SEND old new | btrfs receive --dump`, captured */
static char *send_dump(const char *old, const char *new_, size_t *len, int *rc) {
    int p1[2], p2[2]; if (pipe(p1) || pipe(p2)) return NULL;
    pid_t a = fork();
    if (!a) { dup2(p1[1], 1); close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
              int z = open("/dev/null", O_WRONLY); dup2(z, 2);
              execlp("sudo", "sudo", "-n", SEND, old, new_, (char *)NULL); _exit(127); }
    pid_t b = fork();
    if (!b) { dup2(p1[0], 0); dup2(p2[1], 1); close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
              int z = open("/dev/null", O_WRONLY); dup2(z, 2);
              execlp("btrfs", "btrfs", "receive", "--dump", (char *)NULL); _exit(127); }
    close(p1[0]); close(p1[1]); close(p2[1]);
    size_t cap = 1 << 20, n = 0; char *buf = malloc(cap); ssize_t k;
    while ((k = read(p2[0], buf + n, cap - n)) > 0) { n += (size_t)k; if (n == cap) { cap *= 2; buf = realloc(buf, cap); } }
    close(p2[0]);
    int sa, sb; waitpid(a, &sa, 0); waitpid(b, &sb, 0);
    *rc = (WIFEXITED(sa) && !WEXITSTATUS(sa) && WIFEXITED(sb) && !WEXITSTATUS(sb)) ? 0 : 1;
    *len = n; return buf;
}

int cmd_btrfs(int argc, char **argv, const char *index_path) {
    if (argc < 1) { fprintf(stderr, "usage: tfs [-i INDEX] btrfs {init SUBVOL SNAPDIR | catchup | mark}\n"); return 2; }
    const char *sub = argv[0]; bstate_t st;
    if (!strcmp(sub, "init")) {
        if (argc < 3) { fprintf(stderr, "usage: tfs -i INDEX btrfs init SUBVOL SNAPDIR\n"); return 2; }
        abspath(argv[1], st.subvol); abspath(argv[2], st.snapdir); mkdir(st.snapdir, 0755);
        rows_t r = {0}; char *roots[1] = { st.subvol }; scan_roots(roots, 1, 1, &r);
        next_name(st.snapdir, st.current);
        if (snapshot(st.subvol, st.snapdir, st.current)) { fprintf(stderr, "tfs: snapshot failed\n"); return 1; }
        if (build_index(&r, roots, 1, 1, index_path)) return 1;
        IX.path = strdup(index_path); if (save_state(&st)) { perror("state"); return 1; }
        printf("indexed %zu entries; baseline snapshot %s\n", r.n, st.current); return 0;
    }
    ix_open(index_path);
    if (load_state(&st)) { fprintf(stderr, "tfs: no btrfs state for %s (run btrfs init)\n", index_path); return 1; }
    if (!strcmp(sub, "mark")) {
        char nm[256]; next_name(st.snapdir, nm);
        if (snapshot(st.subvol, st.snapdir, nm)) { fprintf(stderr, "tfs: snapshot failed\n"); return 1; }
        char old[256]; strcpy(old, st.current); strcpy(st.current, nm); save_state(&st); delete_snapshot(st.snapdir, old);
        printf("%s\n", nm); return 0;
    }
    if (!strcmp(sub, "catchup")) {
        double t0 = now(); char nm[256]; next_name(st.snapdir, nm);
        if (snapshot(st.subvol, st.snapdir, nm)) { fprintf(stderr, "tfs: snapshot failed\n"); return 1; }
        sync();
        char *dump = NULL; size_t len = 0; int rc = 1; double ts = now();
        for (int attempt = 0; attempt < 4 && rc; attempt++) {
            free(dump); dump = send_dump(st.current, nm, &len, &rc);
            if (rc) { fprintf(stderr, "catch-up: send attempt %d failed\n", attempt + 1); usleep(500000 * (attempt + 1)); sync(); }
        }
        if (rc) { delete_snapshot(st.snapdir, nm); fprintf(stderr, "tfs: send failed\n"); return 1; }
        double send_s = now() - ts; size_t lines = 0; for (size_t i = 0; i < len; i++) lines += dump[i] == '\n';
        size_t limit = IX.H.n / 16 > 60000 ? IX.H.n / 16 : 60000; const char *mode; size_t ops = lines, deleted = 0, c = 0, m = 0;
        if (lines > limit) { ix_refresh_overlay(&c, &m, &deleted); mode = "rescan"; }
        else { deleted = btrfs_apply_dump(dump, len, st.subvol, &ops); mode = "apply"; }
        free(dump);
        if (ix_live_delta() > compact_threshold()) { char *p = strdup(IX.path); compact_into(p); }
        else ix_save_sidecar();
        char old[256]; strcpy(old, st.current); strcpy(st.current, nm); save_state(&st); delete_snapshot(st.snapdir, old);
        printf("{\"ops\": %zu, \"mode\": \"%s\", \"deleted\": %zu, \"created\": %zu, \"modified\": %zu, \"send_seconds\": %.3f, \"seconds\": %.3f, \"snapshot\": \"%s\"}\n",
               ops, mode, deleted, c, m, send_s, now() - t0, nm);
        return 0;
    }
    fprintf(stderr, "tfs: unknown btrfs command %s\n", sub); return 2;
}

/* every live entry with its attributes, in byte order: the oracle comparison format */
static int cmp_line(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
int cmd_dump(int argc, char **argv) {
    (void)argc; (void)argv; rows_t r = {0}; ix_rows(&r);
    char **lines = malloc((r.n ? r.n : 1) * sizeof *lines); char buf[20000];
    for (size_t i = 0; i < r.n; i++) {
        const char *p = r.parent[i]; const int64_t *a = r.at[i].a;
        int k = snprintf(buf, sizeof buf, strcmp(p, "/") ? "%s/%s" : "%s%s", p, r.name[i]);
        snprintf(buf + k, sizeof buf - k, "\t%c %lld %lld %lld %llo %lld %lld %lld %lld %lld", (char)a[A_TYPE], (long long)a[A_SIZE], (long long)a[A_BLOCKS],
                 (long long)a[A_MTIME], (long long)a[A_MODE], (long long)a[A_UID], (long long)a[A_GID], (long long)a[A_NLINK], (long long)a[A_INODE], (long long)a[A_DEV]);
        lines[i] = strdup(buf);
    }
    qsort(lines, r.n, sizeof *lines, cmp_line);
    for (size_t i = 0; i < r.n; i++) { fputs(lines[i], stdout); putchar('\n'); }
    return 0;
}
