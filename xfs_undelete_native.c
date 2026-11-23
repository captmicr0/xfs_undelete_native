/*
 * xfs_undelete_native.c
 *
 * Native C recovery utility inspired by Jan Kandziora's xfs_undelete.
 * Core on-disk traversal/extent recovery behavior follows xfs_undelete's
 * published algorithm. This source is distributed under GPL-3.0-or-later.
 *
 * Differences / additions:
 *   - No Tcl/Python interpreter and no dd subprocess.
 *   - Metadata discovery pass happens BEFORE any recovered file is written.
 *   - Prints estimated logical size and recoverable extent bytes before copy.
 *   - Per-file and overall byte/progress/speed/ETA displays.
 *   - Sparse holes are preserved with ftruncate()+pwrite().
 *   - Built-in lightweight file signature detection (no libmagic required).
 *   - Never remounts the source. It opens the source read-only and warns/refuses
 *     if the same block device appears mounted read-write unless --force-rw.
 *
 * IMPORTANT:
 *   XFS normally destroys the exact EOF size when an inode is deleted.
 *   "Estimated logical size" is therefore the end of the highest surviving
 *   written extent, rounded naturally to XFS blocks -- same fundamental estimate
 *   used by the Tcl implementation. The final block can contain zero padding.
 *
 * Build:
 *   gcc -O2 -std=gnu11 -Wall -Wextra -Wpedantic -D_FILE_OFFSET_BITS=64 \
 *       -o xfs_undelete_native xfs_undelete_native.c
 *
 * Static-ish musl build (Alpine):
 *   docker run --rm -v "$PWD":/src -w /src alpine:3.22 sh -c \
 *     'apk add --no-cache build-base musl-dev && gcc -O2 -static -std=gnu11 \
 *      -D_FILE_OFFSET_BITS=64 -Wall -Wextra -o xfs_undelete_native xfs_undelete_native.c'
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define XFS_SB_MAGIC "XFSB"
#define MAX_EXTENTS_PER_INODE 64
#define COPY_BUFSIZE (4u * 1024u * 1024u)

typedef struct {
    uint64_t logical_block;
    uint64_t physical_block;
    uint32_t block_count;
} Extent;

typedef struct {
    uint64_t inode;
    int64_t ctime_sec;
    int64_t mtime_sec;
    Extent *extents;
    size_t extent_count;
    uint64_t estimated_size;
    uint64_t data_bytes;
    char extension[64];
    char mimetype[128];
} Candidate;

typedef struct {
    int fd;
    const char *source;
    const char *out_dir;

    uint32_t blocksize;
    uint64_t dblocks;
    uint32_t agblocks;
    uint32_t agcount;
    uint16_t versionnum;
    uint16_t sectsize;
    uint16_t inodesize;
    uint16_t inopblock;
    uint8_t agblklog;
    uint64_t icount;
    bool bigtime;

    uint64_t start_inode;
    bool have_start_inode;
    uint64_t max_size;
    bool have_max_size;
    uint64_t *excluded;
    size_t excluded_count;

    int64_t ctime_min, ctime_max;
    int64_t mtime_min, mtime_max;

    Candidate *candidates;
    size_t candidate_count;
    size_t candidate_cap;

    uint64_t visited_inodes;
    uint64_t total_estimated_size;
    uint64_t total_data_bytes;
    uint64_t rejected_overlapping;
    uint64_t rejected_oversize_sparse;

    bool dry_run;
    bool force_rw;
    bool quiet_scan;

    const char *recover_patterns;
    const char *ignore_patterns;
    const char *zero_patterns;
    const char *magicfiles;
    bool list_types;
    bool passed_start;
    bool allow_oversize_sparse;
} Context;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static int pread_full(int fd, void *buf, size_t n, uint64_t off) {
    uint8_t *p = buf;
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(fd, p + done, n - done, (off_t)(off + done));
        if (r == 0) return -1;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)r;
    }
    return 0;
}

static int pwrite_full(int fd, const void *buf, size_t n, uint64_t off) {
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < n) {
        ssize_t w = pwrite(fd, p + done, n - done, (off_t)(off + done));
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

static const char *human_size(uint64_t n, char out[32]) {
    static const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = (double)n;
    size_t i = 0;
    while (v >= 1024.0 && i + 1 < sizeof(u)/sizeof(u[0])) {
        v /= 1024.0;
        ++i;
    }
    if (i == 0) snprintf(out, 32, "%" PRIu64 " %s", n, u[i]);
    else snprintf(out, 32, "%.2f %s", v, u[i]);
    return out;
}

static bool parse_size(const char *s, uint64_t *out) {
    if (!s || !*s) return false;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s) return false;
    uint64_t mult = 1;
    if (*end) {
        if (!strcasecmp(end, "k") || !strcasecmp(end, "kb")) mult = 1000ull;
        else if (!strcasecmp(end, "m") || !strcasecmp(end, "mb")) mult = 1000ull*1000;
        else if (!strcasecmp(end, "g") || !strcasecmp(end, "gb")) mult = 1000ull*1000*1000;
        else if (!strcasecmp(end, "t") || !strcasecmp(end, "tb")) mult = 1000ull*1000*1000*1000;
        else if (!strcasecmp(end, "kib")) mult = 1024ull;
        else if (!strcasecmp(end, "mib")) mult = 1024ull*1024;
        else if (!strcasecmp(end, "gib")) mult = 1024ull*1024*1024;
        else if (!strcasecmp(end, "tib")) mult = 1024ull*1024*1024*1024;
        else return false;
    }
    if (v > UINT64_MAX / mult) return false;
    *out = (uint64_t)v * mult;
    return true;
}

static bool parse_one_time(const char *s, int64_t *out) {
    if (!strcmp(s, "epoch")) { *out = 0; return true; }
    if (!strcmp(s, "now")) { *out = (int64_t)time(NULL); return true; }

    char *end = NULL;
    errno = 0;
    long long direct = strtoll(s, &end, 10);
    if (!errno && end != s && *end == '\0') {
        *out = direct;
        return true;
    }

    if (s[0] == '-' && s[1]) {
        errno = 0;
        double n = strtod(s + 1, &end);
        if (!errno && end != s + 1) {
            double mult = 0;
            if (!strcasecmp(end, "s") || !strcasecmp(end, "sec") || !strcasecmp(end, "secs") ||
                !strcasecmp(end, "second") || !strcasecmp(end, "seconds")) mult = 1;
            else if (!strcasecmp(end, "m") || !strcasecmp(end, "min") || !strcasecmp(end, "mins") ||
                     !strcasecmp(end, "minute") || !strcasecmp(end, "minutes")) mult = 60;
            else if (!strcasecmp(end, "h") || !strcasecmp(end, "hr") || !strcasecmp(end, "hrs") ||
                     !strcasecmp(end, "hour") || !strcasecmp(end, "hours")) mult = 3600;
            else if (!strcasecmp(end, "d") || !strcasecmp(end, "day") || !strcasecmp(end, "days")) mult = 86400;
            else if (!strcasecmp(end, "w") || !strcasecmp(end, "week") || !strcasecmp(end, "weeks")) mult = 604800;
            if (mult > 0) {
                *out = (int64_t)time(NULL) - (int64_t)(n * mult);
                return true;
            }
        }
    }

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_isdst = -1;
    const char *fmts[] = {
        "%Y-%m-%d-%H-%M-%S", "%Y-%m-%d-%H-%M", "%Y-%m-%d %H:%M:%S",
        "%Y-%m-%d %H:%M", "%Y-%m-%d", NULL
    };
    for (int i = 0; fmts[i]; ++i) {
        memset(&tmv, 0, sizeof(tmv));
        tmv.tm_isdst = -1;
        char *r = strptime(s, fmts[i], &tmv);
        if (r && *r == '\0') {
            time_t t = mktime(&tmv);
            if (t != (time_t)-1) { *out = (int64_t)t; return true; }
        }
    }
    return false;
}

static bool parse_range(const char *s, int64_t *lo, int64_t *hi) {
    if (!s || !*s || !strcmp(s, "..")) {
        *lo = 0; *hi = INT64_MAX; return true;
    }
    const char *dots = strstr(s, "..");
    if (!dots) {
        if (!parse_one_time(s, lo)) return false;
        *hi = INT64_MAX;
        return true;
    }
    char left[128], right[128];
    size_t ln = (size_t)(dots - s);
    if (ln >= sizeof(left) || strlen(dots + 2) >= sizeof(right)) return false;
    memcpy(left, s, ln); left[ln] = 0;
    strcpy(right, dots + 2);
    if (!*left) *lo = 0;
    else if (!parse_one_time(left, lo)) return false;
    if (!*right) *hi = INT64_MAX;
    else if (!parse_one_time(right, hi)) return false;
    return *lo <= *hi;
}

static bool is_excluded(const Context *ctx, uint64_t ino) {
    for (size_t i = 0; i < ctx->excluded_count; ++i)
        if (ctx->excluded[i] == ino) return true;
    return false;
}

static bool add_excluded(Context *ctx, const char *list) {
    char *tmp = strdup(list ? list : "");
    if (!tmp) return false;
    char *save = NULL;
    for (char *p = strtok_r(tmp, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        char *end = NULL;
        errno = 0;
        uint64_t v = strtoull(p, &end, 10);
        if (errno || end == p || *end) { free(tmp); return false; }
        uint64_t *n = realloc(ctx->excluded, (ctx->excluded_count + 1) * sizeof(*n));
        if (!n) { free(tmp); return false; }
        ctx->excluded = n;
        ctx->excluded[ctx->excluded_count++] = v;
    }
    free(tmp);
    return true;
}

static const char *guess_extension(const uint8_t *p, size_t n) {
    if (n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return "png";
    if (n >= 3 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff) return "jpg";
    if (n >= 6 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6))) return "gif";
    if (n >= 5 && !memcmp(p, "%PDF-", 5)) return "pdf";
    if (n >= 4 && !memcmp(p, "PK\x03\x04", 4)) return "zip";
    if (n >= 2 && p[0] == 0x1f && p[1] == 0x8b) return "gz";
    if (n >= 4 && !memcmp(p, "\x7f" "ELF", 4)) return "elf";
    if (n >= 16 && !memcmp(p, "SQLite format 3\0", 16)) return "sqlite";
    if (n >= 4 && !memcmp(p, "OggS", 4)) return "ogg";
    if (n >= 4 && !memcmp(p, "fLaC", 4)) return "flac";
    if (n >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p+8, "WAVE", 4)) return "wav";
    if (n >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p+8, "AVI ", 4)) return "avi";
    if (n >= 16 && !memcmp(p, "\x30\x26\xb2\x75\x8e\x66\xcf\x11\xa6\xd9\x00\xaa\x00\x62\xce\x6c", 16)) return "asf";
    if (n >= 4 && !memcmp(p, "\x1a\x45\xdf\xa3", 4)) return "mkv";
    if (n >= 12 && !memcmp(p+4, "ftyp", 4)) {
        if (!memcmp(p+8, "qt  ", 4)) return "mov";
        return "mp4";
    }
    size_t printable = 0, checked = n < 4096 ? n : 4096;
    for (size_t i = 0; i < checked; ++i) {
        unsigned char c = p[i];
        if (c == 0) return "bin";
        if (c == '\n' || c == '\r' || c == '\t' || (c >= 32 && c < 127)) printable++;
    }
    if (checked && printable * 100 / checked >= 95) return "txt";
    return "bin";
}


static bool pattern_list_match(const char *patterns, const char *mimetype, const char *extension) {
    if (!patterns || !*patterns) return false;
    char *tmp = strdup(patterns);
    if (!tmp) return false;
    bool matched = false;
    char *save = NULL;
    for (char *pat = strtok_r(tmp, ",", &save); pat; pat = strtok_r(NULL, ",", &save)) {
        const char *target = strchr(pat, '/') ? mimetype : extension;
        if (!target) target = "";
        if (fnmatch(pat, target, 0) == 0) { matched = true; break; }
    }
    free(tmp);
    return matched;
}

static void extension_from_mimetype(const char *mime, char out[64]) {
    struct map { const char *mime; const char *ext; };
    static const struct map known[] = {
        {"application/octet-stream","bin"}, {"text/plain","txt"},
        {"image/jpeg","jpg"}, {"image/png","png"}, {"image/gif","gif"},
        {"image/webp","webp"}, {"image/tiff","tif"}, {"image/bmp","bmp"},
        {"image/svg+xml","svg"}, {"application/pdf","pdf"},
        {"application/zip","zip"}, {"application/x-7z-compressed","7z"},
        {"application/x-rar","rar"}, {"application/vnd.rar","rar"},
        {"application/gzip","gz"}, {"application/x-gzip","gz"},
        {"application/x-tar","tar"}, {"application/x-bzip2","bz2"},
        {"application/x-xz","xz"}, {"audio/mpeg","mp3"},
        {"audio/flac","flac"}, {"audio/ogg","ogg"}, {"audio/wav","wav"},
        {"audio/x-wav","wav"}, {"video/mp4","mp4"},
        {"video/quicktime","mov"}, {"video/x-msvideo","avi"},
        {"video/x-matroska","mkv"}, {"video/webm","webm"},
        {"video/mpeg","mpeg"},
        {"application/vnd.openxmlformats-officedocument.wordprocessingml.document","docx"},
        {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet","xlsx"},
        {"application/vnd.openxmlformats-officedocument.presentationml.presentation","pptx"},
        {"application/msword","doc"}, {"application/vnd.ms-excel","xls"},
        {"application/vnd.ms-powerpoint","ppt"},
        {"application/x-sqlite3","sqlite"}, {"application/vnd.sqlite3","sqlite"},
        {NULL,NULL}
    };
    for (size_t i = 0; known[i].mime; ++i) {
        if (!strcmp(mime, known[i].mime)) { snprintf(out, 64, "%s", known[i].ext); return; }
    }
    const char *slash = strchr(mime, '/');
    if (!slash || !slash[1]) { snprintf(out, 64, "bin"); return; }
    const char *sub = slash + 1;
    if (!strncmp(sub, "x-", 2)) sub += 2;
    size_t j = 0;
    for (size_t i = 0; sub[i] && sub[i] != '+' && j + 1 < 64; ++i) {
        unsigned char c = (unsigned char)sub[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.') out[j++] = (char)c;
    }
    out[j] = 0;
    if (!j) snprintf(out, 64, "bin");
}

static int run_file_mime(const uint8_t *data, size_t len, const char *magicfiles, char mime[128]) {
    int inpipe[2], outpipe[2];
    if (pipe(inpipe) < 0) return -1;
    if (pipe(outpipe) < 0) { close(inpipe[0]); close(inpipe[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(inpipe[0]); close(inpipe[1]); close(outpipe[0]); close(outpipe[1]); return -1; }
    if (pid == 0) {
        dup2(inpipe[0], STDIN_FILENO);
        dup2(outpipe[1], STDOUT_FILENO);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) dup2(dn, STDERR_FILENO);
        close(inpipe[0]); close(inpipe[1]); close(outpipe[0]); close(outpipe[1]);
        if (magicfiles && *magicfiles)
            execlp("file", "file", "--brief", "--mime-type", "--magic", magicfiles, "-", (char*)NULL);
        else
            execlp("file", "file", "--brief", "--mime-type", "-", (char*)NULL);
        _exit(127);
    }
    close(inpipe[0]); close(outpipe[1]);
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(inpipe[1], data + off, len - off);
        if (w < 0) { if (errno == EINTR) continue; break; }
        off += (size_t)w;
    }
    close(inpipe[1]);
    char buf[256]; size_t got = 0;
    while (got + 1 < sizeof(buf)) {
        ssize_t r = read(outpipe[0], buf + got, sizeof(buf) - 1 - got);
        if (r == 0) break;
        if (r < 0) { if (errno == EINTR) continue; break; }
        got += (size_t)r;
    }
    close(outpipe[0]);
    int st = 0; waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || !got) return -1;
    while (got && (buf[got-1] == '\n' || buf[got-1] == '\r' || buf[got-1] == ' ' || buf[got-1] == '\t')) --got;
    buf[got] = 0;
    snprintf(mime, 128, "%.127s", buf);
    return 0;
}

static void builtin_identify(const uint8_t *p, size_t n, char mime[128], char ext[64]) {
    const char *e = guess_extension(p, n);
    snprintf(ext, 64, "%s", e);
    if (!strcmp(e,"txt")) snprintf(mime,128,"text/plain");
    else if (!strcmp(e,"jpg")) snprintf(mime,128,"image/jpeg");
    else if (!strcmp(e,"png")) snprintf(mime,128,"image/png");
    else if (!strcmp(e,"gif")) snprintf(mime,128,"image/gif");
    else if (!strcmp(e,"pdf")) snprintf(mime,128,"application/pdf");
    else if (!strcmp(e,"zip")) snprintf(mime,128,"application/zip");
    else if (!strcmp(e,"gz")) snprintf(mime,128,"application/gzip");
    else if (!strcmp(e,"ogg")) snprintf(mime,128,"audio/ogg");
    else if (!strcmp(e,"flac")) snprintf(mime,128,"audio/flac");
    else if (!strcmp(e,"wav")) snprintf(mime,128,"audio/wav");
    else if (!strcmp(e,"avi")) snprintf(mime,128,"video/x-msvideo");
    else if (!strcmp(e,"mkv")) snprintf(mime,128,"video/x-matroska");
    else if (!strcmp(e,"mov")) snprintf(mime,128,"video/quicktime");
    else if (!strcmp(e,"mp4")) snprintf(mime,128,"video/mp4");
    else if (!strcmp(e,"sqlite")) snprintf(mime,128,"application/x-sqlite3");
    else snprintf(mime,128,"application/octet-stream");
}

static void identify_buffer(const Context *ctx, const uint8_t *p, size_t n, char mime[128], char ext[64]) {
    if (run_file_mime(p, n, ctx->magicfiles, mime) == 0) extension_from_mimetype(mime, ext);
    else builtin_identify(p, n, mime, ext);
}

static int list_filetypes(const char *magicfiles) {
    printf("extension    mimetype                                         description\n");
    printf("bin          application/octet-stream                         Binary data\n");
    printf("txt          text/plain                                       Plain Text\n");
    printf("\nOther extensions/MIME types are supplied by the system file(1) magic database.\n");
    printf("Patterns containing '/' match MIME types; other patterns match extensions.\n");
    printf("Examples: video/*, image/*, application/pdf, mp4, mkv, jpg, docx\n");
    if (magicfiles && *magicfiles) printf("Custom magic path: %s\n", magicfiles);
    return 0;
}

static bool decode_extent(const Context *ctx, const uint8_t rec[16], Extent *out) {
    uint64_t hi = be64(rec);
    uint64_t lo = be64(rec + 8);

    bool unwritten = (hi >> 63) != 0;
    if (unwritten) return false;

    uint64_t startoff = (hi >> 9) & ((UINT64_C(1) << 54) - 1);
    uint64_t startblock = ((hi & UINT64_C(0x1ff)) << 43) | (lo >> 21);
    uint32_t count = (uint32_t)(lo & ((UINT64_C(1) << 21) - 1));
    if (count == 0) return false;

    uint64_t agno = startblock >> ctx->agblklog;
    uint64_t agmask = (ctx->agblklog == 64) ? UINT64_MAX : ((UINT64_C(1) << ctx->agblklog) - 1);
    uint64_t agblock = startblock & agmask;

    if (agno >= ctx->agcount || agblock >= ctx->agblocks) return false;
    uint64_t physical = agno * (uint64_t)ctx->agblocks + agblock;
    if (physical >= ctx->dblocks || count > ctx->dblocks - physical) return false;
    if (startoff > (uint64_t)INT64_MAX / ctx->blocksize) return false;

    out->logical_block = startoff;
    out->physical_block = physical;
    out->block_count = count;
    return true;
}

static int extent_cmp(const void *a, const void *b) {
    const Extent *ea = a, *eb = b;
    if (ea->logical_block < eb->logical_block) return -1;
    if (ea->logical_block > eb->logical_block) return 1;
    return 0;
}

static bool add_candidate(Context *ctx, Candidate *c) {
    if (ctx->candidate_count == ctx->candidate_cap) {
        size_t nc = ctx->candidate_cap ? ctx->candidate_cap * 2 : 256;
        Candidate *p = realloc(ctx->candidates, nc * sizeof(*p));
        if (!p) return false;
        ctx->candidates = p;
        ctx->candidate_cap = nc;
    }
    ctx->candidates[ctx->candidate_count++] = *c;
    ctx->total_estimated_size += c->estimated_size;
    ctx->total_data_bytes += c->data_bytes;
    return true;
}

static int64_t decode_xfs_time(bool bigtime, const uint8_t *p) {
    if (bigtime) {
        uint64_t v = be64(p);
        return (int64_t)(v / UINT64_C(1000000000)) - INT64_C(2147483648);
    }
    return (int64_t)(int32_t)be32(p);
}

static void scan_inode_block(Context *ctx, uint32_t ag, uint32_t block) {
    if (g_stop) return;
    uint64_t dblock = (uint64_t)ctx->agblocks * ag + block;
    if (dblock >= ctx->dblocks) return;

    uint8_t *buf = malloc(ctx->blocksize);
    if (!buf) { perror("malloc"); g_stop = 1; return; }
    if (pread_full(ctx->fd, buf, ctx->blocksize, dblock * (uint64_t)ctx->blocksize) < 0) {
        free(buf);
        return;
    }

    for (uint32_t boff = 0; boff + ctx->inodesize <= ctx->blocksize; boff += ctx->inodesize) {
        const uint8_t *ino = buf + boff;
        if (ino[0] != 'I' || ino[1] != 'N') continue;
        ctx->visited_inodes++;

        uint8_t version = ino[4];
        uint64_t inode_no;
        uint32_t extent_off;
        if (version >= 3) {
            if (ctx->inodesize < 176) continue;
            inode_no = be64(ino + 152);
            extent_off = 176;
        } else {
            inode_no = (dblock * (uint64_t)ctx->blocksize + boff) / ctx->inodesize;
            extent_off = 100;
        }

        if (ctx->have_start_inode && !ctx->passed_start) {
            if (inode_no == ctx->start_inode) ctx->passed_start = true;
            else continue;
        }
        if (is_excluded(ctx, inode_no)) continue;
        if (ino[2] != 0 || ino[3] != 0) continue;

        int64_t mtime_sec = decode_xfs_time(ctx->bigtime, ino + 40);
        int64_t ctime_sec = decode_xfs_time(ctx->bigtime, ino + 48);
        if (ctime_sec < ctx->ctime_min || ctime_sec > ctx->ctime_max) continue;
        if (mtime_sec < ctx->mtime_min || mtime_sec > ctx->mtime_max) continue;

        Extent temp[MAX_EXTENTS_PER_INODE];
        size_t ne = 0;
        for (uint32_t off = extent_off; off + 16 <= ctx->inodesize && ne < MAX_EXTENTS_PER_INODE; off += 16) {
            bool allzero = true;
            for (unsigned k = 0; k < 16; ++k) if (ino[off+k]) { allzero = false; break; }
            if (allzero) continue;
            Extent e;
            if (decode_extent(ctx, ino + off, &e)) temp[ne++] = e;
        }
        if (!ne) continue;
        qsort(temp, ne, sizeof(temp[0]), extent_cmp);
        if (temp[0].logical_block != 0) continue;

        /*
         * Deleted inode forks can contain stale 128-bit words that happen to
         * decode as plausible bmbt records. Normalize exact duplicates and
         * reject conflicting/overlapping logical mappings. A valid extent
         * list cannot have two different physical mappings for the same
         * logical range.
         */
        size_t nn = 0;
        bool bad_overlap = false;
        for (size_t i = 0; i < ne; ++i) {
            if (nn > 0) {
                Extent *prev = &temp[nn - 1];
                uint64_t prev_end = prev->logical_block + prev->block_count;

                if (temp[i].logical_block == prev->logical_block) {
                    if (temp[i].physical_block == prev->physical_block &&
                        temp[i].block_count == prev->block_count) {
                        continue; /* exact duplicate stale record */
                    }
                    bad_overlap = true;
                    break;
                }
                if (temp[i].logical_block < prev_end) {
                    bad_overlap = true;
                    break;
                }
            }
            temp[nn++] = temp[i];
        }
        if (bad_overlap) {
            ctx->rejected_overlapping++;
            continue;
        }
        ne = nn;
        if (!ne || temp[0].logical_block != 0) continue;

        uint64_t est_blocks = 0, data_blocks = 0;
        for (size_t i = 0; i < ne; ++i) {
            if (temp[i].logical_block > UINT64_MAX - temp[i].block_count) {
                bad_overlap = true;
                break;
            }
            uint64_t end = temp[i].logical_block + temp[i].block_count;
            if (end > est_blocks) est_blocks = end;
            if (UINT64_MAX - data_blocks < temp[i].block_count) {
                bad_overlap = true;
                break;
            }
            data_blocks += temp[i].block_count;
        }
        if (bad_overlap || data_blocks > est_blocks) {
            ctx->rejected_overlapping++;
            continue;
        }

        /*
         * A sparse file can technically have a logical size larger than its
         * filesystem, but deleted-inode recovery commonly encounters stale
         * records producing absurd PiB logical offsets. Reject those by
         * default; --allow-oversize-sparse disables this guard.
         */
        if (!ctx->allow_oversize_sparse && est_blocks > ctx->dblocks) {
            ctx->rejected_oversize_sparse++;
            continue;
        }

        if (est_blocks > UINT64_MAX / ctx->blocksize) continue;
        uint64_t est = est_blocks * ctx->blocksize;
        if (ctx->have_max_size && est > ctx->max_size) continue;

        Candidate c;
        memset(&c, 0, sizeof(c));
        c.inode = inode_no;
        c.ctime_sec = ctime_sec;
        c.mtime_sec = mtime_sec;
        c.estimated_size = est;
        c.data_bytes = data_blocks > UINT64_MAX / ctx->blocksize ? UINT64_MAX : data_blocks * ctx->blocksize;
        c.extents = malloc(ne * sizeof(*c.extents));
        if (!c.extents) { perror("malloc"); g_stop = 1; break; }
        memcpy(c.extents, temp, ne * sizeof(*c.extents));
        c.extent_count = ne;

        size_t sniff = ctx->blocksize;
        if (sniff < 65536) sniff = 65536;
        if (sniff > 1024 * 1024) sniff = 1024 * 1024;
        uint64_t first_extent_bytes = (uint64_t)c.extents[0].block_count * ctx->blocksize;
        if ((uint64_t)sniff > first_extent_bytes) sniff = (size_t)first_extent_bytes;
        uint8_t *first = malloc(sniff ? sniff : 1);
        uint64_t first_off = c.extents[0].physical_block * (uint64_t)ctx->blocksize;
        if (first && sniff && pread_full(ctx->fd, first, sniff, first_off) == 0)
            identify_buffer(ctx, first, sniff, c.mimetype, c.extension);
        else {
            snprintf(c.mimetype, sizeof(c.mimetype), "application/octet-stream");
            snprintf(c.extension, sizeof(c.extension), "bin");
        }
        free(first);

        if (pattern_list_match(ctx->ignore_patterns, c.mimetype, c.extension) ||
            !pattern_list_match(ctx->recover_patterns, c.mimetype, c.extension)) {
            free(c.extents);
            continue;
        }

        if (!add_candidate(ctx, &c)) {
            free(c.extents);
            perror("realloc");
            g_stop = 1;
            break;
        }
    }
    free(buf);
}

