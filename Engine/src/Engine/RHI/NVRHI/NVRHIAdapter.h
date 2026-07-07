#pragma once

#include <string>

namespace Engine::RHI
{
    struct NVRHIAdapterInfo
    {
        bool Available = false;
        bool HasNativeDevice = false;
        const char* ProbeFormatName = "Unavailable";
        std::string AdapterName;
        std::string NativeBackendName;
    };

    NVRHIAdapterInfo QueryNVRHIAdapter();
}
