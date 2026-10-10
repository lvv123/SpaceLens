#include <windows.h>

#include "gui/app.hpp"

int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*prev*/, PWSTR /*cmd*/, int /*show*/) {
    return gui::run();
}