static void traverse_inode_tree(Context *ctx, uint32_t ag, uint32_t block, unsigned depth) {
    if (g_stop || depth > 32 || block >= ctx->agblocks) return;
    uint8_t *buf = malloc(ctx->blocksize);
    if (!buf) { perror("malloc"); g_stop = 1; return; }
    uint64_t off = ((uint64_t)ctx->agblocks * ag + block) * ctx->blocksize;
    if (pread_full(ctx->fd, buf, ctx->blocksize, off) < 0) {
        free(buf); return;
    }

    uint32_t hdr;
    if (!memcmp(buf, "IABT", 4)) hdr = 16;
    else if (!memcmp(buf, "IAB3", 4)) hdr = 56;
    else { free(buf); return; }

    uint16_t level = be16(buf + 4);
    uint16_t nrecs = be16(buf + 6);

    if (level > 0) {
        uint64_t ptr_index = (ctx->blocksize + (uint64_t)hdr) / 2;
        if (ptr_index + (uint64_t)nrecs * 4 > ctx->blocksize) { free(buf); return; }
        for (uint16_t i = 0; i < nrecs && !g_stop; ++i) {
            uint32_t child = be32(buf + ptr_index + (uint64_t)i * 4);
            traverse_inode_tree(ctx, ag, child, depth + 1);
        }
    } else {
        if ((uint64_t)hdr + (uint64_t)nrecs * 16 > ctx->blocksize) { free(buf); return; }
        for (uint16_t r = 0; r < nrecs && !g_stop; ++r) {
            uint32_t startino = be32(buf + hdr + (uint64_t)r * 16);
            for (uint32_t i = 0; i < 64; i += ctx->inopblock) {
                uint64_t v = (uint64_t)startino + i;
                uint32_t iblock = (uint32_t)(v / ctx->inopblock);
                if (iblock < ctx->agblocks) scan_inode_block(ctx, ag, iblock);
            }
        }
    }
    free(buf);
}

