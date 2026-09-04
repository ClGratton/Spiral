#include "ExternalUrlTests.h"

#include "Engine/Platform/ExternalUrl.h"

#include <string>

bool TestExternalHttpsUrlPolicy()
{
    using Engine::IsAllowedExternalHttpsUrl;
    constexpr std::string_view host = "www.fab.com";
    const std::string oversized = "https://www.fab.com/" + std::string(2048, 'a');

    return IsAllowedExternalHttpsUrl("https://www.fab.com/", host)
        && IsAllowedExternalHttpsUrl(
            "https://www.fab.com/listings/01234567-89ab-cdef-0123-456789abcdef?format=glb#files", host)
        && IsAllowedExternalHttpsUrl("https://WWW.FAB.COM/listings/example", host)
        && !IsAllowedExternalHttpsUrl("http://www.fab.com/", host)
        && !IsAllowedExternalHttpsUrl("https://fab.com/", host)
        && !IsAllowedExternalHttpsUrl("https://www.fab.com.evil.example/", host)
        && !IsAllowedExternalHttpsUrl("https://www.fab.com:443/", host)
        && !IsAllowedExternalHttpsUrl("https://user@www.fab.com/", host)
        && !IsAllowedExternalHttpsUrl("https://www.fab.com\\@evil.example/", host)
        && !IsAllowedExternalHttpsUrl("https://www.fab.com/line\nbreak", host)
        && !IsAllowedExternalHttpsUrl(oversized, host)
        && !IsAllowedExternalHttpsUrl("https://www.fab.com/", "")
        && !IsAllowedExternalHttpsUrl("https://www.fab.com/", ".fab.com")
        && !IsAllowedExternalHttpsUrl("https://www.fab.com/", "www..fab.com");
}
