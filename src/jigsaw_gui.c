/*
 * ============================================================
 *  Jigsaw Image Encrypter — Win32 GUI Interface (v4.0)
 *  Pure C  |  Win32 API  |  Segoe UI  |  Non-blocking Threads
 * ============================================================
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

/* Enable Visual Styles (Modern Common Controls v6.0) */
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

/* ── Declarations of variables and structures from jigsaw.c ── */
typedef struct {
    int w, h, c;
    int status;
    double total_ms;
    double load_ms;
    double shuffle_ms;
    double save_ms;
    int used_fpng_decoder;
} ImageStats;

extern int g_png_compression;
extern void fpng_init_c(void);
extern int  fpng_sse41_supported(void);
extern int  is_directory(const char *path);
extern int  process_image(const char *input_path, const char *output_path,
                          unsigned int seed, int block_size, int decrypt);
extern int  process_directory(const char *input_dir, const char *output_dir,
                             unsigned int seed, int block_size, int decrypt);

/* ── Control IDs ── */
#define ID_RADIO_ENCRYPT  1001
#define ID_RADIO_DECRYPT  1002
#define ID_EDIT_INPUT     1003
#define ID_BTN_BROWSE_IN_F 1004
#define ID_BTN_BROWSE_IN_D 1005
#define ID_EDIT_OUTPUT    1006
#define ID_BTN_BROWSE_OUT 1007
#define ID_EDIT_SEED      1008
#define ID_EDIT_BLOCK     1009
#define ID_EDIT_PNG_LV    1010
#define ID_BTN_RUN        1011
#define ID_EDIT_LOG       1012
#define ID_TIMER_CHECK    1013

/* ── GUI Controls Handles ── */
HWND hRadioEncrypt, hRadioDecrypt;
HWND hEditInput, hEditOutput;
HWND hBtnBrowseInFile, hBtnBrowseInDir, hBtnBrowseOut;
HWND hEditSeed, hEditBlock, hEditPngLv;
HWND hBtnRun, hEditLog;

HFONT hSegoeFont = NULL;

/* ── Thread global variables ── */
HANDLE hWorkerThread = NULL;
BOOL bRunning = FALSE;

/* Struct to pass parameters to the worker thread */
typedef struct {
    char input_path[MAX_PATH];
    char output_path[MAX_PATH];
    unsigned int seed;
    int block_size;
    int decrypt;
    int png_level;
} WorkerParams;

WorkerParams threadParams;

