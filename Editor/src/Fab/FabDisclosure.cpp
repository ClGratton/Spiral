#include "FabDisclosure.h"

#include "Engine/Platform/ExternalUrl.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace Fab
{
    namespace
    {
        using Engine::u32;

        // Display addresses are capped at this many bytes by DisplayAddress; one at
        // the cap may have been truncated mid-path.
        constexpr size_t kDisplayAddressCap = 200;
        constexpr std::string_view kHttps = "https";

        bool IsJsonSpace(char character)
        {
            return character == ' ' || character == '\t' || character == '\n' || character == '\r';
        }

        // Cursor over the one accepted grammar. Every method either consumes what it
        // names or leaves the cursor where it was and reports false.
        class Cursor
        {
        public:
            explicit Cursor(std::string_view text)
                : m_Text(text)
            {
            }

            void SkipSpace()
            {
                while (m_Index < m_Text.size() && IsJsonSpace(m_Text[m_Index]))
                    ++m_Index;
            }

            bool Consume(char expected)
            {
                SkipSpace();
                if (m_Index >= m_Text.size() || m_Text[m_Index] != expected)
                    return false;
                ++m_Index;
                return true;
            }

            bool Peek(char expected)
            {
                SkipSpace();
                return m_Index < m_Text.size() && m_Text[m_Index] == expected;
            }

            bool AtEnd()
            {
                SkipSpace();
                return m_Index == m_Text.size();
            }

            // A quoted run of lower-case ASCII letters only (the two key names).
            bool Key(std::string& out)
            {
                if (!Consume('"'))
                    return false;
                out.clear();
                while (m_Index < m_Text.size() && m_Text[m_Index] != '"')
                {
                    const char character = m_Text[m_Index];
                    if (character < 'a' || character > 'z' || out.size() >= 16)
                        return false;
                    out.push_back(character);
                    ++m_Index;
                }
                if (m_Index >= m_Text.size())
                    return false;
                ++m_Index;
                return true;
            }

            // 1..999999999: no sign, no leading zero, no fraction or exponent.
            bool Number(u32& out)
            {
                SkipSpace();
                const size_t start = m_Index;
                u32 value = 0;
                while (m_Index < m_Text.size() && m_Text[m_Index] >= '0' && m_Text[m_Index] <= '9' && m_Index - start < 9)
                {
                    value = value * 10 + static_cast<u32>(m_Text[m_Index] - '0');
                    ++m_Index;
                }
                if (m_Index == start || m_Text[start] == '0')
                    return false;
                if (m_Index < m_Text.size() && m_Text[m_Index] >= '0' && m_Text[m_Index] <= '9')
                    return false;
                out = value;
                return true;
            }

        private:
            std::string_view m_Text;
            size_t m_Index = 0;
        };

        bool PrintableUrlText(std::string_view text)
        {
            for (const char character : text)
            {
                const unsigned char value = static_cast<unsigned char>(character);
                if (value < 0x21 || value > 0x7e || character == '\\' || character == '?' || character == '#')
                    return false;
            }
            return true;
        }
    }

    // ---- dismissal ----------------------------------------------------------------

    std::string EncodeFabNoticeDismissal(u32 noticeVersion)
    {
        return "{\"schema\":" + std::to_string(FabDisclosureLimits::kSchema) + ",\"notice\":" + std::to_string(noticeVersion) + "}\n";
    }

    bool DecodeFabNoticeDismissal(std::string_view text, u32& noticeVersion, std::string& error)
    {
        if (text.size() > FabDisclosureLimits::kMaximumFileBytes)
        {
            error = "the Fab notice dismissal record is too large";
            return false;
        }
        const auto malformed = [&error]
        {
            error = "the Fab notice dismissal record is malformed";
            return false;
        };

        Cursor cursor(text);
        if (!cursor.Consume('{'))
            return malformed();
        bool haveSchema = false;
        bool haveNotice = false;
        u32 schema = 0;
        u32 notice = 0;
        for (;;)
        {
            std::string key;
            if (!cursor.Key(key) || !cursor.Consume(':'))
                return malformed();
            if (key == "schema" && !haveSchema)
            {
                if (!cursor.Number(schema))
                    return malformed();
                haveSchema = true;
            }
            else if (key == "notice" && !haveNotice)
            {
                if (!cursor.Number(notice))
                    return malformed();
                haveNotice = true;
            }
            else
            {
                error = "the Fab notice dismissal record has an unknown or repeated key";
                return false;
            }
            if (cursor.Peek(','))
            {
                cursor.Consume(',');
                continue;
            }
            break;
        }
        if (!cursor.Consume('}') || !cursor.AtEnd())
            return malformed();
        if (!haveSchema || !haveNotice)
        {
            error = "the Fab notice dismissal record is incomplete";
            return false;
        }
        if (schema != FabDisclosureLimits::kSchema)
        {
            error = "the Fab notice dismissal record has an unsupported schema";
            return false;
        }
        noticeVersion = notice;
        error.clear();
        return true;
    }

    FabNoticeDismissalStatus ClassifyFabNoticeDismissal(u32 storedVersion, u32 currentVersion)
    {
        return storedVersion == currentVersion ? FabNoticeDismissalStatus::Dismissed : FabNoticeDismissalStatus::Outdated;
    }

    FabNoticeDismissalStatus LoadFabNoticeDismissalFile(const std::filesystem::path& path, u32 currentVersion, std::string& error)
    {
        error.clear();
        std::error_code code;
        const std::filesystem::file_status status = std::filesystem::symlink_status(path, code);
        if (code || status.type() == std::filesystem::file_type::not_found)
            return FabNoticeDismissalStatus::Missing;
        if (status.type() != std::filesystem::file_type::regular)
        {
            error = "the Fab notice dismissal record is not a regular file";
            return FabNoticeDismissalStatus::Rejected;
        }
        const std::uintmax_t size = std::filesystem::file_size(path, code);
        if (code || size > FabDisclosureLimits::kMaximumFileBytes)
        {
            error = "the Fab notice dismissal record is unreadable or too large";
            return FabNoticeDismissalStatus::Rejected;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            error = "the Fab notice dismissal record could not be opened";
            return FabNoticeDismissalStatus::Rejected;
        }
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        u32 stored = 0;
        if (!DecodeFabNoticeDismissal(text, stored, error))
            return FabNoticeDismissalStatus::Rejected;
        return ClassifyFabNoticeDismissal(stored, currentVersion);
    }

    bool SaveFabNoticeDismissalFile(const std::filesystem::path& path, u32 noticeVersion, std::string& error)
    {
        std::error_code code;
        if (!path.has_filename() || !std::filesystem::is_directory(path.parent_path(), code))
        {
            error = "the Fab notice dismissal record directory does not exist";
            return false;
        }
        const std::filesystem::file_status existing = std::filesystem::symlink_status(path, code);
        if (!code && existing.type() != std::filesystem::file_type::not_found
            && existing.type() != std::filesystem::file_type::regular)
        {
            error = "the Fab notice dismissal record is not a regular file";
            return false;
        }

        std::filesystem::path temporary = path;
        temporary += ".tmp";
        std::filesystem::remove(temporary, code);
        {
            std::ofstream create(temporary, std::ios::binary | std::ios::trunc);
            if (!create)
            {
                error = "the Fab notice dismissal record could not be created";
                return false;
            }
        }
        std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, code);
        if (code)
        {
            std::filesystem::remove(temporary, code);
            error = "the Fab notice dismissal record could not be made owner-only";
            return false;
        }
        const std::string text = EncodeFabNoticeDismissal(noticeVersion);
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            output.flush();
            if (!output)
            {
                output.close();
                std::filesystem::remove(temporary, code);
                error = "the Fab notice dismissal record could not be written";
                return false;
            }
        }
        std::filesystem::rename(temporary, path, code);
        if (code)
        {
            std::filesystem::remove(temporary, code);
            error = "the Fab notice dismissal record could not be replaced";
            return false;
        }
        error.clear();
        return true;
    }

    std::filesystem::path FabNoticeDismissalPath(const std::filesystem::path& profileDirectory, const std::filesystem::path& explicitFile)
    {
        if (!explicitFile.empty())
            return explicitFile;
        if (profileDirectory.has_parent_path() && profileDirectory.has_filename())
            return profileDirectory.parent_path() / std::string(kFabNoticeDismissalFileName);
        return {};
    }

    bool FabNoticeVisible(FabNoticeDismissalStatus stored, bool dismissedThisSession)
    {
        return !dismissedThisSession && stored != FabNoticeDismissalStatus::Dismissed;
    }

    // ---- kill switch ------------------------------------------------------------------------

    FabBrowserAvailability ResolveFabBrowserAvailability(bool disabledOnCommandLine, bool enabledBySetting)
    {
        FabBrowserAvailability availability;
        if (disabledOnCommandLine)
        {
            availability.Enabled = false;
            availability.Reason = FabBrowserDisabledReason::CommandLine;
        }
        else if (!enabledBySetting)
        {
            availability.Enabled = false;
            availability.Reason = FabBrowserDisabledReason::Setting;
        }
        return availability;
    }

    std::string_view FabBrowserDisabledText(FabBrowserDisabledReason reason)
    {
        switch (reason)
        {
        case FabBrowserDisabledReason::CommandLine: return "turned off by --no-fab-browser";
        case FabBrowserDisabledReason::Setting: return "turned off in Settings";
        case FabBrowserDisabledReason::None: break;
        }
        return "";
    }

    std::string_view FabBrowserDisabledToken(FabBrowserDisabledReason reason)
    {
        switch (reason)
        {
        case FabBrowserDisabledReason::CommandLine: return "fab_browser_disabled_by_command_line";
        case FabBrowserDisabledReason::Setting: return "fab_browser_disabled_by_setting";
        case FabBrowserDisabledReason::None: break;
        }
        return "";
    }

    // ---- open in the system browser ----------------------------------------------------------

    FabOpenDecision DecideFabOpenInBrowser(const BrowserNavigationPolicy& policy, std::string_view scheme,
        std::string_view currentDisplayAddress, std::string_view homeUrl)
    {
        FabOpenDecision decision;
        const auto accept = [&](std::string url, bool currentPage)
        {
            std::string host = BrowserNavigationPolicy::HostForLog(url);
            if (!policy.IsTopLevelAllowed(url) || !Engine::IsAllowedExternalHttpsUrl(url, host))
                return false;
            decision.Valid = true;
            decision.Url = std::move(url);
            decision.Host = std::move(host);
            decision.UsedCurrentPage = currentPage;
            return true;
        };

        if (scheme == kHttps && !currentDisplayAddress.empty() && currentDisplayAddress.size() < kDisplayAddressCap
            && PrintableUrlText(currentDisplayAddress)
            && accept("https://" + std::string(currentDisplayAddress), true))
        {
            return decision;
        }
        if (accept(std::string(homeUrl), false))
            return decision;
        decision.Error = "the Fab home page is not allowed by the navigation policy";
        return decision;
    }

    // ---- page problems ------------------------------------------------------------------------

    bool IsSecurityCheckStatus(int httpStatus)
    {
        return httpStatus == 403 || httpStatus == 503;
    }

    std::string_view FabPageHintName(FabPageHint hint)
    {
        switch (hint)
        {
        case FabPageHint::None: return "none";
        case FabPageHint::LoadError: return "load-error";
        case FabPageHint::NavigationDenied: return "navigation-denied";
        case FabPageHint::SecurityCheckLikely: return "security-check-likely";
        }
        return "none";
    }

    FabPageHint StrongerFabPageHint(FabPageHint current, FabPageHint incoming)
    {
        return static_cast<int>(incoming) > static_cast<int>(current) ? incoming : current;
    }
}
