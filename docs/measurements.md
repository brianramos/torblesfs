# Measurements

One machine: WSL2 on Windows 11, 24 threads, NVMe, warm page cache, best of 3–5 runs, 2026-11-08. Every answer was
compared with the usual tool's. These numbers have not yet been reproduced on other hardware or with cold caches.
`bench/bench.sh [ROOT] [CORPUS]` reproduces the tables below on any tree.

## Metadata questions, one-shot (`tfs` process start + query)

`/usr`: 124,849 entries; `tfs build` 0.23–0.32 s, index 12 MB.

| question | tfs | usual way | answers |
|---|---|---|---|
| bytes of everything below (apparent) | **4.2 ms** | `find -printf \| awk` 136 ms | same |
| count `.py` files | **4.2 ms** | `find \| wc -l` 79 ms | same |
| total bytes of `*.so*` files | **5.6 ms** | `find -printf \| awk` 77 ms | same |
| files over 1 MB | **4.1 ms** | `find \| wc -l` 128 ms | same |
| newest file | **4.6 ms** | `find -printf \| sort \| tail` 168 ms | same |
| empty directories | **4.9 ms** | `find \| wc -l` 99 ms | same |
| paths containing `python3` | **3.8 ms** | `find -path \| wc -l` 74 ms | same |
| files per extension | **5.4 ms** | `find -printf \| awk` 75 ms | same |

A home folder of 1,393,554 entries: `tfs build` 2.4 s, index 153 MB. `tfs watch` was running, because other
processes were writing into the tree during the benchmark.

| question | tfs | usual way |
|---|---|---|
| bytes of everything below | **5.5 ms** | `find -printf \| awk` 1.4 s |
| count `.py` files | **19 ms** | `find \| wc -l` 0.68 s |
| total bytes of `*.so*` files | **29 ms** | 0.67 s |
| files over 1 MB | **15 ms** | 1.3 s |
| newest file | **30 ms** | `find -printf \| sort \| tail` 1.9 s |
| empty directories | **26 ms** | 0.85 s |
| paths containing `python3` | **15 ms** | 0.65 s |
| files per extension | **24 ms** | 0.70 s |

The answers were equal except for the byte total, which differed only by files that a running process created
while the benchmark ran.

## Content questions

Corpus: 312 MB in 21,039 files (`/usr/include`, `/usr/share/doc` and three source trees with their `.git`
directories; 2,157 binary files). `tfs content refresh` read it once in 3.3 s. The store is 107 MB with binary
files unindexed; the matrix holds 25.2M entries over 0.42M distinct trigrams.

| question | tfs | usual way | answers |
|---|---|---|---|
| line / word / byte totals | **15 ms** | `find \| wc --files0-from` (GNU) 383 ms | same |
| per-file `wc -lwcL` of all 21,039 files | | GNU wc | identical on every file |
| git blob ids of every file | **20 ms** | `git hash-object --stdin-paths` 402 ms | same, all 21,039 |
| files containing `SHA1_Init` (preview) | **34 ms** | `grep -rlF` 154 ms | same |
| files containing `TODO` (preview) | **41 ms** | `grep -rlF` 104 ms | same |
| files containing `ab` (2 bytes: no pruning) | 120 ms | `grep -rlF` 70 ms | same |
| duplicate files, digests cached | **18 ms** | `fdupes -r` 136 ms | same sets |

- **Maintenance:** after edits, deletes, a rename and new files, a refresh re-reads only those files. The test
  suite checks this.
- **Search** reads its candidates plus every binary file (2,157 files, 52 MB here), which is why it is a preview.
  With `--index-binary` the binary files drop out of every query's reads, at the cost of a matrix about 4x larger.

## Duplicates on `/usr/share`

59,506 regular files; 11,948 duplicate files.

| | time | files opened | bytes read |
|---|---|---|---|
| `tfs dupes -r`, first run | 0.44 s | 68k | 126 MB |
| `tfs dupes -r --store`, rerun | **0.05 s** | confirmation only | |
| `tfs dupes -r -Q --store`, rerun | **0.02 s** | none | 0 |
| `fdupes -r` | 0.38 s (every run) | | |

6,732 files were never opened, because their size is unique.

## The mount (`tfsd`)

On `/usr`:
- the first `find` walk through the mount takes 0.21 s (repeats are served by the kernel's caches);
- `cat mnt/@type=f/@name=*.so*/@count` takes 21 ms;
- `tfsd` uses 15 MB of memory.

Walking any FUSE mount costs a kernel round trip per request, so the fast path is to ask the index (`tfs`, or the
mount's `@` results) rather than walk the mount with `find` or `du`.

## Catch-up after downtime (btrfs)

See [btrfs-wsl.md](btrfs-wsl.md):
- 1k offline changes caught up in 69 ms, 10k in 0.24 s, and 100k in about 2.1 s (mostly `btrfs send` itself);
- a service restart after offline changes takes 30 ms.
