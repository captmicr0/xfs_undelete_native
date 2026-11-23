# xfs_undelete_native

A native C deleted-file recovery utility for XFS, inspired by [`xfs_undelete`](https://github.com/ianka/xfs_undelete).

The goal is to provide the useful recovery behavior of `xfs_undelete` in a standalone native executable while adding:

- a complete metadata scan **before copying**
- estimated recoverable file sizes before recovery begins
- per-file copy progress
- transfer speed and ETA
- overall recovery progress
- stricter validation of stale/deleted extent metadata
- native MIME/extension filtering compatible with the practical `xfs_undelete` filter syntax

> **The XFS source is always opened read-only.**
>
> `xfs_undelete_native` never writes to or remounts the source filesystem.

---

## Safety

Deleted-file recovery should be performed with as little activity on the source filesystem as possible.

The source device is opened with:

```c
open(source, O_RDONLY | O_CLOEXEC)
```

All XFS source reads use `pread()`.

Recovered files are created on the destination filesystem and written using `pwrite()`.

The program never uses `write()`, `pwrite()`, `ftruncate()`, `fsync()`, or another write operation against the XFS source descriptor.

### Recommended recovery setup

Prefer one of these:

- source filesystem unmounted
- source filesystem mounted read-only
- recovery performed against a block-level image

Always recover to a **different filesystem/device**.

Do not recover files onto the filesystem being recovered. New writes can overwrite deleted file data that would otherwise still be recoverable.

---

# Features

- Native C implementation
- Reads XFS metadata directly
- Source device opened `O_RDONLY`
- No Tcl or Python runtime
- No `dd` subprocess for recovery
- Works with block devices and raw XFS filesystem images
- Pre-copy metadata discovery pass
- Estimated logical file size before recovery
- Total estimated recovery size
- Actual physical extent bytes that will be copied
- Per-file progress percentage
- Per-file throughput
- Per-file ETA
- Overall copy progress
- Sparse-file reconstruction
- MIME-type filtering
- Extension filtering
- MIME wildcard filtering such as `video/*`
- Ignore filters
- Deletion/ctime filtering
- Modification/mtime filtering
- Maximum-size filtering
- Specific inode exclusions
- Resume/start at an exact inode
- Trailing-zero trimming for selected types
- Custom `file(1)` magic database support
- Dry-run mode
- Extent overlap validation
- Stale sparse-extent sanity checks
- Refuses a source block device mounted read/write unless explicitly overridden
- Refuses to recover onto the source block device

---

# Building

## Recommended: static x86-64 build with Docker

For running on Slackware/Unraid while building on another Linux system, a statically linked musl binary is recommended.

From the repository root:

```bash
docker run --rm \
  -v "$PWD":/src \
  -w /src \
  alpine:3.22 \
  sh -c 'apk add --no-cache build-base musl-dev && \
    gcc -O2 -static -std=gnu11 \
    -D_FILE_OFFSET_BITS=64 \
    -Wall -Wextra -Wpedantic \
    -o xfs_undelete_native \
    xfs_undelete_native.c'
```

The resulting executable will be:

```text
xfs_undelete_native
```

Verify it:

```bash
file xfs_undelete_native
ldd xfs_undelete_native
```

Expected output should indicate:

```text
statically linked
```

and:

```text
not a dynamic executable
```

---

## Build directly with GCC

```bash
gcc -O2 -std=gnu11 \
  -D_FILE_OFFSET_BITS=64 \
  -Wall -Wextra -Wpedantic \
  -o xfs_undelete_native \
  xfs_undelete_native.c
```

---

## Static build with musl on Debian/Ubuntu

```bash
sudo apt update
sudo apt install -y musl-tools
```

Then:

```bash
musl-gcc -O2 -static -std=gnu11 \
  -D_FILE_OFFSET_BITS=64 \
  -Wall -Wextra -Wpedantic \
  -o xfs_undelete_native \
  xfs_undelete_native.c
```

---

# Runtime file-type detection

For full MIME detection, `xfs_undelete_native` uses the system:

```text
file
```

utility and its magic database.

On most Unraid/Slackware installations this is already available.

Check with:

```bash
file --version
```

If `file` is unavailable or cannot identify a candidate, the program falls back to built-in signature detection for common formats such as:

- MP4
- MOV
- MKV
- AVI
- PDF
- JPEG
- PNG
- GIF
- ZIP
- gzip
- OGG
- FLAC
- WAV
- SQLite
- plain text
- generic binary data

The recovery executable itself can still be statically linked; `file(1)` is only used as an external MIME identification helper.

---

# Usage

```text
xfs_undelete_native [options] DEVICE_OR_IMAGE
```

Example:

```bash
./xfs_undelete_native \
  -o /mnt/disks/recovery/recovered \
  /dev/sdh1
```

A raw filesystem image can also be used:

```bash
./xfs_undelete_native \
  -o ./recovered \
  xfs-recovery.img
```

---

# Example: recover videos deleted on a specific date

Equivalent to:

```bash
./xfs_undelete \
  -t 2026-11-23 \
  -r 'video/*' \
  -o /mnt/disks/SanDisk_Ultra_USB_3.0/ \
  /dev/sdh1
```

with the native version:

```bash
./xfs_undelete_native \
  -t 2026-11-23 \
  -r 'video/*' \
  -o /mnt/disks/SanDisk_Ultra_USB_3.0/ \
  /dev/sdh1
```

For a dry run first:

```bash
./xfs_undelete_native \
  --dry-run \
  -t 2026-11-23 \
  -r 'video/*' \
  -o /mnt/disks/SanDisk_Ultra_USB_3.0/ \
  /dev/sdh1
```

---

# Dry-run mode

A dry run performs the metadata scan, filtering, extent validation, MIME detection, and size estimation without creating recovered files.

```bash
./xfs_undelete_native \
  --dry-run \
  -t 2026-11-23 \
  -r 'video/*' \
  /dev/sdh1
```

Example:

```text
Phase 1/2: scanning metadata and estimating sizes
(NO file data is being copied)...

Pre-copy estimate complete.
  recoverable candidates:       15
  estimated logical output:     9.84 GiB
  actual extent bytes to copy:  9.81 GiB
  inode records inspected:      148203
  rejected overlapping layouts: 3
  rejected oversize sparse:      2
```

Candidates are then listed before any recovery copy begins:

```text
[1/3] inode=10888791452  est=159.84 MiB  data=159.84 MiB  extents=1  type=.mp4
         mime=video/mp4
```

This makes it possible to inspect likely recovery results before writing anything to the destination.

---

# File-size estimation

XFS normally destroys the exact original EOF/file length when an inode is deleted.

Because of this, the exact original byte size usually cannot be recovered.

`xfs_undelete_native` estimates the logical size using the end of the highest surviving valid logical extent.

For example:

```text
logical extent:
    start block = 1000
    block count = 100
```

ends at logical block:

```text
1100
```

The estimated file size is therefore based on:

```text
1100 * XFS block size
```

The final recovered filesystem block may contain padding beyond the original EOF.

The utility reports two separate measurements:

```text
estimated logical size
```

and:

```text
extent data to copy
```

The first represents the reconstructed logical file span.

The second represents the amount of physical extent data that will actually be read from the XFS source.

---

# Extent sanity checks

Deleted inode forks can contain stale bytes.

Some stale 128-bit values can accidentally resemble valid XFS extent records. Without validation this can produce absurd results such as:

```text
estimated size: 36.94 PiB
data to copy:   20.27 GiB
```

or physically impossible layouts where:

```text
extent data > logical file span
```

The native implementation performs additional validation before accepting a candidate.

## Duplicate logical offsets

Multiple decoded records beginning at the same logical block are collapsed to one logical extent position.

This prevents duplicate stale extent records from being counted repeatedly.

## Overlapping logical extents

If surviving decoded extents overlap after normalization, the candidate is rejected.

The dry-run summary reports:

```text
rejected overlapping layouts
```

## Oversize sparse reconstruction

By default, a candidate whose reconstructed logical size is larger than the entire XFS data device is rejected.

This catches stale metadata that otherwise creates petabyte-scale sparse files from a filesystem many orders of magnitude smaller.

The summary reports:

```text
rejected oversize sparse
```

For the unusual case of a legitimately enormous sparse file, this check can be disabled with:

```bash
--allow-oversize-sparse
```

Example:

```bash
./xfs_undelete_native \
  --allow-oversize-sparse \
  --dry-run \
  /dev/sdh1
```

Use this only when you have a specific reason to expect extremely large sparse files.

---

# Filtering

Filter syntax follows the practical behavior of `xfs_undelete`.

A filter is a comma-separated list of shell-style patterns.

Patterns containing `/` are matched against the MIME type.

Patterns without `/` are matched against the detected extension.

Always quote wildcard expressions so the shell does not expand them.

---

## Recover only videos

```bash
-r 'video/*'
```

Example:

```bash
./xfs_undelete_native \
  -r 'video/*' \
  /dev/sdh1
```

---

## Recover only images

```bash
-r 'image/*'
```

---

## Recover audio and video

```bash
-r 'audio/*,video/*'
```

---

## Recover selected extensions

```bash
-r 'mp4,mkv,avi,mov'
```

---

## Mixed MIME and extension filters

```bash
-r 'image/*,video/*,pdf,docx'
```

---

## Ignore file types

Default:

```text
bin
```

Override it with:

```bash
-i 'bin,tmp'
```

or MIME patterns:

```bash
-i 'application/octet-stream'
```

---

# Command-line options

## `-t RANGE`

Filter by deletion/ctime.

Examples:

```bash
-t 2026-11-23
```

```bash
-t -2hours
```

```bash
-t '2026-11-23..'
```

```bash
-t '..2026-11-23-16-00'
```

```bash
-t '2026-11-23-12-00..2026-11-23-17-00'
```

Supported relative units include seconds, minutes, hours, days, and weeks.

---

## `-T RANGE`

Filter by modification/mtime.

Uses the same range syntax as `-t`.

Example:

```bash
-T -7days
```

---

## `-r TYPES`

Recover only matching MIME types or extensions.

Default:

```text
*
```

Examples:

```bash
-r 'video/*'
```

```bash
-r 'image/*,pdf,docx'
```

```bash
-r 'mp4,mkv,avi,mov'
```

---

## `-i TYPES`

Ignore matching MIME types or extensions.

Default:

```text
bin
```

Example:

```bash
-i 'bin'
```

---

## `-x INODES`

Ignore specific inode numbers.

Example:

```bash
-x 123456,123457,123900
```

---

## `-S SIZE`

Ignore candidates whose estimated logical size exceeds the specified value.

Examples:

```bash
-S 500M
```

```bash
-S 10GiB
```

Supported units include decimal and binary size suffixes such as:

```text
k
M
G
T
KiB
MiB
GiB
TiB
```

---

## `-z TYPES`

Trim trailing zero padding from matching recovered files.

Default:

```text
text/*
```

Example:

```bash
-z 'text/*,svg'
```

Because the exact deleted EOF is normally unavailable, this can improve recovered plain-text files that otherwise end with zero padding.

---

## `-o DIR`

Set the recovery destination.

Default:

```text
xfs_undeleted
```

Example:

```bash
-o /mnt/disks/SanDisk_Ultra_USB_3.0/
```

---

## `-s INODE`

Skip recovery traversal until the exact inode is encountered.

Useful when resuming a previous run.

Example:

```bash
-s 12890064563
```

---

## `-m MAGICFILES`

Pass a custom magic database path to `file(1)`.

Example:

```bash
-m /usr/share/misc/magic.mgc
```

---

## `-l`

Display filetype/filter information and exit.

```bash
./xfs_undelete_native -l
```

---

## `-n`, `--dry-run`

Scan, filter, validate, and estimate candidates without recovering file data.

```bash
./xfs_undelete_native --dry-run /dev/sdh1
```

---

## `--force-rw`

Normally the utility refuses to scan a block device if Linux reports that device as mounted read/write.

This option disables that safety check:

```bash
./xfs_undelete_native --force-rw /dev/sdh1
```

The source is **still opened `O_RDONLY`**.

This option only permits recovery while another mount may be writing to the same filesystem.

That is dangerous for data recovery and is not recommended.

---

## `--no-remount-readonly`

Compatibility alias for:

```text
--force-rw
```

The native program does not actually remount the source.

This option exists for command-line compatibility only.

---

## `--allow-oversize-sparse`

Allow reconstructed logical sizes larger than the entire XFS data device.

Disabled by default.

Use only when recovering a known legitimately enormous sparse file.

---

## `-q`, `--quiet-scan`

Suppress allocation-group scanning progress.

---

## `-h`, `--help`

Display command-line help.

---

# Recovery progress

Before copying each candidate:

```text
Recovering inode 123456 -> /recovery/2026-11-23-15-07_123456.mp4
  estimated logical size: 712.53 MiB
  extent data to copy:    712.53 MiB
```

During recovery:

```text
file               43.67%  311.15 MiB / 712.53 MiB  126.8 MiB/s  ETA 3s
```

Overall copied progress is shown between files.

---

# Sparse files

Recovered files are created at their estimated logical size and surviving extents are written to their original logical offsets.

Missing logical regions remain sparse holes.

Conceptually:

```text
XFS source

extent A ─────────────┐
                      │
extent B ────────┐    │
                 │    │
                 v    v

destination file

[ extent A ][ hole ][ extent B ]
```

This avoids physically writing zeros for missing logical regions.

---

# Source vs destination I/O

The recovery path is intentionally separated:

```text
XFS source block device / image
            |
            | open(..., O_RDONLY)
            |
          pread()
            |
            v
          buffer
            |
          pwrite()
            |
            v
recovery destination filesystem
```

`pwrite()` operates only on the recovered destination file.

It is never called with the XFS source descriptor.

---

# Recommended workflow

## 1. Stop writes to the XFS filesystem

Unmount it if possible:

```bash
umount /dev/sdh1
```

Do not mount it read/write again before recovery.

---

## 2. Run a dry scan

For example:

```bash
./xfs_undelete_native \
  --dry-run \
  -t 2026-11-23 \
  -r 'video/*' \
  /dev/sdh1
```

Review:

- candidate count
- MIME types
- extensions
- estimated sizes
- physical copy sizes
- extent counts
- rejected bogus layouts

---

## 3. Recover to another device

```bash
./xfs_undelete_native \
  -t 2026-11-23 \
  -r 'video/*' \
  -o /mnt/disks/SanDisk_Ultra_USB_3.0/ \
  /dev/sdh1
```

Make sure:

```text
/mnt/disks/SanDisk_Ultra_USB_3.0/
```

is not located on:

```text
/dev/sdh1
```

---

# Working from a disk image

For important data, consider creating a block-level image first.

Using GNU ddrescue:

```bash
ddrescue -f -n \
  /dev/sdh1 \
  xfs-recovery.img \
  xfs-recovery.map
```

Then recover from the image:

```bash
./xfs_undelete_native \
  --dry-run \
  -r 'video/*' \
  xfs-recovery.img
```

Actual recovery:

```bash
./xfs_undelete_native \
  -r 'video/*' \
  -o ./recovered \
  xfs-recovery.img
```

---

# GitHub Actions release build

The repository can use the included release workflow to build a static x86-64 executable using Alpine/musl.

A version tag such as:

```bash
git tag v1.0.0
git push origin v1.0.0
```

can trigger a release build.

The workflow can also be started manually from:

```text
GitHub → Actions → Build and Release → Run workflow
```

Release assets include:

```text
xfs_undelete_native-linux-x86_64
xfs_undelete_native-linux-x86_64.sha256
```

---

# Limitations

Deleted-file recovery cannot recreate information that has already been overwritten.

Potential limitations include:

- original filename may be unavailable
- original directory path may be unavailable
- exact original EOF may be unavailable
- some extents may already have been reallocated
- surviving inode fork data may be incomplete
- fragmented files may recover only partially
- file signatures can be misidentified
- deleted metadata can contain stale extent-looking values
- filesystem activity after deletion reduces recovery probability

The sanity checks intentionally prefer rejecting suspicious extent layouts over creating absurd sparse files from obviously stale metadata.

---

# License

This project is inspired by and implements recovery concepts from `xfs_undelete`.

The source currently identifies itself as:

```text
GPL-3.0-or-later
```

When publishing or distributing the repository, include the appropriate GPL license text as:

```text
LICENSE
```

---

# Disclaimer

Filesystem recovery is inherently best-effort.

There is no guarantee that a candidate inode still points entirely to the original deleted file or that recovered data will be complete and uncorrupted.

For important data:

1. stop writes immediately
2. preserve the source
3. work from an image when possible
4. recover to another filesystem
