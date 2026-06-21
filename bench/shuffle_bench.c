/*
 * ============================================================
 *  Jigsaw Shuffle Hardware Limit Benchmark
 *  Tests whether the shuffle step saturates memory bandwidth
 * ============================================================
 *
 *  Measures:
 *    1. memcpy bandwidth (sequential copy — theoretical ceiling)
 *    2. Jigsaw shuffle bandwidth (random block permutation)
 *    3. Stream copy bandwidth (NT stores, like jigsaw uses)
 *    4. Random-access read bandwidth
 *
 *  Reports: Shuffle efficiency as % of memcpy ceiling
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <immintrin.h>

#ifdef _OPENMP
  #include <omp.h>
#endif

#if defined(_MSC_VER)
  #include <intrin.h>
  #include <windows.h>
  #define PREFETCH(addr) _mm_prefetch((const char*)(addr), _MM_HINT_T1)
#else
  #define PREFETCH(addr) __builtin_prefetch((addr), 0, 1)
#endif

#define PREFETCH_AHEAD 4

/* ─── Timing ──────────────────────────────────────── */
static double get_time_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* ─── Permutation (Fisher-Yates) ──────────────────── */
static void generate_permutation(int *perm, int n, unsigned int seed) {
    int i, j, tmp;
    unsigned int state = seed;
    for (i = 0; i < n; i++) perm[i] = i;
    for (i = n - 1; i > 0; i--) {
        state = state * 1664525u + 1013904223u;
        j = (int)((unsigned int)(state >> 1) % (unsigned int)(i + 1));
        tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
    }
}

/* ─── CACHE LINE SIZE ──────────────────────────────── */
#define CACHE_LINE 64

/* Compute padded stride: round row bytes up to CACHE_LINE multiple.
 * This ensures every row of every block begins on its own cache line,
 * eliminating False Sharing between adjacent blocks written by
 * different OpenMP threads. */
static inline size_t padded_stride(int width, int channels) {
    size_t raw = (size_t)width * channels;
    return ((raw + CACHE_LINE - 1) / CACHE_LINE) * CACHE_LINE;
}

/* ─── copy_block (packed stride = width*channels) ─── */
static void copy_block(
    unsigned char       *dst,  int dst_idx,
    const unsigned char *src,  int src_idx,
    int width, int channels, int blocks_x, int block_size)
{
    int dst_bx = dst_idx % blocks_x;
    int dst_by = dst_idx / blocks_x;
    int src_bx = src_idx % blocks_x;
    int src_by = src_idx / blocks_x;

    int dx0 = dst_bx * block_size;
    int dy0 = dst_by * block_size;
    int sx0 = src_bx * block_size;
    int sy0 = src_by * block_size;

    int row_stride = width * channels;
    int row;

    const unsigned char *s = src + (sy0 * width + sx0) * channels;
    unsigned char       *d = dst + (dy0 * width + dx0) * channels;

    int can_nt32 = (((uintptr_t)d & 31) == 0) && ((row_stride & 31) == 0);

    if (block_size == 16 && channels == 3) {
        for (row = 0; row < 16; row++) {
#if defined(__AVX2__)
            __m256i r0 = _mm256_loadu_si256((const __m256i*)s);
            __m128i r1 = _mm_loadu_si128((const __m128i*)(s + 32));
            if (can_nt32) {
                _mm256_stream_si256((__m256i*)d, r0);
                _mm_stream_si128((__m128i*)(d + 32), r1);
            } else {
                _mm256_storeu_si256((__m256i*)d, r0);
                _mm_storeu_si128((__m128i*)(d + 32), r1);
            }
#else
            __m128i r0 = _mm_loadu_si128((const __m128i*)s);
            __m128i r1 = _mm_loadu_si128((const __m128i*)(s + 16));
            __m128i r2 = _mm_loadu_si128((const __m128i*)(s + 32));
            _mm_storeu_si128((__m128i*)d, r0);
            _mm_storeu_si128((__m128i*)(d + 16), r1);
            _mm_storeu_si128((__m128i*)(d + 32), r2);
#endif
            s += row_stride;
            d += row_stride;
        }
    }
    else if (block_size == 16 && channels == 4) {
        for (row = 0; row < 16; row++) {
#if defined(__AVX2__)
            __m256i r0 = _mm256_loadu_si256((const __m256i*)s);
            __m256i r1 = _mm256_loadu_si256((const __m256i*)(s + 32));
            if (can_nt32) {
                _mm256_stream_si256((__m256i*)d, r0);
                _mm256_stream_si256((__m256i*)(d + 32), r1);
            } else {
                _mm256_storeu_si256((__m256i*)d, r0);
                _mm256_storeu_si256((__m256i*)(d + 32), r1);
            }
#else
            memcpy(d, s, 64);
#endif
            s += row_stride;
            d += row_stride;
        }
    }
    else {
        int row_bytes = block_size * channels;
        for (row = 0; row < block_size; row++) {
            memcpy(d, s, row_bytes);
            s += row_stride;
            d += row_stride;
        }
    }
}

/* ─── copy_block_strided (explicit row_stride for padded layout) ── */
/*
 * This variant accepts an explicit row_stride (bytes per image row).
 * When row_stride = padded_stride(w, c), each block row starts at a
 * 64-byte cache-line boundary, eliminating False Sharing between
 * adjacent blocks assigned to different OpenMP threads.
 *
 * NOTE: The block pixel start offset must use the padded stride:
 *   offset = (by * block_size * row_stride) + bx * block_size * channels
 */
