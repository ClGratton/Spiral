#pragma once

#include <string>
#include <string_view>

namespace Engine
{
    // External navigation is intentionally HTTPS-only. Callers remain
    // responsible for choosing the one product-owned host they expose.
    bool IsAllowedExternalHttpsUrl(std::string_view url, std::string_view requiredHost);
    bool OpenExternalHttpsUrl(
        std::string_view url, std::string_view requiredHost, std::string& outError);
}
