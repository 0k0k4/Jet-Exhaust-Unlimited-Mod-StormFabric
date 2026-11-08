@echo off
rem Copie les 4 fichiers source et les 3 fichiers de test du depot du mod
rem vers un checkout StormFabric (source de verite compilee).
rem
rem Usage :
rem   tools\sync_to_stormfabric.bat "H:\Bordel 4TO NVME\StormFabric"
rem
setlocal

set "DST=%~1"
if not defined DST set "DST=H:\Bordel 4TO NVME\StormFabric"
set "SRC=%~dp0.."

if not exist "%DST%\src\jet_patch.cpp" (
  echo ERREUR: source StormFabric introuvable : "%DST%"
  echo        attendu : "%DST%\src\jet_patch.cpp"
  exit /b 1
)
if not exist "%SRC%\src\jet_patch.cpp" (
  echo ERREUR: copie locale introuvable : "%SRC%\src\jet_patch.cpp"
  exit /b 1
)

echo Copie des hooks et des tests vers :
echo   %DST%
echo.

copy /Y "%SRC%\src\jet_patch.cpp"     "%DST%\src\" || exit /b 1
copy /Y "%SRC%\src\jet_patch.h"       "%DST%\src\" || exit /b 1
copy /Y "%SRC%\src\jet_catalogue.cpp" "%DST%\src\" || exit /b 1
copy /Y "%SRC%\src\jet_catalogue.h"   "%DST%\src\" || exit /b 1

copy /Y "%SRC%\tests\jet_patch_tests.cpp"     "%DST%\tests\" || exit /b 1
copy /Y "%SRC%\tests\jet_catalogue_tests.cpp" "%DST%\tests\" || exit /b 1
copy /Y "%SRC%\tests\jet_test_sigs.h"         "%DST%\tests\" || exit /b 1

echo.
echo Synchronisation terminee.
echo Verifier ensuite dans StormFabric :
echo   cmd /c ".\build.bat"
echo   cmd /c ".\tests\run_tests.bat"
exit /b 0
