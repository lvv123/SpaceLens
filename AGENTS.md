# AGENTS.md

C++20 project built with CMake. No third-party dependencies yet.

## Build & test

```sh
cmake -S . -B build           # configure
cmake --build build           # build
```

Tests are **off by default** and only exist when configured with `-DBUILD_TESTING=ON`:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

There is no test framework; `tests/unit_tests.cpp` is a hand-rolled runner that returns non-zero on failure.

## Toolchain notes (Windows)

- Default CMake generator is **Visual Studio 2022 (MSVC)**, a multi-config generator. Pass `--config Debug` (or Release) to `cmake --build`, and `-C Debug` to `ctest`. The executable is then at `build/bin/<Config>/`.
- MinGW GCC 15 is also installed. To use it: `cmake -S . -B build -G "MinGW Makefiles"`; single-config output is `build/bin/`.
- `CMAKE_RUNTIME_OUTPUT_DIRECTORY` is set, so binaries always live under `build/bin/` — do not look in the source tree.

## Conventions

- Sources are added to `CMakeLists.txt` explicitly per target; there is no globbing. Add new files to the relevant target by hand.
- Headers live in `include/`, implementations in `src/`, tests in `tests/`. Public headers are included as `"version.hpp"` (target include dir is `include/`).
- Version string is defined in `include/version.hpp` and reflected in `CMakeLists.txt` (`project(... VERSION ...)`) — keep them in sync.

## Git

- Repo-local identity is set to `Compute Space Dev <dev@compute-space.local>`; change it with `git config user.name/user.email` if needed.
- `.gitattributes` normalizes line endings to LF (`* text=auto eol=lf`). `build/` and other build output are gitignored.