static bool load_superblock(Context *ctx) {
    uint8_t sb[512];
    if (pread_full(ctx->fd, sb, sizeof(sb), 0) < 0) return false;
    if (memcmp(sb, XFS_SB_MAGIC, 4)) return false;

    ctx->blocksize = be32(sb + 4);
    ctx->dblocks = be64(sb + 8);
    ctx->agblocks = be32(sb + 84);
    ctx->agcount = be32(sb + 88);
    ctx->versionnum = be16(sb + 100);
    ctx->sectsize = be16(sb + 102);
    ctx->inodesize = be16(sb + 104);
    ctx->inopblock = be16(sb + 106);
    ctx->agblklog = sb[124];
    ctx->icount = be64(sb + 128);

    if ((ctx->versionnum & 0x000f) == 5) {
        uint32_t incompat = be32(sb + 216);
        ctx->bigtime = (incompat & 0x08u) != 0;
    }

    if (ctx->blocksize < 512 || ctx->blocksize > (1u << 20) ||
        ctx->sectsize < 512 || ctx->inodesize < 256 || ctx->inopblock == 0 ||
        ctx->agblocks == 0 || ctx->agcount == 0 || ctx->agblklog == 0 ||
        ctx->agblklog >= 63) {
        return false;
    }
    return true;
}

