#pragma once

#include "Engine/Core/Base.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Fab
{
    enum class BrowserNavigationKind
    {
        TopLevel,
        SubFrame,
        Popup
    };

    enum class BrowserNavigationVerdict
    {
        Allow,
        // Frames and sub-resources are deliberately outside this policy. The
        // 2026-10-09 spike observed hCaptcha and Epic asset frames on hosts
        // that cannot be enumerated; only top-level documents are gated.
        AllowSubFrame,
        DenyPopup,
        DenyScheme,
        DenyMalformedUrl,
        DenyHost
    };

    inline bool IsNavigationAllowed(BrowserNavigationVerdict verdict)
    {
        return verdict == BrowserNavigationVerdict::Allow || verdict == BrowserNavigationVerdict::AllowSubFrame;
    }

    // Exact-host, https-only top-level allowlist. The URL grammar (no userinfo,
    // no port, ASCII host characters only, no whitespace/backslash, 2 KiB cap,
    // case-insensitive exact host match) is delegated by composition to
    // Engine::IsAllowedExternalHttpsUrl, one call per allowed host, so the two
    // policies cannot drift apart. This class adds the multi-host list, the
    // popup/sub-frame split, deny reasons, and the validation of data-driven
    // provider hosts.
    class BrowserNavigationPolicy
    {
    public:
        static constexpr std::string_view kFabHost = "www.fab.com";
        static constexpr std::string_view kEpicHost = "www.epicgames.com";

        // Starts with kFabHost and kEpicHost. The provider list starts empty
        // and is filled only from identity-provider hosts observed in a real
        // sign-in flow.
        BrowserNavigationPolicy();

        // Accepts a lower-cased, dot-separated LDH host with at least two
        // labels. Rejects wildcards and suffixes, IP literals (including
        // all-numeric hosts), punycode/IDN labels, ports, userinfo, a trailing
        // dot, and duplicates. On failure the policy is unchanged.
        bool AddProviderHost(std::string_view host, std::string& error);

        BrowserNavigationVerdict Evaluate(std::string_view url, BrowserNavigationKind kind) const;
        bool IsTopLevelAllowed(std::string_view url) const;

        std::span<const std::string> AllowedHosts() const { return m_Hosts; }
        std::span<const std::string> ProviderHosts() const;

        // Host text that is safe to log or show: at most 253 LDH characters,
        // or "<invalid-host>" when the authority carries userinfo, a port, or
        // anything else. Never includes path, query, or fragment.
        static std::string HostForLog(std::string_view url);

        // "host/path" without scheme, userinfo, query, or fragment, truncated
        // to 200 bytes. For panel display only; never log it.
        static std::string DisplayAddress(std::string_view url);

    private:
        // m_Hosts[0..kBuiltInHostCount) are the built-in hosts; the rest are
        // provider hosts in insertion order.
        static constexpr size_t kBuiltInHostCount = 2;
        std::vector<std::string> m_Hosts;
    };
}
