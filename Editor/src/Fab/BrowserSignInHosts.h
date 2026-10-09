#pragma once

#include "BrowserNavigationPolicy.h"
#include "Engine/Core/Base.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The pure half of the sign-in consent flow: the user-granted host book, its
// versioned on-disk codec, and the banner state machine. No CEF, ImGui, GLFW,
// or global state. The panel core owns one book and one consent object; the
// browser engine only ever sees the exact host strings (hosts, never URLs,
// paths, query text, cookies, or form data).
namespace Fab
{
    namespace SignInHostLimits
    {
        constexpr Engine::u32 kSchema = 1;
        // The persisted list ("Allow always") and the session list ("Allow once")
        // are capped independently.
        constexpr size_t kMaximumPersistentHosts = 64;
        constexpr size_t kMaximumSessionHosts = 64;
        constexpr size_t kMaximumFileBytes = 32 * 1024;
        constexpr size_t kMaximumDismissedHosts = 16;
        // A dismissed host is not offered again for this long, so a page that
        // redirects to it in a loop cannot pin the banner on screen.
        constexpr Engine::u64 kDismissSuppressMilliseconds = 10000;
    }

    struct BrowserGrantedHost
    {
        std::string Host;
        bool Persistent = false;
    };

    enum class SignInGrantResult
    {
        Granted,
        // The host is in the fixed list (built-in, default, or configured), or is
        // already granted at least as durably.
        AlreadyAllowed,
        Invalid,
        LimitReached
    };

    // The user's grants in insertion order. "Persistent" entries are the ones
    // that EncodePersistent writes. Every host passes
    // BrowserNavigationPolicy::IsValidProviderHost, the validation
    // AddProviderHost applies.
    class BrowserSignInHostBook
    {
    public:
        // `baseline` is the fixed policy (built-in, default, configured hosts).
        // Granting the same host again as persistent upgrades a session entry.
        SignInGrantResult Grant(std::string_view host, bool persistent, const BrowserNavigationPolicy& baseline, std::string& error);

        // Removes `host` if present; reports whether it was persistent.
        bool Revoke(std::string_view host, bool* wasPersistent = nullptr);
        // Removes every entry; reports whether a persistent one was among them.
        bool Clear();

        std::span<const BrowserGrantedHost> Entries() const { return m_Entries; }
        std::vector<std::string> HostList() const;
        size_t PersistentCount() const;
        size_t SessionCount() const { return m_Entries.size() - PersistentCount(); }

        // {"schema":1,"hosts":["a.example.com", ...]} plus a newline: only the
        // persistent hosts, nothing else.
        std::string EncodePersistent() const;

        // Replaces every entry with the persistent hosts in `text`, or fails and
        // leaves the book unchanged: a malformed document, a wrong schema, an
        // unknown key, an escape sequence, a host that fails validation, a
        // duplicate, or more than kMaximumPersistentHosts hosts rejects the whole
        // file. A host that is part of `baseline` now is dropped silently.
        bool LoadPersistent(std::string_view text, const BrowserNavigationPolicy& baseline, std::string& error);

    private:
        std::vector<BrowserGrantedHost> m_Entries;
    };

    enum class SignInFileStatus
    {
        Loaded,
        Missing,
        Rejected
    };

    // Reads the file at `path` into `book` through LoadPersistent. A missing file
    // is not an error. The reason in `error` never contains the path.
    SignInFileStatus LoadSignInHostsFile(
        const std::filesystem::path& path, BrowserSignInHostBook& book, const BrowserNavigationPolicy& baseline, std::string& error);

    // Writes the persistent hosts to `path` through a temporary file in the same
    // directory (owner read/write only) and a rename. The directory must exist.
    // A symbolic link in place of the file is refused. On failure the previous
    // file is untouched.
    bool SaveSignInHostsFile(const std::filesystem::path& path, const BrowserSignInHostBook& book, std::string& error);

    enum class SignInConsentChoice
    {
        AllowOnce,
        AllowAlways,
        Dismiss
    };

    struct SignInConsentOutcome
    {
        bool Resolved = false;
        std::string Host;
        bool Grant = false;
        bool Persistent = false;
        bool Retry = false;
    };

    // The banner: at most one host is pending; a newer offer replaces it.
    class BrowserSignInConsent
    {
    public:
        // True when the banner now asks about `host`. An invalid host, a host
        // that is allowed already (this also withdraws a pending banner for it),
        // and a recently dismissed host are not offered.
        bool Offer(std::string_view host, Engine::u64 nowMilliseconds, const BrowserNavigationPolicy& effective);

        bool HasPending() const { return !m_Pending.empty(); }
        const std::string& PendingHost() const { return m_Pending; }

        // Dismiss records the host as suppressed; the allow choices ask the owner
        // to grant (session or persistent) and to repeat the denied navigation.
        SignInConsentOutcome Resolve(SignInConsentChoice choice, Engine::u64 nowMilliseconds);

        // Withdraws the banner when its host has become allowed by other means.
        void Withdraw(const BrowserNavigationPolicy& effective);
        void Reset();

    private:
        struct Dismissed
        {
            std::string Host;
            Engine::u64 Until = 0;
        };

        std::string m_Pending;
        std::vector<Dismissed> m_Dismissed;
    };
}
