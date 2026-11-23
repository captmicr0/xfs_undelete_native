# xfs_undelete_native

A standalone native C utility for recovering deleted files from an XFS filesystem.

It is inspired by the behavior of `xfs_undelete`, but does not require Tcl, Python, `dd`, or other runtime interpreters. It reads XFS metadata directly, estimates recoverable file sizes **before any file data is copied**, and displays copy progress during recovery.

## Important safety note

This program opens the XFS source filesystem/device **read-only**.

The source is opened with:

```c
open(source, O_RDONLY | O_CLOEXEC)
```

All source access uses `pread()`.

The program never uses `write()`, `pwrite()`, `ftruncate()`, or any other write operation on the XFS source device.

`pwrite()` is used only on newly created recovery files in the destination directory.

For deleted-file recovery, the source filesystem should ideally be:

- unmounted, or
- mounted read-only

Do **not** recover files back onto the same filesystem you are recovering from. Doing so can overwrite deleted data that may still be recoverable.

---

## Features

- Native C implementation
- No Tcl or Python runtime required
- No `dd` subprocesses
- Reads the XFS source device read-only
- Metadata discovery pass before recovery begins
- Estimates each recoverable file's size before copying
- Shows total estimated recovery size before copying
- Shows actual extent bytes that need to be read
- Per-file copy percentage
- Copy speed
- ETA
- Overall recovery progress
- Sparse-file support
- Basic file signature detection
- Filter by deletion/ctime range
- Filter by modification/mtime range
- Maximum estimated file size filter
- Start/restart from a specific inode
- Exclude specific inode numbers
- Dry-run mode for scanning without recovery
- Refuses to operate on a source block device mounted read/write unless explicitly overridden
- Refuses to write recovered files back to the same source block device

---

## Limitations

### Exact deleted file size

XFS normally destroys the original EOF/file size when an inode is deleted.

Because of this, the program cannot generally determine the exact original byte length of a deleted file.

Instead, the estimated logical size is calculated from the end of the highest surviving valid extent.

For example, if the final surviving extent occupies:

```text
blocks 1000 through 1099
```

then the estimated file size will end at the end of block 1099.

This means the final recovered block can contain zero padding or stale bytes beyond the original EOF.

The program reports this value as:

```text
estimated logical size
```

It separately reports:

```text
extent data to copy
```

which is the number of physical extent bytes that will actually be read from the XFS source.

### File names

Deleted XFS inode metadata generally does not preserve the original directory entry and filename in a way that can reliably be associated with the recovered inode.

Recovered files are therefore named using their timestamp and inode number, for example:

```text
2026-11-23-15-07_279636917.mp4
```

### File type detection

The utility includes lightweight built-in signature detection for common file formats.

It is not intended to replace `libmagic`/`file`, so unknown files may be recovered with a `.bin` extension.

---

# Building

## Recommended: static build using Docker

A statically linked musl build is recommended when compiling on Debian/Ubuntu but running on Slackware or Unraid.

From the directory containing:

```text
xfs_undelete_native.c
```

run:

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

The compiled executable will appear in the current directory:

```text
xfs_undelete_native
```

Verify that it is statically linked:

```bash
file xfs_undelete_native
ldd xfs_undelete_native
```

Expected output should include something similar to:

```text
ELF 64-bit LSB executable, x86-64, statically linked
```

and:

```text
not a dynamic executable
```

This build is suitable for x86-64 Slackware/Unraid systems without needing matching host libraries.

---

## Build directly on Linux

Using GCC:

```bash
gcc -O2 -std=gnu11 \
  -D_FILE_OFFSET_BITS=64 \
  -Wall -Wextra -Wpedantic \
  -o xfs_undelete_native \
  xfs_undelete_native.c
```

For a static build with musl on Debian/Ubuntu:

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

# Usage

```text
xfs_undelete_native [options] DEVICE_OR_IMAGE
```

Example:

```bash
sudo ./xfs_undelete_native \
  -o /mnt/disks/recovery/recovered \
  /dev/sdg1
```

The source may also be a raw filesystem image:

```bash
./xfs_undelete_native \
  -o ./recovered \
  xfs_partition.img
```

---

# Dry run / size estimation

To scan deleted inode metadata and estimate file sizes without copying any file data:

```bash
sudo ./xfs_undelete_native \
  --dry-run \
  /dev/sdg1
```

Example output:

```text
Phase 1/2: scanning metadata and estimating sizes
(NO file data is being copied)...

Pre-copy estimate complete.

  recoverable candidates:       37
  estimated logical output:     18.42 GiB
  actual extent bytes to copy:  17.91 GiB
  inode records inspected:      125849

Candidate sizes (known BEFORE copying):

  [1/37] inode=123456
         est=712.53 MiB
         data=712.53 MiB
         extents=3
         type=.mp4

  [2/37] inode=123981
         est=388.41 MiB
         data=388.40 MiB
         extents=7
         type=.asf
```

No recovered output files are created when `--dry-run` is used.

---

# Recovery progress

During recovery, the utility displays the estimated size before it starts copying that file:

```text
Recovering inode 123456
  estimated logical size: 712.53 MiB
  extent data to copy:    712.53 MiB
```

Progress is then displayed while the extents are copied:

```text
file               43.67%  311.15 MiB / 712.53 MiB  126.8 MiB/s  ETA 3s
```

Overall recovery progress is also shown between files.