static bool source_is_mounted_rw(const char *path) {
#ifdef __linux__
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISBLK(st.st_mode)) return false;
    unsigned maj = major(st.st_rdev), min = minor(st.st_rdev);
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return false;
    char *line = NULL;
    size_t cap = 0;
    bool rw = false;
    while (getline(&line, &cap, f) >= 0) {
        unsigned a, b;
        char opts[512] = {0};
        /* fields: id parent major:minor root mountpoint options ... */
        if (sscanf(line, "%*u %*u %u:%u %*s %*s %511s", &a, &b, opts) == 3 &&
            a == maj && b == min) {
            char wrapped[520];
            snprintf(wrapped, sizeof(wrapped), ",%s,", opts);
            if (strstr(wrapped, ",rw,")) { rw = true; break; }
        }
    }
    free(line);
    fclose(f);
    return rw;
#else
    (void)path;
    return false;
#endif
}

static bool output_is_on_source_device(const char *source, const char *outdir) {
    struct stat ss, os;
    if (stat(source, &ss) < 0 || !S_ISBLK(ss.st_mode)) return false;

    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s", outdir);
    while (stat(probe, &os) < 0) {
        char *slash = strrchr(probe, '/');
        if (!slash) {
            if (stat(".", &os) < 0) return false;
            break;
        }
        if (slash == probe) { probe[1] = 0; }
        else *slash = 0;
    }
    return os.st_dev == ss.st_rdev;
}

