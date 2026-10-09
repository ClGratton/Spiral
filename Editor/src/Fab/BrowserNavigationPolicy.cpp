#include "BrowserNavigationPolicy.h"

#include "Engine/Platform/ExternalUrl.h"

#include <algorithm>

namespace Fab
{
    namespace
    {
        constexpr std::string_view kHttpsScheme = "https://";
        constexpr size_t kMaximumUrlBytes = 2048;
        constexpr size_t kMaximumHostBytes = 253;
        constexpr size_t kMaximumLabelBytes = 63;
        constexpr size_t kMaximumDisplayBytes = 200;
        constexpr std::string_view kInvalidHost = "<invalid-host>";

        bool IsLowerLdh(char character)
        {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9')
                || character == '-';
        }

        bool IsHostCharacter(char character)
        {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9') || character == '-' || character == '.';
        }

        bool StartsWithHttpsCaseInsensitive(std::string_view url)
        {
            if (url.size() < kHttpsScheme.size())
                return false;
            for (size_t index = 0; index < kHttpsScheme.size(); ++index)
            {
                char character = url[index];
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character - 'A' + 'a');
                if (character != kHttpsScheme[index])
                    return false;
            }
            return true;
        }

        // Returns the text between "scheme://" and the first of / ? #, or an
        // empty view when there is no such authority.
        std::string_view AuthorityOf(std::string_view url)
        {
            const size_t schemeEnd = url.find("://");
            if (schemeEnd == std::string_view::npos)
                return {};
            const size_t start = schemeEnd + 3;
            const size_t end = url.find_first_of("/?#", start);
            return url.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        }

        bool ValidateProviderHost(std::string_view host, std::string& error)
        {
            if (host.empty() || host.size() > kMaximumHostBytes)
            {
                error = "provider host is empty or exceeds 253 bytes";
                return false;
            }
            if (!std::all_of(host.begin(), host.end(), [](char character) { return IsLowerLdh(character) || character == '.'; }))
            {
                error = "provider host must be lower-case ASCII letters, digits, hyphen, and dot only";
                return false;
            }
            if (host.front() == '.' || host.back() == '.' || host.find("..") != std::string_view::npos)
            {
                error = "provider host has an empty label";
                return false;
            }
            const size_t lastDot = host.rfind('.');
            if (lastDot == std::string_view::npos)
            {
                error = "provider host needs at least two labels";
                return false;
            }
            const std::string_view topLevelLabel = host.substr(lastDot + 1);
            if (std::all_of(topLevelLabel.begin(), topLevelLabel.end(), [](char character)
                { return character >= '0' && character <= '9'; }))
            {
                error = "provider host must not be an IP literal or have a numeric top-level label";
                return false;
            }
            size_t labelStart = 0;
            while (labelStart <= host.size())
            {
                const size_t dot = host.find('.', labelStart);
                const std::string_view label = host.substr(
                    labelStart, dot == std::string_view::npos ? std::string_view::npos : dot - labelStart);
                if (label.empty() || label.size() > kMaximumLabelBytes || label.front() == '-' || label.back() == '-')
                {
                    error = "provider host label is empty, too long, or starts or ends with a hyphen";
                    return false;
                }
                if (label.size() >= 4 && label.substr(0, 4) == "xn--")
                {
                    error = "provider host must not use punycode/IDN labels";
                    return false;
                }
                if (dot == std::string_view::npos)
                    break;
                labelStart = dot + 1;
            }
            return true;
        }
    }

    BrowserNavigationPolicy::BrowserNavigationPolicy()
        : m_Hosts { std::string(kFabHost), std::string(kEpicHost) }
    {
    }

    bool BrowserNavigationPolicy::AddProviderHost(std::string_view host, std::string& error)
    {
        if (!ValidateProviderHost(host, error))
            return false;
        if (std::find(m_Hosts.begin(), m_Hosts.end(), host) != m_Hosts.end())
        {
            error = "provider host is already allowed";
            return false;
        }
        m_Hosts.emplace_back(host);
        error.clear();
        return true;
    }

    std::span<const std::string> BrowserNavigationPolicy::ProviderHosts() const
    {
        return std::span<const std::string>(m_Hosts).subspan(kBuiltInHostCount);
    }

    BrowserNavigationVerdict BrowserNavigationPolicy::Evaluate(std::string_view url, BrowserNavigationKind kind) const
    {
        if (kind == BrowserNavigationKind::Popup)
            return BrowserNavigationVerdict::DenyPopup;
        if (kind == BrowserNavigationKind::SubFrame)
            return BrowserNavigationVerdict::AllowSubFrame;

        for (const std::string& host : m_Hosts)
        {
            if (Engine::IsAllowedExternalHttpsUrl(url, host))
                return BrowserNavigationVerdict::Allow;
        }

        if (!StartsWithHttpsCaseInsensitive(url))
            return BrowserNavigationVerdict::DenyScheme;
        if (url.size() > kMaximumUrlBytes)
            return BrowserNavigationVerdict::DenyMalformedUrl;
        for (const char character : url)
        {
            const unsigned char value = static_cast<unsigned char>(character);
            if (value < 0x21 || value > 0x7e || character == '\\')
                return BrowserNavigationVerdict::DenyMalformedUrl;
        }
        // Userinfo, ports, percent escapes, brackets, and every other
        // non-host character land in the authority check; an upper-case
        // scheme spelling fails the exact-scheme check of the composed
        // primitive and is reported as malformed rather than as a host miss.
        const std::string_view authority = AuthorityOf(url);
        if (authority.empty() || !std::all_of(authority.begin(), authority.end(), IsHostCharacter)
            || url.substr(0, kHttpsScheme.size()) != kHttpsScheme)
        {
            return BrowserNavigationVerdict::DenyMalformedUrl;
        }
        return BrowserNavigationVerdict::DenyHost;
    }

    bool BrowserNavigationPolicy::IsTopLevelAllowed(std::string_view url) const
    {
        return Evaluate(url, BrowserNavigationKind::TopLevel) == BrowserNavigationVerdict::Allow;
    }

    std::string BrowserNavigationPolicy::HostForLog(std::string_view url)
    {
        const std::string_view authority = AuthorityOf(url);
        if (authority.empty() || authority.size() > kMaximumHostBytes
            || !std::all_of(authority.begin(), authority.end(), IsHostCharacter))
        {
            return std::string(kInvalidHost);
        }
        std::string host(authority);
        std::transform(host.begin(), host.end(), host.begin(), [](char character)
        {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        });
        return host;
    }

    std::string BrowserNavigationPolicy::DisplayAddress(std::string_view url)
    {
        const std::string_view authority = AuthorityOf(url);
        if (authority.empty())
            return {};
        const size_t schemeEnd = url.find("://");
        const size_t pathStart = schemeEnd + 3 + authority.size();
        std::string_view rest = url.substr(pathStart);
        rest = rest.substr(0, rest.find_first_of("?#"));

        std::string_view host = authority;
        const size_t userinfo = host.rfind('@');
        if (userinfo != std::string_view::npos)
            host.remove_prefix(userinfo + 1);

        std::string display;
        display.reserve(host.size() + rest.size());
        display.append(host);
        display.append(rest);
        if (display.size() > kMaximumDisplayBytes)
            display.resize(kMaximumDisplayBytes);
        for (char& character : display)
        {
            const unsigned char value = static_cast<unsigned char>(character);
            if (value < 0x20 || value == 0x7f)
                character = '?';
        }
        return display;
    }
}
