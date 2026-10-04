# cdm — segmented download manager

IDM-style segmented download manager in C: CLI (`cdm`) plus an optional
Nuklear/GLFW GUI (`cdm-gui`). Engine: libcurl + HTTP Range + resume
(`.part` + `.cdm` sidecar), adaptive concurrency, speed limiting,
SHA-256 integrity gate.

## Prerequisites

### Windows (PowerShell)

* Git
* CMake ≥ 3.15 ([Kitware](https://cmake.org/download/) or
  `winget install -e --id Kitware.CMake`)
* Visual Studio 2022 Build Tools with the **Desktop C++ workload** and a
  **Windows 11 SDK**:

  ```powershell
  winget install -e --id Microsoft.VisualStudio.2022.BuildTools --source winget
  # then add the VCTools workload + SDK, e.g. via vs_installer:
  & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe" modify `
    --installPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" `
    --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended `
    --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --quiet
  ```

  Without the SDK, CMake configure fails with
  `MSB8037: The Windows SDK version ... was not found`.

* No system libcurl needed: on Windows the build fetches and builds
  libcurl (HTTP-only, Schannel TLS) automatically (`CDM_FETCH_CURL=ON`,
  the Windows default).

### Linux (Debian/Ubuntu)

```sh
sudo apt install build-essential cmake pkg-config libcurl4-openssl-dev \
     libglfw3-dev libglew-dev libgl1-mesa-dev
```

## Build

```powershell
# Windows — CLI only (recommended; GUI defaults OFF on Windows)
cmake -S . -B build -DCDM_BUILD_GUI=OFF -DCDM_FETCH_CURL=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --prefix install --config Release
.\install\bin\cdm.exe -h
```

```sh
# Linux — CLI + GUI
cmake -S . -B build -DCDM_BUILD_GUI=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

POSIX-only shortcut (no GUI):

```sh
make
```

## Usage

```sh
cdm [options] <url>
  -o <file>     output path (default: derived from URL)
  -n <num>      max connections (default: 16)
  -s <num>      starting connections (default: 4)
  -k <bytes>    chunk size (default: 8388608)
  -l <bps>      max download speed in bytes/sec (0 = unlimited)
  -r <num>      max retries per chunk (default: 5)
  -x            disable adaptive concurrency
  -C            disable resume
  -S <sha256>   verify downloaded file against hex digest
  -q            quiet (no progress bar)
  -h            show help
```

Example:

```powershell
.\install\bin\cdm.exe -o .\file.zip https://example.com/file.zip
```

## GUI (optional)

The GUI needs OpenGL + GLEW + GLFW, which are not present on a vanilla
Windows box — that is why `CDM_BUILD_GUI` defaults to `OFF` on Windows
(and a configure with missing deps now skips `cdm-gui` with a warning
instead of failing). To build it on Windows, install the deps with
[vcpkg](https://vcpkg.io/) first (OpenGL itself comes from the Windows SDK):

```powershell
git clone https://github.com/microsoft/vcpkg.git D:\vcpkg
D:\vcpkg\bootstrap-vcpkg.bat
D:\vcpkg\vcpkg.exe install glfw3 glew --triplet x64-windows

cmake -S . -B build -DCDM_BUILD_GUI=ON -DCDM_FETCH_CURL=ON `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

This produces `build/Release/cdm-gui.exe` (plus `glew32.dll` and
`glfw3.dll` next to it — keep them together). Unlike `cdm.exe`,
`cdm-gui.exe` is a real windowed app, so double-clicking it works.

## Packaging

```powershell
cpack --config build/CPackConfig.cmake -C Release   # ZIP (+ NSIS if makensis is installed)
```

Artifacts land in `build/` (ignored by git; distribute via GitHub Releases).

## Troubleshooting (Windows)

| Symptom | Cause / fix |
|---|---|
| `cmake` / `cl` not recognized | Install CMake + VS Build Tools (see above) and open a fresh terminal. |
| `MSB8037: Windows SDK ... not found` | Install the Windows 11 SDK component (see above). |
| `Could NOT find CURL` | Use `-DCDM_FETCH_CURL=ON` (default on Windows). |
| Configure fails on `GLEW` / `GLFW` | Pass `-DCDM_BUILD_GUI=OFF` (default on Windows). |
| `Makefile: pkg-config: command not found` | The `Makefile` is POSIX-only; on Windows use the CMake steps above. |

## Layout

* `src/cli/` — CLI front-end
* `src/engine/` — portable engine (curl multi, chunks, resume, retry)
* `src/platform/` — `platform_win.c` / `platform_posix.c` (`cdm_thread`, `cdm_mutex`, `cdm_file`)
* `src/ui/` — queue manager + Nuklear GUI (uses `cdm_thread`/`cdm_mutex`, no raw pthreads)
* `tests/` — `test_range`, `test_settings` (run via `ctest`)
* `third_party/nuklear/` — bundled Nuklear headers
