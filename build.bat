@echo off
setlocal enabledelayedexpansion

set PROGNAME=adviso.exe
set OBJDIR=obj
set SRCDIR=server
set LIBDIR=libs

set CFLAGS=/nologo /arch:AVX2 /W4 /O2 /openmp:llvm /MD /utf-8 /openmp

set INCLUDES=/I%SRCDIR% /I%LIBDIR%\cfd_lib /I%LIBDIR%\mongoose

set LIBS=ws2_32.lib advapi32.lib crypt32.lib gdi32.lib user32.lib shell32.lib

if not exist %OBJDIR% mkdir %OBJDIR%

echo --- Building %PROGNAME% for Windows (MSVC) ---

cl %CFLAGS% %INCLUDES% ^
    %SRCDIR%\main.c ^
    %LIBDIR%\mongoose\mongoose.c ^
    /Fe:%PROGNAME% ^
    /Fo:%OBJDIR%\ ^
    /link %LIBS%

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [!] Error while compiling %PROGNAME%!
    exit /b %ERRORLEVEL%
)

echo.
echo --- Done: %PROGNAME% ---
