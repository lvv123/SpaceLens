# AGENTS.md

Windows-only C++20 desktop app that analyzes disk usage by reading the NTFS
`$MFT` directly and visualizes it as a treemap. Spec: `需求文档.md`.

## Prerequisites

The GUI needs Dear ImGui, which is a **git submodule** pinned to a release tag:

```sh
git submodule update --init --recursive
```

If the clone is blocked on this network, set a proxy first
(`$env:HTTP_PROXY` / `$env:HTTPS_PROXY`, e.g. `http://127.0.0.1:7890`).

## Build & test

Default CMake generator here is **Visual Studio 2022** (multi-config), so pass
`--config`. Default build type is **Release** (performance first).

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

- Executable: `build/bin/Release/compute_space_v2.exe` (multi-config adds the
  config subdir; single-config generators put it directly in `build/bin`).
- Tests are **off unless** `-DBUILD_TESTING=ON`. There is no framework:
  `tests/unit_tests.cpp` is a hand-rolled runner returning non-zero on failure.
- MinGW GCC 15 is also installed (`-G "MinGW Makefiles"`), but the GUI assumes
  MSVC/DX11 and the admin manifest is MSVC-only.

## Layout

- `include/` — public headers (`ntfs.hpp`, `tree.hpp`, `treemap.hpp`, `scan.hpp`,
  `format.hpp`, `utf8.hpp`). Included as `"ntfs.hpp"` (include root is `include/`).
- `src/core/` — `compute_core` static lib: MFT parsing, tree, treemap, scanners.
  **No UI dependencies** so it stays unit-testable.
- `src/gui/` — Win32 + DirectX 11 + ImGui app (`app.cpp`, `treemap_view.cpp`).
- `tests/unit_tests.cpp` — core tests: synthetic NTFS records, an end-to-end
  MFT scan over an in-memory volume image, treemap geometry, and a
  recursive-scanner oracle on a temp dir.
- `scan_mft(VolumeReader&, ...)` is the testable core of the scanner; tests feed
  it a `MemoryReader` over a hand-built NTFS image (`build_volume`). Keep new
  scan logic going through `VolumeReader` so it stays testable.
- `third_party/imgui` — ImGui submodule (do not edit; update the pin instead).

Sources are listed explicitly in `CMakeLists.txt` (no globbing) — add new files
by hand to the right target.

## Running / privileges

- The app opens `\\.\X:` to read the MFT, which **requires administrator**. The
  default build embeds a `requireAdministrator` manifest (UAC).
- To run/test the GUI **without** elevation (e.g. automated smoke tests), build
  with `-DCOMPUTE_SPACE_NO_ADMIN_MANIFEST=ON`; the MFT scan then fails cleanly
  and the UI shows a "need admin" notice.
- Dev shortcut: set `COMPUTE_SPACE_DEV_DIR=<dir>` to auto-start a
  `scan_directory_recursive` scan of that directory instead of MFT. Only for
  development without admin; never set in normal runs.

## Core conventions / gotchas

- Size is always `uint64_t`; never cast to `float`/`int` for storage.
- Hard links: one `$FILE_NAME` attribute = one directory entry = one tree node
  (the same MFT record can back several nodes). This matches Explorer.
- Parent linking **validates the FRN sequence number** (high 16 bits) so recycled
  MFT records are not attached to the wrong directory.
- NTFS metadata records are `recordNumber < 16` except the root (record 5);
  everything else (including `$Recycle.Bin`) is counted as a real file.
- Always run `apply_fixups` before parsing a record or its `$DATA` run list.
- `squarified_treemap` expects weights sorted descending; it scales them to the
  bounds area internally.
- The tree is an arena (`std::vector<Node>` + index/sibling links + a name pool),
  not pointer-per-node — keep it that way for large volumes.
