@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM Unified Windows build: godot-cpp + cesium-native + extension in ONE VS 2026 solution.
REM Usage: build_windows_unified.bat [--double] [--no-build]
REM   --double     double-precision build (separate build-double\ folder, uses SCons for godot-cpp)
REM   --no-build   only fetch sources and generate the .sln

set "ROOT=%~dp0"
pushd "%ROOT%" >nul

set "BUILD_DIR=build"
set "EXTRA_ARGS="
set "NO_BUILD="
:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="--no-build" set "NO_BUILD=1"
if /i "%~1"=="--double" (
  set "BUILD_DIR=build-double"
  set "EXTRA_ARGS=-DCESIUM_GD_DOUBLE_PRECISION=ON"
)
shift
goto :parse_args
:args_done

REM Same environment as the working GitHub Actions workflow
set "GIT_LFS_SKIP_SMUDGE=1"
set "VCPKG_DEFAULT_TRIPLET=x64-windows-static"
set "VCPKG_ROOT=%ROOT%vcpkg"
set "VCPKG_FORCE_SYSTEM_BINARIES=1"

echo [INFO] Starting unified Windows build workflow...

call :ensure_tool git "git --version"
if errorlevel 1 goto :fail

call :ensure_tool cmake "cmake --version"
if errorlevel 1 goto :fail

call :ensure_tool python "python --version"
if errorlevel 1 goto :fail

if defined EXTRA_ARGS (
  call :ensure_tool scons "scons --version"
  if errorlevel 1 (
    echo [INFO] Install SCons with: python -m pip install scons
    goto :fail
  )
)

set "GENERATOR=Visual Studio 18 2026"
cmake --help | findstr /c:"%GENERATOR%" >nul
if errorlevel 1 (
  echo [ERROR] Your CMake does not offer the "%GENERATOR%" generator.
  echo         Update CMake and install Visual Studio 2026 with the C++ workload.
  goto :fail
)

REM ---- submodules / vcpkg refresh (CI: "Refresh vcpkg / submodules") ---------
if exist ".git" (
  git submodule update --init --recursive
  if errorlevel 1 goto :fail
)
if exist "vcpkg\.git" (
  git -C vcpkg fetch --all --tags
  if errorlevel 1 goto :fail
  git -C vcpkg pull --ff-only
  if errorlevel 1 goto :fail
)

REM ---- godot-cpp -------------------------------------------------------------
if not exist "godot-cpp\.git" (
  echo [INFO] Cloning godot-cpp ^(godot-4.1.4-stable^)...
  git clone --branch godot-4.1.4-stable --depth 1 https://github.com/godotengine/godot-cpp.git godot-cpp
  if errorlevel 1 goto :fail
) else (
  echo [INFO] godot-cpp already present.
)

REM ---- cesium-native (pinned to the CI commit) -------------------------------
set "NATIVE_DIR=cesium_godot\native"
set "NATIVE_COMMIT=9f6ae299e2709f866db52c4be29b6c31e10718c8"
if not exist "%NATIVE_DIR%\.git" (
  echo [INFO] Cloning cesium-native...
  git clone https://github.com/CesiumGS/cesium-native.git "%NATIVE_DIR%"
  if errorlevel 1 goto :fail
)

set "CUR="
for /f %%H in ('git -C "%NATIVE_DIR%" rev-parse HEAD') do set "CUR=%%H"
if /i "!CUR!"=="%NATIVE_COMMIT%" (
  echo [INFO] cesium-native already at the pinned commit.
) else (
  echo [INFO] Syncing cesium-native to CI-pinned commit...
  git -C "%NATIVE_DIR%" reset --hard
  if errorlevel 1 goto :fail
  git -C "%NATIVE_DIR%" clean -fdx
  if errorlevel 1 goto :fail
  git -C "%NATIVE_DIR%" fetch origin %NATIVE_COMMIT%
  if errorlevel 1 goto :fail
  git -C "%NATIVE_DIR%" -c advice.detachedHead=false checkout %NATIVE_COMMIT%
  if errorlevel 1 goto :fail
  git -C "%NATIVE_DIR%" submodule sync --recursive
  if errorlevel 1 goto :fail
  git -C "%NATIVE_DIR%" submodule update --init --recursive --force
  if errorlevel 1 goto :fail
)

REM The ezvcpkg pin workaround (-> 2025.09.17) and the Material.h OPAQUE patch
REM are applied by CMakeLists.txt at configure time.

REM ---- configure (generates %BUILD_DIR%\Godot3DTiles.sln) --------------------
echo [INFO] Configuring with "%GENERATOR%" into %BUILD_DIR%...
cmake -S . -B "%BUILD_DIR%" -G "%GENERATOR%" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 %EXTRA_ARGS%
if errorlevel 1 goto :fail

echo [OK] Solution: "%CD%\%BUILD_DIR%\Godot3DTiles.sln"

if defined NO_BUILD goto :done

echo [INFO] Building Release ^(cesium-native and godot-cpp are built as dependencies^)...
cmake --build "%BUILD_DIR%" --config Release --target Godot3DTiles
if errorlevel 1 goto :fail

echo [OK] Extension: "%CD%\godot3dtiles\addons\cesium_godot\lib"

:done
echo [OK] Done.
popd >nul
exit /b 0

:ensure_tool
where %~1 >nul 2>&1
if %errorlevel% neq 0 (
  echo [ERROR] Required tool '%~1' not found in PATH.
  exit /b 1
)
%~2 >nul 2>&1
if %errorlevel% neq 0 (
  echo [ERROR] Tool '%~1' is present but not runnable.
  exit /b 1
)
exit /b 0

:fail
echo [FAIL] Unified Windows build workflow failed.
popd >nul
exit /b 1
