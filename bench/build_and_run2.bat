@echo off
setlocal enabledelayedexpansion

set "VSBASE=D:\Microsoft Visual Studio\18\Community"
set "VCVARS=%VSBASE%\VC\Auxiliary\Build\vcvarsall.bat"

call "%VCVARS%" x64 >nul 2>&1

cd /d D:\Work5\JigsawProject

echo === Step 1: Compile only ===
cl.exe /nologo /O2 /openmp /arch:AVX2 /c "D:\Work5\JigsawProject\bench\shuffle_bench.c" /Fo"D:\Work5\JigsawProject\bench\shuffle_bench.obj"
echo COMPILE EXIT CODE: %ERRORLEVEL%

echo.
echo === Step 2: Link ===
link.exe /nologo "D:\Work5\JigsawProject\bench\shuffle_bench.obj" /out:"D:\Work5\JigsawProject\bench\shuffle_bench.exe"
echo LINK EXIT CODE: %ERRORLEVEL%

echo.
echo === Step 3: Run ===
"D:\Work5\JigsawProject\bench\shuffle_bench.exe"
