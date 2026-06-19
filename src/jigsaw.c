/*
 * ============================================================
 *  Jigsaw Image Encrypter
 *  Pure C  |  stb_image  |  OpenMP
 * ============================================================
 *
 *  Usage:
 *    jigsaw encrypt <input> <output> <seed> [block_size]
 *    jigsaw decrypt <input> <output> <seed> [block_size]
 * ============================================================
 */

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "stb_image.h"
#include "stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _OPENMP
  #include <omp.h>
#endif

#if defined(_WIN32) || defined(_MSC_VER)
  #include <windows.h>
#endif

#include "jigsaw.h"
#include <stdint.h>
#include <immintrin.h>

extern int  fpng_encode_to_file_c(const char *path, const void *data,
                                   int w, int h, int num_chans);
extern unsigned char *fpng_decode_from_file_c(const char *path,
                                               int *w, int *h, int *c,
                                               int *not_fpng);

/* MSVC / GCC prefetch intrinsic */
#if defined(_MSC_VER)
  #include <intrin.h>
  /* _mm_prefetch(addr, hint): hint = _MM_HINT_T0 (L1 cache) */
  #define PREFETCH(addr)  _mm_prefetch((const char*)(addr), _MM_HINT_T1)
#elif defined(__GNUC__)
  #define PREFETCH(addr)  __builtin_prefetch((addr), 0, 1)
#else
  #define PREFETCH(addr)  ((void)0)
#endif

/* ── ค่าคงที่ ───────────────────────────────────── */
#define DEFAULT_BLOCK_SIZE   16
#define JPEG_QUALITY         95
#define PREFETCH_AHEAD        4    /* prefetch 4 blocks ahead */

/* ────────────────────────────────────────────────────────────
   generate_permutation
   สร้างอาร์เรย์ perm[0..n-1] ที่สับแบบ Fisher-Yates
   ──────────────────────────────────────────────────────────── */
static void generate_permutation(int *perm, int n, unsigned int seed)
{
    int i, j, tmp;
    unsigned int state = seed; // local state – thread‑safe
    for (i = 0; i < n; i++) perm[i] = i;
    for (i = n - 1; i > 0; i--) {
        state = state * 1664525u + 1013904223u; // Numerical Recipes LCG
        j = (int)((unsigned int)(state >> 1) % (unsigned int)(i + 1));
        tmp = perm[i];
        perm[i] = perm[j];
        perm[j] = tmp;
    }
}

/* ────────────────────────────────────────────────────────────
   block_src_ptr
   คืน pointer ไปยังพิกเซลซ้ายบนของบล็อก block_idx
   ──────────────────────────────────────────────────────────── */
static inline const unsigned char *block_src_ptr(
    const unsigned char *base, int block_idx,
    int width, int channels, int blocks_x, int block_size)
{
    int bx = block_idx % blocks_x;
    int by = block_idx / blocks_x;
    return base + ((by * block_size) * width + bx * block_size) * channels;
}

/* ────────────────────────────────────────────────────────────
   copy_block  (with prefetch hint parameter)
   คัดลอกบล็อก block_size×block_size pixels
   ──────────────────────────────────────────────────────────── */