/* Append text to Log Edit box */
void AppendLogText(const char *text)
{
    int len = GetWindowTextLength(hEditLog);
    SendMessage(hEditLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessage(hEditLog, EM_REPLACESEL, 0, (LPARAM)text);
    SendMessage(hEditLog, EM_SCROLL, SB_PAGEDOWN, 0);
}

/* Direct GUI Printf implementation */
void gui_printf(const char *format, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, format);
    vsprintf_s(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Replace \n with \r\n for Win32 Edit control
    char formatted[4096];
    const char *s = buffer;
    char *d = formatted;
    while (*s && (d - formatted < sizeof(formatted) - 2)) {
        if (*s == '\n') {
            *d++ = '\r';
        }
        *d++ = *s++;
    }
    *d = '\0';

    AppendLogText(formatted);
}

/* Direct GUI Fprintf implementation */
void gui_fprintf(FILE *stream, const char *format, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, format);
    vsprintf_s(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Replace \n with \r\n for Win32 Edit control
    char formatted[4096];
    const char *s = buffer;
    char *d = formatted;
    while (*s && (d - formatted < sizeof(formatted) - 2)) {
        if (*s == '\n') {
            *d++ = '\r';
        }
        *d++ = *s++;
    }
    *d = '\0';

    AppendLogText(formatted);
}

/* Worker Thread for image processing */
unsigned __stdcall ProcessingWorker(void *pParams)
{
    WorkerParams *params = (WorkerParams *)pParams;
    
    // Set PNG level
    extern int stbi_write_png_compression_level;
    g_png_compression = params->png_level;
    stbi_write_png_compression_level = params->png_level;

    gui_printf("============================================\n");
    gui_printf("  Jigsaw Engine: Processing started...\n");
    gui_printf("============================================\n");

    int res = 0;
    if (is_directory(params->input_path)) {
        res = process_directory(params->input_path, params->output_path, params->seed, params->block_size, params->decrypt);
    } else {
        res = process_image(params->input_path, params->output_path, params->seed, params->block_size, params->decrypt);
    }

    if (res == 0) {
        gui_printf("\n>>> [SUCCESS] Processing completed successfully! <<<\n");
    } else {
        gui_printf("\n>>> [ERROR] Processing failed! <<<\n");
    }

    bRunning = FALSE;
    return 0;
}

/* Browse File Dialog */
void BrowseFile(HWND hwndParent, HWND hwndEdit, BOOL bSave)
{
    OPENFILENAMEA ofn = {0};
    char szFile[MAX_PATH] = {0};
    
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwndParent;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Supported Images\0*.png;*.jpg;*.jpeg;*.bmp\0PNG Files (*.png)\0*.png\0JPEG Files (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0BMP Files (*.bmp)\0*.bmp\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (bSave) {
        ofn.Flags |= OFN_OVERWRITEPROMPT;
        if (GetSaveFileNameA(&ofn)) {
            SetWindowTextA(hwndEdit, ofn.lpstrFile);
        }
    } else {
        ofn.Flags |= OFN_FILEMUSTEXIST;
        if (GetOpenFileNameA(&ofn)) {
            SetWindowTextA(hwndEdit, ofn.lpstrFile);
        }
    }
}

/* Browse Folder Dialog */
void BrowseFolder(HWND hwndParent, HWND hwndEdit)
{
    BROWSEINFOA bi = {0};
    bi.hwndOwner = hwndParent;
    bi.lpszTitle = "Select Folder for Batch Mode";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl != NULL) {
        char path[MAX_PATH];
        if (SHGetPathFromIDListA(pidl, path)) {
            SetWindowTextA(hwndEdit, path);
        }
        CoTaskMemFree(pidl);
    }
}

/* Set standard font recursively for all child controls */
BOOL CALLBACK SetFontProc(HWND hwnd, LPARAM lParam)
{
    SendMessage(hwnd, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}

/* Main Window Procedure */
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            // Create custom clean font
            hSegoeFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, 
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, 
                                     DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

            // Group: Mode Selection
            HWND hGroupMode = CreateWindowExA(0, "BUTTON", "Mode Selection", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                              15, 10, 595, 55, hwnd, NULL, NULL, NULL);

            hRadioEncrypt = CreateWindowExA(0, "BUTTON", "Encrypt", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                                            30, 30, 150, 20, hwnd, (HMENU)ID_RADIO_ENCRYPT, NULL, NULL);
            hRadioDecrypt = CreateWindowExA(0, "BUTTON", "Decrypt", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                            200, 30, 150, 20, hwnd, (HMENU)ID_RADIO_DECRYPT, NULL, NULL);
            SendMessage(hRadioEncrypt, BM_SETCHECK, BST_CHECKED, 0);

            // Group: File/Folder Paths
            HWND hGroupPaths = CreateWindowExA(0, "BUTTON", "Files and Paths", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                               15, 75, 595, 120, hwnd, NULL, NULL, NULL);

            CreateWindowExA(0, "STATIC", "Input Path:", WS_CHILD | WS_VISIBLE,
                            30, 100, 90, 20, hwnd, NULL, NULL, NULL);
            hEditInput = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                         125, 98, 280, 22, hwnd, (HMENU)ID_EDIT_INPUT, NULL, NULL);
            hBtnBrowseInFile = CreateWindowExA(0, "BUTTON", "Browse File...", WS_CHILD | WS_VISIBLE,
                                               415, 97, 85, 24, hwnd, (HMENU)ID_BTN_BROWSE_IN_F, NULL, NULL);
            hBtnBrowseInDir = CreateWindowExA(0, "BUTTON", "Browse Dir...", WS_CHILD | WS_VISIBLE,
                                              510, 97, 85, 24, hwnd, (HMENU)ID_BTN_BROWSE_IN_D, NULL, NULL);

            CreateWindowExA(0, "STATIC", "Output Path:", WS_CHILD | WS_VISIBLE,
                            30, 145, 90, 20, hwnd, NULL, NULL, NULL);
            hEditOutput = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                          125, 143, 280, 22, hwnd, (HMENU)ID_EDIT_OUTPUT, NULL, NULL);
            hBtnBrowseOut = CreateWindowExA(0, "BUTTON", "Browse...", WS_CHILD | WS_VISIBLE,
                                            415, 142, 85, 24, hwnd, (HMENU)ID_BTN_BROWSE_OUT, NULL, NULL);

            // Group: Custom Options
            HWND hGroupOpts = CreateWindowExA(0, "BUTTON", "Options", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                              15, 205, 595, 65, hwnd, NULL, NULL, NULL);

            CreateWindowExA(0, "STATIC", "Secret Seed:", WS_CHILD | WS_VISIBLE,
                            30, 233, 80, 20, hwnd, NULL, NULL, NULL);
            hEditSeed = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "555", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                        110, 230, 60, 22, hwnd, (HMENU)ID_EDIT_SEED, NULL, NULL);

            CreateWindowExA(0, "STATIC", "Block Size:", WS_CHILD | WS_VISIBLE,
                            200, 233, 80, 20, hwnd, NULL, NULL, NULL);
            hEditBlock = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "16", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                         280, 230, 60, 22, hwnd, (HMENU)ID_EDIT_BLOCK, NULL, NULL);

            CreateWindowExA(0, "STATIC", "PNG Compression Level (0-9):", WS_CHILD | WS_VISIBLE,
                            370, 233, 175, 20, hwnd, NULL, NULL, NULL);
            hEditPngLv = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "1", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                         545, 230, 45, 22, hwnd, (HMENU)ID_EDIT_PNG_LV, NULL, NULL);

            // Large Run Button
            hBtnRun = CreateWindowExA(0, "BUTTON", "Run Process", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                      15, 285, 595, 38, hwnd, (HMENU)ID_BTN_RUN, NULL, NULL);

            // Group: Output / Log
            HWND hGroupLogs = CreateWindowExA(0, "BUTTON", "Logs / Output", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                              15, 335, 595, 185, hwnd, NULL, NULL, NULL);
            hEditLog = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                                       25, 355, 575, 155, hwnd, (HMENU)ID_EDIT_LOG, NULL, NULL);

            // Apply fonts
            EnumChildWindows(hwnd, SetFontProc, (LPARAM)hSegoeFont);
            SendMessage(hGroupMode, WM_SETFONT, (WPARAM)hSegoeFont, TRUE);
            SendMessage(hGroupPaths, WM_SETFONT, (WPARAM)hSegoeFont, TRUE);
            SendMessage(hGroupOpts, WM_SETFONT, (WPARAM)hSegoeFont, TRUE);
            SendMessage(hGroupLogs, WM_SETFONT, (WPARAM)hSegoeFont, TRUE);

            // Print initial details to output log
            gui_printf("============================================================\n");
            gui_printf("  Jigsaw Image Encrypter GUI v4.0\n");
            gui_printf("  SSE4.1 fpng SIMD + OpenMP Multi-threading Ready\n");
            gui_printf("============================================================\n\n");
            gui_printf("PNG Codec: fpng/SSE4.1=%s\n", fpng_sse41_supported() ? "YES" : "NO");

            // Set thread termination checking timer
            SetTimer(hwnd, ID_TIMER_CHECK, 50, NULL);
            break;
        }

        case WM_TIMER:
        {
            if (wParam == ID_TIMER_CHECK) {
                if (!bRunning && hWorkerThread != NULL) {
                    CloseHandle(hWorkerThread);
                    hWorkerThread = NULL;
                    EnableWindow(hBtnRun, TRUE);
                    EnableWindow(hRadioEncrypt, TRUE);
                    EnableWindow(hRadioDecrypt, TRUE);
                    EnableWindow(hBtnBrowseInFile, TRUE);
                    EnableWindow(hBtnBrowseInDir, TRUE);
                    EnableWindow(hBtnBrowseOut, TRUE);
                    EnableWindow(hEditInput, TRUE);
                    EnableWindow(hEditOutput, TRUE);
                    EnableWindow(hEditSeed, TRUE);
                    EnableWindow(hEditBlock, TRUE);
                    EnableWindow(hEditPngLv, TRUE);
                }
            }
            break;
        }

        case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            switch (wmId)
            {
                case ID_BTN_BROWSE_IN_F:
                    BrowseFile(hwnd, hEditInput, FALSE);
                    break;

                case ID_BTN_BROWSE_IN_D:
                    BrowseFolder(hwnd, hEditInput);
                    break;

                case ID_BTN_BROWSE_OUT:
                {
                    char in_path[MAX_PATH];
                    GetWindowTextA(hEditInput, in_path, MAX_PATH);
                    if (is_directory(in_path)) {
                        BrowseFolder(hwnd, hEditOutput);
                    } else {
                        BrowseFile(hwnd, hEditOutput, TRUE);
                    }
                    break;
                }

                case ID_BTN_RUN:
                {
                    if (bRunning) break;

                    char in_path[MAX_PATH];
                    char out_path[MAX_PATH];
                    char seed_str[64];
                    char block_str[64];
                    char png_str[64];

                    GetWindowTextA(hEditInput, in_path, MAX_PATH);
                    GetWindowTextA(hEditOutput, out_path, MAX_PATH);
                    GetWindowTextA(hEditSeed, seed_str, 64);
                    GetWindowTextA(hEditBlock, block_str, 64);
                    GetWindowTextA(hEditPngLv, png_str, 64);

                    if (strlen(in_path) == 0 || strlen(out_path) == 0) {
                        MessageBoxA(hwnd, "Please fill in both Input and Output paths.", "Missing Information", MB_ICONWARNING | MB_OK);
                        break;
                    }

                    // Parse parameters
                    threadParams.seed = (unsigned int)atol(seed_str);
                    threadParams.block_size = atoi(block_str);
                    threadParams.png_level = atoi(png_str);
                    threadParams.decrypt = (SendMessage(hRadioDecrypt, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    strcpy_s(threadParams.input_path, MAX_PATH, in_path);
                    strcpy_s(threadParams.output_path, MAX_PATH, out_path);

                    if (threadParams.block_size < 1) {
                        MessageBoxA(hwnd, "Block Size must be >= 1.", "Invalid Parameter", MB_ICONWARNING | MB_OK);
                        break;
                    }

                    // Clear previous logs on GUI
                    SetWindowTextA(hEditLog, "");

                    // Disable controls to prevent duplicate run
                    EnableWindow(hBtnRun, FALSE);
                    EnableWindow(hRadioEncrypt, FALSE);
                    EnableWindow(hRadioDecrypt, FALSE);
                    EnableWindow(hBtnBrowseInFile, FALSE);
                    EnableWindow(hBtnBrowseInDir, FALSE);
                    EnableWindow(hBtnBrowseOut, FALSE);
                    EnableWindow(hEditInput, FALSE);
                    EnableWindow(hEditOutput, FALSE);
                    EnableWindow(hEditSeed, FALSE);
                    EnableWindow(hEditBlock, FALSE);
                    EnableWindow(hEditPngLv, FALSE);

                    bRunning = TRUE;
                    hWorkerThread = (HANDLE)_beginthreadex(NULL, 0, ProcessingWorker, &threadParams, 0, NULL);
                    break;
                }
            }
            break;
        }

        case WM_DESTROY:
            KillTimer(hwnd, ID_TIMER_CHECK);
            if (hSegoeFont) DeleteObject(hSegoeFont);
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

/* GUI Entry Point */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    /* Initialize Common Controls for Modern visual styling */
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    /* Initialize COM for Folder Browser */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    /* Detect fpng */
    fpng_init_c();

    /* Register Window Class */
    const char szClassName[] = "JigsawGuiClass";
    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(WNDCLASSEXA);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = szClassName;

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Window Registration Failed!", "Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    /* Calculate window rectangle to fit client size of 625 x 535 */
    RECT rect = {0, 0, 625, 535};
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);

    /* Create Main Window */
    HWND hwnd = CreateWindowExA(
        0,
        szClassName,
        "Jigsaw Image Encrypter - GUI Control Panel v4.0",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) {
        MessageBoxA(NULL, "Window Creation Failed!", "Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    /* Message Loop */
    MSG Msg;
    while (GetMessage(&Msg, NULL, 0, 0) > 0) {
        TranslateMessage(&Msg);
        DispatchMessage(&Msg);
    }

    CoUninitialize();
    return (int)Msg.wParam;
}