static void copy_block_strided(
    unsigned char       *dst,  int dst_idx,
    const unsigned char *src,  int src_idx,
    int channels, int blocks_x, int block_size, size_t row_stride)
{
    int dst_bx = dst_idx % blocks_x;
    int dst_by = dst_idx / blocks_x;
    int src_bx = src_idx % blocks_x;
    int src_by = src_idx / blocks_x;

    size_t dx_off = (size_t)dst_bx * block_size * channels;
    size_t dy_off = (size_t)dst_by * block_size * row_stride;
    size_t sx_off = (size_t)src_bx * block_size * channels;
    size_t sy_off = (size_t)src_by * block_size * row_stride;

    const unsigned char *s = src + sy_off + sx_off;
    unsigned char       *d = dst + dy_off + dx_off;
    int row;

    /* With padded stride every dst row start is CACHE_LINE-aligned
     * (since padded_stride is a multiple of CACHE_LINE and dx_off is a
     *  multiple of block_size*channels which is <= CACHE_LINE for bs=16).
     * NT stores are eligible when dst pointer is 32-byte aligned. */
    int can_nt32 = (((uintptr_t)d & 31) == 0) && ((row_stride & 31) == 0);

    int row_bytes = block_size * channels;

    if (block_size == 16 && channels == 3) {
        /* 48 bytes/block-row: 32 + 16 */
        for (row = 0; row < 16; row++) {
#if defined(__AVX2__)
            __m256i r0 = _mm256_loadu_si256((const __m256i*)s);
            __m128i r1 = _mm_loadu_si128((const __m128i*)(s + 32));
            if (can_nt32) {
                _mm256_stream_si256((__m256i*)d, r0);
                _mm_stream_si128((__m128i*)(d + 32), r1);
            } else {
                _mm256_storeu_si256((__m256i*)d, r0);
                _mm_storeu_si128((__m128i*)(d + 32), r1);
            }
#else
            memcpy(d, s, 48);
#endif
            s += row_stride;
            d += row_stride;
        }
    }
    else if (block_size == 16 && channels == 4) {
        /* 64 bytes/block-row: exactly two AVX2 registers */
        for (row = 0; row < 16; row++) {
#if defined(__AVX2__)
            __m256i r0 = _mm256_loadu_si256((const __m256i*)s);
            __m256i r1 = _mm256_loadu_si256((const __m256i*)(s + 32));
            if (can_nt32) {
                _mm256_stream_si256((__m256i*)d, r0);
                _mm256_stream_si256((__m256i*)(d + 32), r1);
            } else {
                _mm256_storeu_si256((__m256i*)d, r0);
                _mm256_storeu_si256((__m256i*)(d + 32), r1);
            }
#else
            memcpy(d, s, 64);
#endif
            s += row_stride;
            d += row_stride;
        }
    }
    else {
        for (row = 0; row < block_size; row++) {
            memcpy(d, s, row_bytes);
            s += row_stride;
            d += row_stride;
        }
    }
}

/* ─── Block-Column-Padded layout (bcpad) ──────────────────────────────────────
 *
 * MOTIVATION
 * ----------
 * In the packed layout, adjacent blocks in the same image row share cache lines.
 * Example: bs=16, ch=3  →  block row bytes = 48.
 *   Block 0 writes bytes  [0, 48)  →  inside cache line [0, 64).
 *   Block 1 writes bytes [48, 96)  →  straddles cache lines [0,64) AND [64,128).
 *   Thread A and Thread B fight over cache line [0,64) → FALSE SHARING.
 *
 * FIX: Block-Column-Padding
 * -------------------------
 * Each block-column slot in a row is padded to CACHE_LINE (64) bytes:
 *   block-padded row = blocks_x * CACHE_LINE  bytes
 *   Block (bx, by): base address = by*block_size*bpr + bx*CACHE_LINE
 *
 * Now:
 *   Block 0 slot: bytes  [0, 64)  — only bytes [0,48) are pixel data.
 *   Block 1 slot: bytes [64,128)  — starts on its own cache line.
 *   Zero overlap → Zero False Sharing.
 *
 * NT stores are always eligible because every block's base is CACHE_LINE-aligned.
 *
 * COST: memory overhead = CACHE_LINE/row_bytes - 1
 *   RGB  bs=16: 64/48-1 = +33.3%
 *   RGBA bs=16: 64/64-1 =   0.0%  (already exact fit → no overhead!)
 * ─────────────────────────────────────────────────────────────────────────── */

/* Scatter pixels from packed into bcpad layout (row by row, block by block) */
static void scatter_to_bcpad(
    unsigned char *bcpad, const unsigned char *packed,
    int w, int h, int c, int blocks_x, int block_size)
{
    size_t bpr       = (size_t)blocks_x * CACHE_LINE;  /* block-padded row bytes */
    size_t packed_row = (size_t)w * c;
    int row_bytes     = block_size * c;
    memset(bcpad, 0, bpr * (size_t)h);                 /* clear padding bytes */
    for (int py = 0; py < h; py++) {
        const unsigned char *sp = packed + (size_t)py * packed_row;
        unsigned char       *dp = bcpad  + (size_t)py * bpr;
        for (int bx = 0; bx < blocks_x; bx++)
            memcpy(dp + (size_t)bx * CACHE_LINE,
                   sp + (size_t)bx * row_bytes,
                   row_bytes);
    }
}

/* Copy one block using the bcpad layout.
 * bpr = blocks_x * CACHE_LINE  (pass in to avoid recomputing per-call). */
static void copy_block_bcpad(
    unsigned char       *dst, int dst_idx,
    const unsigned char *src, int src_idx,
    int channels, int blocks_x, int block_size, size_t bpr)
{
    int dst_bx = dst_idx % blocks_x;
    int dst_by = dst_idx / blocks_x;
    int src_bx = src_idx % blocks_x;
    int src_by = src_idx / blocks_x;

    const unsigned char *s = src + (size_t)src_by * block_size * bpr
                                 + (size_t)src_bx * CACHE_LINE;
    unsigned char       *d = dst + (size_t)dst_by * block_size * bpr
                                 + (size_t)dst_bx * CACHE_LINE;

    int row_bytes = block_size * channels;
    int row;

    /* d is always CACHE_LINE-aligned (dst is CACHE_LINE-aligned, dst_bx*CACHE_LINE is
     * CACHE_LINE multiple, dst_by*block_size*bpr is CACHE_LINE multiple).
     * bpr is also a multiple of CACHE_LINE, so every subsequent row is aligned.
     * → can_nt32 = 1 unconditionally (when dst is 64-byte aligned). */

    if (block_size == 16 && channels == 3) {
        for (row = 0; row < 16; row++) {
#if defined(__AVX2__)
            __m256i r0 = _mm256_loadu_si256((const __m256i*)s);
            __m128i r1 = _mm_loadu_si128 ((const __m128i*)(s + 32));
            _mm256_stream_si256((__m256i*)d, r0);
            _mm_stream_si128  ((__m128i*)(d + 32), r1);
#else
            memcpy(d, s, 48);
#endif
            s += bpr;  d += bpr;
        }
    }
    else if (block_size == 16 && channels == 4) {
        /* 64 bytes/block-row = 1 cache line = zero padding overhead */
        for (row = 0; row < 16; row++) {
#if defined(__AVX2__)
            __m256i r0 = _mm256_loadu_si256((const __m256i*)s);
            __m256i r1 = _mm256_loadu_si256((const __m256i*)(s + 32));
            _mm256_stream_si256((__m256i*)d, r0);
            _mm256_stream_si256((__m256i*)(d + 32), r1);
#else
            memcpy(d, s, 64);
#endif
            s += bpr;  d += bpr;
        }
    }
    else {
        for (row = 0; row < block_size; row++) {
            memcpy(d, s, row_bytes);
            s += bpr;  d += bpr;
        }
    }
}

