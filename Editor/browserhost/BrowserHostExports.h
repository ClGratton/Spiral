#pragma once

#include "BrowserSurface.h"

// The only symbols libSpiralBrowserHost.so exports. The Editor loads the library
// on first use of the Fab panel (dlopen of "<Editor directory>/cef/" + kLibraryName),
// so a headless run, or a machine without libcef's system libraries, never maps
// libcef. The library is built with the same compiler and standard library as
// the Editor, which is what makes sharing IBrowserSurface and BrowserSurfaceConfig
// across the boundary sound; the handshake below catches a stale or mismatched build.
namespace Fab
{
    // Bumped whenever IBrowserSurface, its Listener, or any type they carry changes layout.
    inline constexpr Engine::u32 kBrowserHostAbiVersion = 2;
    inline constexpr const char* kBrowserHostLibraryName = "libSpiralBrowserHost.so";
    inline constexpr const char* kBrowserHostCreateSymbol = "SpiralBrowserHost_CreateSurface";
    inline constexpr const char* kBrowserHostCreateForTestSymbol = "SpiralBrowserHost_CreateSurfaceForTest";
    inline constexpr const char* kBrowserHostDestroySymbol = "SpiralBrowserHost_DestroySurface";

    // Returns nullptr when abiVersion or configSize (sizeof(BrowserSurfaceConfig) in
    // the caller) differs from the library's, or on allocation failure. The surface
    // is uninitialized; the caller runs Initialize(config, listener, error) on it
    // and owns it until SpiralBrowserHost_DestroySurface.
    using BrowserHostCreateFunction = IBrowserSurface* (*)(Engine::u32 abiVersion, Engine::u32 configSize);

    // Test seam: switches is a newline-separated list of "name=value" or "name"
    // Chromium switches (host-resolver rules, certificate pins). The Editor never
    // resolves this symbol.
    using BrowserHostCreateForTestFunction =
        IBrowserSurface* (*)(Engine::u32 abiVersion, Engine::u32 configSize, const char* switches);

    // Runs Shutdown(2000) if the caller has not, then destroys the surface inside the library.
    using BrowserHostDestroyFunction = void (*)(IBrowserSurface* surface);
}
