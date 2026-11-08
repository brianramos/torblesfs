#ifndef TFSADMIN_H
#define TFSADMIN_H
void abspath(const char *in, char *out);
int cmd_build(int argc, char **argv);
int cmd_refresh(int argc, char **argv);
int cmd_compact(int argc, char **argv);
int cmd_dump(int argc, char **argv);
int cmd_watch(int argc, char **argv);
int cmd_btrfs(int argc, char **argv, const char *index_path);
/* the inotify loop, with an optional second fd (e.g. /dev/fuse) serviced in the same loop */
int watch_loop(int (*extra_fd)(void), void (*extra)(void), double save_every);
#endif
