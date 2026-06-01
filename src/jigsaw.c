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

#ifdef JIGSAW_GUI_BUILD
  #define printf(...) gui_printf(__VA_ARGS__)
  #define fprintf(stream, ...) gui_fprintf(stream, __VA_ARGS__)
  extern void gui_printf(const char *format, ...);
  extern void gui_fprintf(FILE *stream, const char *format, ...);
#endif

/*
 * fpng C wrapper — ประกาศ extern เพื่อให้ jigsaw.c เรียก fpng_c.cpp ได้
 * fpng ใช้ SSE4.1 SIMD → เร็วกว่า stb PNG encoder 2-5×
 * รองรับเฉพาะ 3ch (RGB) และ 4ch (RGBA)
 */
extern void fpng_init_c(void);
extern int  fpng_sse41_supported(void);
extern int  fpng_encode_to_file_c(const char *path, const void *data,
                                   int w, int h, int num_chans);
extern unsigned char *fpng_decode_from_file_c(const char *path,
                                               int *w, int *h, int *c,
                                               int *not_fpng);

/* MSVC / GCC prefetch intrinsic */
#if defined(_MSC_VER)
  #include <intrin.h>
  /* _mm_prefetch(addr, hint): hint = _MM_HINT_T0 (L1 cache) */
  #define PREFETCH(addr)  _mm_prefetch((const char*)(addr), _MM_HINT_T0)
#elif defined(__GNUC__)
  #define PREFETCH(addr)  __builtin_prefetch((addr), 0, 1)
#else
  #define PREFETCH(addr)  ((void)0)
#endif

/* ── ค่าคงที่ ───────────────────────────────────── */
#define DEFAULT_BLOCK_SIZE   16
#define JPEG_QUALITY         95
#define PREFETCH_AHEAD        4    /* prefetch 4 blocks ahead */

/* [OPT-1] สำหรับเก็บ compression level ที่ user เลือก
 * จะถูก assign ให้ stbi_write_png_compression_level ใน main() */
int g_png_compression = 1;

/* ────────────────────────────────────────────────────────────
   generate_permutation
   สร้างอาร์เรย์ perm[0..n-1] ที่สับแบบ Fisher-Yates
   ──────────────────────────────────────────────────────────── */