static inline void copy_row_generic_simd(unsigned char *d, const unsigned char *s, int len)
{
    int i = 0;
#if defined(__AVX2__)
    for (; i <= len - 32; i += 32) {
        _mm256_storeu_si256((__m256i*)(d + i), _mm256_loadu_si256((const __m256i*)(s + i)));
    }
#endif
    for (; i <= len - 16; i += 16) {
        _mm_storeu_si128((__m128i*)(d + i), _mm_loadu_si128((const __m128i*)(s + i)));
    }
    int rem = len - i;
    if (rem >= 8) {
        *(uint64_t*)(d + i) = *(const uint64_t*)(s + i);
        i += 8;
        rem -= 8;
    }
    if (rem >= 4) {
        *(uint32_t*)(d + i) = *(const uint32_t*)(s + i);
        i += 4;
        rem -= 4;
    }
    if (rem >= 2) {
        *(uint16_t*)(d + i) = *(const uint16_t*)(s + i);
        i += 2;
        rem -= 2;
    }
    if (rem >= 1) {
        d[i] = s[i];
    }
}

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

    int row_bytes = block_size * channels;
    int row_stride = width * channels;
    int row;

    const unsigned char *s = src + (sy0 * width + sx0) * channels;
    unsigned char       *d = dst + (dy0 * width + dx0) * channels;

    /* ── Non-Temporal (NT) store eligibility ──────────────────────
     * NT stores bypass cache (no Read-For-Ownership), writing directly
     * to RAM via write-combining buffers. Requires 16/32-byte alignment
     * on both the block start pointer AND row_stride so every subsequent
     * row remains aligned throughout the block copy loop.
     * Caller must call _mm_sfence() after all copy_block calls. */
    int can_nt16 = (((uintptr_t)d & 15) == 0) && ((row_stride & 15) == 0);
    int can_nt32 = (((uintptr_t)d & 31) == 0) && ((row_stride & 31) == 0);

    if (block_size == 16 && channels == 3) {
        /* row_bytes = 48 = 32 + 16 */
        for (row = 0; row < 16; row++) {
#if defined(__AVX2__)
            __m256i r0 = _mm256_loadu_si256((const __m256i*)s);
            __m128i r1 = _mm_loadu_si128((const __m128i*)(s + 32));
            if (can_nt32) {
                _mm256_stream_si256((__m256i*)d,        r0);
                _mm_stream_si128  ((__m128i*)(d + 32),  r1);
            } else {
                _mm256_storeu_si256((__m256i*)d,        r0);
                _mm_storeu_si128   ((__m128i*)(d + 32), r1);
            }
#else
            __m128i r0 = _mm_loadu_si128((const __m128i*)s);
            __m128i r1 = _mm_loadu_si128((const __m128i*)(s + 16));
            __m128i r2 = _mm_loadu_si128((const __m128i*)(s + 32));
            if (can_nt16) {
                _mm_stream_si128((__m128i*)d,        r0);
                _mm_stream_si128((__m128i*)(d + 16), r1);
                _mm_stream_si128((__m128i*)(d + 32), r2);
            } else {
                _mm_storeu_si128((__m128i*)d,        r0);
                _mm_storeu_si128((__m128i*)(d + 16), r1);
                _mm_storeu_si128((__m128i*)(d + 32), r2);
            }
#endif
            s += row_stride;
            d += row_stride;
        }
    }
    else if (block_size == 16 && channels == 4) {
        /* row_bytes = 64 = 32 + 32 */
        for (row = 0; row < 16; row++) {
#if defined(__AVX2__)
            __m256i r0 = _mm256_loadu_si256((const __m256i*)s);
            __m256i r1 = _mm256_loadu_si256((const __m256i*)(s + 32));
            if (can_nt32) {
                _mm256_stream_si256((__m256i*)d,        r0);
                _mm256_stream_si256((__m256i*)(d + 32), r1);
            } else {
                _mm256_storeu_si256((__m256i*)d,        r0);
                _mm256_storeu_si256((__m256i*)(d + 32), r1);
            }
#else
            __m128i r0 = _mm_loadu_si128((const __m128i*)s);
            __m128i r1 = _mm_loadu_si128((const __m128i*)(s + 16));
            __m128i r2 = _mm_loadu_si128((const __m128i*)(s + 32));
            __m128i r3 = _mm_loadu_si128((const __m128i*)(s + 48));
            if (can_nt16) {
                _mm_stream_si128((__m128i*)d,        r0);
                _mm_stream_si128((__m128i*)(d + 16), r1);
                _mm_stream_si128((__m128i*)(d + 32), r2);
                _mm_stream_si128((__m128i*)(d + 48), r3);
            } else {
                _mm_storeu_si128((__m128i*)d,        r0);
                _mm_storeu_si128((__m128i*)(d + 16), r1);
                _mm_storeu_si128((__m128i*)(d + 32), r2);
                _mm_storeu_si128((__m128i*)(d + 48), r3);
            }
#endif
            s += row_stride;
            d += row_stride;
        }
    }
    else if (block_size == 8 && channels == 3) {
        /* row_bytes = 24 = 16 + 8 (tail 8 bytes: no NT store available) */
        for (row = 0; row < 8; row++) {
            __m128i r0   = _mm_loadu_si128((const __m128i*)s);
            uint64_t r1  = *(const uint64_t*)(s + 16);
            if (can_nt16)
                _mm_stream_si128((__m128i*)d, r0);
            else
                _mm_storeu_si128((__m128i*)d, r0);
            *(uint64_t*)(d + 16) = r1;   /* 8-byte tail: write-combining not available */
            s += row_stride;
            d += row_stride;
        }
    }
    else if (block_size == 8 && channels == 4) {
        /* row_bytes = 32 */
        for (row = 0; row < 8; row++) {
#if defined(__AVX2__)
            __m256i r0 = _mm256_loadu_si256((const __m256i*)s);
            if (can_nt32)
                _mm256_stream_si256((__m256i*)d, r0);
            else
                _mm256_storeu_si256((__m256i*)d, r0);
#else
            __m128i r0 = _mm_loadu_si128((const __m128i*)s);
            __m128i r1 = _mm_loadu_si128((const __m128i*)(s + 16));
            if (can_nt16) {
                _mm_stream_si128((__m128i*)d,        r0);
                _mm_stream_si128((__m128i*)(d + 16), r1);
            } else {
                _mm_storeu_si128((__m128i*)d,        r0);
                _mm_storeu_si128((__m128i*)(d + 16), r1);
            }
#endif
            s += row_stride;
            d += row_stride;
        }
    }
    else {
        for (row = 0; row < block_size; row++) {
            copy_row_generic_simd(d, s, row_bytes);
            s += row_stride;
            d += row_stride;
        }
    }
}


