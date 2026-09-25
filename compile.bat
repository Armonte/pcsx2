@echo off

where msbuild
if errorlevel 1 (
  if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  ) else (
    echo Visual Studio 2022 not found.
  )
)

for %%a in (%*) do (
  if "%%a"=="--clean" (
    msbuild "PCSX2_qt.sln" /m /v:m /p:Configuration=release /t:Clean
  ) else if "%%a"=="--help" (
    echo --clean: duh
  )
  
)


msbuild "PCSX2_qt.sln" /m /v:m /p:Configuration=release
pause