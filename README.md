# Compute Space v2

A C++20 / CMake project.

## Build

```sh
cmake -S . -B build
cmake --build build
./build/bin/compute_space_v2      # Windows: .\build\bin\compute_space_v2.exe
```

## Test

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
