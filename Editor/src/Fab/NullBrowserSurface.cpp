#include "NullBrowserSurface.h"

#include <utility>

namespace Fab
{
    NullBrowserSurface::NullBrowserSurface(std::string reason)
        : m_Reason(std::move(reason))
    {
    }

    bool NullBrowserSurface::Initialize(const BrowserSurfaceConfig&, Listener&, std::string& error)
    {
        error = "browser unavailable: " + m_Reason;
        return false;
    }
}