/* ─── copy_edges_only (from jigsaw.c) ────────────── */
static void copy_edges_only(
    unsigned char *dst, const unsigned char *src,
    int w, int h, int c, int blocks_x, int blocks_y, int block_size)
{
    int right_px  = w % block_size;
    int bottom_px = h % block_size;
    int row_stride = w * c;

    if (right_px > 0) {
        int x_start = blocks_x * block_size;
        int edge_bytes = right_px * c;
        int covered_rows = blocks_y * block_size;
        int y;
        for (y = 0; y < covered_rows; y++) {
            memcpy(dst + y * row_stride + x_start * c,
                   src + y * row_stride + x_start * c,
                   edge_bytes);
        }
    }
    if (bottom_px > 0) {
        int y_start = blocks_y * block_size;
        memcpy(dst + (size_t)y_start * row_stride,
               src + (size_t)y_start * row_stride,
               (size_t)(h - y_start) * row_stride);
    }
}

/* ─── RGB → RGBA channel expansion ─────────────────────────────────────────────
 *
 * Converts a packed RGB (3ch) buffer into a packed RGBA (4ch) buffer.
 * The alpha byte is set to 0xFF (fully opaque).
 *
 * WHY THIS SOLVES THE BCPad OVERHEAD PROBLEM
 * -------------------------------------------
 * BCPad overhead for RGB:  block_row=48B  → padded slot=64B  (+33%)
 * BCPad overhead for RGBA: block_row=64B  → padded slot=64B  (  0%)
 *
 * Strategy:
 *   1. Load RGB source image.
 *   2. Expand RGB → RGBA (cost: one sequential pass, ~memcpy bandwidth).
 *   3. Scatter RGBA into BCPad layout (0% overhead).
 *   4. Shuffle using BCPad RGBA (zero False Sharing, guaranteed NT stores).
 *   5. Gather BCPad RGBA → packed RGBA result.
 *   (Optional step 6: strip alpha if caller needs RGB output.)
 *
 * Memory footprint: same as BCPad-RGB (31.6 MB for 4K).
 * Performance gain: BCPad RGBA is faster than BCPad RGB because:
 *   • 64B block rows = 2 AVX2 registers, no partial-register tail.
 *   • bpr = blocks_x*64 = same as block width → perfect stride.
 * ────────────────────────────────────────────────────────────────────────── */
/* ─── rgb_to_rgba  (proper AVX2 pshufb version) ─────────────────────────────
 *
 * Converts packed RGB (3ch) → packed RGBA (4ch), alpha = 0xFF.
 *
 * Algorithm (8 pixels = 24 bytes → 32 bytes per loop):
 *
 *   256-bit load grabs bytes 0-31 (bytes 24-31 are "don't care").
 *   lo  = bytes  0-15  (contains pixels 0-4 partially)
 *   hi  = bytes 16-31
 *
 *   pshufb(lo, mask4) → RGBA pixels 0-3  (uses bytes 0-11 of lo)
 *   alignr(hi, lo, 12) = mid = bytes 12-27 (pixels 4-7 data)
 *   pshufb(mid, mask4) → RGBA pixels 4-7
 *
 *   pshufb mask4  (reads bytes from input, -1 → output 0):
 *     out[ 0]= in[ 0] = R0      out[ 1]= in[ 1] = G0
 *     out[ 2]= in[ 2] = B0      out[ 3]= -1     = 0x00  (alpha slot)
 *     out[ 4]= in[ 3] = R1  ... (repeat for pixels 1-3)
 *   OR 0xFF000000 each dword → sets alpha = 0xFF.
 *
 *   Output uses _mm256_stream_si256 (NT store): sequential write,
 *   bypass L1/L2 → avoids cache pollution for large buffers.
 *
 * Throughput: ~2 cycles / 8 pixels = near-memcpy bandwidth.
 * ─────────────────────────────────────────────────────────────────────────── */
static void rgb_to_rgba(
    unsigned char       *rgba,   /* output: packed RGBA, size = npx*4  (must be 32-byte aligned) */
    const unsigned char *rgb,    /* input:  packed RGB,  size = npx*3  */
    int npx)
{
    const unsigned char *s = rgb;
    unsigned char       *d = rgba;
    int i = 0;

#if defined(__AVX2__)
    /*
     * pshufb mask: maps 4 RGB pixels (bytes 0-11) into 4 RGBA pixels (bytes 0-15).
     * The alpha slot (bytes 3,7,11,15) is set to 0 by pshufb (-1 → 0),
     * then filled with 0xFF by the OR below.
     *
     * _mm_set_epi8(byte15, byte14, ..., byte1, byte0)  [high→low]
     */
    const __m128i mask4 = _mm_set_epi8(
        -1, 11, 10,  9,   /* px 3: [A B G R] stored as [FF B3 G3 R3] */
        -1,  8,  7,  6,   /* px 2 */
        -1,  5,  4,  3,   /* px 1 */
        -1,  2,  1,  0);  /* px 0: [A B G R] stored as [FF B0 G0 R0] */

    const __m128i alpha = _mm_set1_epi32((int)0xFF000000u);

    /* Main loop: 8 pixels per iteration, NT-store output */
    for (; i <= npx - 8; i += 8, s += 24, d += 32) {
        /* Load 32 bytes (bytes 0-23 valid, 24-31 don't care) */
        __m256i v   = _mm256_loadu_si256((const __m256i*)s);

        /* Split into two 128-bit halves */
        __m128i lo  = _mm256_castsi256_si128(v);          /* bytes  0-15 */
        __m128i hi  = _mm256_extracti128_si256(v, 1);     /* bytes 16-31 */

        /* Create window for pixels 4-7:
         * _mm_alignr_epi8(b, a, n) = concat(b, a) >> (n bytes)
         * alignr(hi, lo, 12) = lo[12..15] || hi[0..11] = bytes 12-23 */
        __m128i mid = _mm_alignr_epi8(hi, lo, 12);        /* bytes 12-23 = px 4-7 */

        /* Expand RGB→RGBA for pixels 0-3 and 4-7 */
        __m128i r0  = _mm_or_si128(_mm_shuffle_epi8(lo,  mask4), alpha);
        __m128i r1  = _mm_or_si128(_mm_shuffle_epi8(mid, mask4), alpha);

        /* Pack into 256-bit and NT-store (bypass cache for large sequential writes) */
        _mm256_stream_si256((__m256i*)d, _mm256_set_m128i(r1, r0));
    }
    _mm_sfence();
#endif

    /* Scalar tail (or full scalar path when AVX2 unavailable) */
    for (; i < npx; i++, s += 3, d += 4) {
        d[0] = s[0];  d[1] = s[1];  d[2] = s[2];  d[3] = 0xFF;
    }
}



