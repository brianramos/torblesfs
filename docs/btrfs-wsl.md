# btrfs: catch-up after downtime

inotify only sees changes while a watcher runs. On btrfs, changes made while nothing was watching are recovered
exactly from a metadata-only `btrfs send` stream between the snapshot taken at the last stop and a new one, so a
restart costs O(changes) instead of a rescan. The one privileged step is `contrib/tfs-btrfs-send`.

On any Linux with a btrfs subvolume, the cycle is:

```bash
tfs -i ~/.local/share/torblesfs/data.tfsn btrfs init /mnt/data /mnt/.snaps    # builds the index + baseline snapshot
tfs -i ~/.local/share/torblesfs/data.tfsn btrfs mark                          # at stop
tfs -i ~/.local/share/torblesfs/data.tfsn btrfs catchup                       # at start
```

`contrib/torblesfs.service` runs this cycle around the mount: uncomment its two btrfs lines.

`catchup` takes a new snapshot, asks `tfs-btrfs-send` (through `sudo -n`) for the metadata-only stream between the
two snapshots, applies it, and deletes the old snapshot. A stream above max(60,000, n/16) operations is cheaper to
replace with a rescan, so `catchup` decides from the stream's line count before parsing and reports
`"mode": "apply"` or `"mode": "rescan"`.

## Worked example: a dedicated btrfs disk under WSL2

This is how the development machine runs it (Windows 11 Home, WSL 3.0, one NVMe holding Windows). `wsl --mount` cannot attach the disk Windows boots from, so the "partition" is a dedicated dynamically expanding VHDX, which is the same kind of disk WSL's own root uses.

1. **Windows, administrator PowerShell:**
   - `diskpart` creates `C:\WSL\torblesfs.vhdx` (256 GB maximum, expandable);
   - `wsl --mount --vhd C:\WSL\torblesfs.vhdx --bare` attaches it;
   - a scheduled task "torblesFS disk" (at logon, highest privileges) repeats the attach and runs `wsl -u root mount /mnt/tfs`.
2. **WSL:**
   - `mkfs.btrfs -L torblesfs` (guarded to the one empty 256 GB disk);
   - in `/etc/fstab`: `LABEL=torblesfs /mnt/tfs btrfs noatime,user_subvol_rm_allowed,nofail 0 0`;
   - a `data` subvolume (linked as `~/tfs-data`) and `/mnt/tfs/.snaps`.
3. **The one root operation:** `contrib/tfs-btrfs-send`, installed root-owned in `/usr/local/sbin`, plus `/etc/sudoers.d/torblesfs`: `brian ALL=(root) NOPASSWD: /usr/local/sbin/tfs-btrfs-send`.
   - The wrapper only runs `btrfs send --no-data` between two `tfs-N` subvolumes in `/mnt/tfs/.snaps`.
   - sudo-rs does not allow wildcards in command arguments, so the rule cannot be narrowed with patterns; the wrapper does that instead. Validate a candidate rule with `visudo -c -f FILE` before installing it.
4. **The index and the service:**
   - `tfs -i ~/.local/share/torblesfs/data.tfsn btrfs init /mnt/tfs/data /mnt/tfs/.snaps` (builds the index and the baseline snapshot);
   - the unit is `contrib/torblesfs.service` with the btrfs lines enabled, plus an `ExecStartPre` that waits for `/mnt/tfs` and, if it is missing, runs the scheduled task (`schtasks.exe /run /tn "torblesFS disk"`); then `systemctl --user enable --now torblesfs`.

**What the service does:**
1. **Waits for `/mnt/tfs`.** If it is missing (for example after `wsl --shutdown`), it first runs the scheduled task from Linux (`schtasks.exe /run`), which re-attaches the disk without a UAC prompt.
2. **Catches up:** a new snapshot, then `sudo -n tfs-btrfs-send OLD NEW | btrfs receive --dump`, applied to the index; then the old snapshot is deleted. Before sending, it syncs; failures are retried and logged.
3. **Runs the sidecar mount at `~/tfs`** with the watcher.
4. **At stop, takes a `mark` snapshot while the watcher still runs**, then unmounts.

The snapshot state lives in `INDEX.btrfs.json`, separate from the index, so marking never races with the mount saving the index.

**Why nothing is lost:**
- the current snapshot is always taken when the index is current or ahead of it;
- every catch-up re-reads every path changed since that snapshot;
- re-reading is idempotent.

So a crash, a `wsl --shutdown` or edits made while stopped are caught up at the next start.

**Measured on the real disk** with the native tools (every step compared with a fresh build: 0 differences):
- **Catch-up after offline changes:** 1k changes in 69 ms, 10k in 0.24 s, 100k in about 2.1 s, of which 1.7–1.9 s is
  `btrfs send` + `btrfs receive --dump` itself.
- **Service restart:** stop, offline changes (a delete, a rename, a create), start: caught up in 30 ms. The service
  uses about 2 MB of memory.
- **Warm metadata performance** is the same on btrfs and ext4; btrfs costs about 2x when creating many small files
  (copy-on-write).

The test suite (`tests/run.sh btrfs`) runs two catch-up rounds on such a disk when `TFS_BTRFS_DATA`,
`TFS_BTRFS_SNAPS` and the sudoers rule are in place.
