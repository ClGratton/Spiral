#include "Engine/RHI/NVRHI/NVRHIAdapter.h"

#if defined(GE_HAS_NVRHI)
    #include <nvrhi/nvrhi.h>
#endif

namespace Engine::RHI
{
    NVRHIAdapterInfo QueryNVRHIAdapter()
    {
#if defined(GE_HAS_NVRHI)
        const nvrhi::FormatInfo& info = nvrhi::getFormatInfo(nvrhi::Format::RGBA8_UNORM);
        NVRHIAdapterInfo adapterInfo;
        adapterInfo.Available = true;
        adapterInfo.ProbeFormatName = info.name;
        return adapterInfo;
#else
        return {};
#endif
    }
}
