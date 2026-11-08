# torblesFS 0.1.0 (release candidate 1)

**Exact, live answers about large file trees, from an index instead of a walk.**

> This repository began as a fork of [sharkdp/fd](https://github.com/sharkdp/fd): `fd` walks the tree faster,
> torblesFS doesn't walk it at all. Its first commit replaces fd's tree with torblesFS. fd's history is kept below it,
> under fd's own licenses (MIT / Apache-2.0). torblesFS is not affiliated with or endorsed by fd's authors.

torblesFS keeps an index of a directory tree current: through inotify while it runs, and through btrfs snapshot
diffs across downtime. Questions that normally mean walking the tree or reading every file then become lookups:

1. **Metadata questions.** `find`, `du`, `ls`, `stat` and `locate`-style questions are answered in milliseconds
   instead of seconds, from an index kept exact as files change. Examples: how many `.py` files are under here, how
   much space do files over 10 MB take, which directories are empty. The index can also be mounted, so ordinary
   tools see live query results as directories and files.
2. **Content questions without rereading files.** Per-file content summaries kept beside the index: line, word and
   byte counts (GNU `wc`), git blob ids (`git hash-object`) and duplicate files (fdupes-compatible), plus
   fixed-string search with a trigram prefilter (a preview). Only new or changed files are ever read again.

It is two small C programs, `tfs` and `tfsd`, with no dependencies beyond libc. Every answer is checked against
the original tool in the test suite: GNU find, `stat`, GNU `wc`, `git hash-object`, GNU grep, and a brute-force
duplicate finder.

## Who it is for

- **Storage and HPC administration:** quotas, purge and tiering candidates, and "what is big, old or empty", without
  scanning millions of entries each time.
- **Build, CI and developer tooling** that stats or hashes thousands of files to decide what changed.
- **Backup selection and duplicate finding** across large trees, repeated cheaply.
- **Anyone who runs the same `find | xargs` pipeline over a big tree more than once.**

## Measured

One machine (WSL2, NVMe), warm caches, one-shot commands including process start-up. More in
[docs/measurements.md](docs/measurements.md); `bench/bench.sh` reproduces them on any tree.

| question | tfs | the usual way |
|---|---|---|
| count `.py` files in `/usr` (125k entries) | **4.2 ms** | `find \| wc -l` 79 ms |
| newest file in `/usr` | **4.6 ms** | `find -printf \| sort \| tail` 168 ms |
| bytes below a 1.39M-entry home folder | **5.5 ms** | `find -printf \| awk` 1.4 s |
| files over 1 MB in it | **15 ms** | `find \| wc -l` 1.3 s |
| build the index of that home folder | 2.4 s (153 MB) | |
| catch up after 10k offline changes (btrfs) | **0.24 s** | rebuilding the index |
| line/word/byte totals of a 312 MB, 21k-file corpus | **15 ms** | `find \| wc --files0-from` 383 ms |
| git blob ids of all its files | **20 ms** | `git hash-object --stdin-paths` 402 ms |
| duplicate files in it, rerun | **18 ms** | `fdupes -r` 136 ms |
| files containing `SHA1_Init` (preview) | **34 ms** | `grep -rlF` 154 ms |

## Install

Linux. You need a C compiler and make. Mounting needs `fusermount3` (the `fuse3` package).

```bash
make                    # native/tfs and native/tfsd
make install            # copies them to ~/.local/bin (PREFIX=... to change)
make test               # tests/run.sh
```

## Quick start

```bash
tfs build -o home.tfsn ~                          # index a tree (one filesystem)
tfs -i home.tfsn du ~/src                         # entries and bytes below a path
tfs -i home.tfsn find ~/src -type f -name '*.py' --count
tfs -i home.tfsn find ~ -type f -size +10M --bytes
tfs -i home.tfsn find ~ -type f --group-by ext    # files and bytes per extension
tfs -i home.tfsn locate -b -c '*.pdf'
tfs -i home.tfsn watch                            # keep it current with inotify (or: tfs refresh)
```

**Mount it**, and queries become paths:

```bash
tfsd -i home.tfsn --links --watch ~/tfs           # sidecar mount: files are links to the real ones
cat ~/tfs/src/@type=f/'@name=*.py'/@count         # a live answer, read like a file
ls  ~/tfs/src/@type=f/'@size>10000000'/           # a live view of the matching files
mkdir ~/tfs/.q/big && echo "type=f min_size=10000000" > ~/tfs/.q/big/.expression
cat ~/tfs/.q/big/.bytes                           # a saved query, re-evaluated on every read
```

**Content questions** use a store beside the index (`home.tfsn.content`). The first command reads every file below
the path once. Later commands re-read only files whose identity (device, inode, size, mtime) changed in the index;
`--no-refresh` skips even that check.

```bash
tfs -i home.tfsn content wc -s ~/src              # line, word, byte totals and max line length
tfs -i home.tfsn content hash ~/src/lib           # git blob ids
tfs -i home.tfsn content grep 'TODO(release)' ~/src     # preview: files containing a fixed string
tfs -i home.tfsn dupes -r --store ~/.cache/tfs-dupes ~/photos
```

**As a service:** `contrib/torblesfs.service` is a systemd user unit template that runs the mount with the watcher.
On btrfs, `tfs btrfs init|catchup|mark` recovers changes made while nothing was watching; see
[docs/btrfs-wsl.md](docs/btrfs-wsl.md).

## Commands

| command | answers |
|---|---|
| `tfs build -o INDEX [--cross-filesystems] ROOT...` | builds an index |
| `tfs -i INDEX stat PATH` / `ls DIR` / `du PATH` | one entry; a listing; entries and apparent bytes below a path |
| `tfs -i INDEX find ROOT [EXPR] [--count\|--bytes\|--newest\|--group-by ext]` | a compatible subset of find(1): `-name -iname -path -ipath -wholename -regex -iregex -type -size -empty -perm -user -group -uid -gid -nouser -nogroup -links -inum -newer -mtime -mmin -maxdepth -mindepth -prune -print -print0 -true -false`, `! -not -a -o ( ) ,` |
| `tfs -i INDEX locate [-b -i -r -A -c -0 -l N --root DIR] PATTERN...` | findutils' locate rules over the live index |
| `tfs -i INDEX refresh` / `compact` / `watch` / `dump` | maintenance |
| `tfs -i INDEX btrfs init SUBVOL SNAPDIR` / `catchup` / `mark` | catch-up across downtime on btrfs |
| `tfs -i INDEX content [--no-refresh] [--index-binary] refresh ROOT \| wc [-s] PATH... \| hash PATH... \| grep [-c] PATTERN ROOT \| compact \| info` | content columns and (preview) search |
| `tfs -i INDEX dupes [-r -n -A -H -G N -L N -Q -S -m -1 -f] [--store DIR] DIR...` | duplicate files, fdupes' options and output |
| `tfsd -i INDEX [--links] [--watch] [--save-every S] MOUNTPOINT` | the mount, with `@` URIs and `.q/` saved queries |

## What "exact" means here

- **Metadata** answers equal what the live tree shows when the index is current: with a watcher running (`tfs
  watch`, `tfsd --watch`), after `tfs refresh`, or after a btrfs catch-up. Without one, answers describe the tree as
  of the last update.
- **Content** answers are exact for files whose index identity is unchanged. A file rewritten in place with its
  size and mtime restored afterwards is not re-read.

## Limitations

- **`content grep` is a preview.** It is exact, and faster than GNU grep for selective patterns. But it is
  fixed-string and case-sensitive only (no regexes), and by default it reads every binary file on each query.
  Patterns shorter than 3 bytes get no pruning and are slower than grep. `--index-binary` removes the binary reads,
  at the cost of a store about 4x larger.
- **The content store** is about 40% of the size of the files it covers (107 MB for 261 MB of files), almost all of
  it the trigram matrix. Building it reads every file once.
- **`tfs du`** reports apparent sizes of the entries below a path, counting each name of a hard-linked file. It is
  not the allocated-block total of `du -s`.
- **inotify** needs `max_user_watches` above the directory count, and does not see changes made through other mount
  namespaces or network filesystems. Without btrfs, downtime costs a rescan (`tfs refresh`).
- **Mounts:** an index with one root, built one-filesystem; nested mounts appear empty. Walking any FUSE mount is
  slower than walking the real filesystem, so ask the index (or the mount's `@` results) for the fast path.
- **Measured on one machine**, warm caches. Cold-cache and other-hardware numbers are still to come.

## Repository

| path | what |
|---|---|
| `native/` | `tfs` (index, queries, maintenance, btrfs, content, dupes) and `tfsd` (FUSE mount + watcher) |
| `contrib/` | a systemd user unit template; `tfs-btrfs-send`, the root-owned wrapper for btrfs catch-up |
| `tests/run.sh` | the test suite |
| `bench/bench.sh` | the benchmark behind docs/measurements.md |
| `docs/` | [design](docs/design.md), [measurements](docs/measurements.md), [btrfs](docs/btrfs-wsl.md) |

## Tests

`make test` runs `tests/run.sh`: about 570 checks on generated trees. The trees include names that sort around `/`,
unicode, undecodable bytes, spaces, symlinks, hard links and empty directories. What the checks cover:
- **The index** against GNU find, after building, two rounds of churn plus `refresh`, `compact`, and live changes
  under `watch`.
- **Queries:** 32 `find` expressions (listing, `--count`, `--bytes`, with a delta entry and a tombstone) against GNU
  find; 13 `locate` cases against findutils' rules; `stat`, `ls` and `du` against `stat(1)` and GNU find.
- **Content:** every file's `wc -lwcL` against GNU wc, blob ids against `git hash-object`, and 7 searches against
  `grep -rlF`, before and after changes and compaction, with two roots and both binary policies.
- **Duplicates** against a brute-force grouping by content, for 6 option sets, cached reruns and the output
  options.
- **The mount**, in passthrough and sidecar modes:
  - full metadata and data comparisons, writes through the mount, external changes, and the saved index after
    unmount;
  - 25 random URIs per mode (`@count`, `@bytes`, `@list` and a walk of the view) against GNU find;
  - saved queries.
- **btrfs catch-up** on a prepared disk (skipped otherwise).

Tests that need FUSE, git, GNU wc or a prepared btrfs disk are skipped when those are missing.

## License

Apache-2.0.