/* ── forward declarations ─────────────────────────── */
static const char *get_extension(const char *path);
static int         str_iequal(const char *a, const char *b);

/* ────────────────────────────────────────────────────────────
   GrowBuf — dynamic buffer เพื่อรับ encoded bytes จาก stb callback
   ──────────────────────────────────────────────────────────── */
typedef struct {
    unsigned char *data;
    size_t         size;
    size_t         capacity;
} GrowBuf;

static void growbuf_callback(void *ctx, void *data, int size)
{
    GrowBuf *g = (GrowBuf *)ctx;
    size_t need = g->size + (size_t)size;
    if (need > g->capacity) {
        size_t newcap = g->capacity ? g->capacity * 2 : (size_t)4 * 1024 * 1024;
        while (newcap < need) newcap *= 2;
        unsigned char *tmp = (unsigned char *)realloc(g->data, newcap);
        if (!tmp) { g->data = NULL; g->capacity = 0; return; } // OOM: bail out
        g->data = tmp;
        g->capacity = newcap;
    }
    if (!g->data) return; // guard from OOM from previous allocation
    memcpy(g->data + g->size, data, (size_t)size);
    g->size += (size_t)size;
}

/* ────────────────────────────────────────────────────────────
   save_image_fast
   [OPT-SAVE] encode ทั้งภาพลง RAM buffer ก่อน (ผ่าน callback)
   แล้วค่อย fwrite ครั้งเดียวใหญ่ ๆ ลงดิสก์
   แทนที่การ fwrite ทีละ 64 bytes ของ stb default
   ──────────────────────────────────────────────────────────── */