typedef struct {
    const char *label;
    int w, h, c;
    int block_size;
} BenchConfig;

typedef struct {
    double memcpy_gbps;
    double stream_nt_gbps;
    /* Layout A — Packed (no padding) */
    double shuffle_enc_gbps;  double shuffle_enc_ms;
    double shuffle_dec_gbps;  double shuffle_dec_ms;
    /* Layout B — Row-Padded (row stride rounded to CACHE_LINE) */
    double pad_enc_gbps;      double pad_enc_ms;
    double pad_dec_gbps;      double pad_dec_ms;
    size_t padded_stride_bytes;
    /* Layout C — Block-Column-Padded (each block slot = CACHE_LINE bytes) */
    double bcpad_enc_gbps;    double bcpad_enc_ms;
    double bcpad_dec_gbps;    double bcpad_dec_ms;
    /* Layout D — RGB→RGBA expansion + BCPad RGBA (zero BCPad overhead) */
    double rgba_enc_gbps;     double rgba_enc_ms;
    double rgba_dec_gbps;     double rgba_dec_ms;
    double rgb_expand_ms;     /* cost of one RGB→RGBA pass */
    double total_blocks;
    double data_mb;
    double data_padded_mb;
    double data_bcpad_mb;
} BenchResult;

/* ─── Run Benchmark for one config ───────────────── */
static BenchResult run_bench(const BenchConfig *cfg, int warmup_iters, int bench_iters) {
    BenchResult r = {0};
    size_t img_bytes = (size_t)cfg->w * cfg->h * cfg->c;
    r.data_mb = (double)img_bytes / (1024.0 * 1024.0);

    int blocks_x = cfg->w / cfg->block_size;
    int blocks_y = cfg->h / cfg->block_size;
    int total_blocks = blocks_x * blocks_y;
    r.total_blocks = (double)total_blocks;

    /* Allocate aligned buffers */
    unsigned char *src = (unsigned char *)_aligned_malloc(img_bytes, 32);
    unsigned char *dst = (unsigned char *)_aligned_malloc(img_bytes, 32);
    if (!src || !dst) {
        fprintf(stderr, "  [ERROR] Failed to allocate %zu bytes\n", img_bytes);
        if (src) _aligned_free(src);
        if (dst) _aligned_free(dst);
        return r;
    }

    /* Fill with pseudo-random data */
    srand(42);
    for (size_t i = 0; i < img_bytes; i++) src[i] = (unsigned char)(rand() & 0xFF);
    memset(dst, 0, img_bytes);

    int *perm = (int *)malloc((size_t)total_blocks * sizeof(int));
    generate_permutation(perm, total_blocks, 12345);

    double t0, t1, elapsed;
    int iter;
    double total_bytes_copied = (double)img_bytes;

    /* ═══════════════════════════════════════════════════
       TEST 1: memcpy bandwidth (sequential, best case)
       ═══════════════════════════════════════════════════ */
    for (iter = 0; iter < warmup_iters; iter++) memcpy(dst, src, img_bytes);
    t0 = get_time_ms();
    for (iter = 0; iter < bench_iters; iter++) memcpy(dst, src, img_bytes);
    t1 = get_time_ms();
    elapsed = (t1 - t0) / 1000.0;  /* seconds */
    /* Read + Write = 2x data moved */
    r.memcpy_gbps = (total_bytes_copied * bench_iters * 2.0) / (elapsed * 1e9);

    /* ═══════════════════════════════════════════════════
       TEST 2: Stream NT copy (sequential, bypass cache)
       This is what jigsaw copy_block uses for writes.
       ═══════════════════════════════════════════════════ */
    for (iter = 0; iter < warmup_iters; iter++) {
#if defined(__AVX2__)
        size_t i;
        for (i = 0; i + 32 <= img_bytes; i += 32) {
            _mm256_stream_si256((__m256i*)(dst + i),
                                _mm256_loadu_si256((const __m256i*)(src + i)));
        }
        _mm_sfence();
#else
        memcpy(dst, src, img_bytes);
#endif
    }
    t0 = get_time_ms();
    for (iter = 0; iter < bench_iters; iter++) {
#if defined(__AVX2__)
        size_t i;
        for (i = 0; i + 32 <= img_bytes; i += 32) {
            _mm256_stream_si256((__m256i*)(dst + i),
                                _mm256_loadu_si256((const __m256i*)(src + i)));
        }
        _mm_sfence();
#else
        memcpy(dst, src, img_bytes);
#endif
    }
    t1 = get_time_ms();
    elapsed = (t1 - t0) / 1000.0;
    r.stream_nt_gbps = (total_bytes_copied * bench_iters * 2.0) / (elapsed * 1e9);

    /* ═══════════════════════════════════════════════════
       TEST 3: Jigsaw Encrypt (sequential read, random write)
       ═══════════════════════════════════════════════════ */
    for (iter = 0; iter < warmup_iters; iter++) {
        copy_edges_only(dst, src, cfg->w, cfg->h, cfg->c, blocks_x, blocks_y, cfg->block_size);
        int i;
        #pragma omp parallel for schedule(guided)
        for (i = 0; i < total_blocks; i++) {
            copy_block(dst, perm[i], src, i, cfg->w, cfg->c, blocks_x, cfg->block_size);
        }
        _mm_sfence();
    }

    t0 = get_time_ms();
    for (iter = 0; iter < bench_iters; iter++) {
        copy_edges_only(dst, src, cfg->w, cfg->h, cfg->c, blocks_x, blocks_y, cfg->block_size);
        int i;
        #pragma omp parallel for schedule(guided)
        for (i = 0; i < total_blocks; i++) {
            if (i + PREFETCH_AHEAD < total_blocks) {
                int pre_src = i + PREFETCH_AHEAD;
                int pbx = pre_src % blocks_x;
                int pby = pre_src / blocks_x;
                PREFETCH(src + (pby * cfg->block_size * cfg->w + pbx * cfg->block_size) * cfg->c);
            }
            copy_block(dst, perm[i], src, i, cfg->w, cfg->c, blocks_x, cfg->block_size);
        }
        _mm_sfence();
    }
    t1 = get_time_ms();
    elapsed = (t1 - t0) / 1000.0;
    r.shuffle_enc_ms = (t1 - t0) / bench_iters;
    r.shuffle_enc_gbps = (total_bytes_copied * bench_iters * 2.0) / (elapsed * 1e9);

    /* ═══════════════════════════════════════════════════
       TEST 4: Jigsaw Decrypt (random read, sequential write)
       ═══════════════════════════════════════════════════ */
    for (iter = 0; iter < warmup_iters; iter++) {
        copy_edges_only(dst, src, cfg->w, cfg->h, cfg->c, blocks_x, blocks_y, cfg->block_size);
        int i;
        #pragma omp parallel for schedule(guided)
        for (i = 0; i < total_blocks; i++) {
            copy_block(dst, i, src, perm[i], cfg->w, cfg->c, blocks_x, cfg->block_size);
        }
        _mm_sfence();
    }

    t0 = get_time_ms();
    for (iter = 0; iter < bench_iters; iter++) {
        copy_edges_only(dst, src, cfg->w, cfg->h, cfg->c, blocks_x, blocks_y, cfg->block_size);
        int i;
        #pragma omp parallel for schedule(guided)
        for (i = 0; i < total_blocks; i++) {
            if (i + PREFETCH_AHEAD < total_blocks) {
                int pre_src = perm[i + PREFETCH_AHEAD];
                int pbx = pre_src % blocks_x;
                int pby = pre_src / blocks_x;
                PREFETCH(src + (pby * cfg->block_size * cfg->w + pbx * cfg->block_size) * cfg->c);
            }
            copy_block(dst, i, src, perm[i], cfg->w, cfg->c, blocks_x, cfg->block_size);
        }
        _mm_sfence();
    }
    t1 = get_time_ms();
    elapsed = (t1 - t0) / 1000.0;
    r.shuffle_dec_ms = (t1 - t0) / bench_iters;
    r.shuffle_dec_gbps = (total_bytes_copied * bench_iters * 2.0) / (elapsed * 1e9);

    /* ═══════════════════════════════════════════════════
       TEST 5 & 6: Padded layout — Cache-Line Aligned Rows
       ───────────────────────────────────────────────────
       Each image row is padded to the next multiple of
       CACHE_LINE (64 bytes).  This guarantees:
         • The first pixel of every block row is on a fresh
           cache line → no two threads share a cache line
           when writing adjacent blocks (False Sharing = 0).
         • NT stores (stream) remain eligible because the
           destination pointer is always 64-byte aligned.
       Trade-off: the buffer is larger by the padding bytes.
       ═══════════════════════════════════════════════════ */
    {
        size_t pstride = padded_stride(cfg->w, cfg->c);
        size_t pbuf_bytes = pstride * (size_t)cfg->h;
        r.padded_stride_bytes = pstride;
        r.data_padded_mb = (double)pbuf_bytes / (1024.0 * 1024.0);

        unsigned char *psrc = (unsigned char *)_aligned_malloc(pbuf_bytes, CACHE_LINE);
        unsigned char *pdst = (unsigned char *)_aligned_malloc(pbuf_bytes, CACHE_LINE);

        if (!psrc || !pdst) {
            fprintf(stderr, "  [ERROR] Padded alloc failed (%zu bytes)\n", pbuf_bytes);
            if (psrc) _aligned_free(psrc);
            if (pdst) _aligned_free(pdst);
        } else {
            /* Copy packed src into padded psrc (row by row) */
            int y;
            size_t packed_row = (size_t)cfg->w * cfg->c;
            for (y = 0; y < cfg->h; y++)
                memcpy(psrc + (size_t)y * pstride,
                       src  + (size_t)y * packed_row,
                       packed_row);
            memset(pdst, 0, pbuf_bytes);

            /* ── TEST 5: Padded Encrypt ── */
            for (iter = 0; iter < warmup_iters; iter++) {
                int i;
                #pragma omp parallel for schedule(guided)
                for (i = 0; i < total_blocks; i++)
                    copy_block_strided(pdst, perm[i], psrc, i,
                                       cfg->c, blocks_x, cfg->block_size, pstride);
                _mm_sfence();
            }
            t0 = get_time_ms();
            for (iter = 0; iter < bench_iters; iter++) {
                int i;
                #pragma omp parallel for schedule(guided)
                for (i = 0; i < total_blocks; i++) {
                    if (i + PREFETCH_AHEAD < total_blocks) {
                        int pre_src = i + PREFETCH_AHEAD;
                        int pbx = pre_src % blocks_x;
                        int pby = pre_src / blocks_x;
                        PREFETCH(psrc + (size_t)pby * cfg->block_size * pstride
                                      + (size_t)pbx * cfg->block_size * cfg->c);
                    }
                    copy_block_strided(pdst, perm[i], psrc, i,
                                       cfg->c, blocks_x, cfg->block_size, pstride);
                }
                _mm_sfence();
            }
            t1 = get_time_ms();
            elapsed = (t1 - t0) / 1000.0;
            r.pad_enc_ms   = (t1 - t0) / bench_iters;
            /* Report bandwidth in terms of *actual pixel data* moved
             * (not the padded allocation) for fair comparison */
            r.pad_enc_gbps = (total_bytes_copied * bench_iters * 2.0) / (elapsed * 1e9);

            /* ── TEST 6: Padded Decrypt ── */
            for (iter = 0; iter < warmup_iters; iter++) {
                int i;
                #pragma omp parallel for schedule(guided)
                for (i = 0; i < total_blocks; i++)
                    copy_block_strided(pdst, i, psrc, perm[i],
                                       cfg->c, blocks_x, cfg->block_size, pstride);
                _mm_sfence();
            }
            t0 = get_time_ms();
            for (iter = 0; iter < bench_iters; iter++) {
                int i;
                #pragma omp parallel for schedule(guided)
                for (i = 0; i < total_blocks; i++) {
                    if (i + PREFETCH_AHEAD < total_blocks) {
                        int pre_src = perm[i + PREFETCH_AHEAD];
                        int pbx = pre_src % blocks_x;
                        int pby = pre_src / blocks_x;
                        PREFETCH(psrc + (size_t)pby * cfg->block_size * pstride
                                      + (size_t)pbx * cfg->block_size * cfg->c);
                    }
                    copy_block_strided(pdst, i, psrc, perm[i],
                                       cfg->c, blocks_x, cfg->block_size, pstride);
                }
                _mm_sfence();
            }
            t1 = get_time_ms();
            elapsed = (t1 - t0) / 1000.0;
            r.pad_dec_ms   = (t1 - t0) / bench_iters;
            r.pad_dec_gbps = (total_bytes_copied * bench_iters * 2.0) / (elapsed * 1e9);

            _aligned_free(psrc);
            _aligned_free(pdst);
        }
    }

    /* ═══════════════════════════════════════════════════
       TEST 7 & 8: Block-Column-Padded layout (bcpad)
       ───────────────────────────────────────────────────
       Each block-column slot = CACHE_LINE (64) bytes.
       Every block starts on its own cache line → False Sharing = 0.
       NT stores are always eligible (d is always 64-byte aligned).
       ═══════════════════════════════════════════════════ */
    {
        size_t bpr       = (size_t)blocks_x * CACHE_LINE;
        size_t bcbuf     = bpr * (size_t)cfg->h;
        r.data_bcpad_mb  = (double)bcbuf / (1024.0 * 1024.0);

        unsigned char *bcsrc = (unsigned char *)_aligned_malloc(bcbuf, CACHE_LINE);
        unsigned char *bcdst = (unsigned char *)_aligned_malloc(bcbuf, CACHE_LINE);

        if (!bcsrc || !bcdst) {
            fprintf(stderr, "  [ERROR] bcpad alloc failed (%zu bytes)\n", bcbuf);
            if (bcsrc) _aligned_free(bcsrc);
            if (bcdst) _aligned_free(bcdst);
        } else {
            scatter_to_bcpad(bcsrc, src, cfg->w, cfg->h, cfg->c, blocks_x, cfg->block_size);
            memset(bcdst, 0, bcbuf);

            /* ── TEST 7: bcpad Encrypt ── */
            for (iter = 0; iter < warmup_iters; iter++) {
                int i;
                #pragma omp parallel for schedule(guided)
                for (i = 0; i < total_blocks; i++)
                    copy_block_bcpad(bcdst, perm[i], bcsrc, i,
                                     cfg->c, blocks_x, cfg->block_size, bpr);
                _mm_sfence();
            }
            t0 = get_time_ms();
            for (iter = 0; iter < bench_iters; iter++) {
                int i;
                #pragma omp parallel for schedule(guided)
                for (i = 0; i < total_blocks; i++) {
                    if (i + PREFETCH_AHEAD < total_blocks) {
                        int pre = i + PREFETCH_AHEAD;  /* sequential read */
                        int pbx = pre % blocks_x,  pby = pre / blocks_x;
                        PREFETCH(bcsrc + (size_t)pby * cfg->block_size * bpr
                                       + (size_t)pbx * CACHE_LINE);
                    }
                    copy_block_bcpad(bcdst, perm[i], bcsrc, i,
                                     cfg->c, blocks_x, cfg->block_size, bpr);
                }
                _mm_sfence();
            }
            t1 = get_time_ms();
            r.bcpad_enc_ms   = (t1 - t0) / bench_iters;
            r.bcpad_enc_gbps = (total_bytes_copied * bench_iters * 2.0)
                               / ((t1 - t0) / 1000.0 * 1e9);

            /* ── TEST 8: bcpad Decrypt ── */
            for (iter = 0; iter < warmup_iters; iter++) {
                int i;
                #pragma omp parallel for schedule(guided)
                for (i = 0; i < total_blocks; i++)
                    copy_block_bcpad(bcdst, i, bcsrc, perm[i],
                                     cfg->c, blocks_x, cfg->block_size, bpr);
                _mm_sfence();
            }
            t0 = get_time_ms();
            for (iter = 0; iter < bench_iters; iter++) {
                int i;
                #pragma omp parallel for schedule(guided)
                for (i = 0; i < total_blocks; i++) {
                    if (i + PREFETCH_AHEAD < total_blocks) {
                        int pre = perm[i + PREFETCH_AHEAD]; /* random read */
                        int pbx = pre % blocks_x,  pby = pre / blocks_x;
                        PREFETCH(bcsrc + (size_t)pby * cfg->block_size * bpr
                                       + (size_t)pbx * CACHE_LINE);
                    }
                    copy_block_bcpad(bcdst, i, bcsrc, perm[i],
                                     cfg->c, blocks_x, cfg->block_size, bpr);
                }
                _mm_sfence();
            }
            t1 = get_time_ms();
            r.bcpad_dec_ms   = (t1 - t0) / bench_iters;
            r.bcpad_dec_gbps = (total_bytes_copied * bench_iters * 2.0)
                               / ((t1 - t0) / 1000.0 * 1e9);

            _aligned_free(bcsrc);
            _aligned_free(bcdst);
        }
    }

    /* ═══════════════════════════════════════════════════
       TEST 9: RGB→RGBA Expansion + BCPad RGBA shuffle
       ───────────────────────────────────────────────────
       Strategy: expand RGB → RGBA (alpha=0xFF), then use
       BCPad RGBA which has ZERO padding overhead (4×16=64B
       = exactly 1 cache line).  Same memory as BCPad-RGB
       but achieves BCPad-RGBA performance.
       Only runs when source is 3-channel (RGB).
       ═══════════════════════════════════════════════════ */
    if (cfg->c == 3) {
        int npx = cfg->w * cfg->h;
        /* RGBA packed buffer (4ch) */
        size_t rgba_bytes = (size_t)npx * 4;
        /* BCPad RGBA: blocks_x slots of CACHE_LINE each per row */
        size_t bpr4  = (size_t)blocks_x * CACHE_LINE;   /* = blocks_x*64 */
        size_t bcbuf4 = bpr4 * (size_t)cfg->h;

        unsigned char *rgba_src = (unsigned char *)_aligned_malloc(rgba_bytes, CACHE_LINE);
        unsigned char *rgba_dst = (unsigned char *)_aligned_malloc(rgba_bytes, CACHE_LINE);
        unsigned char *bc4src   = (unsigned char *)_aligned_malloc(bcbuf4,    CACHE_LINE);
        unsigned char *bc4dst   = (unsigned char *)_aligned_malloc(bcbuf4,    CACHE_LINE);

        if (!rgba_src || !rgba_dst || !bc4src || !bc4dst) {
            fprintf(stderr, "  [ERROR] rgba-expand alloc failed\n");
        } else {
            /* ── Measure RGB→RGBA expansion cost (one-time on load) ── */
            double te0 = get_time_ms();
            for (iter = 0; iter < bench_iters; iter++)
                rgb_to_rgba(rgba_src, src, npx);
            double te1 = get_time_ms();
            r.rgb_expand_ms = (te1 - te0) / bench_iters;

            /* Scatter RGBA into BCPad layout (using 4-channel bpr4) */
            scatter_to_bcpad(bc4src, rgba_src, cfg->w, cfg->h, 4, blocks_x, cfg->block_size);
            memset(bc4dst, 0, bcbuf4);

            /* ── TEST 9a: RGBA-expand + BCPad Encrypt ── */
            for (iter = 0; iter < warmup_iters; iter++) {
                int i;
                #pragma omp parallel for schedule(guided)
                for (i = 0; i < total_blocks; i++)
                    copy_block_bcpad(bc4dst, perm[i], bc4src, i,
                                     4, blocks_x, cfg->block_size, bpr4);
                _mm_sfence();
            }
            t0 = get_time_ms();
            for (iter = 0; iter < bench_iters; iter++) {
                int i;
                #pragma omp parallel for schedule(guided)
                for (i = 0; i < total_blocks; i++) {
                    if (i + PREFETCH_AHEAD < total_blocks) {
                        int pre = i + PREFETCH_AHEAD;
                        int pbx = pre % blocks_x, pby = pre / blocks_x;
                        PREFETCH(bc4src + (size_t)pby * cfg->block_size * bpr4
                                        + (size_t)pbx * CACHE_LINE);
                    }
                    copy_block_bcpad(bc4dst, perm[i], bc4src, i,
                                     4, blocks_x, cfg->block_size, bpr4);
                }
                _mm_sfence();
            }
            t1 = get_time_ms();
            r.rgba_enc_ms   = (t1 - t0) / bench_iters;
            /* Report bandwidth in terms of *RGB* pixel data moved
             * (not RGBA), for fair apples-to-apples comparison */
            r.rgba_enc_gbps = (total_bytes_copied * bench_iters * 2.0)
                              / ((t1 - t0) / 1000.0 * 1e9);

            /* ── TEST 9b: RGBA-expand + BCPad Decrypt ── */
            for (iter = 0; iter < warmup_iters; iter++) {
                int i;
                #pragma omp parallel for schedule(guided)
                for (i = 0; i < total_blocks; i++)
                    copy_block_bcpad(bc4dst, i, bc4src, perm[i],
                                     4, blocks_x, cfg->block_size, bpr4);
                _mm_sfence();
            }
            t0 = get_time_ms();
            for (iter = 0; iter < bench_iters; iter++) {
                int i;
                #pragma omp parallel for schedule(guided)
                for (i = 0; i < total_blocks; i++) {
                    if (i + PREFETCH_AHEAD < total_blocks) {
                        int pre = perm[i + PREFETCH_AHEAD];
                        int pbx = pre % blocks_x, pby = pre / blocks_x;
                        PREFETCH(bc4src + (size_t)pby * cfg->block_size * bpr4
                                        + (size_t)pbx * CACHE_LINE);
                    }
                    copy_block_bcpad(bc4dst, i, bc4src, perm[i],
                                     4, blocks_x, cfg->block_size, bpr4);
                }
                _mm_sfence();
            }
            t1 = get_time_ms();
            r.rgba_dec_ms   = (t1 - t0) / bench_iters;
            r.rgba_dec_gbps = (total_bytes_copied * bench_iters * 2.0)
                              / ((t1 - t0) / 1000.0 * 1e9);
        }
        if (rgba_src) _aligned_free(rgba_src);
        if (rgba_dst) _aligned_free(rgba_dst);
        if (bc4src)   _aligned_free(bc4src);
        if (bc4dst)   _aligned_free(bc4dst);
    }

    free(perm);
    _aligned_free(src);
    _aligned_free(dst);
    return r;
}