static void format_time_name(int64_t t, char out[32]) {
    time_t tt = (time_t)t;
    struct tm tmv;
    if (!localtime_r(&tt, &tmv) || !strftime(out, 32, "%Y-%m-%d-%H-%M", &tmv))
        snprintf(out, 32, "unknown-time");
}

static void render_progress(const char *label, uint64_t done, uint64_t total,
                            double seconds, bool final_line) {
    double pct = total ? (100.0 * (double)done / (double)total) : 100.0;
    double speed = seconds > 0 ? (double)done / seconds : 0;
    double eta = (speed > 0 && total > done) ? (double)(total - done) / speed : 0;
    char hd[32], ht[32], hs[32];
    fprintf(stderr, "\r%-18s %6.2f%%  %s / %s  %s/s",
            label, pct, human_size(done, hd), human_size(total, ht),
            human_size((uint64_t)speed, hs));
    if (!final_line && eta > 0) fprintf(stderr, "  ETA %.0fs", eta);
    fprintf(stderr, "%s", final_line ? "\n" : "        ");
    fflush(stderr);
}

static int trim_trailing_zeroes(int fd, uint64_t size, uint32_t blocksize) {
    if (size == 0) return 0;
    size_t n = size < blocksize ? (size_t)size : blocksize;
    uint8_t *buf = malloc(n);
    if (!buf) return -1;
    uint64_t off = size - n;
    if (pread_full(fd, buf, n, off) < 0) { free(buf); return -1; }
    size_t keep = n;
    while (keep && buf[keep - 1] == 0) --keep;
    int rc = ftruncate(fd, (off_t)(off + keep));
    free(buf);
    return rc;
}

