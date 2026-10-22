@echo off
rem Builds the Windows frontend (GUI, tests) with MSVC, Qt and vcpkg.
rem
rem   tools\build_windows.bat [OPT_DIR]
rem
rem OPT_DIR (default %USERPROFILE%\Opt) must contain
rem   Qt\6.8.3\msvc2022_64   (aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O OPT_DIR\Qt)
rem   vcpkg                  (vcpkg install zeromq cppzmq tiff libpng cfitsio
rem                           nlohmann-json gtest spdlog --triplet x64-windows --classic)
rem The deployable program ends up in build-win\deploy.

setlocal
set "SRC=%~dp0.."
set "OPT=%~1"
if "%OPT%"=="" set "OPT=%USERPROFILE%\Opt"
if "%QT_DIR%"=="" set "QT_DIR=%OPT%\Qt\6.8.3\msvc2022_64"
set "BUILD=%SRC%\build-win"

if "%VSCMD_VER%"=="" (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSDIR=%%i"
)
if "%VSCMD_VER%"=="" call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem Set after vcvars64, which points VCPKG_ROOT at the copy bundled with VS.
set "VCPKG_ROOT=%OPT%\vcpkg"

cmake -S "%SRC%\src" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DCMAKE_PREFIX_PATH="%QT_DIR%" || exit /b 1
cmake --build "%BUILD%" || exit /b 1
ctest --test-dir "%BUILD%" --output-on-failure || exit /b 1

rem Collect the GUI with its Qt and vcpkg DLLs into one folder.
set "DEPLOY=%BUILD%\deploy"
if not exist "%DEPLOY%" mkdir "%DEPLOY%"
copy /y "%BUILD%\exec\gui\andor_emccd_gui.exe" "%DEPLOY%\" >nul || exit /b 1
copy /y "%VCPKG_ROOT%\installed\x64-windows\bin\*.dll" "%DEPLOY%\" >nul
"%QT_DIR%\bin\windeployqt.exe" --release --no-translations "%DEPLOY%\andor_emccd_gui.exe" || exit /b 1
echo Deployed to %DEPLOY%