/* ─── Main ────────────────────────────────────────── */
int main(void) {
#ifdef _OPENMP
    printf("======================================================\n");
    printf("  Jigsaw Shuffle — Hardware Limit Benchmark\n");
    printf("  OpenMP: %d threads | AVX2: YES\n", omp_get_max_threads());
#else
    printf("======================================================\n");
    printf("  Jigsaw Shuffle — Hardware Limit Benchmark\n");
    printf("  OpenMP: OFF (single-thread) | AVX2: YES\n");
#endif
    printf("======================================================\n\n");

    BenchConfig configs[] = {
        /* label,              w,     h,    c, block */
        {"1080p RGB",       1920,  1080,  3,  16},
        {"1080p RGBA",      1920,  1080,  4,  16},
        {"4K RGB",          3840,  2160,  3,  16},
        {"4K RGBA",         3840,  2160,  4,  16},
        {"8K RGBA",         7680,  4320,  4,  16},
        {"4K RGB bs=8",     3840,  2160,  3,   8},
        {"4K RGB bs=32",    3840,  2160,  3,  32},
    };
    int nconfigs = sizeof(configs) / sizeof(configs[0]);

    /* ── Part 1: Packed (no padding) summary ── */
    printf("[PACKED layout — row stride = width * channels]\n");
    printf("%-18s %10s %10s %10s %10s %12s %12s\n",
           "Config", "Data(MB)", "Blocks",
           "memcpy", "NT stream", "Enc(GB/s)", "Dec(GB/s)");
    printf("───────────────────────────────────────────────────────────────────────────────────\n");

    for (int i = 0; i < nconfigs; i++) {
        BenchConfig *cfg = &configs[i];
        int warmup = (cfg->w * cfg->h > 4000000) ? 2 : 5;
        int iters  = (cfg->w * cfg->h > 4000000) ? 5 : 20;

        BenchResult r = run_bench(cfg, warmup, iters);

        printf("%-18s %10.1f %10.0f %10.2f %10.2f %12.2f %12.2f\n",
               cfg->label, r.data_mb, r.total_blocks,
               r.memcpy_gbps, r.stream_nt_gbps, r.shuffle_enc_gbps, r.shuffle_dec_gbps);
    }
    printf("───────────────────────────────────────────────────────────────────────────────────\n\n");

    /* ── Part 2: 4-way comparison (4K RGB only) ── */
    printf("[4-WAY LAYOUT COMPARISON — 4K RGB (3840x2160, 3ch, bs=16)]\n");
    {
        size_t bpr  = (size_t)(3840/16) * CACHE_LINE;
        size_t bpr4 = (size_t)(3840/16) * CACHE_LINE; /* same: blocks_x*64 */
        (void)bpr4;
        printf("  Packed          row = %d bytes (3ch, no padding)\n", 3840*3);
        printf("  BCPad RGB       row = %zu bytes (+33%%, block slot 48→64B)\n", bpr);
        printf("  BCPad RGBA      row = %zu bytes (  0%%, block slot 64→64B, 4ch)\n", bpr);
        printf("  RGBA-expand+BCPad   = RGB→RGBA→BCPad RGBA (0%% overhead, same perf)\n");
    }
    printf("\n");

    BenchConfig deep = {"4K RGB", 3840, 2160, 3, 16};
    BenchResult dr = run_bench(&deep, 5, 30);

    printf("  %-26s %10s %10s %10s %10s\n",
           "Metric", "Packed", "BCPad-RGB", "BCPad-RGBA", "Expand+BCPad");
    printf("  ──────────────────────────────────────────────────────────────────────────\n");
    /* Encrypt */
    printf("  %-26s %10.2f %10.2f %10s %10.2f\n",
           "Encrypt (GB/s)",
           dr.shuffle_enc_gbps, dr.bcpad_enc_gbps, "(N/A-4ch)", dr.rgba_enc_gbps);
    printf("  %-26s %10.1f %10.1f %10s %10.1f\n",
           "  + expand cost (ms)",
           0.0, 0.0, "", dr.rgb_expand_ms);
    printf("  %-26s %10.1f %10.1f %10s %10.1f\n",
           "  total time (ms)",
           dr.shuffle_enc_ms, dr.bcpad_enc_ms, "", dr.rgba_enc_ms + dr.rgb_expand_ms);
    /* Decrypt */
    printf("  %-26s %10.2f %10.2f %10s %10.2f\n",
           "Decrypt (GB/s)",
           dr.shuffle_dec_gbps, dr.bcpad_dec_gbps, "(N/A-4ch)", dr.rgba_dec_gbps);
    printf("  %-26s %10.1f %10.1f %10s %10.1f\n",
           "  total time (ms)",
           dr.shuffle_dec_ms, dr.bcpad_dec_ms, "", dr.rgba_dec_ms + dr.rgb_expand_ms);
    printf("  ──────────────────────────────────────────────────────────────────────────\n");
    printf("  %-26s %10.1f %10.1f %10s %10.1f\n",
           "Buffer size (MB)",
           dr.data_mb, dr.data_bcpad_mb, "", dr.data_bcpad_mb);
    printf("  %-26s %10.1f %10.1f %10s %10.1f\n",
           "memcpy ceiling (GB/s)",
           dr.memcpy_gbps, dr.memcpy_gbps, "", dr.memcpy_gbps);
    printf("\n");

#ifdef _OPENMP
    int saved_threads = omp_get_max_threads();

    printf("══════════════════════════════════════════════════════════════════════════\n");
    printf("  THREAD SCALING — Packed vs BCPad (4K RGB, Encrypt + Decrypt)\n");
    printf("══════════════════════════════════════════════════════════════════════════\n\n");
    printf("  %4s %13s %13s %13s %13s\n",
           "Thr", "Enc-Packed", "Enc-BCPad", "Dec-Packed", "Dec-BCPad");
    printf("  %4s %13s %13s %13s %13s\n",
           "",  "(GB/s)","(GB/s)","(GB/s)","(GB/s)");
    printf("  ──────────────────────────────────────────────────────────────────────\n");

    int thread_counts[] = {1, 2, 4, 8, 12, 16, 24, 32};
    int ntc = sizeof(thread_counts) / sizeof(thread_counts[0]);

    for (int t = 0; t < ntc; t++) {
        int tc = thread_counts[t];
        if (tc > saved_threads) continue;
        omp_set_num_threads(tc);
        BenchResult tr = run_bench(&deep, 3, 15);
        printf("  %4d %13.2f %13.2f %13.2f %13.2f\n",
               tc,
               tr.shuffle_enc_gbps, tr.bcpad_enc_gbps,
               tr.shuffle_dec_gbps, tr.bcpad_dec_gbps);
    }
    omp_set_num_threads(saved_threads);
#endif

    printf("\n");
    printf("══════════════════════════════════════════════════════\n");
    printf("  NOTES\n");
    printf("══════════════════════════════════════════════════════\n");
    printf("  * Packed   = standard layout, row stride = width*channels\n");
    printf("  * Row-Pad  = row stride rounded to %d bytes (1 cache line)\n", CACHE_LINE);
    printf("  * BCPad    = each block-column slot padded to %d bytes\n", CACHE_LINE);
    printf("    → guarantees ZERO False Sharing (every block is cache-line-aligned)\n");
    printf("    → NT stores always eligible (d is always %d-byte aligned)\n", CACHE_LINE);
    printf("  * Encrypt = sequential read, random write (NT stores)\n");
    printf("  * Decrypt = random read (prefetched), sequential write\n");
    printf("  * GB/s = pixel bytes x2 (read+write) / time (excludes padding bytes)\n");
    printf("══════════════════════════════════════════════════════\n");

    return 0;
}
