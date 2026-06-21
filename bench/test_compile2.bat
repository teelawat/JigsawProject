@echo off
setlocal

set "VSBASE=D:\Microsoft Visual Studio\18\Community"
set "VCVARS=%VSBASE%\VC\Auxiliary\Build\vcvarsall.bat"

call "%VCVARS%" x64 >nul 2>&1

echo int main(void) { return 0; } > D:\Work5\JigsawProject\bench\test_simple.c

echo === Compile test ===
cmd /c "cl.exe /nologo D:\Work5\JigsawProject\bench\test_simple.c /Fe:D:\Work5\JigsawProject\bench\test_simple.exe > D:\Work5\JigsawProject\bench\cl_output.txt 2>&1"
echo EXIT CODE: %ERRORLEVEL%

echo === CL output: ===
type D:\Work5\JigsawProject\bench\cl_output.txt

echo === File check ===
dir D:\Work5\JigsawProject\bench\test_simple.* 2>&1
