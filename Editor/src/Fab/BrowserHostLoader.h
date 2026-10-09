#pragma once

#include "BrowserPanelCore.h"

#include <filesystem>

namespace Fab
{
    // Loads <editorDirectory>/cef/libSpiralBrowserHost.so on demand and creates an
    // uninitialised surface through its C factory. Linux only; elsewhere, and for
    // every failure (library missing, dlopen error, missing symbol, ABI or
    // BrowserSurfaceConfig size mismatch, allocation failure) the result carries a
    // plain reason and no surface. The library is mapped with RTLD_NOW|RTLD_LOCAL
    // and is never dlclosed, because libcef cannot be unloaded safely. The returned
    // surface must be destroyed through the library, which its deleter does.
    BrowserSurfaceLoadResult LoadBrowserHostSurface(const std::filesystem::path& editorDirectory);
}
