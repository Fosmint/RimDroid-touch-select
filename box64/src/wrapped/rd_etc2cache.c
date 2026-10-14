// RimDroid: on-disk cache of the S3TC->ETC2 transcode output. See rd_etc2cache.h.
//
// One append-only pack file, RIMDROID_CACHE_DIR/etc2cache.bin:
//   file header  { magic "RDE2", version, reserved }                         16 bytes
//   record       { magic "DREC", dstsize, key, fmt, efmt, w, h, srcsize, 0 } 40 bytes + dstsize
// There is no index on disk. At open the records are walked by their headers into an in-memory
// hash table (key -> record offset); a torn tail left by a crash fails the walk and is truncated.
// Writes append under a mutex (MobileGlues' threaded mode uploads from two threads); reads are
// positional and need no lock. The key is a 64-bit hash of the DXT bytes seeded with format and
// size, and the record header is checked against the request on every hit, so a colliding key
// can only cost a miss, never a wrong texture.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "debug.h"
#include "khash.h"
#include "rd_etc2cache.h"

#define E2C_FILE_MAGIC 0x32454452u   /* "RDE2" */
#define E2C_REC_MAGIC  0x43455244u   /* "DREC" */
#define E2C_VERSION    1u
#define E2C_MAX_REC    (64u << 20)   /* no single upload is anywhere near this */

typedef struct { uint32_t magic; uint32_t version; uint64_t reserved; } e2c_filehdr_t;
typedef struct {
    uint32_t magic;
    uint32_t dstsize;
    uint64_t key;
    uint32_t fmt;
    uint32_t efmt;
    int32_t  w;
    int32_t  h;
    uint32_t srcsize;
    uint32_t reserved;
} e2c_rec_t;

KHASH_MAP_INIT_INT64(rd_e2c, uint64_t)

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_state = 0;              /* 0 = not opened yet, 1 = on, -1 = off */
static int g_fd = -1;
static uint64_t g_end = 0;           /* append position = file size */
static uint64_t g_cap = 0;
static kh_rd_e2c_t* g_map = NULL;
static char g_path[1024];
static uint64_t g_hits = 0, g_misses = 0, g_entries_at_open = 0, g_bytes_at_open = 0;
static int g_cap_logged = 0;

/* MurmurHash64A (Austin Appleby, public domain): 8 bytes per step, plenty fast for ~250 MB. */
static uint64_t e2c_hash(const void* key, size_t len, uint64_t seed) {
    const uint64_t m = 0xc6a4a7935bd1e995ull;
    const int r = 47;
    uint64_t h = seed ^ (len * m);
    const uint8_t* p = (const uint8_t*)key;
    const uint8_t* end = p + (len / 8) * 8;
    while (p != end) {
        uint64_t k;
        memcpy(&k, p, 8);
        p += 8;
        k *= m; k ^= k >> r; k *= m;
        h ^= k; h *= m;
    }
    switch (len & 7) {
        case 7: h ^= (uint64_t)p[6] << 48; /* fall through */
        case 6: h ^= (uint64_t)p[5] << 40; /* fall through */
        case 5: h ^= (uint64_t)p[4] << 32; /* fall through */
        case 4: h ^= (uint64_t)p[3] << 24; /* fall through */
        case 3: h ^= (uint64_t)p[2] << 16; /* fall through */
        case 2: h ^= (uint64_t)p[1] << 8;  /* fall through */
        case 1: h ^= (uint64_t)p[0]; h *= m;
    }
    h ^= h >> r; h *= m; h ^= h >> r;
    return h;
}

static uint64_t e2c_key(const void* src, size_t srcsize, uint32_t fmt, uint32_t efmt, int32_t w, int32_t h) {
    uint32_t hdr[5] = { fmt, efmt, (uint32_t)w, (uint32_t)h, (uint32_t)srcsize };
    uint64_t seed = e2c_hash(hdr, sizeof(hdr), 0x52696d44726f6964ull);   /* "RimDroid" */
    return e2c_hash(src, srcsize, seed);
}

