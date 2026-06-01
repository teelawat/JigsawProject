# Jigsaw Image Encrypter (CLI & GUI)

[![Language](https://img.shields.io/badge/Language-Pure%20C%2FC%2B%2B-blue.svg)](#)
[![Parallelism](https://img.shields.io/badge/Parallel-OpenMP-orange.svg)](#)
[![SIMD](https://img.shields.io/badge/SIMD-SSE4.1-red.svg)](#)
[![Platform](https://img.shields.io/badge/Platform-Windows-lightgrey.svg)](#)

A command-line (CLI) and graphical (GUI) utility written in **Pure C/C++** that encrypts and decrypts images using key-based block-shuffling (pixel pixel-block permutation).

โปรแกรมสำหรับเข้ารหัส/ถอดรหัสรูปภาพด้วยการสับบล็อกพิกเซลแบบสุ่มตามรหัสผ่าน (Seed) พัฒนาด้วยภาษา **C/C++** แท้

---

## 📁 Repository Structure

```
JigsawProject/
├── bin/                       # Compiled executables (git ignored)
│   ├── jigsaw.exe             # CLI application
│   └── jigsaw_gui.exe         # Win32 GUI Control Panel
├── include/                   # Third-party dependency headers
│   ├── stb_image.h            # Image loading (public domain)
│   ├── stb_image_write.h      # Image writing (fallback encoder)
│   └── fpng.h                 # High-speed SIMD PNG encoder header
├── src/                       # Main source code
│   ├── jigsaw.c               # Core CLI logic and shuffle algorithms
│   ├── jigsaw_gui.c           # Native Win32 API GUI panel
│   ├── jigsaw_gui.exe.manifest# Win32 Visual styles manifest
│   ├── fpng.cpp               # FPNG SIMD C++ encoder implementation
│   └── fpng_c.cpp             # C wrapper for FPNG
├── scripts/                   # Verification and test helpers
│   ├── make_test_image.ps1    # Generates a synthetic test JPG
│   ├── verify.ps1             # Pixel comparison script for JPEG lossy test
│   └── verify.py              # Lossless identity verification (Pillow)
├── test_images/               # Directory for source and generated images
│   ├── test.jpg               # Synthetic test JPG (committed)
│   └── test.png               # Synthetic test PNG (committed)
├── build.bat                  # MSVC automated compiler script
└── README.md                  # Comprehensive documentation
```

---

## 🔨 How to Build / วิธีคอมไพล์โปรแกรม

### Requirements:
- **Windows OS**
- **Visual Studio** (with **Desktop development with C++** workload installed).
- Make sure **MSVC** with **OpenMP** support is selected.

### Steps:
1. Open Command Prompt (`cmd.exe`).
2. Navigate to the project directory:
   ```cmd
   cd C:\path\to\JigsawProject
   ```
3. Run the automated build script:
   ```cmd
   build.bat
   ```
The script will detect your MSVC environment, compile the C/C++ sources, link them, embed the visual style manifest into the GUI version, and output both binaries into the `bin/` directory.

---

## 📖 CLI Usage / วิธีใช้งานผ่าน Command Line

```
bin\jigsaw.exe <mode> <input> <output> <seed> [block_size] [png_level]
```

### Parameters:
* **`mode`**: `encrypt` to scramble, or `decrypt` to restore.
* **`input`**: Path to the input image file (supports PNG, JPG, BMP) or a **directory** for batch mode.
* **`output`**: Path to save the output image or output directory (when in batch mode).
* **`seed`**: Integer secret key (e.g. `555`) to initialize the Fisher-Yates random generator.
* **`block_size`**: Size of square block in pixels (default: `16`). Smaller block size = more scrambled image but takes slightly more CPU processing time.
* **`png_level`**: PNG compression level `0-9` (default: `1` for fast path, `9` for smallest file size).

### Examples:
```bat
:: 1. Encrypt a JPG image
bin\jigsaw.exe encrypt test_images\test.jpg test_images\encrypted.jpg 98765

:: 2. Decrypt the image (must use the exact same seed!)
bin\jigsaw.exe decrypt test_images\encrypted.jpg test_images\restored.jpg 98765

:: 3. Scramble using smaller block sizes (8px blocks)
bin\jigsaw.exe encrypt test_images\test.png test_images\encrypted_fine.png 555 8

:: 4. Batch mode: encrypt all images inside a folder in parallel
bin\jigsaw.exe encrypt test_images\input_folder test_images\output_folder 12345
```

---

## 🖥️ GUI Control Panel / โหมดหน้าต่างกราฟิก

If you prefer a visual interface, you can run:
```bat
bin\jigsaw_gui.exe
```
This launches a native, lightweight Windows Win32 API application. It runs the encryption engine on separate background worker threads so that the interface remains completely non-blocking and responsive.

หากคุณต้องการใช้งานแบบหน้าต่างโปรแกรม ให้รัน `bin\jigsaw_gui.exe` เพื่อเปิดหน้าต่างควบคุมที่พัฒนาด้วยระบบ Win32 API ดั้งเดิม ซึ่งทำงานแบบ Multi-threading ไม่ทำให้ตัวโปรแกรมค้างขณะเข้ารหัสรูปภาพ

---

## 🔬 Testing & Verification / การทดสอบความถูกต้อง

We have provided validation scripts under the `scripts/` directory to ensure that the block-shuffling process is 100% mathematically correct and lossless.

### 1. Python Lossless Identity Verification (PNG)
Requires Python with Pillow library installed (`pip install pillow`).
This test encrypts and decrypts a PNG file and asserts that the decrypted file matches the original byte-for-byte (identity match).
```cmd
:: Scramble PNG
bin\jigsaw.exe encrypt test_images\test.png test_images\encrypted.png 555
:: Restore PNG
bin\jigsaw.exe decrypt test_images\encrypted.png test_images\restored.png 555
:: Run test (should return 0 pixel difference and PASS status)
python scripts\verify.py
```

### 2. PowerShell Lossy Comparison (JPEG)
JPEG compression is inherently lossy. This script performs pixel comparison and identifies if differences between original and restored images remain within expected limits of standard JPEG noise.
```powershell
powershell -ExecutionPolicy Bypass -File scripts\verify.ps1
```

---

## 📜 License
This project is open-source and free to modify under the MIT License. Dependencies (`stb_image`, `stb_image_write`, and `fpng`) belong to their respective authors under public domain/zlib licenses.