static int save_image(const char *path, unsigned char *img_data,
                      int w, int h, int c)
{
    char final_path[MAX_PATH];
    strcpy_s(final_path, sizeof(final_path), path);
    const char *ext = get_extension(path);
    if (ext[0] == '\0') {
        sprintf_s(final_path, sizeof(final_path), "%s.png", path);
        ext = "png";
    }

    GrowBuf g = {NULL, 0, 0};
    int ok = 0;
    FILE *fp;

    /* ── PNG: ลองใช้ fpng ก่อน (SSE4.1 fast path) ──
     * fpng รองรับแค่ RGB(3) และ RGBA(4)
     * ถ้า grayscale (1ch/2ch) → fallback ไป stb ตามปกติ */
    if (str_iequal(ext, "png") && (c == 3 || c == 4)) {
        ok = fpng_encode_to_file_c(final_path, img_data, w, h, c);
        return ok;
    }

    /* ── JPG / BMP / PNG-fallback: ใช้ stb + single fwrite ── */
    if (str_iequal(ext, "jpg") || str_iequal(ext, "jpeg"))
        ok = stbi_write_jpg_to_func(growbuf_callback, &g, w, h, c, img_data, JPEG_QUALITY);
    else if (str_iequal(ext, "png"))
        ok = stbi_write_png_to_func(growbuf_callback, &g, w, h, c, img_data, w * c);
    else if (str_iequal(ext, "bmp"))
        ok = stbi_write_bmp_to_func(growbuf_callback, &g, w, h, c, img_data);
    else {
        fprintf(stderr, "Error: unsupported format '.%s'\n", ext);
        return 0;
    }

    if (!ok || !g.data) {
        free(g.data);
        return 0;
    }

    /* single fwrite ลงดิสก์ */
#if defined(_MSC_VER)
    {
        errno_t err = fopen_s(&fp, final_path, "wb");
        if (err != 0 || !fp) { free(g.data); return 0; }
    }
#else
    fp = fopen(final_path, "wb");
    if (!fp) { free(g.data); return 0; }
#endif
    ok = (fwrite(g.data, 1, g.size, fp) == g.size);
    fclose(fp);
    free(g.data);
    return ok;
}

/* ────────────────────────────────────────────────────────────
   get_extension  /  str_iequal
   ──────────────────────────────────────────────────────────── */
static const char *get_extension(const char *path)
{
    const char *dot = strrchr(path, '.');
    return (dot && dot[1] != '\0') ? dot + 1 : "";
}

static int str_iequal(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
}

/* ────────────────────────────────────────────────────────────
   copy_edges_only
   [OPT-2] แทนที่จะ memcpy ทั้งภาพก่อน shuffle
   เราคัดลอกเฉพาะ "ขอบ" ที่ block shuffle ไม่ครอบคลุม:
     - คอลัมน์ขวา (ถ้า w % block_size != 0)
     - แถวล่าง   (ถ้า h % block_size != 0)
   ประหยัด: สำหรับ 4K → ไม่ต้อง copy 24MB โดยไม่จำเป็น
   ──────────────────────────────────────────────────────────── */
static void copy_edges_only(
    unsigned char *dst, const unsigned char *src,
    int w, int h, int c, int blocks_x, int blocks_y, int block_size)
{
    int right_px  = w % block_size;   /* กว้างของขอบขวา */
    int bottom_px = h % block_size;   /* สูงของขอบล่าง  */
    int row_stride = w * c;

    /* ── คอลัมน์ขวา (ทุก row ตั้งแต่ y=0 ถึง blocks_y*block_size-1) ── */
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

    /* ── แถวล่าง (ทั้ง row ตั้งแต่ y=blocks_y*block_size ถึง h-1) ── */
    if (bottom_px > 0) {
        int y_start = blocks_y * block_size;
        memcpy(dst + (size_t)y_start * row_stride,
               src + (size_t)y_start * row_stride,
               (size_t)(h - y_start) * row_stride);
    }
}

static void free_src_image(unsigned char *src, int used_fpng)
{
    if (!src) return;
    if (used_fpng) {
        free(src);
    } else {
        stbi_image_free(src);
    }
}

/* ── Batch / Directory Mode Types ─────────────────────────── */

typedef struct {
    char name[MAX_PATH];
} FileInfo;

/* ── Directory helpers ────────────────────────────────────── */
int is_directory(const char *path)
{
    DWORD dwAttrib = GetFileAttributesA(path);
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

static int create_directory_if_not_exists(const char *path)
{
    if (is_directory(path)) {
        return 1;
    }
    return CreateDirectoryA(path, NULL) ? 1 : 0;
}

static int scan_directory(const char *dir_path, FileInfo **out_files)
{
    char search_pattern[MAX_PATH];
    sprintf_s(search_pattern, sizeof(search_pattern), "%s\\*", dir_path);

    WIN32_FIND_DATAA find_data;
    HANDLE hFind = FindFirstFileA(search_pattern, &find_data);
    if (hFind == INVALID_HANDLE_VALUE) {
        return 0;
    }

    int capacity = 64;
    FileInfo *files = (FileInfo *)malloc(capacity * sizeof(FileInfo));
    if (!files) { FindClose(hFind); return 0; }   /* Fix: ตรวจ malloc fail */
    int count = 0;

    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const char *file_name = find_data.cFileName;
            const char *ext = get_extension(file_name);
            if (str_iequal(ext, "png") || str_iequal(ext, "jpg") || 
                str_iequal(ext, "jpeg") || str_iequal(ext, "bmp")) {
                if (count >= capacity) {
                    capacity *= 2;
                    FileInfo *tmp = (FileInfo *)realloc(files, capacity * sizeof(FileInfo));
                    if (!tmp) { free(files); FindClose(hFind); return 0; }
                    files = tmp;
                }
                strcpy_s(files[count].name, sizeof(files[count].name), file_name);
                count++;
            }
        }
    } while (FindNextFileA(hFind, &find_data));

    FindClose(hFind);
    *out_files = files;
    return count;
}

