@echo off
setlocal

:: ============================================================
::  build.bat - Build Jigsaw Image Encrypter with MSVC
::  Usage: Double-click or run in Command Prompt
:: ============================================================

set "VSBASE=D:\Microsoft Visual Studio\18\Community"
set "VCVARS=%VSBASE%\VC\Auxiliary\Build\vcvarsall.bat"

:: Check if vcvarsall.bat exists
if not exist "%VCVARS%" (
    echo [ERROR] Cannot find vcvarsall.bat at:
    echo         %VCVARS%
    echo         Please adjust the VSBASE path in this script.
    pause
    exit /b 1
)

:: Set MSVC environment (x64)
echo [INFO] Setting up MSVC environment (x64)...
call "%VCVARS%" x64 >nul 2>&1

:: Create bin directory if it doesn't exist
if not exist "bin" mkdir "bin"

:: Compiler flags
set "CFLAGS=/nologo /O2 /W3 /openmp /arch:AVX2 /Iinclude"
set "CXXFLAGS=/nologo /O2 /W3 /openmp /arch:AVX2 /Iinclude /std:c++17"
set "OUT_CLI=bin\jigsaw.exe"
set "OUT_GUI=bin\jigsaw_gui.exe"

echo [INFO] Compiling src\fpng.cpp (SIMD PNG encoder) ...
cl %CXXFLAGS% /c src\fpng.cpp /Fo:fpng.obj

echo [INFO] Compiling src\fpng_c.cpp (C wrapper) ...
cl %CXXFLAGS% /c src\fpng_c.cpp /Fo:fpng_c.obj

echo -----------------------------------------------------
echo [INFO] BUILDING CLI VERSION (%OUT_CLI%) ...
echo -----------------------------------------------------

echo [INFO] Compiling src\jigsaw.c ...
cl %CFLAGS% /c src\jigsaw.c /Fo:jigsaw.obj

echo [INFO] Linking %OUT_CLI% ...
link /nologo jigsaw.obj fpng.obj fpng_c.obj /out:%OUT_CLI%

if %ERRORLEVEL% neq 0 (
    echo [FAIL] CLI Build failed
    pause
    exit /b %ERRORLEVEL%
)

echo -----------------------------------------------------
echo [INFO] BUILDING GUI VERSION (%OUT_GUI%) ...
echo -----------------------------------------------------

echo [INFO] Compiling src\jigsaw.c (GUI dependency) ...
cl %CFLAGS% /DJIGSAW_GUI_BUILD /c src\jigsaw.c /Fo:jigsaw_gui_dep.obj

echo [INFO] Compiling src\jigsaw_gui.c (GUI entry) ...
cl %CFLAGS% /c src\jigsaw_gui.c /Fo:jigsaw_gui.obj

echo [INFO] Linking %OUT_GUI% (Embedding manifest) ...
link /nologo jigsaw_gui.obj jigsaw_gui_dep.obj fpng.obj fpng_c.obj /subsystem:windows /manifest:embed /manifestinput:src\jigsaw_gui.exe.manifest /out:%OUT_GUI%

if %ERRORLEVEL% neq 0 (
    echo [FAIL] GUI Build failed
    pause
    exit /b %ERRORLEVEL%
)

:: Clean up intermediate obj files
if exist "jigsaw.obj"         del /f /q "jigsaw.obj"
if exist "jigsaw_gui.obj"     del /f /q "jigsaw_gui.obj"
if exist "jigsaw_gui_dep.obj" del /f /q "jigsaw_gui_dep.obj"
if exist "fpng.obj"           del /f /q "fpng.obj"
if exist "fpng_c.obj"         del /f /q "fpng_c.obj"

echo.
echo [OK] Build completed successfully!
echo    - CLI version: %OUT_CLI%
echo    - GUI version: %OUT_GUI%
echo.
echo -- Usage ---------------------------------------------
echo   [CLI] : %OUT_CLI% encrypt input.jpg encrypted.jpg 555
echo   [GUI] : Run %OUT_GUI% for GUI Control Panel
echo -----------------------------------------------------

endlocal
