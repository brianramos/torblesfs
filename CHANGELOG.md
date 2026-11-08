# Changelog

## 0.1.0-rc1 (2026-11-08)

First release candidate. Two native programs, no dependencies beyond libc.
- **`tfs`:**
  - builds an index of a tree (one filesystem by default);
  - answers `stat`, `ls`, `du`, `find` (a compatible subset, plus `--count`, `--bytes`, `--newest` and
    `--group-by ext`) and `locate` from it;
  - keeps it current with `refresh`, `watch` (inotify) and `btrfs init|catchup|mark` (snapshot diffs across
    downtime).
- **`tfs content`:** per-file GNU `wc` counts and git blob ids, kept beside the index and re-read only for changed
  files; fixed-string search with a trigram prefilter (preview).
- **`tfs dupes`:** duplicate files with fdupes' options and output. Unique sizes are never opened, and digests can
  be cached across runs.
- **`tfsd`:** a FUSE mount of the index:
  - passthrough or sidecar (`--links`) mode, with the watcher;
  - `@` URI segments (`dir/@type=f/@name=*.py/@count`);
  - `.q/` saved queries.
- **`tests/run.sh`:** about 570 checks against GNU find, `stat`, GNU wc, git, GNU grep and a brute-force duplicate
  finder.