static unsigned char *load_image(
    const char *path, int *w, int *h, int *c, int *used_fpng)
{
    *used_fpng = 0;
    const char *ext = get_extension(path);
    if (str_iequal(ext, "png")) {
        int not_fpng = 0;
        unsigned char *px = fpng_decode_from_file_c(path, w, h, c, &not_fpng);
        if (px) { *used_fpng = 1; return px; }
        if (!not_fpng) return NULL; // Corrupted PNG file
    }
    return stbi_load(path, w, h, c, 0);
}

/* ────────────────────────────────────────────────────────────
   process_image_internal
   ──────────────────────────────────────────────────────────── */
int process_image_internal(const char *input_path, const char *output_path,
                           unsigned int seed, int block_size, int decrypt,
                           int png_level, ImageStats *stats)
{
    memset(stats, 0, sizeof(ImageStats));
    if (block_size < 1) {
        JLOG_ERR("Error: block_size must be >= 1\n");
        stats->status = 1;
        return 1;
    }
    clock_t t_total_start = clock();
    /* NOTE: stbi_write_png_compression_level ถูก set โดย caller
     * (process_image หรือ process_directory ก่อน parallel block)
     * ไม่ set ที่นี่เพื่อหลีกเลี่ยง data race ใน OpenMP batch mode */

    /* Resolve output path: if output_path is a directory, append input filename */
    char final_output_path[MAX_PATH];
    strcpy_s(final_output_path, sizeof(final_output_path), output_path);
    if (is_directory(output_path)) {
        const char *filename = strrchr(input_path, '\\');
        if (!filename) {
            filename = strrchr(input_path, '/');
        }
        if (filename) {
            filename++;
        } else {
            filename = input_path;
        }

        size_t len = strlen(output_path);
        if (len > 0 && (output_path[len - 1] == '\\' || output_path[len - 1] == '/')) {
            sprintf_s(final_output_path, sizeof(final_output_path), "%s%s", output_path, filename);
        } else {
            sprintf_s(final_output_path, sizeof(final_output_path), "%s\\%s", output_path, filename);
        }
    }
    strcpy_s(stats->final_output_path, sizeof(stats->final_output_path), final_output_path);

    /* ── โหลดภาพ ─────────────────────────────────── */
    int w = 0, h = 0, c = 0;
    int used_fpng_decoder = 0;
    clock_t t_load_start = clock();
    unsigned char *src = load_image(input_path, &w, &h, &c, &used_fpng_decoder);
    clock_t t_load_end = clock();

    if (!src) {
        stats->status = 1;
        return 1;
    }

    stats->w = w;
    stats->h = h;
    stats->c = c;
    stats->used_fpng_decoder = used_fpng_decoder;

    int blocks_x = w / block_size;
    int blocks_y = h / block_size;
    int total    = blocks_x * blocks_y;

    if (total < 2) {
        clock_t t_save_start = clock();
        int ok = save_image(final_output_path, src, w, h, c);
        clock_t t_save_end = clock();
        free_src_image(src, used_fpng_decoder);
        stats->load_ms = (double)(t_load_end - t_load_start) * 1000.0 / CLOCKS_PER_SEC;
        stats->save_ms = (double)(t_save_end - t_save_start) * 1000.0 / CLOCKS_PER_SEC;
        stats->total_ms = (double)(clock() - t_total_start) * 1000.0 / CLOCKS_PER_SEC;
        stats->status = ok ? 0 : 1;
        return ok ? 0 : 1;
    }

    /* ── permutation ─────────────────────────────── */
    int *perm = (int *)malloc((size_t)total * sizeof(int));
    if (!perm) { 
        free_src_image(src, used_fpng_decoder); 
        stats->status = 1;
        return 1; 
    }
    generate_permutation(perm, total, seed);

    /* ── output buffer ───────────────────────────── */
    size_t img_bytes = (size_t)w * h * c;
    /* 32-byte aligned: enables AVX2 NT (non-temporal) stream stores safely */
    unsigned char *dst = (unsigned char *)_aligned_malloc(img_bytes, 32);
    if (!dst) { 
        free(perm); 
        free_src_image(src, used_fpng_decoder); 
        stats->status = 1;
        return 1; 
    }

    copy_edges_only(dst, src, w, h, c, blocks_x, blocks_y, block_size);

    clock_t t_shuffle_start = clock();
    int i;
    #pragma omp parallel for schedule(guided)
    for (i = 0; i < total; i++) {
        int src_idx = decrypt ? perm[i] : i;
        int dst_idx = decrypt ? i       : perm[i];

        /* Prefetch src block into L2 (T1) ahead of time.
         * We do NOT prefetch dst because NT stores bypass cache entirely —
         * prefetching dst would only pollute L2 with data we overwrite. */
        if (i + PREFETCH_AHEAD < total) {
            int pre_src = decrypt ? perm[i + PREFETCH_AHEAD]
                                  : (i + PREFETCH_AHEAD);
            PREFETCH(block_src_ptr(src, pre_src, w, c, blocks_x, block_size));
        }

        copy_block(dst, dst_idx, src, src_idx, w, c, blocks_x, block_size);
    }
    /* Flush all NT write-combining buffers before any subsequent read of dst */
    _mm_sfence();
    clock_t t_shuffle_end = clock();

    free(perm);
    free_src_image(src, used_fpng_decoder);

    /* ── Save ────────────────────────────────────── */
    clock_t t_save_start = clock();
    int save_ok = save_image(final_output_path, dst, w, h, c);
    clock_t t_save_end = clock();

    _aligned_free(dst);

    stats->load_ms    = (double)(t_load_end    - t_load_start)    * 1000.0 / CLOCKS_PER_SEC;
    stats->shuffle_ms = (double)(t_shuffle_end - t_shuffle_start) * 1000.0 / CLOCKS_PER_SEC;
    stats->save_ms    = (double)(t_save_end    - t_save_start)    * 1000.0 / CLOCKS_PER_SEC;
    stats->total_ms   = (double)(clock()       - t_total_start)   * 1000.0 / CLOCKS_PER_SEC;
    stats->status     = save_ok ? 0 : 1;

    return save_ok ? 0 : 1;
}

