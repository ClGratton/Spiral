// Chromium subprocess executable (zygote, renderer, GPU, utility). It
// deliberately links only the CEF wrapper: Engine::Core's EntryPoint, logging,
// crash handler and job system must not run inside every Chromium child.
#include "include/cef_app.h"

#include <cstdio>
#include <unistd.h>

#if !defined(__linux__)
#error "SpiralBrowserHelper is Linux-only until its Windows and macOS CEF slices exist."
#endif

int main(int argc, char** argv)
{
    CefMainArgs mainArgs(argc, argv);
    const int exitCode = CefExecuteProcess(mainArgs, nullptr, nullptr);
    if (exitCode >= 0)
    {
        // Skipping the epilogue avoids a stack-protector canary abort that the
        // component-unzipping utility process otherwise raises on exit.
        _exit(exitCode);
    }

    std::fputs("SpiralBrowserHelper is a Chromium subprocess and is launched only by CEF.\n", stderr);
    return 2;
}
