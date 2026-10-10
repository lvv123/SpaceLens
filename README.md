# Compute Space v2

Windows disk-usage analyzer: reads the NTFS `$MFT` directly (fast, no per-file
API calls) and shows the result as a squarified **treemap**. See `需求文档.md`
for the full specification.

## Requirements

- Windows 10/11, NTFS volume, and **administrator rights** (opening the volume
  to read the MFT requires elevation).
- Visual Studio 2022 (C++20) or MinGW GCC 15.
- Dear ImGui, vendored as a git submodule.

## Setup

```sh
git submodule update --init --recursive
```

## Build

```sh
cmake -S . -B build
cmake --build build --config Release
```

The executable is `build/bin/Release/compute_space_v2.exe` (single-config
generators place it in `build/bin/`). The default build type is Release.

## Test

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Usage

1. Launch as administrator (the app requests elevation via its manifest).
2. Pick an NTFS volume and press **扫描**.
3. Click a folder in the treemap or the list to drill in; use the threshold
   slider to hide small items (they are folded into an **其他** bucket).