static ssize_t e2c_pwrite_all(int fd, const void* buf, size_t n, uint64_t off) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t done = 0;
    while (done < n) {
        ssize_t r = pwrite(fd, p + done, n - done, (off_t)(off + done));
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

static ssize_t e2c_pread_all(int fd, void* buf, size_t n, uint64_t off) {
    uint8_t* p = (uint8_t*)buf;
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(fd, p + done, n - done, (off_t)(off + done));
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

/* Called once, under g_mu. */
static void e2c_open_locked(void) {
    g_state = -1;
    const char* on = getenv("RIMDROID_GLT_ETC2_CACHE");
    if (on && on[0] == '0') { printf_log(LOG_NONE, "RIMDROID GLT ETC2 cache: off (RIMDROID_GLT_ETC2_CACHE=0)\n"); return; }
    const char* dir = getenv("RIMDROID_CACHE_DIR");
    if (!dir || !dir[0]) { printf_log(LOG_NONE, "RIMDROID GLT ETC2 cache: off (no RIMDROID_CACHE_DIR)\n"); return; }
    snprintf(g_path, sizeof(g_path), "%s/etc2cache.bin", dir);

    uint64_t cap_mb = 1024;
    const char* capenv = getenv("RIMDROID_GLT_ETC2_CACHE_MAX_MB");
    if (capenv && capenv[0]) { long v = strtol(capenv, NULL, 10); if (v > 0) cap_mb = (uint64_t)v; }
    g_cap = cap_mb << 20;

    int fd = open(g_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) { printf_log(LOG_NONE, "RIMDROID GLT ETC2 cache: off (open %s: %s)\n", g_path, strerror(errno)); return; }
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); printf_log(LOG_NONE, "RIMDROID GLT ETC2 cache: off (fstat: %s)\n", strerror(errno)); return; }
    uint64_t size = (uint64_t)st.st_size;

    e2c_filehdr_t fh;
    int reset = 0;
    const char* why = NULL;
    if (size < sizeof(fh)) { reset = 1; why = size ? "short header" : "new file"; }
    else if (e2c_pread_all(fd, &fh, sizeof(fh), 0) != (ssize_t)sizeof(fh)
             || fh.magic != E2C_FILE_MAGIC || fh.version != E2C_VERSION) { reset = 1; why = "unknown header"; }
    else if (size > g_cap) { reset = 1; why = "over the size cap"; }
    if (reset) {
        if (ftruncate(fd, 0) != 0) { close(fd); printf_log(LOG_NONE, "RIMDROID GLT ETC2 cache: off (truncate: %s)\n", strerror(errno)); return; }
        memset(&fh, 0, sizeof(fh));
        fh.magic = E2C_FILE_MAGIC;
        fh.version = E2C_VERSION;
        if (e2c_pwrite_all(fd, &fh, sizeof(fh), 0) != (ssize_t)sizeof(fh)) { close(fd); printf_log(LOG_NONE, "RIMDROID GLT ETC2 cache: off (write header: %s)\n", strerror(errno)); return; }
        size = sizeof(fh);
        printf_log(LOG_NONE, "RIMDROID GLT ETC2 cache: starting empty (%s)\n", why);
    }

    g_map = kh_init(rd_e2c);
    uint64_t off = sizeof(fh);
    uint64_t n = 0;
    int torn = 0;
    while (off + sizeof(e2c_rec_t) <= size) {
        e2c_rec_t r;
        if (e2c_pread_all(fd, &r, sizeof(r), off) != (ssize_t)sizeof(r)) { torn = 1; break; }
        if (r.magic != E2C_REC_MAGIC || r.dstsize == 0 || r.dstsize > E2C_MAX_REC
            || off + sizeof(r) + r.dstsize > size) { torn = 1; break; }
        int absent;
        khint_t k = kh_put(rd_e2c, g_map, r.key, &absent);
        kh_val(g_map, k) = off;
        n++;
        off += sizeof(r) + r.dstsize;
    }
    if (torn || off != size) {
        if (ftruncate(fd, (off_t)off) == 0)
            printf_log(LOG_NONE, "RIMDROID GLT ETC2 cache: dropped a torn tail (%llu bytes) after %llu records\n",
                       (unsigned long long)(size - off), (unsigned long long)n);
    }
    g_fd = fd;
    g_end = off;
    g_entries_at_open = n;
    g_bytes_at_open = off;
    g_state = 1;
    printf_log(LOG_NONE, "RIMDROID GLT ETC2 cache: %s — %llu entries, %llu MB (cap %llu MB)\n",
               g_path, (unsigned long long)n, (unsigned long long)(off >> 20), (unsigned long long)cap_mb);
    fflush(NULL);
}

