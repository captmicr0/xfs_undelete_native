# xfs_undelete_native

A native C deleted-file recovery utility for XFS, inspired by [`xfs_undelete`](https://github.com/ianka/xfs_undelete).

It reads XFS metadata directly, performs a complete discovery/estimation pass before copying data, and adds detailed recovery progress tracking.

Key goals:

- standalone native executable
- source filesystem opened read-only
- pre-copy file-size estimation
- per-file copy progress
- transfer speed and ETA
- overall recovery progress
- MIME/extension filtering
- stronger validation of stale deleted-inode extent metadata
- explicit diagnostic/force-recovery controls for suspicious candidates

> **The XFS source is always opened read-only.**
>
> `xfs_undelete_native` never writes to or remounts the source filesystem.

---

# Safety

Deleted-file recovery should be performed with as little activity on the source filesystem as possible.

The XFS source is opened with:

```c
open(source, O_RDONLY | O_CLOEXEC)
```

Source data is read with:

```c
pread()
```

Recovered files are created on the destination filesystem and written with:

```c
pwrite()
```

The source descriptor is never passed to `write()`, `pwrite()`, `ftruncate()`, `fsync()`, or another write operation.

## Recommended setup

Prefer one of these:

- source filesystem unmounted
- source filesystem mounted read-only
- recovery performed from a block-level image

Always recover to a **different filesystem/device**.

Do not recover files onto the filesystem being recovered. New writes can overwrite deleted data that may still be recoverable.

---

# Features

- Native C implementation
- Reads XFS metadata directly
- Source device opened `O_RDONLY`
- No Tcl runtime
- No Python runtime
- No `dd` subprocess for recovery
- Works with block devices and raw XFS filesystem images
- Metadata discovery pass before recovery
- Estimated logical file size before copying
- Total estimated recovery size
- Actual physical extent bytes to copy
- Per-file copy percentage
- Transfer speed
- ETA
- Overall copy progress
- Sparse-file reconstruction
- MIME-type filtering
- Extension filtering
- MIME wildcards such as `video/*`
- Ignore filters
- Deletion/ctime filtering
- Modification/mtime filtering
- Maximum-size filtering
- Specific inode exclusions
- Resume/start at an exact inode
- Trailing-zero trimming
- Custom `file(1)` magic database support
- Dry-run mode
- Extent-overlap validation
- Oversize sparse-file sanity validation
- Diagnostic mode for viewing rejected candidates
- Exact-inode forced recovery mode
- Refuses a source block device mounted read/write unless explicitly overridden
- Refuses to recover onto the source block device

---

# Building

## Recommended: static x86-64 build using Docker

For building on Debian/Ubuntu or another Linux system and running on Slackware/Unraid, a statically linked musl build is recommended.

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

The executable will appear as:

```text
xfs_undelete_native
```

Verify that it is static:

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

For full MIME detection, the program uses the system:

```text
file
```

utility and its magic database.

Check availability with:

```bash
file --version
```

If `file` is unavailable or cannot identify the data, built-in signature detection is used for common formats including:

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

The recovery executable itself can still be fully statically linked.

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

Raw filesystem images can also be used:

```bash
./xfs_undelete_native \
  -o ./recovered \
  xfs-recovery.img
```

---

# Typical video-recovery example

Recover videos deleted on November 23, 2026:

```bash
./xfs_undelete_native \
  -t 2026-11-23 \
  -r 'video/*' \
  -o /mnt/disks/SanDisk_Ultra_USB_3.0/ \
  /dev/sdh1
```

Dry run first:

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

A dry run performs:

- XFS metadata scanning
- deleted-inode discovery
- extent decoding
- MIME detection
- filtering
- sanity validation
- size estimation

It does **not** copy recovered file data.

Example:

```bash
./xfs_undelete_native \
  --dry-run \
  -t 2026-11-23 \
  -r 'video/*' \
  /dev/sdh1
```

Example output:

```text
Phase 1/2: scanning metadata and estimating sizes
(NO file data is being copied)...

Pre-copy estimate complete.
  recoverable candidates:        8
  estimated logical output:      25.70 GiB
  actual extent bytes to copy:   25.02 GiB
  inode records inspected:       303360
  rejected overlapping layouts:  6
  rejected oversize sparse:       1
```

Candidates are listed before any copy operation begins:

```text
[1/8] inode=10888791452  est=159.84 MiB  data=159.84 MiB  extents=1  type=.mp4
         mime=video/mp4
```

---

# File-size estimation

XFS normally destroys the exact original EOF/file length when an inode is deleted.

Because of that, the exact original byte size usually cannot be recovered from deleted inode metadata.

The program estimates logical size from the end of the highest surviving valid logical extent.

The utility reports:

```text
estimated logical size
```

and separately:

```text
extent data to copy
```

The first is the reconstructed logical span.

The second is the amount of physical XFS data that will actually be read.

Sparse holes therefore increase logical size without increasing physical bytes copied.

---

# Extent sanity checks

Deleted inode forks can contain stale data.

Some stale 128-bit values can accidentally decode into values that look like XFS extent records.

Without validation, this can produce impossible or absurd candidates such as:

```text
estimated logical size: 36.94 PiB
extent data to copy:    20.27 GiB
```

or:

```text
estimated logical size: 587 MiB
extent data to copy:    739 MiB
```

The program performs additional sanity validation before accepting candidates.

## Duplicate logical offsets

Exact duplicate stale extent records are collapsed during normal recovery validation.

## Overlapping logical extents

Conflicting or overlapping logical extents are rejected.

The dry-run summary reports:

```text
rejected overlapping layouts
```

## Oversize sparse candidates

By default, a reconstructed logical file larger than the entire XFS data device is rejected.

This catches stale metadata that would otherwise generate absurd sparse files.

The dry-run summary reports:

```text
rejected oversize sparse
```

---

# Inspecting rejected candidates

Sometimes a rejected candidate may still contain recoverable data.

For diagnosis, use:

```bash
--ignore-checks
```

This option is intentionally restricted to **dry-run mode only**.

Example:

```bash
./xfs_undelete_native \
  --dry-run \
  --ignore-checks \
  -t 2026-11-23 \
  -r 'video/*' \
  /dev/sde1
```

This lists candidates that would normally be rejected by the overlap/oversize sanity checks.

No file data is copied.

If `--ignore-checks` is used without `--dry-run`, the program refuses to run:

```text
REFUSING: --ignore-checks is diagnostic-only and may only be used with --dry-run.
To intentionally recover one rejected inode, use --force-inode INODE instead.
```

This separation is intentional:

1. use `--ignore-checks --dry-run` to discover the inode
2. inspect the candidate
3. explicitly force only that inode if desired

---

# Forced recovery of one inode

To intentionally recover a candidate that failed optional sanity validation, use:

```bash
--force-inode INODE
```

Example:

```bash
./xfs_undelete_native \
  --force-inode 10887782598 \
  -o /mnt/disks/SanDisk_Ultra_USB_3.0/ \
  /dev/sde1
```

`--force-inode`:

- considers only the exact inode specified
- bypasses overlapping-logical-extent rejection for that inode
- bypasses oversize-sparse rejection for that inode
- bypasses normal time/type/max-size filtering for that inode
- does **not** bypass physical source-device bounds checks
- still opens the XFS source strictly `O_RDONLY`

This is intentionally safer than a global force mode because only one explicitly selected inode is allowed through.

## Recommended workflow for a rejected inode

First expose rejected candidates:

```bash
./xfs_undelete_native \
  --dry-run \
  --ignore-checks \
  -t 2026-11-23 \
  -r 'video/*' \
  /dev/sde1
```

Suppose the candidate you want is:

```text
inode=10887782598
```

Then recover only it:

```bash
./xfs_undelete_native \
  --force-inode 10887782598 \
  -o /mnt/disks/SanDisk_Ultra_USB_3.0/ \
  /dev/sde1
```

---

# `--allow-oversize-sparse`

For the unusual case of a legitimately huge sparse file, the normal oversize-sparse sanity check can be disabled:

```bash
--allow-oversize-sparse
```

Example:

```bash
./xfs_undelete_native \
  --dry-run \
  --allow-oversize-sparse \
  /dev/sdh1
```

This does not disable overlap validation.

For a specific suspicious inode, prefer:

```bash
--force-inode INODE
```

instead of broadly disabling sanity checks.

---

# Filtering

Filtering follows the practical behavior of `xfs_undelete`.

A filter is a comma-separated list of shell-style patterns.

Patterns containing `/` match MIME types.

Patterns without `/` match detected extensions.

Quote wildcard expressions so the shell does not expand them.

---

## Recover only videos

```bash
-r 'video/*'
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

## Ignore types

Default ignored extension:

```text
bin
```

Override with:

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
-x 12890064564
```

Multiple inodes:

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

Supported suffixes include:

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

---

## `-o DIR`

Set recovery destination.

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

Skip traversal until the exact inode is encountered.

Useful when resuming a previous scan/recovery.

Example:

```bash
-s 12890064563
```

This is different from:

```bash
--force-inode
```

`-s` resumes normal traversal and normal sanity checks.

`--force-inode` selects one exact inode and bypasses optional sanity rejection for that inode.

---

## `-m MAGICFILES`

Pass a custom magic database to `file(1)`.

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

Scan, validate, filter, and estimate without copying recovered data.

```bash
./xfs_undelete_native --dry-run /dev/sdh1
```

---

## `--ignore-checks`

Diagnostic mode for displaying candidates rejected by the optional overlap/oversize checks.

**May only be used together with `--dry-run`.**

Example:

```bash
./xfs_undelete_native \
  --dry-run \
  --ignore-checks \
  /dev/sdh1
```

This option can never perform recovery.

---

## `--force-inode INODE`

Recover exactly one inode while bypassing the optional overlap/oversize sanity checks for that inode.

Example:

```bash
./xfs_undelete_native \
  --force-inode 10887782598 \
  -o /mnt/disks/recovery/ \
  /dev/sdh1
```

Physical source bounds checks remain enforced.

---

## `--allow-oversize-sparse`

Permit logical sparse files larger than the entire XFS data device.

This only bypasses the oversize-sparse check.

---

## `--force-rw`

Normally the program refuses to scan a block device reported as mounted read/write.

This option disables that protection:

```bash
./xfs_undelete_native \
  --force-rw \
  /dev/sdh1
```

The source is still opened `O_RDONLY`.

This is not recommended for normal data recovery.

---

## `-q`, `--quiet-scan`

Suppress allocation-group scan progress.

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

Overall copied progress is displayed between files.

---

# Sparse files

Recovered files are created at their estimated logical size and surviving extents are written to their logical offsets.

Missing logical regions remain sparse holes.

Conceptually:

```text
source extent A ─────────────┐
                             │
source extent B ────────┐    │
                        │    │
                        v    v

destination:

[ extent A ][ hole ][ extent B ]
```

The program does not physically write zeros for sparse holes.

---

# Source vs destination I/O

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

`pwrite()` is used only for recovery output files.

It is never used on the source descriptor.

---

# Recommended recovery workflow

## 1. Stop writes to the source

Unmount if possible:

```bash
umount /dev/sdh1
```

---

## 2. Run a normal dry scan

```bash
./xfs_undelete_native \
  --dry-run \
  -t 2026-11-23 \
  -r 'video/*' \
  /dev/sdh1
```

Review:

- candidate count
- MIME type
- extension
- estimated size
- physical data size
- extent count
- rejected overlap count
- rejected oversize-sparse count

---

## 3. Recover normal candidates

```bash
./xfs_undelete_native \
  -t 2026-11-23 \
  -r 'video/*' \
  -o /mnt/disks/SanDisk_Ultra_USB_3.0/ \
  /dev/sdh1
```

---

## 4. Inspect suspicious rejected candidates if necessary

```bash
./xfs_undelete_native \
  --dry-run \
  --ignore-checks \
  -t 2026-11-23 \
  -r 'video/*' \
  /dev/sdh1
```

---

## 5. Force only a specific rejected inode if desired

```bash
./xfs_undelete_native \
  --force-inode 10887782598 \
  -o /mnt/disks/SanDisk_Ultra_USB_3.0/ \
  /dev/sdh1
```

This limits the dangerous override to one explicitly selected inode.

---

# Working from a filesystem image

For valuable data, consider making a block-level image first.

Using GNU ddrescue:

```bash
ddrescue -f -n \
  /dev/sdh1 \
  xfs-recovery.img \
  xfs-recovery.map
```

Then scan the image:

```bash
./xfs_undelete_native \
  --dry-run \
  -r 'video/*' \
  xfs-recovery.img
```

Recover from the image:

```bash
./xfs_undelete_native \
  -r 'video/*' \
  -o ./recovered \
  xfs-recovery.img
```

---

# GitHub Actions release build

The repository can use the release workflow to build a static x86-64 executable using Alpine/musl.

A version tag such as:

```bash
git tag v1.0.0
git push origin v1.0.0
```

can trigger a release build.

The workflow can also be started manually from:

```text
GitHub
→ Actions
→ Build and Release
→ Run workflow
```

Typical release assets:

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
- some extents may have been reallocated
- fragmented files may recover partially
- stale inode-fork bytes can resemble valid extent records
- file signatures can be misidentified
- recovered output may contain padding or stale tail bytes
- continued filesystem activity reduces recovery probability

The default sanity checks intentionally prefer rejecting suspicious mappings over automatically creating absurd or conflicting reconstructed files.

`--ignore-checks` and `--force-inode` exist for cases where the operator intentionally wants to inspect or attempt recovery of one of those suspicious candidates.

---

# License

This project is inspired by and implements recovery concepts from `xfs_undelete`.

The source currently identifies itself as:

```text
GPL-3.0-or-later
```

When distributing the repository, include the appropriate GPL license text as:

```text
LICENSE
```

---

# Disclaimer

Filesystem recovery is inherently best-effort.

There is no guarantee that a candidate inode still points entirely to the original deleted file or that recovered output will be complete or uncorrupted.

For important data:

1. stop writes immediately
2. preserve the source
3. work from an image when practical
4. recover to another filesystem
5. treat forced recovery of rejected inode layouts as experimental