---

# Command-line options

```text
-o, --output DIR
```

Destination directory for recovered files.

Default:

```text
xfs_undeleted
```

Example:

```bash
-o /mnt/disks/recovery/files
```

---

```text
-s, --start-inode N
```

Ignore candidate inodes below the specified inode number.

Useful when restarting a recovery operation.

Example:

```bash
--start-inode 279636917
```

---

```text
-S, --max-size SIZE
```

Ignore files whose estimated logical size exceeds the specified size.

Supported suffix examples:

```text
500M
2G
750MiB
4GiB
```

Example:

```bash
--max-size 10GiB
```

---

```text
-x, --exclude LIST
```

Skip specific inode numbers.

Provide a comma-separated list:

```bash
--exclude 12345,12346,12350
```

---

```text
-t, --deleted RANGE
```

Filter by inode ctime/deletion-time range.

Examples:

```bash
--deleted -2hours
```

```bash
--deleted 2026-11-23..
```

```bash
--deleted ..2026-11-23-16-00
```

```bash
--deleted 2026-11-23-12-00..2026-11-23-17-00
```

---

```text
-T, --modified RANGE
```

Filter by modification time.

Uses the same range format as `--deleted`.

Example:

```bash
--modified -7days
```

---

```text
-n, --dry-run
```

Scan and estimate recoverable files without creating output files.

Recommended before a large recovery operation.

---

```text
--force-rw
```

By default, the program refuses to scan a block device if it appears to be mounted read/write.

This option overrides that protection.

Using it is generally **not recommended** because filesystem activity can overwrite deleted data.

The program still opens the source itself with `O_RDONLY`; this option only disables the mounted-read/write safety check.

---

```text
--no-trim-text
```

Disable trimming of trailing zero padding on files detected as plain text.

---

```text
-q, --quiet-scan
```

Suppress allocation-group scan progress.

---

```text
-h, --help
```

Display command-line help.

---

# Recommended recovery workflow

First stop activity on the source filesystem.

Unmount it if possible:

```bash
umount /dev/sdg1
```

Or ensure it is mounted read-only.

Create a destination on a **different filesystem/device**.

Then perform a dry run:

```bash
sudo ./xfs_undelete_native \
  --dry-run \
  /dev/sdg1
```

Review the estimated files and total recovery size.

Then perform the actual recovery:

```bash
sudo ./xfs_undelete_native \
  -o /mnt/disks/RECOVERY/recovered \
  /dev/sdg1
```

---

# Example on Unraid

Assume:

```text
Source XFS partition:
/dev/sdg1

Recovery destination:
/mnt/disks/WINXPPX86IE/recovered
```

First estimate:

```bash
./xfs_undelete_native \
  --dry-run \
  /dev/sdg1
```

Then recover:

```bash
./xfs_undelete_native \
  -o /mnt/disks/WINXPPX86IE/recovered \
  /dev/sdg1
```

Make absolutely sure `/mnt/disks/WINXPPX86IE` is **not located on `/dev/sdg1`**.

---

# How recovery works

At a high level, the utility:

1. Opens the XFS source read-only.
2. Reads the XFS superblock.
3. Walks allocation groups and inode allocation B+trees.
4. Locates inode records representing deleted files.
5. Examines surviving XFS extent records.
6. Validates physical and logical extent addresses.
7. Calculates the highest surviving logical extent.
8. Estimates the resulting logical file size.
9. Determines the total amount of physical extent data that must be copied.
10. Displays all estimates.
11. Creates recovery files on the destination filesystem.
12. Reads extent data from the XFS source using `pread()`.
13. Writes those bytes to the destination files using `pwrite()`.
14. Preserves sparse holes when logical extents are discontinuous.

The source filesystem descriptor is never opened writable.

---

# Source vs destination I/O

Source:

```text
XFS filesystem / block device
        |
        | O_RDONLY
        v
     pread()
        |
        v
      buffer
```

Destination:

```text
      buffer
        |
        v
     pwrite()
        |
        v
recovered output file
```

`pwrite()` does **not** operate on the XFS source.

---

# Recovery precautions

Deleted-file recovery is inherently destructive if the source filesystem continues receiving writes.

For the best chance of recovery:

- stop all writes to the filesystem immediately
- unmount the filesystem if possible
- never run filesystem repair unless necessary before recovery
- never recover files onto the same filesystem
- avoid mounting the source read/write
- ideally work from a block-level clone or disk image

For valuable or irreplaceable data, create an image first and perform recovery against the image.

Example using `ddrescue`:

```bash
ddrescue -f -n /dev/sdg1 xfs-recovery.img xfs-recovery.map
```

Then run:

```bash
./xfs_undelete_native \
  -o ./recovered \
  xfs-recovery.img
```

---

# License

This project is based on recovery concepts and behavior from the `xfs_undelete` project.

If distributing a derivative implementation that incorporates GPL-licensed code or a translation of that code, the resulting project must comply with the applicable GPL license terms.

The source file currently identifies itself as:

```text
GPL-3.0-or-later
```

Include the appropriate `LICENSE` file in the repository when publishing or distributing the project.

---

# Disclaimer

This software operates directly on filesystem metadata and is intended for data-recovery use.

There is no guarantee that deleted files are still intact, that all extents belong to the original file, or that recovered files will be complete or uncorrupted.

Always recover to another filesystem and preserve the original source whenever the data is important.
