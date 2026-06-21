@echo off
setlocal enabledelayedexpansion

set "VSBASE=D:\Microsoft Visual Studio\18\Community"
set "VCVARS=%VSBASE%\VC\Auxiliary\Build\vcvarsall.bat"

if not exist "%VCVARS%" (
    echo [ERROR] Cannot find vcvarsall.bat at %VCVARS%
    pause
    exit /b 1
)

echo [INFO] Setting up MSVC environment (x64)...
call "%VCVARS%" x64

cd /d D:\Work5\JigsawProject

echo [INFO] Current directory: %CD%
echo [INFO] Source file exists check:
if exist "bench\shuffle_bench.c" (
    echo   bench\shuffle_bench.c EXISTS
) else (
    echo   bench\shuffle_bench.c NOT FOUND
)

echo.
echo [INFO] Compiling...
cl.exe /nologo /O2 /openmp /arch:AVX2 "D:\Work5\JigsawProject\bench\shuffle_bench.c" /Fe:"D:\Work5\JigsawProject\bench\shuffle_bench.exe" /Fo:"D:\Work5\JigsawProject\bench\shuffle_bench.obj"
set COMPILE_ERR=%ERRORLEVEL%
echo COMPILE EXIT CODE: %COMPILE_ERR%

if %COMPILE_ERR% neq 0 (
    echo [FAIL] Compilation failed
    goto :eof
)

echo.
echo [OK] Compiled successfully!
echo.
echo [INFO] Running benchmark...
echo ================================================================
"D:\Work5\JigsawProject\bench\shuffle_bench.exe"

:eof
endlocal
