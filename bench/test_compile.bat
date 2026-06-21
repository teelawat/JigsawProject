@echo off
setlocal

set "VSBASE=D:\Microsoft Visual Studio\18\Community"
set "VCVARS=%VSBASE%\VC\Auxiliary\Build\vcvarsall.bat"

call "%VCVARS%" x64 >nul 2>&1

echo === Test: compile simple file ===
echo int main(void) { return 0; } > D:\Work5\JigsawProject\bench\test_simple.c
cl.exe /nologo D:\Work5\JigsawProject\bench\test_simple.c /Fe:D:\Work5\JigsawProject\bench\test_simple.exe
echo EXIT CODE: %ERRORLEVEL%
dir D:\Work5\JigsawProject\bench\test_simple.exe 2>&1

echo.
echo === Test: compile shuffle_bench.c with /showIncludes ===
cl.exe /nologo /O2 /openmp /arch:AVX2 /c D:\Work5\JigsawProject\bench\shuffle_bench.c /FoD:\Work5\JigsawProject\bench\shuffle_bench.obj
echo EXIT CODE: %ERRORLEVEL%
