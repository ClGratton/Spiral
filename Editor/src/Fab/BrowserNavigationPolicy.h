#pragma once

#include "Engine/Core/Base.h"

#include <array>
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

    // How a default sign-in host is known. Only these three levels may appear in
    // the default table; a host that is merely guessed is never a default and
    // can only enter through the consent flow. ObservedRedirect: seen in a
    // redirect chain of a real sign-in. DocumentedRole: an official provider
    // document names the host as that provider's sign-in or OAuth step.
    // DocumentedHostOnly: an official document names the host but not its role
    // in the flow Epic uses. None of them proves that Epic's integration uses
    // the host; that is what the consent flow and the manual matrix are for.
    enum class SignInHostEvidence
    {
        ObservedRedirect,
        DocumentedRole,
        DocumentedHostOnly
    };

    struct BrowserSignInProvider
    {
        std::string_view Provider;
        std::string_view Host;
        SignInHostEvidence Evidence;
        std::string_view Source;
    };

    inline constexpr std::array<BrowserSignInProvider, 6> kDefaultSignInProviders { {
        { "Google", "accounts.google.com", SignInHostEvidence::DocumentedRole,
            "https://developers.google.com/identity/protocols/oauth2/web-server" },
        { "Apple", "appleid.apple.com", SignInHostEvidence::DocumentedRole,
            "https://developer.apple.com/documentation/signinwithapple/configuring-your-webpage-for-sign-in-with-apple" },
        { "Facebook", "www.facebook.com", SignInHostEvidence::DocumentedRole,
            "https://developers.facebook.com/docs/facebook-login/guides/advanced/manual-flow" },
        { "Steam", "steamcommunity.com", SignInHostEvidence::DocumentedRole, "https://partner.steamgames.com/doc/features/auth" },
        { "Xbox", "login.microsoftonline.com", SignInHostEvidence::DocumentedRole,
            "https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-auth-code-flow" },
        { "Nintendo", "accounts.nintendo.com", SignInHostEvidence::DocumentedHostOnly,
            "https://en-americas-support.nintendo.com/app/answers/detail/a_id/27499" },
    } };

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

        // At most this many user-granted hosts are held by one policy.
        static constexpr size_t kMaximumGrantedHosts = 64;

        // Starts with kFabHost and kEpicHost. The provider list starts empty;
        // AddDefaultProviderHosts adds kDefaultSignInProviders and AddProviderHost
        // adds the hosts a user or the command line supplied.
        BrowserNavigationPolicy();

        // Accepts a lower-cased, dot-separated LDH host with at least two
        // labels. Rejects wildcards and suffixes, IP literals (including
        // all-numeric hosts), punycode/IDN labels, ports, userinfo, a trailing
        // dot, and duplicates. On failure the policy is unchanged.
        bool AddProviderHost(std::string_view host, std::string& error);

        // The validation AddProviderHost applies, without changing any policy.
        static bool IsValidProviderHost(std::string_view host, std::string& error);

        // Adds every kDefaultSignInProviders host that is not already allowed and
        // returns how many were added. Idempotent.
        size_t AddDefaultProviderHosts();
        static std::span<const BrowserSignInProvider> DefaultSignInProviders() { return kDefaultSignInProviders; }

        // Replaces the user-granted host set. Entries that fail IsValidProviderHost,
        // repeat, or are already allowed by the fixed list are skipped, and the set
        // is capped at kMaximumGrantedHosts; returns how many were kept. Granted
        // hosts are evaluated exactly like provider hosts.
        size_t SetGrantedHosts(std::span<const std::string> hosts);

        // Exact, lower-case host comparison against the fixed list and the
        // granted set. Does not parse URLs.
        bool IsHostAllowed(std::string_view host) const;

        // The method of a request is not part of the decision: a form POST that
        // arrives from a provider (Apple answers its sign-in with one) is allowed
        // or denied by its destination host exactly like a GET.
        BrowserNavigationVerdict Evaluate(std::string_view url, BrowserNavigationKind kind) const;
        bool IsTopLevelAllowed(std::string_view url) const;

        // A popup window is never created (Evaluate(url, Popup) is DenyPopup).
        // This is the decision the adapter takes instead: Allow means "load the
        // target in the panel's own main frame", anything else is the top-level
        // deny verdict. A popup that depends on window.opener cannot work this
        // way (its opener is the panel's page, not a window that survives).
        BrowserNavigationVerdict EvaluatePopupTarget(std::string_view url) const;

        // The log-safe host of a denied top-level target when the user could
        // reasonably be asked to allow it: a well-formed https URL (no userinfo,
        // port, IDN, or IP literal) whose host passes IsValidProviderHost and is
        // not allowed yet. Empty otherwise.
        std::string ConsentHost(std::string_view url) const;

        std::span<const std::string> AllowedHosts() const { return m_Hosts; }
        std::span<const std::string> ProviderHosts() const;
        std::span<const std::string> GrantedHosts() const { return m_Granted; }

        // Host text that is safe to log or show: at most 253 LDH characters,
        // or "<invalid-host>" when the authority carries userinfo, a port, or
        // anything else. Never includes path, query, or fragment.
        static std::string HostForLog(std::string_view url);

        // "host/path" without scheme, userinfo, query, or fragment, truncated
        // to 200 bytes. For panel display only; never log it.
        static std::string DisplayAddress(std::string_view url);

        // The lower-case scheme of an absolute URL for panel display ("https"), or
        // empty when the text has no "scheme:" prefix made only of a-z, A-Z, 0-9,
        // '+', '-', '.' and starting with a letter, or the scheme is over 16 bytes.
        // "about:blank" yields "about". It does not judge the URL.
        static std::string SchemeForDisplay(std::string_view url);

    private:
        // m_Hosts[0..kBuiltInHostCount) are the built-in hosts; the rest are
        // provider hosts in insertion order.
        static constexpr size_t kBuiltInHostCount = 2;
        std::vector<std::string> m_Hosts;
        std::vector<std::string> m_Granted;
    };
}