/* ────────────────────────────────────────────────────────────
   process_image (Single-image console print wrapper)
   ──────────────────────────────────────────────────────────── */
int process_image(const char *input_path, const char *output_path,
                  unsigned int seed, int block_size, int decrypt, int png_level)
{
    /* Set stb PNG compression level (single-image path — ปลอดภัย ไม่มี race) */
    extern int stbi_write_png_compression_level;
    stbi_write_png_compression_level = png_level;

    ImageStats stats;
    int res = process_image_internal(input_path, output_path, seed, block_size, decrypt, png_level, &stats);
    if (res != 0) {
        return res;
    }

    JLOG("Image     : %d x %d  (%d ch, %.2f MB)\n",
         stats.w, stats.h, stats.c, (double)(stats.w * stats.h * stats.c) / (1024.0 * 1024.0));

    int blocks_x = stats.w / block_size;
    int blocks_y = stats.h / block_size;
    int total = blocks_x * blocks_y;
    JLOG("Blocks    : %d x %d = %d  (block=%dpx)\n",
         blocks_x, blocks_y, total, block_size);

    /* ── ตรวจ format เพื่อแสดง label ที่ถูกต้อง ── */
    {
        const char *ext = get_extension(stats.final_output_path);
        char fmt_label[32];
        if (str_iequal(ext, "jpg") || str_iequal(ext, "jpeg"))
            sprintf_s(fmt_label, sizeof(fmt_label), "JPG (quality %d)", JPEG_QUALITY);
        else if (str_iequal(ext, "png"))
            sprintf_s(fmt_label, sizeof(fmt_label), "PNG (level %d)", png_level);
        else
            sprintf_s(fmt_label, sizeof(fmt_label), "%s", ext);

        char dec_label[32];
        if (stats.used_fpng_decoder) {
            sprintf_s(dec_label, sizeof(dec_label), "fpng SIMD");
        } else {
            sprintf_s(dec_label, sizeof(dec_label), "stb_image");
        }

        JLOG("\n--- Performance Breakdown ---\n");
        JLOG("1. Load & Decode  (Disk->RAM)  : %8.2f ms  [%s]\n", stats.load_ms, dec_label);
        JLOG("2. Shuffle        (RAM->RAM)   : %8.2f ms  [OpenMP %d threads + prefetch]\n",
             stats.shuffle_ms, omp_get_max_threads());
        JLOG("3. Encode & Save  (RAM->Disk)  : %8.2f ms  [%s]\n", stats.save_ms, fmt_label);
        JLOG("----------------------------------------------\n");
        JLOG("Total                          : %8.2f ms\n\n", stats.total_ms);
        JLOG("Output    : %s\n", stats.final_output_path);
    }

    return 0;
}

