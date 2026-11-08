# Design

How the index answers questions, how it stays current, and how the mount, the content store and the duplicate
finder sit on top of it. Measurements are in [measurements.md](measurements.md).

## The index

**Layout.** `tfs build` walks the roots once (`openat` / `fstatat`, one filesystem unless `--cross-filesystems`):
- **Two dictionaries**, sorted in byte order (the C locale's order, as `memcmp`): every directory path, and every
  entry name. A string's code is its rank.
- **One key per entry:** `K = dir_code << wn | name_code`. Rows are stored in K order, so:
  - every directory's entries are one contiguous run of rows (`dstart[d]` is the first row of directory `d`);
  - every subtree is one interval of directory codes, since paths that share the prefix `p/` are adjacent in byte
    order: the directories below `p` are exactly `[lower_bound(p + "/"), lower_bound(p + "0"))`. So a subtree is
    a row range.
- **Columns**, one array per attribute in row order: type, size, blocks, mtime (ns), mode, uid, gid, nlink, inode,
  device.

**One entry** (`stat`, a lookup in the mount) is three binary searches: the directory and the name in their
dictionaries, then the key in the directory's run. A listing is the directory's run itself. **Many entries** (`du`, `find`, `locate`, counts and sums) are a
scan of the subtree's row range, never a walk of the tree. `find` evaluates its expression per row with find's
short-circuit semantics. Name and path tests run once per distinct name or directory code present, not once per row,
and paths are built only when a test or the output needs them.

**The file (`TFSNIDX1`).** A 512-byte header of little-endian u64 fields, then 64-byte aligned sections:
`K | type | size, blocks, mtime, mode, uid, gid, nlink, inode, dev | tombstones | dstart | directory dictionary |
name dictionary | roots | delta`. Dictionary strings are NUL-terminated and used in place. The file is mapped as it
is: nothing is parsed or rebuilt at open. Each index has a random identity that pairs it with its sidecar.

## Keeping it current

**The sidecar (`INDEX.side`).** The index file is never rewritten in place. A change sets a tombstone bit on the
old row (if any) and records the entry's new state in a delta keyed by path. Both live in the sidecar, which every
reader (`tfs`, `tfsd`) loads with the index. `tfs compact` folds the sidecar into a new index file. The watcher
compacts on its own above max(200k, n/8) changed entries.

**Sources of change:**
- **`tfs refresh`** rescans the roots and applies the differences.
- **`tfs watch`** (and `tfsd --watch`) put an inotify watch on every indexed directory:
  - an event re-reads the one path it names;
  - a new directory is walked and watched, and a moved or deleted directory drops its subtree;
  - a change through one name of a hard-linked file re-reads its other names;
  - a queue overflow falls back to a rescan.
  
  The sidecar is saved at most every `--save-every` seconds (default 1).
- **btrfs catch-up** recovers changes made while no watcher ran, from a snapshot diff. See [btrfs-wsl.md](btrfs-wsl.md).

## The mount (`tfsd`)

`tfsd -i INDEX [--links] [--watch] MOUNTPOINT` speaks the FUSE kernel protocol (7.31) over `/dev/fuse` directly,
mounted through `fusermount3`, with no libfuse.
- **Metadata from the index.** `LOOKUP`, `GETATTR` and `READDIRPLUS` are answered from the index. A base row's node
  id is its row number + 2, so serving a listing computes nothing per entry. `READDIRPLUS` is used on every page, so
  the kernel sends no `LOOKUP` per entry.
- **Passthrough (default).** File data is read from and written to the backing tree, and every change made through
  the mount (create, write, mkdir, rename, link, symlink, chmod, utime, unlink, rmdir) is applied to the tree, then
  to the index.
- **Sidecar (`--links`).** Every non-directory appears as a symlink to its real path, so opening a file reads the
  real filesystem natively. Writes into the mount are refused; write to the real tree, and `--watch` brings the
  change in.

**Queries are paths.** `/home/me/src/@type=f/@name=*.py/@count` is:
- a subtree (plain segments);
- conditions (`@type=`, `@name=` (repeatable), `@size>N` / `@min_size=N`, `@size<N` / `@max_size=N`, `@newer=`,
  `@older=`; combined with AND);
- a result file: `@count`, `@bytes`, `@newest` or `@list`.

Some rules:
- **Conditions commute** with each other and with plain segments, so a view keeps the tree's shape.
- **Views list only directories that hold a match**, but every real subdirectory stays reachable by name. A URI
  therefore resolves the same however its segments are ordered.
- **`@@x` is the literal name `@x`.** A segment that does not parse as a condition is a literal name anyway.

**Saved queries (`.q/`).**
- `mkdir .q/NAME` creates one; writing `.q/NAME/.expression` defines it, for example
  `type=f name='*.py' min_size=10000 newer=2026-11-01` or `path=src type=f`; `mv` and `rmdir` rename and delete it.
- `.q/NAME/` is a live view of the tree whose matches are the real files.
- `.count`, `.bytes` and `.newest` evaluate the expression on every read.
- Queries are stored in `INDEX.queries`.

## The content store (`tfs content`)

One row per regular file path, valid while the file's identity in the index (device, inode, size, mtime) is
unchanged. A single read of the file produces:
- **GNU `wc` counts in the C locale:** lines, words, bytes and maximum display width;
- **the git blob id:** SHA-1 of `blob <len>\0` + content, equal to `git hash-object --no-filters`;
- **its set of distinct byte trigrams**, for search.

The trigram sets form a files × trigrams matrix stored column-sparse (for each trigram, the sorted rows of the files
that contain it). A fixed-string search intersects the columns of the pattern's trigrams into a candidate set and
reads only the candidates to confirm, so its answers are exact. Patterns under 3 bytes have no trigram, and every
file is a candidate.

**Binary files** (a NUL byte) are kept out of the matrix by default and are always candidates. They hold almost all
distinct trigrams: on the measured corpus, indexing them makes the matrix about 4x larger. `--index-binary`, given
when the store is created, indexes them too.

**Maintenance.** A refresh compares each path's identity with the index, a metadata-only step:
- rows whose file is gone or changed are marked dead;
- new or changed files are read, and their trigram entries go to a delta;
- compaction merges the delta into the matrix and drops dead rows. It happens automatically when the delta passes a
  quarter of the matrix, or when a quarter of the rows are dead.

**The file (`INDEX.content`).** Fixed sections, 8-byte aligned: path offsets and paths, identities, counts, blob ids,
flags, then the matrix (trigrams, starts, rows) and the delta. It is mapped copy-on-write, so `wc` and `hash` never
touch the matrix, and a search touches only the posting lists of its pattern's trigrams.

## Duplicates (`tfs dupes`)

fdupes' decision, restated as stages. Each stage consumes only what the previous one kept:

| stage | input | file access |
|---|---|---|
| candidates | regular files below the given directories (`-r`), with fdupes' filters `-n -A -G -L`, hard links collapsed by (device, inode) unless `-H` | none |
| size groups | sizes held by at least two files: **a file with a unique size is never opened** | none |
| partial digest | SHA-1 of the first 4096 bytes, grouped by (size, digest) | partial reads |
| full digest | only partial collisions larger than 4096 bytes | full reads |
| confirm | byte comparison with the set's first file (skipped with `-Q`) | full reads |
| order | within a set by mtime, then path; sets by their first path | none |

With `--store DIR`, digests are cached by identity, so a rerun hashes only files whose metadata changed. The output
follows fdupes' format (`-S`, `-m`, `-1`, `-f`).
