#include "BrowserHostExports.h"

#include "CefBrowserSurface.h"

#include <sstream>
#include <utility>

#define SPIRAL_BROWSER_HOST_EXPORT extern "C" __attribute__((visibility("default")))

namespace
{
    bool AbiMatches(Engine::u32 abiVersion, Engine::u32 configSize)
    {
        return abiVersion == Fab::kBrowserHostAbiVersion && configSize == sizeof(Fab::BrowserSurfaceConfig);
    }
}

SPIRAL_BROWSER_HOST_EXPORT Fab::IBrowserSurface* SpiralBrowserHost_CreateSurface(Engine::u32 abiVersion, Engine::u32 configSize)
{
    if (!AbiMatches(abiVersion, configSize))
        return nullptr;
    return new Fab::CefBrowserSurface();
}

SPIRAL_BROWSER_HOST_EXPORT Fab::IBrowserSurface* SpiralBrowserHost_CreateSurfaceForTest(
    Engine::u32 abiVersion, Engine::u32 configSize, const char* switches)
{
    if (!AbiMatches(abiVersion, configSize))
        return nullptr;
    Fab::CefBrowserSurfaceTestOptions options;
    std::istringstream lines(switches != nullptr ? switches : "");
    for (std::string line; std::getline(lines, line);)
    {
        if (!line.empty())
            options.CommandLineSwitches.push_back(line);
    }
    return new Fab::CefBrowserSurface(std::move(options));
}

SPIRAL_BROWSER_HOST_EXPORT void SpiralBrowserHost_DestroySurface(Fab::IBrowserSurface* surface)
{
    delete surface;
}
