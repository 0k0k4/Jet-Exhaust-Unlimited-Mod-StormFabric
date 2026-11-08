@echo off
rem Runs the mod's off-game unit tests (pure logic only; never executes game code).
rem Usage: run_tests.bat ["<SDK include>"]
setlocal

if not defined VCVARS (
  if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if not defined VCVARS (
  echo ERROR: vcvars64.bat not found. Set VCVARS environment variable.
  exit /b 1
)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0.."

set "SDK=%~1"
if not defined SDK set "SDK=%~dp0..\..\StormFabric\include"
if not exist "%SDK%\sw_mod_sdk.h" (
  echo ERROR: StormFabric SDK not found: "%SDK%\sw_mod_sdk.h"
  exit /b 1
)
if not exist build\tests mkdir build\tests

echo --- Building mod logic tests ---
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /I"%SDK%" /I"%~dp0..\src" ^
   tests\mod_logic_tests.cpp src\jet_patch.cpp src\jet_catalogue.cpp src\jet_sdk.cpp ^
   /Fo:build\tests\ /Fe:build\tests\mod_logic_tests.exe || exit /b 1

build\tests\mod_logic_tests.exe || exit /b 1
echo.
echo All mod logic tests passed.
