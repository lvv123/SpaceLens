#include <cstring>
#include <iostream>

#include "version.hpp"

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    expect(std::strcmp(compute_space::kVersion, "0.1.0") == 0,
           "version string should be 0.1.0");

    if (failures == 0) {
        std::cout << "All tests passed\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed\n";
    return 1;
}
