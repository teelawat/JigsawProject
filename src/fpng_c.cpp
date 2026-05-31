/*
 * fpng_c.cpp — C-compatible wrapper สำหรับ fpng (C++ library)
 *
 * Encoder: SSE4.1 SIMD → เร็วกว่า stb PNG encoder 10-12×
 * Decoder: SSE4.1 SIMD → เร็วกว่า stb PNG decoder 3-5×
 *          (รองรับเฉพาะ PNG ที่ encode ด้วย fpng เท่านั้น)
 *          ถ้าไม่ใช่ fpng-encoded → คืน not_fpng=1 ให้ caller ทำ fallback
 *
 * รองรับ RGB (3ch) และ RGBA (4ch) เท่านั้น
 */

#include "fpng.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

extern "C" {

/* ─── init: detect SSE4.1 ต้องเรียกครั้งเดียวตอน startup ─── */
void fpng_init_c(void)
{
    fpng::fpng_init();
}

/* ─── ตรวจว่า CPU รองรับ SSE4.1 ไหม ─── */
int fpng_sse41_supported(void)
{
    return fpng::fpng_cpu_supports_sse41() ? 1 : 0;
}

/* ─────────────────────────────────────────────────────────────
   fpng_encode_to_file_c
   Encode RGB/RGBA image ลงไฟล์ PNG ด้วย fpng SIMD encoder
   คืน 1 = OK, 0 = fail
   num_chans ต้องเป็น 3 หรือ 4
   ───────────────────────────────────────────────────────────── */
int fpng_encode_to_file_c(const char *path,
                           const void *data,
                           int w, int h, int num_chans)
{
    if (num_chans != 3 && num_chans != 4)
        return 0;
    return fpng::fpng_encode_image_to_file(
        path, data,
        (uint32_t)w, (uint32_t)h,
        (uint32_t)num_chans) ? 1 : 0;
}

/* ─────────────────────────────────────────────────────────────
   fpng_decode_from_file_c
   Decode PNG ที่ encode ด้วย fpng — ใช้ SSE4.1 SIMD เร็ว 3-5×
   
   คืน:
     - pointer ไปยัง pixel data (malloc, caller ต้อง free)
       พร้อม set *w, *h, *c
     - NULL + *not_fpng = 1  → ไม่ใช่ fpng-encoded PNG
                                caller ควร fallback ไป stb_image
     - NULL + *not_fpng = 0  → error จริง (ไฟล์เสีย ฯลฯ)
   ───────────────────────────────────────────────────────────── */
unsigned char *fpng_decode_from_file_c(const char *path,
                                        int *w, int *h, int *c,
                                        int *not_fpng)
{
    *not_fpng = 0;

    FILE* pFile = nullptr;
#ifdef _MSC_VER
    fopen_s(&pFile, path, "rb");
#else
    pFile = fopen(path, "rb");
#endif
    if (!pFile) return NULL;

    fseek(pFile, 0, SEEK_END);
    long long filesize = ftell(pFile);
    fseek(pFile, 0, SEEK_SET);

    if (filesize <= 0 || filesize > 0x70000000) {
        fclose(pFile);
        return NULL;
    }

    std::vector<uint8_t> file_buf((size_t)filesize);
    if (fread(file_buf.data(), 1, file_buf.size(), pFile) != file_buf.size()) {
        fclose(pFile);
        return NULL;
    }
    fclose(pFile);

    uint32_t width = 0, height = 0, ch = 0;
    int info_res = fpng::fpng_get_info(file_buf.data(), (uint32_t)file_buf.size(), width, height, ch);
    if (info_res == fpng::FPNG_DECODE_NOT_FPNG) {
        *not_fpng = 1;
        return NULL;
    }
    if (info_res != fpng::FPNG_DECODE_SUCCESS) {
        return NULL;
    }

    std::vector<uint8_t> out;
    int res = fpng::fpng_decode_memory(file_buf.data(), (uint32_t)file_buf.size(), out, width, height, ch, ch);
    if (res == fpng::FPNG_DECODE_NOT_FPNG) {
        *not_fpng = 1;
        return NULL;
    }
    if (res != fpng::FPNG_DECODE_SUCCESS || out.empty())
        return NULL;

    unsigned char *buf = (unsigned char *)malloc(out.size());
    if (!buf) return NULL;
    memcpy(buf, out.data(), out.size());

    *w = (int)width;
    *h = (int)height;
    *c = (int)ch;
    return buf;
}

} /* extern "C" */