static void generate_permutation(int *perm, int n, unsigned int seed)
{
    int i, j, tmp;
    for (i = 0; i < n; i++) perm[i] = i;
    srand(seed);
    for (i = n - 1; i > 0; i--) {
        j       = (int)(rand() % (i + 1));
        tmp     = perm[i];
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

    for (row = 0; row < block_size; row++) {
        memcpy(d, s, row_bytes);
        s += row_stride;
        d += row_stride;
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
        g->data = (unsigned char *)realloc(g->data, newcap);
        g->capacity = newcap;
    }
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
    fp = fopen(path, "wb");
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
    int w, h, c;
    int status;
    double total_ms;
    double load_ms;
    double shuffle_ms;
    double save_ms;
    int used_fpng_decoder;
} ImageStats;

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
    int count = 0;

    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const char *file_name = find_data.cFileName;
            const char *ext = get_extension(file_name);
            if (str_iequal(ext, "png") || str_iequal(ext, "jpg") || 
                str_iequal(ext, "jpeg") || str_iequal(ext, "bmp")) {
                if (count >= capacity) {
                    capacity *= 2;
                    files = (FileInfo *)realloc(files, capacity * sizeof(FileInfo));
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

/* ────────────────────────────────────────────────────────────
   process_image_internal
   ──────────────────────────────────────────────────────────── */
int process_image_internal(const char *input_path, const char *output_path,
                                  unsigned int seed, int block_size, int decrypt,
                                  ImageStats *stats)
{
    int w, h, c;
    unsigned char *src, *dst;
    int           *perm;
    size_t         img_bytes;
    int            blocks_x, blocks_y, total;
    int            i;
    clock_t        t_total_start, t_load_start, t_load_end;
    clock_t        t_shuffle_start, t_shuffle_end;
    clock_t        t_save_start, t_save_end;
    int            used_fpng_decoder = 0;
    char           final_output_path[MAX_PATH];

    memset(stats, 0, sizeof(ImageStats));
    t_total_start = clock();

    /* Resolve output path: if output_path is a directory, append input filename */
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

    /* ── โหลดภาพ ─────────────────────────────────── */
    t_load_start = clock();
    {
        const char *in_ext = get_extension(input_path);
        if (str_iequal(in_ext, "png")) {
            int not_fpng = 0;
            src = fpng_decode_from_file_c(input_path, &w, &h, &c, &not_fpng);
            if (src) {
                used_fpng_decoder = 1;
            } else {
                if (not_fpng) {
                    src = stbi_load(input_path, &w, &h, &c, 0);
                } else {
                    src = NULL;
                }
            }
        } else {
            src = stbi_load(input_path, &w, &h, &c, 0);
        }
    }
    t_load_end = clock();

    if (!src) {
        stats->status = 1;
        return 1;
    }

    stats->w = w;
    stats->h = h;
    stats->c = c;
    stats->used_fpng_decoder = used_fpng_decoder;

    blocks_x = w / block_size;
    blocks_y = h / block_size;
    total    = blocks_x * blocks_y;

    if (total < 2) {
        t_save_start = clock();
        int ok = save_image(final_output_path, src, w, h, c);
        t_save_end = clock();
        free_src_image(src, used_fpng_decoder);
        stats->load_ms = (double)(t_load_end - t_load_start) * 1000.0 / CLOCKS_PER_SEC;
        stats->save_ms = (double)(t_save_end - t_save_start) * 1000.0 / CLOCKS_PER_SEC;
        stats->total_ms = (double)(clock() - t_total_start) * 1000.0 / CLOCKS_PER_SEC;
        stats->status = ok ? 0 : 1;
        return ok ? 0 : 1;
    }

    /* ── permutation ─────────────────────────────── */
    perm = (int *)malloc((size_t)total * sizeof(int));
    if (!perm) { 
        free_src_image(src, used_fpng_decoder); 
        stats->status = 1;
        return 1; 
    }
    generate_permutation(perm, total, seed);

    /* ── output buffer ───────────────────────────── */
    img_bytes = (size_t)w * h * c;
    dst = (unsigned char *)malloc(img_bytes);
    if (!dst) { 
        free(perm); 
        free_src_image(src, used_fpng_decoder); 
        stats->status = 1;
        return 1; 
    }

    copy_edges_only(dst, src, w, h, c, blocks_x, blocks_y, block_size);

    t_shuffle_start = clock();
    #pragma omp parallel for schedule(guided)
    for (i = 0; i < total; i++) {
        int src_idx = decrypt ? perm[i] : i;
        int dst_idx = decrypt ? i       : perm[i];

        if (i + PREFETCH_AHEAD < total) {
            int pre_src = decrypt ? perm[i + PREFETCH_AHEAD]
                                  : (i + PREFETCH_AHEAD);
            PREFETCH(block_src_ptr(src, pre_src, w, c, blocks_x, block_size));
        }

        copy_block(dst, dst_idx, src, src_idx, w, c, blocks_x, block_size);
    }
    t_shuffle_end = clock();

    free(perm);
    free_src_image(src, used_fpng_decoder);

    /* ── Save ────────────────────────────────────── */
    t_save_start = clock();
    int save_ok = save_image(final_output_path, dst, w, h, c);
    t_save_end = clock();

    free(dst);

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
                 unsigned int seed, int block_size, int decrypt)
{
    ImageStats stats;
    int res = process_image_internal(input_path, output_path, seed, block_size, decrypt, &stats);
    if (res != 0) {
        return res;
    }

    printf("Image     : %d x %d  (%d ch, %.2f MB)\n",
           stats.w, stats.h, stats.c, (double)(stats.w * stats.h * stats.c) / (1024.0 * 1024.0));

    int blocks_x = stats.w / block_size;
    int blocks_y = stats.h / block_size;
    int total = blocks_x * blocks_y;
    printf("Blocks    : %d x %d = %d  (block=%dpx)\n",
           blocks_x, blocks_y, total, block_size);

    /* ── ตรวจ format เพื่อแสดง label ที่ถูกต้อง ── */
    {
        const char *ext = get_extension(output_path);
        char fmt_label[32];
        if (str_iequal(ext, "jpg") || str_iequal(ext, "jpeg"))
            sprintf_s(fmt_label, sizeof(fmt_label), "JPG (quality %d)", JPEG_QUALITY);
        else if (str_iequal(ext, "png"))
            sprintf_s(fmt_label, sizeof(fmt_label), "PNG (level %d)", g_png_compression);
        else
            sprintf_s(fmt_label, sizeof(fmt_label), "%s", ext);

        char dec_label[32];
        if (stats.used_fpng_decoder) {
            sprintf_s(dec_label, sizeof(dec_label), "fpng SIMD");
        } else {
            sprintf_s(dec_label, sizeof(dec_label), "stb_image");
        }

        printf("\n--- Performance Breakdown ---\n");
        printf("1. Load & Decode  (Disk->RAM)  : %8.2f ms  [%s]\n", stats.load_ms, dec_label);
        printf("2. Shuffle        (RAM->RAM)   : %8.2f ms  [OpenMP %d threads + prefetch]\n",
               stats.shuffle_ms, omp_get_max_threads());
        printf("3. Encode & Save  (RAM->Disk)  : %8.2f ms  [%s]\n", stats.save_ms, fmt_label);
        printf("----------------------------------------------\n");
        printf("Total                          : %8.2f ms\n\n", stats.total_ms);
        printf("Output    : %s\n", output_path);
    }

    return 0;
}

/* ────────────────────────────────────────────────────────────
   process_directory (Parallel Batch Mode)
   ──────────────────────────────────────────────────────────── */
int process_directory(const char *input_dir, const char *output_dir,
                             unsigned int seed, int block_size, int decrypt)
{
    FileInfo *files = NULL;
    int file_count = scan_directory(input_dir, &files);
    int i;

    if (file_count <= 0) {
        fprintf(stderr, "Error: No valid image files (.png, .jpg, .jpeg, .bmp) found in '%s'\n", input_dir);
        free(files);
        return 1;
    }

    if (!create_directory_if_not_exists(output_dir)) {
        fprintf(stderr, "Error: Cannot create output directory '%s'\n", output_dir);
        free(files);
        return 1;
    }

    printf("Batch Mode: Processing %d images in '%s'...\n", file_count, input_dir);
    printf("Threads   : %d concurrent threads (File-level Parallelism)\n\n", omp_get_max_threads());

    ImageStats *stats_array = (ImageStats *)malloc(file_count * sizeof(ImageStats));
    memset(stats_array, 0, file_count * sizeof(ImageStats));

    clock_t t_batch_start = clock();

    #pragma omp parallel for schedule(dynamic)
    for (i = 0; i < file_count; i++) {
        char in_path[MAX_PATH];
        char out_path[MAX_PATH];

        sprintf_s(in_path, sizeof(in_path), "%s\\%s", input_dir, files[i].name);
        sprintf_s(out_path, sizeof(out_path), "%s\\%s", output_dir, files[i].name);

        int res = process_image_internal(in_path, out_path, seed, block_size, decrypt, &stats_array[i]);

        if (res == 0) {
            printf("[OK] %-30s | %4dx%-4d (%dch) | %7.2f ms | %s\n",
                   files[i].name, stats_array[i].w, stats_array[i].h, stats_array[i].c,
                   stats_array[i].total_ms, stats_array[i].used_fpng_decoder ? "fpng" : "stb");
        } else {
            printf("[FAIL] %-28s | Failed\n", files[i].name);
        }
    }

    double batch_total_ms = (double)(clock() - t_batch_start) * 1000.0 / CLOCKS_PER_SEC;

    printf("\n-------------------------------------------------------------\n");
    printf("Batch Completed: %d files processed in %.2f ms\n", file_count, batch_total_ms);
    printf("Average Time   : %.2f ms per image\n", batch_total_ms / file_count);
    printf("-------------------------------------------------------------\n");

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
    seed        = (unsigned int)atol(argv[4]);
    block_size  = (argc >= 6) ? atoi(argv[5]) : DEFAULT_BLOCK_SIZE;

    /* [OPT-1] PNG compression level: arg7 or default 1 (fast) */
    g_png_compression = (argc >= 7) ? atoi(argv[6]) : 1;
    if (g_png_compression < 0) g_png_compression = 0;
    if (g_png_compression > 9) g_png_compression = 9;

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

    /* [OPT-1] Set stb PNG compression level at runtime */
    stbi_write_png_compression_level = g_png_compression;

    printf("Mode      : %s\n", decrypt ? "DECRYPT" : "ENCRYPT");
    printf("Input     : %s\n", input_path);
    printf("Output    : %s\n", output_path);
    printf("Seed      : %u\n", seed);
    printf("Block Size: %d x %d px\n", block_size, block_size);
    printf("PNG Level : %d  (0=fastest/big, 9=slowest/small)\n", g_png_compression);

    printf("PNG Codec  : fpng/SSE4.1=%s  stb-fallback=(grayscale/non-fpng)\n",
           fpng_sse41_supported() ? "YES (fast path)" : "NO (scalar)");
#ifdef _OPENMP
    printf("OpenMP    : %d thread(s)\n", omp_get_max_threads());
#else
    printf("OpenMP    : off (single-thread)\n");
#endif
    printf("\n");

    if (is_directory(input_path)) {
        return process_directory(input_path, output_path, seed, block_size, decrypt);
    } else {
        return process_image(input_path, output_path, seed, block_size, decrypt);
    }
}
#endif
