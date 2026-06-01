#pragma once

#if defined(_WIN32) || defined(_MSC_VER)
  #include <windows.h>
#else
  #ifndef MAX_PATH
    #define MAX_PATH 260
  #endif
#endif
#include <stdio.h>

typedef struct {
    int w, h, c;
    int status;
    double total_ms;
    double load_ms;
    double shuffle_ms;
    double save_ms;
    int used_fpng_decoder;
    char final_output_path[MAX_PATH];
} ImageStats;

extern void fpng_init_c(void);
extern int  fpng_sse41_supported(void);
extern int  is_directory(const char *path);
extern int  process_image(const char *input_path, const char *output_path,
                          unsigned int seed, int block_size, int decrypt, int png_level);
extern int  process_directory(const char *input_dir, const char *output_dir,
                              unsigned int seed, int block_size, int decrypt, int png_level);

#ifdef JIGSAW_GUI_BUILD
  extern void gui_printf(const char *format, ...);
  extern void gui_fprintf(FILE *stream, const char *format, ...);
  #define JLOG(...)     gui_printf(__VA_ARGS__)
  #define JLOG_ERR(...) gui_fprintf(stderr, __VA_ARGS__)
#else
  #define JLOG(...)     printf(__VA_ARGS__)
  #define JLOG_ERR(...) fprintf(stderr, __VA_ARGS__)
#endif