/* ────────────────────────────────────────────────────────────
   process_directory (Parallel Batch Mode)
   ──────────────────────────────────────────────────────────── */
int process_directory(const char *input_dir, const char *output_dir,
                      unsigned int seed, int block_size, int decrypt, int png_level)
{
    FileInfo *files = NULL;
    int file_count = scan_directory(input_dir, &files);

    if (file_count <= 0) {
        JLOG_ERR("Error: No valid image files (.png, .jpg, .jpeg, .bmp) found in '%s'\n", input_dir);
        free(files);
        return 1;
    }

    if (!create_directory_if_not_exists(output_dir)) {
        JLOG_ERR("Error: Cannot create output directory '%s'\n", output_dir);
        free(files);
        return 1;
    }

    JLOG("Batch Mode: Processing %d images in '%s'...\n", file_count, input_dir);
    JLOG("Threads   : %d concurrent threads (File-level Parallelism)\n\n", omp_get_max_threads());

    ImageStats *stats_array = (ImageStats *)calloc(file_count, sizeof(ImageStats));
    if (!stats_array) {
        JLOG_ERR("Error: out of memory for stats_array\n");
        free(files);
        return 1;
    }

    /* Set stb PNG compression level once before parallel block */
    extern int stbi_write_png_compression_level;
    stbi_write_png_compression_level = png_level;

    clock_t t_batch_start = clock();
    int i;
    #pragma omp parallel for schedule(dynamic)
    for (i = 0; i < file_count; i++) {
        char in_path[MAX_PATH];
        char out_path[MAX_PATH];

        sprintf_s(in_path, sizeof(in_path), "%s\\%s", input_dir, files[i].name);
        sprintf_s(out_path, sizeof(out_path), "%s\\%s", output_dir, files[i].name);

        int res = process_image_internal(in_path, out_path, seed, block_size, decrypt, png_level, &stats_array[i]);

        #pragma omp critical (print_log)
        {
            if (res == 0) {
                JLOG("[OK] %-30s | %4dx%-4d (%dch) | %7.2f ms | %s\n",
                     files[i].name, stats_array[i].w, stats_array[i].h, stats_array[i].c,
                     stats_array[i].total_ms, stats_array[i].used_fpng_decoder ? "fpng" : "stb");
            } else {
                JLOG("[FAIL] %-28s | Failed\n", files[i].name);
            }
        }
    }

    double batch_total_ms = (double)(clock() - t_batch_start) * 1000.0 / CLOCKS_PER_SEC;

    JLOG("\n-------------------------------------------------------------\n");
    JLOG("Batch Completed: %d files processed in %.2f ms\n", file_count, batch_total_ms);
    JLOG("Average Time   : %.2f ms per image\n", batch_total_ms / file_count);
    JLOG("-------------------------------------------------------------\n");

    free(files);
    free(stats_array);
    return 0;
}

