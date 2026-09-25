@echo off
REM Build a worktree: build_wt.bat <worktree-name> [deps|configure|build]
REM   deps      = build that worktree's own Windows deps (wt\<name>\deps), matching its CI script
REM   configure = cmake configure (Ninja, Release) into wt\<name>\build
REM   build     = incremental build
setlocal
set "WT=C:\dev\pcsx2\wt\%1"
cd /d "%WT%" || exit /b 1
set "VSCMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "VSNINJA=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "PATH=%VSCMAKE%;%VSNINJA%;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
if "%2"=="deps" (
  set DEBUG=0
  set BUILD_FFMPEG=0
  call ".github\workflows\scripts\windows\build-dependencies.bat" || ( echo === DEPS_EXITCODE=1 === & exit /b 1 )
  echo === DEPS_EXITCODE=%errorlevel% ===
  exit /b %errorlevel%
)
if "%2"=="configure" (
  cmake . -B build "-DCMAKE_PREFIX_PATH=%WT%\deps" -DQT_BUILD=ON -DCMAKE_BUILD_TYPE=Release -DDISABLE_ADVANCE_SIMD=ON -G Ninja || ( echo === CONFIGURE_FAILED === && exit /b 2 )
)
cmake --build build --config Release
echo === BUILD_EXITCODE=%errorlevel% ===
