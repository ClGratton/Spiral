#include "BrowserHostLoader.h"

#if defined(GE_PLATFORM_LINUX)
    #include "BrowserHostExports.h"

    #include <dlfcn.h>
    #include <system_error>
#endif

namespace Fab
{
    BrowserSurfaceLoadResult LoadBrowserHostSurface(const std::filesystem::path& editorDirectory)
    {
        BrowserSurfaceLoadResult result;
#if defined(GE_PLATFORM_LINUX)
        if (editorDirectory.empty())
        {
            result.Error = "the Editor directory is unknown, so the browser host library cannot be located";
            return result;
        }
        const std::filesystem::path library = editorDirectory / "cef" / kBrowserHostLibraryName;
        std::error_code code;
        if (!std::filesystem::is_regular_file(library, code))
        {
            result.Error = std::string("the Fab browser runtime is not installed (cef/") + kBrowserHostLibraryName
                + " was not found next to the Editor)";
            return result;
        }

        void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr)
        {
            const char* reason = dlerror();
            result.Error = std::string("the browser host library could not be loaded: ") + (reason != nullptr ? reason : "unknown error");
            return result;
        }

        const auto create = reinterpret_cast<BrowserHostCreateFunction>(dlsym(handle, kBrowserHostCreateSymbol));
        const auto destroy = reinterpret_cast<BrowserHostDestroyFunction>(dlsym(handle, kBrowserHostDestroySymbol));
        if (create == nullptr || destroy == nullptr)
        {
            result.Error = "the browser host library does not export the expected factory functions (stale build)";
            return result;
        }

        IBrowserSurface* surface = create(kBrowserHostAbiVersion, static_cast<Engine::u32>(sizeof(BrowserSurfaceConfig)));
        if (surface == nullptr)
        {
            result.Error = "the browser host library was built for a different interface version or could not allocate "
                           "a surface (stale build)";
            return result;
        }
        result.Surface = MakeBrowserSurfacePtr(surface, destroy);
#else
        (void)editorDirectory;
        result.Error = "the integrated Fab browser is available on Linux only";
#endif
        return result;
    }
}