static int recover_candidate(Context *ctx, const Candidate *c,
                             uint64_t *overall_done, uint64_t overall_total) {
    char ts[32], path[PATH_MAX];
    format_time_name(c->ctime_sec, ts);
    snprintf(path, sizeof(path), "%s/%s_%" PRIu64 ".%s",
             ctx->out_dir, ts, c->inode, c->extension[0] ? c->extension : "bin");

    char he[32], hd[32];
    fprintf(stderr, "\nRecovering inode %" PRIu64 " -> %s\n"
                    "  estimated logical size: %s\n"
                    "  extent data to copy:    %s\n",
            c->inode, path,
            human_size(c->estimated_size, he),
            human_size(c->data_bytes, hd));

    int ofd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (ofd < 0) {
        fprintf(stderr, "  ERROR opening output: %s\n", strerror(errno));
        return -1;
    }
    if (ftruncate(ofd, (off_t)c->estimated_size) < 0) {
        fprintf(stderr, "  ERROR sizing output: %s\n", strerror(errno));
        close(ofd); return -1;
    }

    uint8_t *buf = malloc(COPY_BUFSIZE);
    if (!buf) { perror("malloc"); close(ofd); return -1; }

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t file_done = 0;
    double last_draw = -1;

    for (size_t ei = 0; ei < c->extent_count && !g_stop; ++ei) {
        const Extent *e = &c->extents[ei];
        uint64_t remain = (uint64_t)e->block_count * ctx->blocksize;
        uint64_t src = e->physical_block * (uint64_t)ctx->blocksize;
        uint64_t dst = e->logical_block * (uint64_t)ctx->blocksize;

        while (remain && !g_stop) {
            size_t chunk = remain > COPY_BUFSIZE ? COPY_BUFSIZE : (size_t)remain;
            if (pread_full(ctx->fd, buf, chunk, src) < 0) {
                fprintf(stderr, "\n  ERROR reading source at byte %" PRIu64 ": %s\n",
                        src, strerror(errno));
                free(buf); close(ofd); return -1;
            }
            if (pwrite_full(ofd, buf, chunk, dst) < 0) {
                fprintf(stderr, "\n  ERROR writing output: %s\n", strerror(errno));
                free(buf); close(ofd); return -1;
            }
            src += chunk; dst += chunk; remain -= chunk;
            file_done += chunk; *overall_done += chunk;

            clock_gettime(CLOCK_MONOTONIC, &now);
            double sec = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec)/1e9;
            if (sec - last_draw >= 0.2 || file_done == c->data_bytes) {
                render_progress("  file", file_done, c->data_bytes, sec, false);
                last_draw = sec;
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    double sec = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec)/1e9;
    render_progress("  file", file_done, c->data_bytes, sec, true);

    if (!g_stop && pattern_list_match(ctx->zero_patterns, c->mimetype, c->extension)) {
        if (trim_trailing_zeroes(ofd, c->estimated_size, ctx->blocksize) < 0)
            fprintf(stderr, "  warning: failed to trim trailing zeroes: %s\n", strerror(errno));
    }
    if (fsync(ofd) < 0) fprintf(stderr, "  warning: fsync failed: %s\n", strerror(errno));
    free(buf);
    close(ofd);

    if (overall_total) {
        double pct = 100.0 * (double)*overall_done / (double)overall_total;
        fprintf(stderr, "  overall copied: %.2f%%\n", pct);
    }
    return g_stop ? -1 : 0;
}

static void usage(FILE *f, const char *prog) {
    fprintf(f,
"Usage: %s [options] DEVICE_OR_IMAGE\n"
"       %s -l [-m MAGICFILES]\n"
"\n"
"Native XFS deleted-file recovery with pre-copy size estimation.\n"
"\n"
"Upstream-compatible options where practical:\n"
"  -t RANGE              Deleted/ctime range\n"
"  -T RANGE              Modified/mtime range\n"
"  -r TYPES              Recover matching MIME/extensions (default: *)\n"
"  -i TYPES              Ignore matching MIME/extensions (default: bin)\n"
"  -x INODES             Comma-separated inode numbers to ignore\n"
"  -S SIZE               Ignore estimated files larger than SIZE\n"
"  -z TYPES              Trim trailing zeroes for types (default: text/*)\n"
"  -o DIR                Output directory (default: xfs_undeleted)\n"
"  -s INODE              Resume when this exact inode is encountered\n"
"  -m MAGICFILES         Magic path passed to file(1)\n"
"  -l                    List filter/filetype information and exit\n"
"\n"
"Native additions:\n"
"  -n, --dry-run         Scan and estimate only\n"
"      --force-rw        Permit reading a block device mounted read-write\n"
"      --no-remount-readonly  Upstream-compatible alias for --force-rw\n"
"      --allow-oversize-sparse  Permit logical files larger than the XFS device\n"
"  -q, --quiet-scan      Suppress allocation-group scan progress\n"
"  -h, --help            Show help\n"
"\n"
"TYPE lists are comma-separated shell patterns. Patterns containing '/' match\n"
"MIME types; all other patterns match extensions. Quote wildcards in the shell.\n"
"Examples: -r 'video/*'   -r 'image/*,pdf,docx'   -i 'bin'\n"
"\n"
"The XFS source is always opened O_RDONLY and is never written or remounted.\n",
            prog, prog);
}

int main(int argc, char **argv) {
    Context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = "xfs_undeleted";
    ctx.ctime_min = ctx.mtime_min = 0;
    ctx.ctime_max = ctx.mtime_max = INT64_MAX;
    ctx.recover_patterns = "*";
    ctx.ignore_patterns = "bin";
    ctx.zero_patterns = "text/*";
    ctx.passed_start = true;

    enum { OPT_FORCE_RW = 1000, OPT_ALLOW_OVERSIZE_SPARSE };
    static const struct option opts[] = {
        {"output", required_argument, 0, 'o'},
        {"start-inode", required_argument, 0, 's'},
        {"max-size", required_argument, 0, 'S'},
        {"exclude", required_argument, 0, 'x'},
        {"deleted", required_argument, 0, 't'},
        {"modified", required_argument, 0, 'T'},
        {"recover", required_argument, 0, 'r'},
        {"ignore", required_argument, 0, 'i'},
        {"trim-zeroes", required_argument, 0, 'z'},
        {"magic", required_argument, 0, 'm'},
        {"list", no_argument, 0, 'l'},
        {"dry-run", no_argument, 0, 'n'},
        {"force-rw", no_argument, 0, OPT_FORCE_RW},
        {"no-remount-readonly", no_argument, 0, OPT_FORCE_RW},
        {"allow-oversize-sparse", no_argument, 0, OPT_ALLOW_OVERSIZE_SPARSE},
        {"quiet-scan", no_argument, 0, 'q'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    int ch;
    while ((ch = getopt_long(argc, argv, "o:s:S:x:t:T:r:i:z:m:lnqh", opts, NULL)) != -1) {
        switch (ch) {
        case 'o': ctx.out_dir = optarg; break;
        case 's': {
            char *e = NULL; errno = 0;
            ctx.start_inode = strtoull(optarg, &e, 10);
            if (errno || e == optarg || *e) { fprintf(stderr, "Invalid start inode\n"); return 2; }
            ctx.have_start_inode = true; ctx.passed_start = false; break;
        }
        case 'S':
            if (!parse_size(optarg, &ctx.max_size)) { fprintf(stderr, "Invalid max size\n"); return 2; }
            ctx.have_max_size = true; break;
        case 'x':
            if (!add_excluded(&ctx, optarg)) { fprintf(stderr, "Invalid exclude list\n"); return 2; }
            break;
        case 't':
            if (!parse_range(optarg, &ctx.ctime_min, &ctx.ctime_max)) {
                fprintf(stderr, "Invalid deletion/ctime range: %s\n", optarg); return 2;
            }
            break;
        case 'T':
            if (!parse_range(optarg, &ctx.mtime_min, &ctx.mtime_max)) {
                fprintf(stderr, "Invalid modification/mtime range: %s\n", optarg); return 2;
            }
            break;
        case 'r': ctx.recover_patterns = (*optarg ? optarg : "*"); break;
        case 'i': ctx.ignore_patterns = optarg; break;
        case 'z': ctx.zero_patterns = optarg; break;
        case 'm': ctx.magicfiles = optarg; break;
        case 'l': ctx.list_types = true; break;
        case 'n': ctx.dry_run = true; break;
        case 'q': ctx.quiet_scan = true; break;
        case OPT_FORCE_RW: ctx.force_rw = true; break;
        case OPT_ALLOW_OVERSIZE_SPARSE: ctx.allow_oversize_sparse = true; break;
        case 'h': usage(stdout, argv[0]); return 0;
        default: usage(stderr, argv[0]); return 2;
        }
    }

    if (ctx.list_types) return list_filetypes(ctx.magicfiles);

    if (optind + 1 != argc) {
        usage(stderr, argv[0]);
        return 2;
    }
    ctx.source = argv[optind];

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (source_is_mounted_rw(ctx.source) && !ctx.force_rw) {
        fprintf(stderr,
                "REFUSING: source block device appears to be mounted read-write.\n"
                "Unmount it or remount it read-only first. If you intentionally accept\n"
                "the risk, use --force-rw. The program itself never writes the source.\n");
        return 3;
    }
    if (!ctx.dry_run && output_is_on_source_device(ctx.source, ctx.out_dir)) {
        fprintf(stderr,
                "REFUSING: output directory appears to be on the same block device as\n"
                "the XFS source. Recover to a different filesystem.\n");
        return 3;
    }

    ctx.fd = open(ctx.source, O_RDONLY | O_CLOEXEC);
    if (ctx.fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", ctx.source, strerror(errno));
        return 1;
    }
    if (!load_superblock(&ctx)) {
        fprintf(stderr, "%s is not a supported/valid XFS filesystem or image.\n", ctx.source);
        close(ctx.fd);
        return 1;
    }

    fprintf(stderr,
            "XFS: block=%u inode=%u AGs=%u AG-blocks=%u dblocks=%" PRIu64
            " bigtime=%s\n"
            "Phase 1/2: scanning metadata and estimating sizes (NO file data is being copied)...\n",
            ctx.blocksize, ctx.inodesize, ctx.agcount, ctx.agblocks, ctx.dblocks,
            ctx.bigtime ? "yes" : "no");

    for (uint32_t ag = 0; ag < ctx.agcount && !g_stop; ++ag) {
        uint8_t *agi = malloc(ctx.sectsize);
        if (!agi) { perror("malloc"); g_stop = 1; break; }
        uint64_t off = (uint64_t)ctx.blocksize * ctx.agblocks * ag + 2ull * ctx.sectsize;
        if (pread_full(ctx.fd, agi, ctx.sectsize, off) == 0) {
            uint32_t root = be32(agi + 20);
            traverse_inode_tree(&ctx, ag, root, 0);
        }
        free(agi);
        if (!ctx.quiet_scan) {
            fprintf(stderr, "\r  AG %u/%u  candidates=%zu", ag + 1, ctx.agcount, ctx.candidate_count);
            fflush(stderr);
        }
    }
    if (!ctx.quiet_scan) fputc('\n', stderr);

    if (g_stop) {
        fprintf(stderr, "Interrupted during metadata scan; no recovery copy phase started.\n");
        goto done;
    }

    char hlog[32], hdata[32];
    fprintf(stderr,
            "\nPre-copy estimate complete.\n"
            "  recoverable candidates:       %zu\n"
            "  estimated logical output:     %s\n"
            "  actual extent bytes to copy:  %s\n"
            "  inode records inspected:      %" PRIu64 "\n"
            "  rejected overlapping layouts: %" PRIu64 "\n"
            "  rejected oversize sparse:      %" PRIu64 "\n",
            ctx.candidate_count,
            human_size(ctx.total_estimated_size, hlog),
            human_size(ctx.total_data_bytes, hdata),
            ctx.visited_inodes,
            ctx.rejected_overlapping,
            ctx.rejected_oversize_sparse);

    if (ctx.candidate_count) {
        fprintf(stderr, "\nCandidate sizes (known BEFORE copying):\n");
        for (size_t i = 0; i < ctx.candidate_count; ++i) {
            Candidate *c = &ctx.candidates[i];
            char a[32], b[32];
            fprintf(stderr, "  [%zu/%zu] inode=%" PRIu64 "  est=%s  data=%s  extents=%zu  type=.%s\n",
                    i + 1, ctx.candidate_count, c->inode,
                    human_size(c->estimated_size, a), human_size(c->data_bytes, b),
                    c->extent_count, c->extension);
            fprintf(stderr, "           mime=%s\n", c->mimetype);
        }
    }

    if (ctx.dry_run || ctx.candidate_count == 0) {
        if (ctx.dry_run) fprintf(stderr, "\nDry run requested; copy phase skipped.\n");
        goto done;
    }

    if (mkdir(ctx.out_dir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "Cannot create output directory %s: %s\n", ctx.out_dir, strerror(errno));
        goto done;
    }

    fprintf(stderr, "\nPhase 2/2: recovering file data...\n");
    uint64_t overall_done = 0;
    size_t ok = 0, failed = 0;
    for (size_t i = 0; i < ctx.candidate_count && !g_stop; ++i) {
        fprintf(stderr, "\n[%zu/%zu]", i + 1, ctx.candidate_count);
        if (recover_candidate(&ctx, &ctx.candidates[i], &overall_done, ctx.total_data_bytes) == 0) ok++;
        else failed++;
    }
    fprintf(stderr, "\nRecovery finished: %zu recovered, %zu failed%s.\n",
            ok, failed, g_stop ? " (interrupted)" : "");

done:
    for (size_t i = 0; i < ctx.candidate_count; ++i) free(ctx.candidates[i].extents);
    free(ctx.candidates);
    free(ctx.excluded);
    close(ctx.fd);
    return g_stop ? 130 : 0;
}
