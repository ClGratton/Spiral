#include "BrowserSignInHosts.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace Fab
{
    namespace
    {
        using Engine::u64;

        bool IsJsonSpace(char character)
        {
            return character == ' ' || character == '\t' || character == '\n' || character == '\r';
        }

        bool IsHostStringCharacter(char character)
        {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-'
                || character == '.';
        }

        // The only grammar the codec accepts:
        //   {"schema":1,"hosts":["a.example.com",...]}   (keys in either order, spaces allowed)
        // No escapes, no other value types, no unknown keys, no trailing commas.
        class Reader
        {
        public:
            explicit Reader(std::string_view text)
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

            bool String(std::string& out)
            {
                if (!Consume('"'))
                    return false;
                out.clear();
                while (m_Index < m_Text.size() && m_Text[m_Index] != '"')
                {
                    if (!IsHostStringCharacter(m_Text[m_Index]) || out.size() > 253)
                        return false;
                    out.push_back(m_Text[m_Index++]);
                }
                if (m_Index >= m_Text.size())
                    return false;
                ++m_Index;
                return true;
            }

            bool Schema(u64& out)
            {
                SkipSpace();
                const size_t start = m_Index;
                out = 0;
                while (m_Index < m_Text.size() && m_Text[m_Index] >= '0' && m_Text[m_Index] <= '9' && m_Index - start < 9)
                {
                    out = out * 10 + static_cast<u64>(m_Text[m_Index] - '0');
                    ++m_Index;
                }
                if (m_Index == start || (m_Text[start] == '0' && m_Index - start > 1))
                    return false;
                return m_Index >= m_Text.size() || !(m_Text[m_Index] >= '0' && m_Text[m_Index] <= '9');
            }

        private:
            std::string_view m_Text;
            size_t m_Index = 0;
        };
    }

    // ---- host book ----------------------------------------------------------------------

    SignInGrantResult BrowserSignInHostBook::Grant(
        std::string_view host, bool persistent, const BrowserNavigationPolicy& baseline, std::string& error)
    {
        if (!BrowserNavigationPolicy::IsValidProviderHost(host, error))
            return SignInGrantResult::Invalid;
        if (baseline.IsHostAllowed(host))
        {
            error = "host is already allowed";
            return SignInGrantResult::AlreadyAllowed;
        }
        const auto existing = std::find_if(m_Entries.begin(), m_Entries.end(),
            [host](const BrowserGrantedHost& entry) { return entry.Host == host; });
        if (existing != m_Entries.end())
        {
            if (!persistent || existing->Persistent)
            {
                error = "host is already granted";
                return SignInGrantResult::AlreadyAllowed;
            }
            if (PersistentCount() >= SignInHostLimits::kMaximumPersistentHosts)
            {
                error = "the saved sign-in host list is full";
                return SignInGrantResult::LimitReached;
            }
            existing->Persistent = true;
            error.clear();
            return SignInGrantResult::Granted;
        }
        if (persistent ? PersistentCount() >= SignInHostLimits::kMaximumPersistentHosts
                       : SessionCount() >= SignInHostLimits::kMaximumSessionHosts)
        {
            error = persistent ? "the saved sign-in host list is full" : "too many hosts are allowed for this session";
            return SignInGrantResult::LimitReached;
        }
        m_Entries.push_back({ std::string(host), persistent });
        error.clear();
        return SignInGrantResult::Granted;
    }

    bool BrowserSignInHostBook::Revoke(std::string_view host, bool* wasPersistent)
    {
        const auto existing = std::find_if(m_Entries.begin(), m_Entries.end(),
            [host](const BrowserGrantedHost& entry) { return entry.Host == host; });
        if (existing == m_Entries.end())
            return false;
        if (wasPersistent != nullptr)
            *wasPersistent = existing->Persistent;
        m_Entries.erase(existing);
        return true;
    }

    bool BrowserSignInHostBook::Clear()
    {
        const bool hadPersistent = PersistentCount() > 0;
        m_Entries.clear();
        return hadPersistent;
    }

    std::vector<std::string> BrowserSignInHostBook::HostList() const
    {
        std::vector<std::string> hosts;
        hosts.reserve(m_Entries.size());
        for (const BrowserGrantedHost& entry : m_Entries)
            hosts.push_back(entry.Host);
        return hosts;
    }

    size_t BrowserSignInHostBook::PersistentCount() const
    {
        return static_cast<size_t>(std::count_if(
            m_Entries.begin(), m_Entries.end(), [](const BrowserGrantedHost& entry) { return entry.Persistent; }));
    }

    std::string BrowserSignInHostBook::EncodePersistent() const
    {
        std::string text = "{\"schema\":" + std::to_string(SignInHostLimits::kSchema) + ",\"hosts\":[";
        bool first = true;
        for (const BrowserGrantedHost& entry : m_Entries)
        {
            if (!entry.Persistent)
                continue;
            if (!first)
                text += ',';
            first = false;
            text += '"';
            text += entry.Host;
            text += '"';
        }
        text += "]}\n";
        return text;
    }

    bool BrowserSignInHostBook::LoadPersistent(std::string_view text, const BrowserNavigationPolicy& baseline, std::string& error)
    {
        if (text.size() > SignInHostLimits::kMaximumFileBytes)
        {
            error = "the sign-in host list is too large";
            return false;
        }

        Reader reader(text);
        bool haveSchema = false;
        bool haveHosts = false;
        std::vector<std::string> hosts;
        if (!reader.Consume('{'))
        {
            error = "the sign-in host list is not a JSON object";
            return false;
        }
        const auto malformed = [&]
        {
            error = "the sign-in host list is malformed";
            return false;
        };
        if (!reader.Peek('}'))
        {
            for (;;)
            {
                std::string key;
                if (!reader.String(key) || !reader.Consume(':'))
                    return malformed();
                if (key == "schema" && !haveSchema)
                {
                    u64 schema = 0;
                    if (!reader.Schema(schema) || schema != SignInHostLimits::kSchema)
                    {
                        error = "the sign-in host list has an unsupported schema";
                        return false;
                    }
                    haveSchema = true;
                }
                else if (key == "hosts" && !haveHosts)
                {
                    if (!reader.Consume('['))
                        return malformed();
                    if (!reader.Peek(']'))
                    {
                        for (;;)
                        {
                            std::string host;
                            std::string hostError;
                            if (!reader.String(host) || !BrowserNavigationPolicy::IsValidProviderHost(host, hostError))
                            {
                                error = "the sign-in host list contains an invalid host";
                                return false;
                            }
                            if (hosts.size() >= SignInHostLimits::kMaximumPersistentHosts
                                || std::find(hosts.begin(), hosts.end(), host) != hosts.end())
                            {
                                error = "the sign-in host list has a duplicate or too many hosts";
                                return false;
                            }
                            hosts.push_back(std::move(host));
                            if (reader.Peek(','))
                            {
                                reader.Consume(',');
                                continue;
                            }
                            break;
                        }
                    }
                    if (!reader.Consume(']'))
                        return malformed();
                    haveHosts = true;
                }
                else
                {
                    error = "the sign-in host list has an unknown or repeated key";
                    return false;
                }
                if (reader.Peek(','))
                {
                    reader.Consume(',');
                    continue;
                }
                break;
            }
        }
        if (!reader.Consume('}'))
            return malformed();
        if (!haveSchema || !haveHosts || !reader.AtEnd())
        {
            error = "the sign-in host list is incomplete or has trailing text";
            return false;
        }

        std::vector<BrowserGrantedHost> next;
        for (std::string& host : hosts)
        {
            if (!baseline.IsHostAllowed(host))
                next.push_back({ std::move(host), true });
        }
        m_Entries = std::move(next);
        error.clear();
        return true;
    }

    // ---- file ---------------------------------------------------------------------------

    SignInFileStatus LoadSignInHostsFile(
        const std::filesystem::path& path, BrowserSignInHostBook& book, const BrowserNavigationPolicy& baseline, std::string& error)
    {
        std::error_code code;
        const std::filesystem::file_status status = std::filesystem::symlink_status(path, code);
        if (code || status.type() == std::filesystem::file_type::not_found)
        {
            error.clear();
            return SignInFileStatus::Missing;
        }
        if (status.type() != std::filesystem::file_type::regular)
        {
            error = "the sign-in host list is not a regular file";
            return SignInFileStatus::Rejected;
        }
        const std::uintmax_t size = std::filesystem::file_size(path, code);
        if (code || size > SignInHostLimits::kMaximumFileBytes)
        {
            error = "the sign-in host list is unreadable or too large";
            return SignInFileStatus::Rejected;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            error = "the sign-in host list could not be opened";
            return SignInFileStatus::Rejected;
        }
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return book.LoadPersistent(text, baseline, error) ? SignInFileStatus::Loaded : SignInFileStatus::Rejected;
    }

    bool SaveSignInHostsFile(const std::filesystem::path& path, const BrowserSignInHostBook& book, std::string& error)
    {
        std::error_code code;
        if (!path.has_filename() || !std::filesystem::is_directory(path.parent_path(), code))
        {
            error = "the sign-in host list directory does not exist";
            return false;
        }
        const std::filesystem::file_status existing = std::filesystem::symlink_status(path, code);
        if (!code && existing.type() != std::filesystem::file_type::not_found
            && existing.type() != std::filesystem::file_type::regular)
        {
            error = "the sign-in host list is not a regular file";
            return false;
        }

        std::filesystem::path temporary = path;
        temporary += ".tmp";
        std::filesystem::remove(temporary, code);
        {
            std::ofstream create(temporary, std::ios::binary | std::ios::trunc);
            if (!create)
            {
                error = "the sign-in host list could not be created";
                return false;
            }
        }
        std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, code);
        if (code)
        {
            std::filesystem::remove(temporary, code);
            error = "the sign-in host list could not be made owner-only";
            return false;
        }
        const std::string text = book.EncodePersistent();
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            output.flush();
            if (!output)
            {
                output.close();
                std::filesystem::remove(temporary, code);
                error = "the sign-in host list could not be written";
                return false;
            }
        }
        std::filesystem::rename(temporary, path, code);
        if (code)
        {
            std::filesystem::remove(temporary, code);
            error = "the sign-in host list could not be replaced";
            return false;
        }
        error.clear();
        return true;
    }

    // ---- consent ------------------------------------------------------------------------

    bool BrowserSignInConsent::Offer(std::string_view host, u64 nowMilliseconds, const BrowserNavigationPolicy& effective)
    {
        std::string error;
        if (!BrowserNavigationPolicy::IsValidProviderHost(host, error))
            return false;
        if (effective.IsHostAllowed(host))
        {
            if (m_Pending == host)
                m_Pending.clear();
            return false;
        }
        m_Dismissed.erase(std::remove_if(m_Dismissed.begin(), m_Dismissed.end(),
                              [nowMilliseconds](const Dismissed& entry) { return entry.Until <= nowMilliseconds; }),
            m_Dismissed.end());
        if (std::any_of(m_Dismissed.begin(), m_Dismissed.end(), [host](const Dismissed& entry) { return entry.Host == host; }))
            return false;
        m_Pending = std::string(host);
        return true;
    }

    SignInConsentOutcome BrowserSignInConsent::Resolve(SignInConsentChoice choice, u64 nowMilliseconds)
    {
        SignInConsentOutcome outcome;
        if (m_Pending.empty())
            return outcome;
        outcome.Resolved = true;
        outcome.Host = std::move(m_Pending);
        m_Pending.clear();
        if (choice == SignInConsentChoice::Dismiss)
        {
            m_Dismissed.erase(std::remove_if(m_Dismissed.begin(), m_Dismissed.end(),
                                  [&](const Dismissed& entry) { return entry.Host == outcome.Host; }),
                m_Dismissed.end());
            if (m_Dismissed.size() >= SignInHostLimits::kMaximumDismissedHosts)
                m_Dismissed.erase(m_Dismissed.begin());
            m_Dismissed.push_back({ outcome.Host, nowMilliseconds + SignInHostLimits::kDismissSuppressMilliseconds });
            return outcome;
        }
        outcome.Grant = true;
        outcome.Persistent = choice == SignInConsentChoice::AllowAlways;
        outcome.Retry = true;
        return outcome;
    }

    void BrowserSignInConsent::Withdraw(const BrowserNavigationPolicy& effective)
    {
        if (!m_Pending.empty() && effective.IsHostAllowed(m_Pending))
            m_Pending.clear();
    }

    void BrowserSignInConsent::Reset()
    {
        m_Pending.clear();
        m_Dismissed.clear();
    }
}