/* ────────────────────────────────────────────────────────────
   print_usage
   ──────────────────────────────────────────────────────────── */
static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s encrypt <input> <output> <seed> [block_size] [png_level]\n", prog);
    printf("  %s decrypt <input> <output> <seed> [block_size] [png_level]\n\n", prog);
    printf("Arguments:\n");
    printf("  mode       : encrypt | decrypt\n");
    printf("  input      : image file (jpg, png, bmp) or directory for batch mode\n");
    printf("  output     : output file or directory\n");
    printf("  seed       : integer secret key (e.g. 555)\n");
    printf("  block_size : block size in pixels (default: %d)\n", DEFAULT_BLOCK_SIZE);
    printf("  png_level  : PNG compression 0-9 (default: 1=fast, 9=smallest)\n\n");
    printf("Examples:\n");
    printf("  %s encrypt  photo.jpg    encrypted.jpg  555\n", prog);
    printf("  %s decrypt  encrypted.jpg restored.jpg  555\n", prog);
    printf("  %s encrypt  photo.png    locked.png     555  16  1\n", prog);
    printf("  %s encrypt  input_dir    output_dir     555\n", prog);
}

#ifndef JIGSAW_GUI_BUILD
/* ────────────────────────────────────────────────────────────
   main
   ──────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    const char  *mode, *input_path, *output_path;
    unsigned int seed;
    int          block_size, decrypt;

    /* init fpng: detect SSE4.1 capability — ต้องเรียกก่อนใช้งาน */
    fpng_init_c();

    printf("============================================\n");
    printf("  Jigsaw Image Encrypter (Batch Mode)\n");
    printf("  OpenMP | CPU Prefetch | fpng SIMD PNG\n");
    printf("============================================\n\n");

    if (argc < 5) {
        print_usage(argv[0]);
        return 1;
    }

    mode        = argv[1];
    input_path  = argv[2];
    output_path = argv[3];
    /* Fix: ใช้ strtoul แทน atol เพื่อ detect invalid seed */
    {
        char *endptr;
        unsigned long raw = strtoul(argv[4], &endptr, 10);
        if (*endptr != '\0') {
            fprintf(stderr, "Error: invalid seed '%s' — must be a non-negative integer\n", argv[4]);
            return 1;
        }
        seed = (unsigned int)raw;
    }
    block_size  = (argc >= 6) ? atoi(argv[5]) : DEFAULT_BLOCK_SIZE;

    /* [OPT-1] PNG compression level: arg7 or default 1 (fast) */
    int png_level = (argc >= 7) ? atoi(argv[6]) : 1;
    if (png_level < 0) png_level = 0;
    if (png_level > 9) png_level = 9;

    if (block_size < 1) {
        fprintf(stderr, "Error: block_size must be >= 1\n");
        return 1;
    }

    if (strcmp(mode, "encrypt") == 0)
        decrypt = 0;
    else if (strcmp(mode, "decrypt") == 0)
        decrypt = 1;
    else {
        fprintf(stderr, "Error: mode must be 'encrypt' or 'decrypt'\n");
        return 1;
    }

    printf("Mode      : %s\n", decrypt ? "DECRYPT" : "ENCRYPT");
    printf("Input     : %s\n", input_path);
    printf("Output    : %s\n", output_path);
    printf("Seed      : %u\n", seed);
    printf("Block Size: %d x %d px\n", block_size, block_size);
    printf("PNG Level : %d  (0=fastest/big, 9=slowest/small)\n", png_level);

    printf("PNG Codec  : fpng/SSE4.1=%s  stb-fallback=(grayscale/non-fpng)\n",
           fpng_sse41_supported() ? "YES (fast path)" : "NO (scalar)");
#ifdef _OPENMP
    printf("OpenMP    : %d thread(s)\n", omp_get_max_threads());
#else
    printf("OpenMP    : off (single-thread)\n");
#endif
    printf("\n");

    if (is_directory(input_path)) {
        return process_directory(input_path, output_path, seed, block_size, decrypt, png_level);
    } else {
        return process_image(input_path, output_path, seed, block_size, decrypt, png_level);
    }
}
#endif
