@echo off
rem Builds mod\jet_unlimited\jet_unlimited.dll against the StormFabric SDK.
rem
rem Usage:
rem   build_mod.bat                 (SDK = ..\StormFabric\include)
rem   build_mod.bat "<SDK include>" (explicit SDK include directory)
rem Requires MSVC x64 (cl.exe).
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
cd /d "%~dp0"

set "SDK=%~1"
if not defined SDK set "SDK=%~dp0..\StormFabric\include"
if not exist "%SDK%\sw_mod_sdk.h" (
  echo ERROR: StormFabric SDK not found: "%SDK%\sw_mod_sdk.h"
  echo        Pass the include directory as the first argument.
  exit /b 1
)

if not exist build\mod mkdir build\mod

echo ========================================================
echo Building Jet Exhaust Unlimited (jet_unlimited.dll)
echo SDK: %SDK%
echo ========================================================
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /GS- /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /I"%SDK%" /I"%~dp0src" ^
   /LD src\jet_sdk.cpp src\jet_patch.cpp src\jet_catalogue.cpp mod\jet_unlimited\jet_unlimited.cpp ^
   /Fo:build\mod\ /Fe:mod\jet_unlimited\jet_unlimited.dll ^
   /link /DLL /MANIFEST:NO /DEBUG:NONE /OPT:REF /OPT:ICF || exit /b 1

del /Q build\mod\*.obj build\mod\*.exp build\mod\*.lib >nul 2>&1
echo.
echo Build successful:
echo   - %~dp0mod\jet_unlimited\jet_unlimited.dll