int rd_etc2cache_on(void) {
    if (g_state == 0) {
        pthread_mutex_lock(&g_mu);
        if (g_state == 0) e2c_open_locked();
        pthread_mutex_unlock(&g_mu);
    }
    return g_state > 0;
}

const void* rd_etc2cache_get(const void* src, size_t srcsize, uint32_t fmt, uint32_t efmt,
                             int32_t w, int32_t h, size_t* out_sz) {
    if (!src || !srcsize || !rd_etc2cache_on()) return NULL;
    uint64_t key = e2c_key(src, srcsize, fmt, efmt, w, h);

    pthread_mutex_lock(&g_mu);
    khint_t k = kh_get(rd_e2c, g_map, key);
    uint64_t off = (k == kh_end(g_map)) ? UINT64_MAX : kh_val(g_map, k);
    if (off == UINT64_MAX) g_misses++;
    pthread_mutex_unlock(&g_mu);
    if (off == UINT64_MAX) return NULL;

    e2c_rec_t r;
    if (e2c_pread_all(g_fd, &r, sizeof(r), off) != (ssize_t)sizeof(r)) return NULL;
    if (r.magic != E2C_REC_MAGIC || r.key != key || r.fmt != fmt || r.efmt != efmt
        || r.w != w || r.h != h || r.srcsize != (uint32_t)srcsize || r.dstsize == 0 || r.dstsize > E2C_MAX_REC) {
        __atomic_fetch_add(&g_misses, 1, __ATOMIC_RELAXED);   /* a key collision: treat as a miss */
        return NULL;
    }
    static _Thread_local uint8_t* rbuf = NULL;
    static _Thread_local size_t rcap = 0;
    if (rcap < r.dstsize) {
        void* nb = realloc(rbuf, r.dstsize);
        if (!nb) return NULL;
        rbuf = (uint8_t*)nb;
        rcap = r.dstsize;
    }
    if (e2c_pread_all(g_fd, rbuf, r.dstsize, off + sizeof(r)) != (ssize_t)r.dstsize) return NULL;
    __atomic_fetch_add(&g_hits, 1, __ATOMIC_RELAXED);
    *out_sz = r.dstsize;
    return rbuf;
}

void rd_etc2cache_put(const void* src, size_t srcsize, uint32_t fmt, uint32_t efmt,
                      int32_t w, int32_t h, const void* dst, size_t dstsize) {
    if (!src || !srcsize || !dst || !dstsize || dstsize > E2C_MAX_REC || !rd_etc2cache_on()) return;
    e2c_rec_t r;
    memset(&r, 0, sizeof(r));
    r.magic = E2C_REC_MAGIC;
    r.dstsize = (uint32_t)dstsize;
    r.key = e2c_key(src, srcsize, fmt, efmt, w, h);
    r.fmt = fmt;
    r.efmt = efmt;
    r.w = w;
    r.h = h;
    r.srcsize = (uint32_t)srcsize;

    pthread_mutex_lock(&g_mu);
    if (g_state > 0 && kh_get(rd_e2c, g_map, r.key) == kh_end(g_map)) {
        uint64_t off = g_end;
        if (off + sizeof(r) + dstsize > g_cap) {
            if (!g_cap_logged) {
                g_cap_logged = 1;
                printf_log(LOG_NONE, "RIMDROID GLT ETC2 cache: size cap reached, not adding more this run\n");
            }
        } else if (e2c_pwrite_all(g_fd, &r, sizeof(r), off) != (ssize_t)sizeof(r)
                   || e2c_pwrite_all(g_fd, dst, dstsize, off + sizeof(r)) != (ssize_t)dstsize) {
            /* Leave the file as it was; the walk at the next open drops any partial record anyway. */
            if (ftruncate(g_fd, (off_t)off) != 0) { /* nothing better to do */ }
            printf_log(LOG_NONE, "RIMDROID GLT ETC2 cache: write failed (%s), cache is read-only for this run\n", strerror(errno));
            g_cap = 0;   /* every later put hits the cap branch, silently */
            g_cap_logged = 1;
        } else {
            g_end = off + sizeof(r) + dstsize;
            int absent;
            khint_t k = kh_put(rd_e2c, g_map, r.key, &absent);
            kh_val(g_map, k) = off;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void rd_etc2cache_stats(uint64_t out[4]) {
    out[0] = __atomic_load_n(&g_hits, __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_misses, __ATOMIC_RELAXED);
    out[2] = g_entries_at_open;
    out[3] = g_bytes_at_open >> 20;
}
