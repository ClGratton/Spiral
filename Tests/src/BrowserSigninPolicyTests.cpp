#include "BrowserSigninPolicyTests.h"

#include "BrowserNavigationPolicy.h"
#include "BrowserPanelCore.h"
#include "BrowserSignInHosts.h"
#include "BrowserSurface.h"
#include "TestSupport/FakeBrowserSurface.h"
#include "TestSupport/GeneratedTest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__linux__)
    #include <sys/stat.h>
#endif

namespace
{
    using namespace Fab;
    using Engine::u32;
    using Engine::u64;

    // Failure hypotheses, oracles, and non-claims for the whole file:
    // - Units: the default sign-in host table and its policy additions (popup
    //   target decision, consent host, granted-host set), the accept-language
    //   derivation, the persisted host list codec and file, the host book, the
    //   consent state machine, and their wiring in BrowserPanelCore against
    //   FakeBrowserSurface.
    // - Oracles: a hand-written copy of the evidence table (the research survey
    //   of 2026-10-09), a naive host validity checker that shares no code with
    //   the production validator, a hand-built canonical encoding, and
    //   independent re-statements of the book and banner rules as models.
    // - Generated properties are deterministic (fixed seed), replayable through
    //   SPIRAL_FAB_SIGNIN_SEED / SPIRAL_FAB_SIGNIN_REPLAY, and print the seed, the
    //   original and minimised traces, and a counterexample JSON in the system
    //   temp directory. Tier: fast, in-process, no browser engine, no GPU, no
    //   network; file tests use unique directories under the system temp path.
    // - Not claimed: any real provider's sign-in behaviour or redirect chain,
    //   CEF behaviour (the adapter is exercised by Scripts/TestBrowserSurface.sh),
    //   ImGui drawing of the banner and the host list, or Windows file modes.

    struct Checker
    {
        const char* Suite;
        bool Ok = true;

        void Expect(bool condition, const std::string& message)
        {
            if (!condition)
            {
                std::cerr << "Fab sign-in policy test failed [" << Suite << "]: " << message << '\n';
                Ok = false;
            }
        }
    };

    std::atomic<u64> g_FixtureCounter { 0 };

    class TempDir
    {
    public:
        explicit TempDir(std::string_view name)
        {
            const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
            m_Path = std::filesystem::temp_directory_path()
                / ("spiral-fab-signin-" + std::string(name) + "-" + std::to_string(stamp) + "-"
                    + std::to_string(g_FixtureCounter.fetch_add(1)));
            std::error_code error;
            std::filesystem::create_directories(m_Path, error);
        }

        ~TempDir()
        {
            std::error_code error;
            std::filesystem::remove_all(m_Path, error);
        }

        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;

        const std::filesystem::path& Path() const { return m_Path; }

    private:
        std::filesystem::path m_Path;
    };

    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }

    void WriteText(const std::filesystem::path& path, std::string_view text)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    bool Contains(std::string_view text, std::string_view needle)
    {
        return text.find(needle) != std::string_view::npos;
    }

    bool RunProperty(std::string_view name, const Spiral::Tests::Property& property, size_t iterations)
    {
        Spiral::Tests::CampaignOptions options;
        options.Iterations = iterations;
        if (const char* seed = std::getenv("SPIRAL_FAB_SIGNIN_SEED"))
            options.Seed = std::strtoull(seed, nullptr, 10);
        Spiral::Tests::ChoiceTrace replay;
        if (const char* trace = std::getenv("SPIRAL_FAB_SIGNIN_REPLAY"); trace && Spiral::Tests::ParseTrace(trace, replay))
            options.Replay = replay;

        Spiral::Tests::Counterexample failure;
        if (Spiral::Tests::RunCampaign(options, property, failure))
            return true;

        const std::string minimized = Spiral::Tests::SerializeTrace(failure.MinimizedTrace);
        const std::string rerun = "SPIRAL_FAB_SIGNIN_SEED=" + std::to_string(failure.Seed) + " SPIRAL_FAB_SIGNIN_REPLAY=\""
            + minimized + "\" EngineTests --test <registered name of " + std::string(name) + ">";
        const std::filesystem::path artifact = std::filesystem::temp_directory_path()
            / ("spiral-fab-signin-counterexample-" + std::string(name) + ".json");
        std::string artifactError;
        const bool written = Spiral::Tests::WriteCounterexample(artifact, name, failure, rerun, artifactError);
        std::cerr << "Fab sign-in property failed [" << name << "]: " << failure.Message << "\n  seed " << failure.Seed
                  << ", iteration " << failure.Iteration << "\n  original trace: " << Spiral::Tests::SerializeTrace(failure.OriginalTrace)
                  << "\n  minimised trace: " << minimized << "\n  rerun: " << rerun
                  << (written ? "\n  counterexample written to " + artifact.string() : "\n  counterexample not written: " + artifactError)
                  << '\n';
        return false;
    }

    // A deliberately naive host validity rule written without reference to the
    // production validator.
    bool ReferenceHostValid(const std::string& host)
    {
        if (host.empty() || host.size() > 253)
            return false;
        std::vector<std::string> labels;
        std::string current;
        for (const char c : host)
        {
            if (c == '.')
            {
                labels.push_back(current);
                current.clear();
            }
            else
            {
                current.push_back(c);
            }
        }
        labels.push_back(current);
        if (labels.size() < 2)
            return false;
        for (const std::string& label : labels)
        {
            if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' || label.rfind("xn--", 0) == 0)
                return false;
            for (const char c : label)
            {
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                    return false;
            }
        }
        const std::string& last = labels.back();
        return !std::all_of(last.begin(), last.end(), [](char c) { return c >= '0' && c <= '9'; });
    }

    // ---- 1. the default host table -------------------------------------------------------------

    bool CheckDefaultHostTable()
    {
        Checker check { "default-table" };

        struct Expected
        {
            const char* Provider;
            const char* Host;
            SignInHostEvidence Evidence;
        };
        // The 2026-10-09 survey: documented-role hosts, plus the one documented
        // host whose role in Epic's flow is not documented.
        const Expected expected[] = {
            { "Google", "accounts.google.com", SignInHostEvidence::DocumentedRole },
            { "Apple", "appleid.apple.com", SignInHostEvidence::DocumentedRole },
            { "Facebook", "www.facebook.com", SignInHostEvidence::DocumentedRole },
            { "Steam", "steamcommunity.com", SignInHostEvidence::DocumentedRole },
            { "Xbox", "login.microsoftonline.com", SignInHostEvidence::DocumentedRole },
            { "Nintendo", "accounts.nintendo.com", SignInHostEvidence::DocumentedHostOnly },
        };
        const std::span<const BrowserSignInProvider> table = BrowserNavigationPolicy::DefaultSignInProviders();
        check.Expect(table.size() == std::size(expected), "the table lists exactly the documented providers");
        std::set<std::string_view> seen;
        for (size_t index = 0; index < std::min(table.size(), std::size(expected)); ++index)
        {
            check.Expect(table[index].Provider == expected[index].Provider && table[index].Host == expected[index].Host
                    && table[index].Evidence == expected[index].Evidence,
                std::string("table row ") + expected[index].Host);
            check.Expect(!table[index].Source.empty() && table[index].Source.rfind("https://", 0) == 0,
                std::string("row cites an https source: ") + expected[index].Host);
            check.Expect(ReferenceHostValid(std::string(table[index].Host)), std::string("row host is a valid provider host: ") + expected[index].Host);
            check.Expect(seen.insert(table[index].Host).second, "no duplicate row");
            check.Expect(table[index].Host != BrowserNavigationPolicy::kFabHost && table[index].Host != BrowserNavigationPolicy::kEpicHost,
                "the built-in hosts are not repeated as providers");
        }

        BrowserNavigationPolicy policy;
        check.Expect(policy.ProviderHosts().empty() && policy.AllowedHosts().size() == 2, "a new policy has only the built-in hosts");
        check.Expect(policy.AddDefaultProviderHosts() == std::size(expected), "all defaults are added");
        check.Expect(policy.AddDefaultProviderHosts() == 0, "adding the defaults again changes nothing");
        check.Expect(policy.ProviderHosts().size() == std::size(expected) && policy.AllowedHosts().size() == 2 + std::size(expected),
            "defaults are provider hosts after the two built-in hosts");

        const auto verdict = [&](std::string_view url) { return policy.Evaluate(url, BrowserNavigationKind::TopLevel); };
        for (const Expected& row : expected)
        {
            const std::string host = row.Host;
            check.Expect(verdict("https://" + host + "/") == BrowserNavigationVerdict::Allow, "https://" + host + "/ is allowed");
            check.Expect(verdict("https://" + host + "/oauth2/auth?client_id=1&state=%2F#x") == BrowserNavigationVerdict::Allow,
                host + " with path, query and fragment");
            std::string upper = host;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
            check.Expect(verdict("https://" + upper + "/") == BrowserNavigationVerdict::Allow, "host comparison is case-insensitive: " + upper);
            check.Expect(verdict("http://" + host + "/") == BrowserNavigationVerdict::DenyScheme, "http is refused: " + host);
            check.Expect(verdict("https://" + host + ":8443/") == BrowserNavigationVerdict::DenyMalformedUrl, "a port is refused: " + host);
            check.Expect(verdict("https://user@" + host + "/") == BrowserNavigationVerdict::DenyMalformedUrl, "userinfo is refused: " + host);
            check.Expect(verdict("https://" + host + "@evil.example/") == BrowserNavigationVerdict::DenyMalformedUrl, "userinfo trick is refused: " + host);
            check.Expect(verdict("https://" + host + ".evil.example/") == BrowserNavigationVerdict::DenyHost, "suffix lookalike: " + host);
            check.Expect(verdict("https://evil-" + host + "/") == BrowserNavigationVerdict::DenyHost, "prefix lookalike: " + host);
            check.Expect(verdict("https://sub." + host + "/") == BrowserNavigationVerdict::DenyHost, "no wildcard: " + host);
            check.Expect(verdict("https://" + host + "./") == BrowserNavigationVerdict::DenyHost, "trailing dot: " + host);
            check.Expect(verdict("https://" + host + "%2eevil.example/") == BrowserNavigationVerdict::DenyMalformedUrl, "percent-dot trick: " + host);
        }
        check.Expect(verdict("https://acc\xD0\xBEunts.google.com/") == BrowserNavigationVerdict::DenyMalformedUrl, "a non-ASCII lookalike of Google is refused");
        check.Expect(verdict("https://xn--ccounts-8fe.google.com/") == BrowserNavigationVerdict::DenyHost, "an IDN lookalike label is refused");
        check.Expect(verdict("https://142.250.0.1/") == BrowserNavigationVerdict::DenyHost && verdict("https://[::1]/") == BrowserNavigationVerdict::DenyMalformedUrl
                && verdict("https://2130706433/") == BrowserNavigationVerdict::DenyHost,
            "IP literals stay refused");

        // Hosts that were only guessed (survey 2026-10-09) are not defaults: they
        // can reach the panel only through the consent flow.
        for (const char* guessed : { "accounts.epicgames.com", "epicgames.com", "fab.com", "ca.account.sony.com", "my.account.sony.com",
                 "account.lego.com", "registerdisney.go.com", "login.live.com", "account.live.com", "m.facebook.com", "accounts.youtube.com",
                 "store.steampowered.com", "login.steampowered.com", "id.epicgames.com" })
        {
            check.Expect(verdict(std::string("https://") + guessed + "/") == BrowserNavigationVerdict::DenyHost,
                std::string("a guessed host is not a default: ") + guessed);
            check.Expect(!policy.ConsentHost(std::string("https://") + guessed + "/").empty(),
                std::string("a guessed host can be offered through consent: ") + guessed);
        }

        // Provider hosts supplied by hand still go through the same validation and
        // may not repeat a default.
        std::string error;
        check.Expect(!policy.AddProviderHost("accounts.google.com", error) && error == "provider host is already allowed",
            "a default cannot be added a second time");
        check.Expect(policy.AddProviderHost("accounts.example.org", error), "other validated hosts are still accepted");
        for (const char* invalid : { "*.google.com", "google", "127.0.0.1", "xn--a.com", "ACCOUNTS.google.com", "accounts.google.com." })
            check.Expect(!policy.AddProviderHost(invalid, error) && !error.empty(), std::string("rejected: ") + invalid);
        return check.Ok;
    }

    // ---- 2. consent host, popup decision, granted hosts ---------------------------------------

    bool CheckConsentAndPopupPolicy()
    {
        Checker check { "consent-popup" };
        BrowserNavigationPolicy policy;
        policy.AddDefaultProviderHosts();

        const std::pair<const char*, const char*> consentable[] = {
            { "https://accounts.example.org/signin?token=secret#frag", "accounts.example.org" },
            { "https://ACCOUNTS.Example.ORG/", "accounts.example.org" },
            { "https://login.partner.example.net/a/b", "login.partner.example.net" },
            { "https://a.b/", "a.b" },
        };
        for (const auto& [url, host] : consentable)
            check.Expect(policy.ConsentHost(url) == host, std::string("consent host of ") + host);

        for (const char* url : { "https://www.fab.com/x", "https://accounts.google.com/", "http://accounts.example.org/",
                 "https://user@accounts.example.org/", "https://accounts.example.org:8443/", "https://127.0.0.1/", "https://[::1]/",
                 "https://xn--e1afmkfd.xn--p1ai/", "https://localhost/", "https://accounts.example.org./", "https://exa_mple.org/",
                 "javascript:alert(1)", "about:blank", "", "https://", "https://2130706433/", "https://-bad.example.org/",
                 "HTTPS://accounts.example.org/", "https://accounts.example.org\\.evil/" })
        {
            check.Expect(policy.ConsentHost(url).empty(), std::string("no consent for '") + url + "'");
        }
        check.Expect(policy.ConsentHost("https://accounts.example.org/" + std::string(2048, 'a')).empty(), "no consent for an over-long URL");

        // The popup decision: never a window; an allowed target is a main-frame load.
        for (const char* url : { "https://www.fab.com/new-window", "https://accounts.google.com/o/oauth2/v2/auth?x=1",
                 "https://WWW.EPICGAMES.COM/id/login" })
        {
            check.Expect(policy.Evaluate(url, BrowserNavigationKind::Popup) == BrowserNavigationVerdict::DenyPopup,
                std::string("Evaluate still never creates a popup window: ") + url);
            check.Expect(policy.EvaluatePopupTarget(url) == BrowserNavigationVerdict::Allow, std::string("allowed popup target: ") + url);
        }
        const std::pair<const char*, BrowserNavigationVerdict> popupDenials[] = {
            { "https://evil.example/", BrowserNavigationVerdict::DenyHost },
            { "http://www.fab.com/", BrowserNavigationVerdict::DenyScheme },
            { "about:blank", BrowserNavigationVerdict::DenyScheme },
            { "", BrowserNavigationVerdict::DenyScheme },
            { "javascript:alert(1)", BrowserNavigationVerdict::DenyScheme },
            { "https://www.fab.com:444/", BrowserNavigationVerdict::DenyMalformedUrl },
            { "https://user@www.fab.com/", BrowserNavigationVerdict::DenyMalformedUrl },
            { "https://www.fab.com.evil.example/", BrowserNavigationVerdict::DenyHost },
        };
        for (const auto& [url, expectedVerdict] : popupDenials)
            check.Expect(policy.EvaluatePopupTarget(url) == expectedVerdict, std::string("popup target '") + url + "'");

        // The granted set is evaluated like provider hosts and replaced wholesale.
        std::vector<std::string> granted = { "accounts.example.org", "accounts.example.org", "ACCOUNTS.example.net", "127.0.0.1", "www.fab.com",
            "accounts.google.com", "login.example.com" };
        check.Expect(policy.SetGrantedHosts(granted) == 2, "invalid, duplicate, and already allowed entries are skipped");
        check.Expect(policy.GrantedHosts().size() == 2 && policy.GrantedHosts()[0] == "accounts.example.org" && policy.GrantedHosts()[1] == "login.example.com",
            "the granted set keeps the valid new hosts in order");
        check.Expect(policy.IsTopLevelAllowed("https://accounts.example.org/x") && policy.IsHostAllowed("login.example.com")
                && !policy.IsHostAllowed("accounts.example.net"),
            "granted hosts are allowed, others are not");
        check.Expect(policy.Evaluate("https://accounts.example.org:8443/", BrowserNavigationKind::TopLevel) == BrowserNavigationVerdict::DenyMalformedUrl
                && policy.Evaluate("https://evil.accounts.example.org/", BrowserNavigationKind::TopLevel) == BrowserNavigationVerdict::DenyHost,
            "a granted host is exact: no port, no subdomain");
        check.Expect(policy.EvaluatePopupTarget("https://login.example.com/") == BrowserNavigationVerdict::Allow, "a granted host also opens popups in the panel");
        check.Expect(policy.ConsentHost("https://accounts.example.org/").empty(), "a granted host is not offered again");
        std::string error;
        check.Expect(!policy.AddProviderHost("accounts.example.org", error), "a granted host cannot be added as a provider host");
        check.Expect(policy.AllowedHosts().size() == 2 + BrowserNavigationPolicy::DefaultSignInProviders().size(), "granting does not touch the fixed list");

        check.Expect(policy.SetGrantedHosts({}) == 0 && policy.GrantedHosts().empty() && !policy.IsTopLevelAllowed("https://accounts.example.org/x"),
            "an empty replacement revokes every grant");

        std::vector<std::string> many;
        for (size_t index = 0; index < BrowserNavigationPolicy::kMaximumGrantedHosts + 10; ++index)
            many.push_back("h" + std::to_string(index) + ".example.org");
        check.Expect(policy.SetGrantedHosts(many) == BrowserNavigationPolicy::kMaximumGrantedHosts, "the granted set is capped");
        check.Expect(policy.IsHostAllowed("h63.example.org") && !policy.IsHostAllowed("h64.example.org"), "the cap keeps the first hosts");
        return check.Ok;
    }

    // ---- 3. accept-language ---------------------------------------------------------------------

    bool CheckAcceptLanguageList()
    {
        Checker check { "accept-language" };
        struct Row
        {
            const char* Language;
            const char* All;
            const char* Messages;
            const char* Lang;
            const char* Expected;
        };
        const Row rows[] = {
            { "", "", "", "en_US.UTF-8", "en-US,en" },
            { "", "", "", "it_IT.UTF-8", "it-IT,it" },
            { "it_IT:en_US", "", "", "en_US.UTF-8", "it-IT,it,en-US,en" },
            { "", "de_DE@euro", "fr_FR", "en_US", "de-DE,de" },
            { "", "", "fr_CA.UTF-8", "en_US", "fr-CA,fr" },
            { "", "", "", "", "en-US,en" },
            { "", "", "", "C", "en-US,en" },
            { "", "", "", "C.UTF-8", "en-US,en" },
            { "", "POSIX", "", "it_IT", "en-US,en" },
            { "en", "", "", "it_IT", "en" },
            { "pt_BR:pt:en", "", "", "", "pt-BR,pt,en" },
            { "ZH_cn", "", "", "", "zh-CN,zh" },
            { "es_419", "", "", "", "es-419,es" },
            { "en_US:en_GB:en", "", "", "", "en-US,en,en-GB" },
            { "bogus_tag_here:it_IT", "", "", "", "it-IT,it" },
            { "e:en_USA:it_IT.UTF-8@euro", "", "", "", "it-IT,it" },
            { ":::", "", "", "en_US", "en-US,en" },
            { "xx_YY,evil", "", "", "", "en-US,en" },
            { "it_IT\n:en", "", "", "", "en" },
            { "a_b:c_d:ab_12", "", "", "", "en-US,en" },
        };
        for (const Row& row : rows)
        {
            const std::string actual = BuildAcceptLanguageList(row.Language, row.All, row.Messages, row.Lang);
            check.Expect(actual == row.Expected,
                std::string("LANGUAGE='") + row.Language + "' LC_ALL='" + row.All + "' LC_MESSAGES='" + row.Messages + "' LANG='"
                    + row.Lang + "' expected '" + row.Expected + "' got '" + actual + "'");
        }
        // At most eight tags, never an empty result.
        std::string many;
        for (char first = 'a'; first <= 'l'; ++first)
            many += std::string(1, first) + "x_" + "XX:";
        const std::string capped = BuildAcceptLanguageList(many, "", "", "");
        check.Expect(std::count(capped.begin(), capped.end(), ',') + 1 <= 8 && !capped.empty(), "the list is capped at eight tags: " + capped);
        return check.Ok;
    }

    // ---- 4. codec and file -------------------------------------------------------------------------

    BrowserNavigationPolicy DefaultPolicy()
    {
        BrowserNavigationPolicy policy;
        policy.AddDefaultProviderHosts();
        return policy;
    }

    std::vector<BrowserGrantedHost> Snapshot(const BrowserSignInHostBook& book)
    {
        return std::vector<BrowserGrantedHost>(book.Entries().begin(), book.Entries().end());
    }

    bool SameEntries(const std::vector<BrowserGrantedHost>& a, const std::vector<BrowserGrantedHost>& b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const BrowserGrantedHost& x, const BrowserGrantedHost& y)
            { return x.Host == y.Host && x.Persistent == y.Persistent; });
    }

    bool CheckCodecAndFile()
    {
        Checker check { "codec-file" };
        const BrowserNavigationPolicy baseline = DefaultPolicy();
        std::string error;

        // Golden text.
        BrowserSignInHostBook book;
        check.Expect(book.EncodePersistent() == "{\"schema\":1,\"hosts\":[]}\n", "an empty book encodes to the empty document");
        check.Expect(book.Grant("accounts.example.org", true, baseline, error) == SignInGrantResult::Granted, "grant persistent");
        check.Expect(book.Grant("session.example.org", false, baseline, error) == SignInGrantResult::Granted, "grant session");
        check.Expect(book.Grant("login.example.net", true, baseline, error) == SignInGrantResult::Granted, "grant second persistent");
        check.Expect(book.EncodePersistent() == "{\"schema\":1,\"hosts\":[\"accounts.example.org\",\"login.example.net\"]}\n",
            "only persistent hosts are written, in insertion order, nothing else");

        // Grant rules.
        check.Expect(book.Grant("accounts.google.com", true, baseline, error) == SignInGrantResult::AlreadyAllowed, "a default is not granted");
        check.Expect(book.Grant("www.fab.com", false, baseline, error) == SignInGrantResult::AlreadyAllowed, "a built-in host is not granted");
        check.Expect(book.Grant("accounts.example.org", false, baseline, error) == SignInGrantResult::AlreadyAllowed, "a session grant does not weaken a persistent one");
        check.Expect(book.Grant("session.example.org", true, baseline, error) == SignInGrantResult::Granted && book.PersistentCount() == 3,
            "granting a session host as persistent upgrades it in place");
        for (const char* invalid : { "", "127.0.0.1", "*.example.org", "UPPER.example.org", "xn--a.example.org", "example", "a.example.org/path",
                 "a.example.org:443", "user@a.example.org" })
        {
            check.Expect(book.Grant(invalid, true, baseline, error) == SignInGrantResult::Invalid && !error.empty(), std::string("invalid: ") + invalid);
        }
        bool wasPersistent = false;
        check.Expect(book.Revoke("session.example.org", &wasPersistent) && wasPersistent && !book.Revoke("session.example.org"), "revoke reports what it removed");
        check.Expect(book.Clear() && book.Entries().empty() && !book.Clear(), "clear reports whether a persistent entry existed");

        // Decode accepts the canonical text and the spacing variants.
        const std::string canonical = "{\"schema\":1,\"hosts\":[\"a.example.org\",\"b.example.org\"]}\n";
        for (const std::string& text : std::vector<std::string> {
                 canonical, " { \"schema\" : 1 , \"hosts\" : [ \"a.example.org\" , \"b.example.org\" ] } \r\n",
                 "{\"hosts\":[\"a.example.org\",\"b.example.org\"],\"schema\":1}", "{\"schema\":1,\"hosts\":[\"a.example.org\",\"b.example.org\"]}" })
        {
            BrowserSignInHostBook loaded;
            check.Expect(loaded.LoadPersistent(text, baseline, error) && loaded.Entries().size() == 2 && loaded.Entries()[0].Host == "a.example.org"
                    && loaded.Entries()[1].Host == "b.example.org" && loaded.Entries()[0].Persistent && loaded.Entries()[1].Persistent,
                "accepted: " + text);
        }
        {
            BrowserSignInHostBook loaded;
            check.Expect(loaded.LoadPersistent("{\"schema\":1,\"hosts\":[]}", baseline, error) && loaded.Entries().empty(), "an empty list loads");
            check.Expect(loaded.LoadPersistent("{\"schema\":1,\"hosts\":[\"accounts.google.com\",\"c.example.org\"]}", baseline, error)
                    && loaded.Entries().size() == 1 && loaded.Entries()[0].Host == "c.example.org",
                "a host that is a default now is dropped silently");
        }

        // Every malformed document is rejected as a whole, with the book untouched.
        BrowserSignInHostBook keep;
        keep.Grant("keep.example.org", true, baseline, error);
        keep.Grant("session-keep.example.org", false, baseline, error);
        const std::vector<BrowserGrantedHost> before = Snapshot(keep);
        std::string many = "{\"schema\":1,\"hosts\":[";
        for (size_t index = 0; index <= SignInHostLimits::kMaximumPersistentHosts; ++index)
            many += std::string(index == 0 ? "" : ",") + "\"h" + std::to_string(index) + ".example.org\"";
        many += "]}";
        const std::vector<std::string> hostile = {
            "", "{", "}", "[]", "null", "{}", "{\"schema\":1}", "{\"hosts\":[]}", "{\"schema\":2,\"hosts\":[]}", "{\"schema\":0,\"hosts\":[]}",
            "{\"schema\":01,\"hosts\":[]}", "{\"schema\":1.0,\"hosts\":[]}", "{\"schema\":\"1\",\"hosts\":[]}", "{\"schema\":1,\"schema\":1,\"hosts\":[]}",
            "{\"schema\":1,\"hosts\":[],\"hosts\":[]}", "{\"schema\":1,\"hosts\":[],\"extra\":1}", "{\"schema\":1,\"hosts\":[],}",
            "{\"schema\":1,\"hosts\":[\"a.example.org\",]}", "{\"schema\":1,\"hosts\":[,\"a.example.org\"]}", "{\"schema\":1,\"hosts\":[\"a.example.org\"",
            "{\"schema\":1,\"hosts\":[\"a.example.org\"]", "{\"schema\":1,\"hosts\":[\"a.example.org\"]}x", "{\"schema\":1,\"hosts\":[\"a.example.org\"]}{}",
            "{\"schema\":1,\"hosts\":[\"a.example.org\",\"a.example.org\"]}", "{\"schema\":1,\"hosts\":[\"A.example.org\"]}",
            "{\"schema\":1,\"hosts\":[\"127.0.0.1\"]}", "{\"schema\":1,\"hosts\":[\"*.example.org\"]}", "{\"schema\":1,\"hosts\":[\"xn--a.example.org\"]}",
            "{\"schema\":1,\"hosts\":[\"example\"]}", "{\"schema\":1,\"hosts\":[\"a.example.org:443\"]}", "{\"schema\":1,\"hosts\":[\"a.example.org/x\"]}",
            "{\"schema\":1,\"hosts\":[\"a\\u002eexample.org\"]}", "{\"schema\":1,\"hosts\":[\"a.example.org\\n\"]}", "{\"schema\":1,\"hosts\":[1]}",
            "{\"schema\":1,\"hosts\":[null]}", "{\"schema\":1,\"hosts\":\"a.example.org\"}", "{\"schema\":1,\"hosts\":{}}", "{'schema':1,'hosts':[]}",
            "{\"schema\":1,\"hosts\":[\"a.example.org\"]} // c", "\xEF\xBB\xBF{\"schema\":1,\"hosts\":[]}", std::string("{\"schema\":1,\"hosts\":[\"a.example.org") + '\0' + "\"]}",
            "{\"schema\":1,\"hosts\":[\"https://a.example.org/x\"]}", many, std::string(SignInHostLimits::kMaximumFileBytes + 1, ' '),
            "{\"schema\":999999999999,\"hosts\":[]}",
        };
        for (const std::string& text : hostile)
        {
            std::string localError;
            check.Expect(!keep.LoadPersistent(text, baseline, localError) && !localError.empty(),
                "rejected: " + text.substr(0, 60));
            check.Expect(SameEntries(Snapshot(keep), before), "a rejected document leaves the book unchanged: " + text.substr(0, 60));
            check.Expect(!Contains(localError, "example") && !Contains(localError, "/"), "the reason never echoes the content");
        }

        // The cap: exactly 64 persistent hosts load; the 65th grant is refused.
        {
            std::string full = "{\"schema\":1,\"hosts\":[";
            for (size_t index = 0; index < SignInHostLimits::kMaximumPersistentHosts; ++index)
                full += std::string(index == 0 ? "" : ",") + "\"h" + std::to_string(index) + ".example.org\"";
            full += "]}";
            BrowserSignInHostBook loaded;
            check.Expect(loaded.LoadPersistent(full, baseline, error) && loaded.PersistentCount() == SignInHostLimits::kMaximumPersistentHosts,
                "64 hosts load");
            check.Expect(loaded.Grant("extra.example.org", true, baseline, error) == SignInGrantResult::LimitReached, "the 65th persistent host is refused");
            check.Expect(loaded.Grant("extra.example.org", false, baseline, error) == SignInGrantResult::Granted, "a session grant is counted separately");
            check.Expect(loaded.EncodePersistent().find("extra") == std::string::npos, "session hosts never reach the file text");
            for (size_t index = 1; index < SignInHostLimits::kMaximumSessionHosts; ++index)
                check.Expect(loaded.Grant("s" + std::to_string(index) + ".example.org", false, baseline, error) == SignInGrantResult::Granted,
                    "session grant " + std::to_string(index + 1));
            check.Expect(loaded.SessionCount() == SignInHostLimits::kMaximumSessionHosts
                    && loaded.Grant("one-more.example.org", false, baseline, error) == SignInGrantResult::LimitReached,
                "exactly 64 session hosts fit and the next one is refused");
        }

        // File round trip, permissions, atomicity.
        TempDir fixture("file");
        const std::filesystem::path path = fixture.Path() / "SignInHosts.json";
        BrowserSignInHostBook source;
        source.Grant("one.example.org", true, baseline, error);
        source.Grant("two.example.org", true, baseline, error);
        source.Grant("temp.example.org", false, baseline, error);
        {
            BrowserSignInHostBook empty;
            std::string loadError;
            check.Expect(LoadSignInHostsFile(path, empty, baseline, loadError) == SignInFileStatus::Missing && empty.Entries().empty(), "a missing file is not an error");
        }
        check.Expect(SaveSignInHostsFile(path, source, error), "save: " + error);
        check.Expect(ReadText(path) == "{\"schema\":1,\"hosts\":[\"one.example.org\",\"two.example.org\"]}\n", "the file holds exactly the encoded persistent hosts");
        check.Expect(!std::filesystem::exists(fixture.Path() / "SignInHosts.json.tmp"), "no temporary file remains");
#if defined(__linux__)
        struct stat info {};
        check.Expect(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "the file is owner read/write only");
#endif
        {
            BrowserSignInHostBook loaded;
            std::string loadError;
            check.Expect(LoadSignInHostsFile(path, loaded, baseline, loadError) == SignInFileStatus::Loaded && loaded.Entries().size() == 2
                    && loaded.Entries()[0].Host == "one.example.org",
                "load returns what was saved");
        }
        // A stale temporary (even a symlink) is replaced, never followed.
        const std::filesystem::path victim = fixture.Path() / "victim.txt";
        WriteText(victim, "precious");
        std::error_code linkError;
        std::filesystem::create_symlink(victim, fixture.Path() / "SignInHosts.json.tmp", linkError);
        check.Expect(SaveSignInHostsFile(path, source, error), "save with a stale temporary symlink: " + error);
        check.Expect(ReadText(victim) == "precious", "a symlinked temporary is not followed");

        // Refusals leave the previous file alone.
        const std::string previous = ReadText(path);
        {
            BrowserSignInHostBook other;
            other.Grant("other.example.org", true, baseline, error);
            check.Expect(!SaveSignInHostsFile(fixture.Path() / "missing-directory" / "SignInHosts.json", other, error) && !error.empty(), "a missing directory is refused");
            check.Expect(!Contains(error, "missing-directory") && !Contains(error, fixture.Path().string()), "the reason never echoes the path");
            const std::filesystem::path linked = fixture.Path() / "linked.json";
            std::filesystem::create_symlink(victim, linked, linkError);
            check.Expect(!SaveSignInHostsFile(linked, other, error), "a symbolic link in place of the file is refused");
            check.Expect(ReadText(victim) == "precious", "the link target is untouched");
            const std::filesystem::path directoryPath = fixture.Path() / "is-a-directory";
            std::filesystem::create_directory(directoryPath);
            check.Expect(!SaveSignInHostsFile(directoryPath, other, error), "a directory in place of the file is refused");
            BrowserSignInHostBook loadedFromLink;
            const std::filesystem::path validTarget = fixture.Path() / "valid-target.json";
            WriteText(validTarget, "{\"schema\":1,\"hosts\":[\"linked.example.org\"]}\n");
            const std::filesystem::path validLink = fixture.Path() / "valid-link.json";
            std::filesystem::create_symlink(validTarget, validLink, linkError);
            check.Expect(LoadSignInHostsFile(validLink, loadedFromLink, baseline, error) == SignInFileStatus::Rejected && loadedFromLink.Entries().empty(),
                "a symbolic link is not read even when its target is a valid list");
        }
        check.Expect(ReadText(path) == previous, "the saved file is unchanged by every refusal");

        // Corrupt and oversized files are rejected with the book unchanged.
        WriteText(path, "{\"schema\":1,\"hosts\":[\"a.example.org\",");
        BrowserSignInHostBook target;
        target.Grant("stay.example.org", true, baseline, error);
        const std::vector<BrowserGrantedHost> targetBefore = Snapshot(target);
        check.Expect(LoadSignInHostsFile(path, target, baseline, error) == SignInFileStatus::Rejected && SameEntries(Snapshot(target), targetBefore),
            "a truncated file is rejected transactionally");
        check.Expect(!Contains(error, fixture.Path().string()), "the load reason never echoes the path");
        WriteText(path, std::string(SignInHostLimits::kMaximumFileBytes + 1, 'x'));
        check.Expect(LoadSignInHostsFile(path, target, baseline, error) == SignInFileStatus::Rejected && SameEntries(Snapshot(target), targetBefore),
            "an oversized file is rejected without being parsed");
        return check.Ok;
    }

    // ---- 5. generated: codec ----------------------------------------------------------------------

    std::string HostFromChoice(Spiral::Tests::ChoiceStream& stream, bool allowInvalid)
    {
        static const std::vector<std::string> invalid = { "", "x", "A.example.org", "127.0.0.1", "xn--a.example.org", "a..b.org", "-a.example.org",
            "a-.example.org", "a.example.org.", "*.example.org", "a.b:80", "a b.example.org", "a.example.123" };
        if (allowInvalid && stream.Next() % 5 == 0)
            return invalid[stream.Next() % invalid.size()];
        static const std::vector<std::string> fixed = { "accounts.google.com", "www.fab.com", "appleid.apple.com" };
        const u64 choice = stream.Next() % 24;
        if (choice == 0)
            return fixed[stream.Next() % fixed.size()];
        return "h" + std::to_string(choice) + (stream.Next() % 2 == 0 ? ".example.org" : ".example.net");
    }

    bool CodecProperty(Spiral::Tests::ChoiceStream& stream, std::string& message)
    {
        const BrowserNavigationPolicy baseline = DefaultPolicy();
        std::string error;
        BrowserSignInHostBook book;
        std::vector<std::string> persistent;
        const size_t operations = stream.NextSize(0, 40);
        for (size_t index = 0; index < operations; ++index)
        {
            const std::string host = HostFromChoice(stream, false);
            if (book.Grant(host, true, baseline, error) == SignInGrantResult::Granted)
                persistent.push_back(host);
        }

        std::string expected = "{\"schema\":1,\"hosts\":[";
        for (size_t index = 0; index < persistent.size(); ++index)
            expected += std::string(index == 0 ? "" : ",") + "\"" + persistent[index] + "\"";
        expected += "]}\n";
        const std::string encoded = book.EncodePersistent();
        if (encoded != expected)
        {
            message = "encoding differs from the hand-built text: " + encoded;
            return false;
        }

        std::string text = encoded;
        const u64 mutation = stream.Next() % 7;
        bool mustAccept = false;
        bool mustReject = false;
        switch (mutation)
        {
        case 0: // insert JSON whitespace at any position that is a token boundary: before or after a structural character
        {
            std::string spaced;
            for (const char c : encoded)
            {
                const bool structural = c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':';
                if (structural && stream.Next() % 3 == 0)
                    spaced += (stream.Next() % 2 == 0 ? " " : "\t");
                spaced.push_back(c);
                if (structural && stream.Next() % 3 == 0)
                    spaced += "\r\n";
            }
            text = spaced;
            mustAccept = true;
            break;
        }
        case 1: // truncate by at least two bytes: the closing brace is gone
            text = encoded.substr(0, encoded.size() - 2 - stream.NextSize(0, encoded.size() - 3));
            mustReject = true;
            break;
        case 2: // delete one byte
            text.erase(stream.NextSize(0, text.size() - 1), 1);
            break;
        case 3: // replace one byte
            text[stream.NextSize(0, text.size() - 1)] = static_cast<char>(stream.Next());
            break;
        case 4: // insert one byte
            text.insert(stream.NextSize(0, text.size()), 1, static_cast<char>(stream.Next()));
            break;
        case 5: // trailing garbage
            text += "x";
            mustReject = true;
            break;
        default: // unmodified
            mustAccept = true;
            break;
        }

        BrowserSignInHostBook target;
        target.Grant("sentinel.example.org", true, baseline, error);
        const std::vector<BrowserGrantedHost> before = Snapshot(target);
        std::string loadError;
        const bool accepted = target.LoadPersistent(text, baseline, loadError);
        if (mustAccept && !accepted)
        {
            message = "a whitespace-only variation was rejected: " + loadError;
            return false;
        }
        if (mustReject && accepted)
        {
            message = "a document with its end removed or text appended was accepted";
            return false;
        }
        if (!accepted)
        {
            if (!SameEntries(Snapshot(target), before))
            {
                message = "a rejected document changed the book";
                return false;
            }
            return true;
        }
        std::set<std::string> unique;
        for (const BrowserGrantedHost& entry : target.Entries())
        {
            if (!entry.Persistent || !ReferenceHostValid(entry.Host) || baseline.IsHostAllowed(entry.Host) || !unique.insert(entry.Host).second)
            {
                message = "an accepted document produced an invalid, duplicate, non-persistent, or already allowed host: " + entry.Host;
                return false;
            }
        }
        if (target.Entries().size() > SignInHostLimits::kMaximumPersistentHosts)
        {
            message = "an accepted document exceeded the cap";
            return false;
        }
        if (mustAccept)
        {
            std::vector<std::string> loaded = target.HostList();
            if (loaded != persistent)
            {
                message = "a document that only differs in spacing lost or reordered hosts";
                return false;
            }
        }
        BrowserSignInHostBook second;
        if (!second.LoadPersistent(target.EncodePersistent(), baseline, loadError) || !SameEntries(Snapshot(second), Snapshot(target)))
        {
            message = "an accepted document does not survive a second round trip";
            return false;
        }
        return true;
    }

    // ---- 6. generated: host book vs model ------------------------------------------------------------

    bool BookModelProperty(Spiral::Tests::ChoiceStream& stream, std::string& message)
    {
        const BrowserNavigationPolicy baseline = DefaultPolicy();
        BrowserSignInHostBook book;
        std::vector<BrowserGrantedHost> model;
        const auto persistentCount = [&] { return static_cast<size_t>(std::count_if(model.begin(), model.end(), [](const BrowserGrantedHost& e) { return e.Persistent; })); };

        const size_t operations = stream.NextSize(1, 160);
        for (size_t index = 0; index < operations; ++index)
        {
            const u64 operation = stream.Next() % 10;
            std::string error;
            if (operation < 6)
            {
                const std::string host = operation < 5 ? "g" + std::to_string(stream.Next() % 90) + ".example.org" : HostFromChoice(stream, true);
                const bool persistent = stream.NextBool();

                SignInGrantResult expected = SignInGrantResult::Granted;
                if (!ReferenceHostValid(host))
                    expected = SignInGrantResult::Invalid;
                else if (baseline.IsHostAllowed(host))
                    expected = SignInGrantResult::AlreadyAllowed;
                else
                {
                    const auto found = std::find_if(model.begin(), model.end(), [&](const BrowserGrantedHost& e) { return e.Host == host; });
                    if (found != model.end())
                    {
                        if (!persistent || found->Persistent)
                            expected = SignInGrantResult::AlreadyAllowed;
                        else if (persistentCount() >= 64)
                            expected = SignInGrantResult::LimitReached;
                        else
                            found->Persistent = true;
                    }
                    else
                    {
                        const size_t sessionCount = model.size() - persistentCount();
                        if (persistent ? persistentCount() >= 64 : sessionCount >= 64)
                            expected = SignInGrantResult::LimitReached;
                        else
                            model.push_back({ host, persistent });
                    }
                }
                const SignInGrantResult actual = book.Grant(host, persistent, baseline, error);
                if (actual != expected)
                {
                    message = "Grant('" + host + "', " + (persistent ? "persistent" : "session") + ") returned " + std::to_string(static_cast<int>(actual))
                        + " expected " + std::to_string(static_cast<int>(expected));
                    return false;
                }
            }
            else if (operation < 8)
            {
                const std::string host = "g" + std::to_string(stream.Next() % 90) + ".example.org";
                const auto found = std::find_if(model.begin(), model.end(), [&](const BrowserGrantedHost& e) { return e.Host == host; });
                bool persistent = false;
                const bool expectedRemoved = found != model.end();
                const bool expectedPersistent = expectedRemoved && found->Persistent;
                if (expectedRemoved)
                    model.erase(found);
                if (book.Revoke(host, &persistent) != expectedRemoved || (expectedRemoved && persistent != expectedPersistent))
                {
                    message = "Revoke('" + host + "') disagrees with the model";
                    return false;
                }
            }
            else if (operation == 8)
            {
                const bool expected = persistentCount() > 0;
                model.clear();
                if (book.Clear() != expected)
                {
                    message = "Clear disagrees with the model";
                    return false;
                }
            }
            else
            {
                // Save/restore: the persistent subset survives, session entries are replaced.
                const std::string text = book.EncodePersistent();
                std::vector<BrowserGrantedHost> next;
                for (const BrowserGrantedHost& entry : model)
                {
                    if (entry.Persistent)
                        next.push_back(entry);
                }
                model = next;
                if (!book.LoadPersistent(text, baseline, error))
                {
                    message = "a fresh encoding failed to load: " + error;
                    return false;
                }
            }

            if (!SameEntries(Snapshot(book), model))
            {
                message = "entries diverged from the model after operation " + std::to_string(index);
                return false;
            }
            if (book.PersistentCount() != persistentCount() || book.HostList().size() != model.size())
            {
                message = "counts diverged from the model";
                return false;
            }
        }
        return true;
    }

    // ---- 7. generated: consent state machine vs model --------------------------------------------------

    bool ConsentModelProperty(Spiral::Tests::ChoiceStream& stream, std::string& message)
    {
        BrowserNavigationPolicy effective = DefaultPolicy();
        std::vector<std::string> granted;
        BrowserSignInConsent consent;

        std::string pending;
        std::vector<std::pair<std::string, u64>> dismissed; // host, until
        u64 now = 1000;

        const size_t steps = stream.NextSize(1, 120);
        for (size_t index = 0; index < steps; ++index)
        {
            const u64 event = stream.Next() % 9;
            if (event == 0)
            {
                now += stream.Next() % 6000;
            }
            else if (event < 4)
            {
                const std::string host = HostFromChoice(stream, true);
                // Model of Offer.
                bool expected = false;
                if (ReferenceHostValid(host))
                {
                    if (effective.IsHostAllowed(host))
                    {
                        if (pending == host)
                            pending.clear();
                    }
                    else
                    {
                        dismissed.erase(std::remove_if(dismissed.begin(), dismissed.end(), [&](const auto& e) { return e.second <= now; }), dismissed.end());
                        if (std::none_of(dismissed.begin(), dismissed.end(), [&](const auto& e) { return e.first == host; }))
                        {
                            pending = host;
                            expected = true;
                        }
                    }
                }
                if (consent.Offer(host, now, effective) != expected)
                {
                    message = "Offer('" + host + "') disagrees with the model at step " + std::to_string(index);
                    return false;
                }
            }
            else if (event < 6)
            {
                const SignInConsentChoice choice = static_cast<SignInConsentChoice>(stream.Next() % 3);
                const SignInConsentOutcome outcome = consent.Resolve(choice, now);
                const bool expectedResolved = !pending.empty();
                if (outcome.Resolved != expectedResolved)
                {
                    message = "Resolve resolved=" + std::to_string(outcome.Resolved) + " expected " + std::to_string(expectedResolved);
                    return false;
                }
                if (expectedResolved)
                {
                    const bool allow = choice != SignInConsentChoice::Dismiss;
                    if (outcome.Host != pending || outcome.Grant != allow || outcome.Retry != allow
                        || outcome.Persistent != (choice == SignInConsentChoice::AllowAlways))
                    {
                        message = "Resolve outcome disagrees with the model for host " + pending;
                        return false;
                    }
                    if (!allow)
                    {
                        dismissed.erase(std::remove_if(dismissed.begin(), dismissed.end(), [&](const auto& e) { return e.first == pending; }), dismissed.end());
                        if (dismissed.size() >= 16)
                            dismissed.erase(dismissed.begin());
                        dismissed.emplace_back(pending, now + 10000);
                    }
                    pending.clear();
                }
            }
            else if (event == 6)
            {
                // The allowed set changes behind the banner's back.
                granted.clear();
                const size_t count = stream.NextSize(0, 4);
                for (size_t g = 0; g < count; ++g)
                    granted.push_back(HostFromChoice(stream, false));
                effective.SetGrantedHosts(granted);
                consent.Withdraw(effective);
                if (!pending.empty() && effective.IsHostAllowed(pending))
                    pending.clear();
            }
            else if (event == 7)
            {
                consent.Reset();
                pending.clear();
                dismissed.clear();
            }
            else
            {
                // no-op observation step
            }
            if (consent.PendingHost() != pending || consent.HasPending() != !pending.empty())
            {
                message = "pending host '" + consent.PendingHost() + "' disagrees with model '" + pending + "' at step " + std::to_string(index);
                return false;
            }
        }
        return true;
    }

    // ---- 8. panel core flow ---------------------------------------------------------------------------------

    class FlowTextures final : public IBrowserUiTextures
    {
    public:
        Engine::UiTextureHandle Create(u32, u32, std::string_view) override { return ++m_Next; }
        bool Update(Engine::UiTextureHandle, const Engine::UiTextureUpdate&) override { return true; }
        bool Destroy(Engine::UiTextureHandle) override { return true; }
        u64 GetImGuiId(Engine::UiTextureHandle handle) override { return handle; }
        Engine::UiTextureError LastError() override { return Engine::UiTextureError::None; }

    private:
        Engine::UiTextureHandle m_Next = 0;
    };

    void NoDestroy(IBrowserSurface*) {}

    struct Flow
    {
        explicit Flow(const std::filesystem::path& directory, BrowserPanelConfig config = {})
        {
            config.EditorDirectory = directory / "editor";
            config.ProfileDirectory = directory / "Profile";
            config.DownloadStagingDirectory = directory / "staging";
            Config = config;
            BrowserPanelEnvironment environment;
            environment.Textures = &Textures;
            environment.LoadSurface = [this](const BrowserPanelConfig&)
            {
                BrowserSurfaceLoadResult result;
                result.Surface = MakeBrowserSurfacePtr(&Fake, &NoDestroy);
                return result;
            };
            environment.NowMs = [this] { return Now; };
            environment.LogInfo = [this](std::string_view message) { Infos.emplace_back(message); };
            environment.LogWarn = [this](std::string_view message) { Warnings.emplace_back(message); };
            Core = std::make_unique<BrowserPanelCore>(std::move(environment));
            Core->Configure(Config);
            Core->SetVisible(true);
            Pump();
            // One frame moves the panel from Starting to Running, where the status line shows notices.
            Fake.ScriptFrame(8, 8, std::vector<Engine::u8>(8 * 8 * 4, 0xFF), { { 0, 0, 8, 8 } });
            Pump();
        }

        void Pump()
        {
            Core->Pump();
            Now += 16;
        }

        void Draw()
        {
            BrowserPanelLayout layout;
            layout.Surface = { 100.0f, 50.0f, 640.0f, 360.0f };
            layout.SurfaceHovered = true;
            Core->Pump();
            Core->UpdateLayout(layout);
            Now += 16;
        }

        bool AnyLogContains(std::string_view needle) const
        {
            for (const std::vector<std::string>* log : { &Infos, &Warnings })
            {
                for (const std::string& line : *log)
                {
                    if (Contains(line, needle))
                        return true;
                }
            }
            return false;
        }

        FlowTextures Textures;
        SpiralTests::FakeBrowserSurface Fake;
        BrowserPanelConfig Config;
        std::unique_ptr<BrowserPanelCore> Core;
        std::vector<std::string> Infos;
        std::vector<std::string> Warnings;
        u64 Now = 100000;
    };

    bool CheckPanelFlow()
    {
        Checker check { "panel-flow" };
        TempDir fixture("flow");
        const std::filesystem::path hostsFile = fixture.Path() / "SignInHosts.json";

        {
            // Defaults reach the surface; a configured duplicate of a default is not an error.
            BrowserPanelConfig config;
            config.ProviderHosts = { "accounts.google.com", "partner.example.org" };
            config.InitialDeviceScale = 2.0f;
            config.RenderMode = BrowserRenderMode::Hardware;
            Flow flow(fixture.Path(), config);
            check.Expect(flow.Core->Live() && flow.Fake.IsInitialized(), "the panel starts with a duplicate configured host");
            const BrowserNavigationPolicy& policy = flow.Fake.Config().Navigation;
            for (const BrowserSignInProvider& provider : BrowserNavigationPolicy::DefaultSignInProviders())
                check.Expect(policy.IsHostAllowed(provider.Host), std::string("the surface policy has the default ") + std::string(provider.Host));
            check.Expect(policy.IsHostAllowed("partner.example.org") && !policy.IsHostAllowed("ca.account.sony.com"), "configured hosts are added, guessed ones are not");
            check.Expect(flow.Fake.Config().RenderMode == BrowserRenderMode::Hardware && flow.Core->GetDiagnostics().RenderMode == "hardware",
                "the render mode reaches the surface and the diagnostics");
            flow.Draw();
            check.Expect(!flow.Fake.ViewSizes.empty() && flow.Fake.ViewSizes[0].DeviceScale == 2.0f, "the initial device scale is applied before the first view size");
            check.Expect(flow.Fake.GrantedHostCalls.empty(), "an empty host list is not pushed to the surface");
        }

        BrowserPanelConfig config;
        config.SignInHostsFile = hostsFile;
        Flow flow(fixture.Path(), config);
        flow.Draw();
        check.Expect(flow.Core->GetDiagnostics().RenderMode == "software" && flow.Fake.Config().RenderMode == BrowserRenderMode::Software, "software rendering is the default");

        // A denial: counter, host-only status, diagnostics, log.
        flow.Fake.ScriptNavigationRequest("https://accounts.example.org/signin?token=secret&x=1#frag", BrowserNavigationKind::TopLevel);
        flow.Pump();
        BrowserPanelDiagnostics diagnostics = flow.Core->GetDiagnostics();
        check.Expect(diagnostics.NavigationDenials == 1 && diagnostics.LastDeniedHost == "accounts.example.org"
                && diagnostics.DeniedHosts == std::vector<std::string> { "accounts.example.org" } && diagnostics.ConsentHost == "accounts.example.org",
            "the denial is counted and its host is the consent host");
        check.Expect(flow.Core->StatusLine() == "Blocked navigation to accounts.example.org (blocked: 1).", "status line: " + flow.Core->StatusLine());
        check.Expect(flow.AnyLogContains("Fab navigation denied: host=accounts.example.org blocked=1"), "the denial is logged with its host and counter");
        check.Expect(!flow.AnyLogContains("secret") && !flow.AnyLogContains("signin") && !flow.AnyLogContains("frag") && !flow.AnyLogContains("http"),
            "no path, query, fragment, or scheme in any log line");
        check.Expect(flow.Core->PendingConsentHost() == "accounts.example.org", "the banner asks about the host");

        // Allow once: granted for the session, target repeated, nothing saved.
        flow.Core->ResolveConsent(SignInConsentChoice::AllowOnce);
        check.Expect(flow.Core->PendingConsentHost().empty(), "the banner closes");
        check.Expect(flow.Fake.GrantedHostCalls.size() == 1 && flow.Fake.GrantedHostCalls.back() == std::vector<std::string> { "accounts.example.org" }
                && flow.Fake.RetryCalls == 1,
            "the surface got the granted list and was asked to repeat the navigation");
        flow.Pump();
        check.Expect(flow.Core->GetDiagnostics().DisplayHost == "accounts.example.org" && flow.Core->GetDiagnostics().GrantedHostCount == 1,
            "the repeated navigation reached the host");
        check.Expect(flow.Core->StatusLine() == "Allowed accounts.example.org for this session.", "status line: " + flow.Core->StatusLine());
        check.Expect(!std::filesystem::exists(hostsFile), "allow once writes no file");
        check.Expect(flow.Core->GrantedHosts().size() == 1 && !flow.Core->GrantedHosts()[0].Persistent, "listed as a session grant");

        // Allow always: saved, owner-only, survives a new panel.
        flow.Fake.ScriptNavigationRequest("https://login.example.net/", BrowserNavigationKind::Popup);
        flow.Pump();
        check.Expect(flow.Core->PendingConsentHost() == "login.example.net" && flow.Core->GetDiagnostics().PopupRedirects == 0,
            "a popup to an unlisted host is denied and offers consent");
        flow.Core->ResolveConsent(SignInConsentChoice::AllowAlways);
        flow.Pump();
        check.Expect(ReadText(hostsFile) == "{\"schema\":1,\"hosts\":[\"login.example.net\"]}\n", "only the always-allowed host is saved: " + ReadText(hostsFile));
        check.Expect(flow.Core->GetDiagnostics().DisplayHost == "login.example.net", "the denied popup target loads in the panel after the grant");
#if defined(__linux__)
        {
            struct stat info {};
            check.Expect(::stat(hostsFile.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "the saved list is owner-only");
        }
#endif
        check.Expect(!Contains(ReadText(hostsFile), "http") && !Contains(ReadText(hostsFile), "/") && !Contains(ReadText(hostsFile), "secret"),
            "the saved list holds no URL, path, or query");

        // Allowed popup: opened in the panel, counted, not denied.
        flow.Fake.ScriptNavigationRequest("https://accounts.google.com/o/oauth2/v2/auth?client_id=1", BrowserNavigationKind::Popup);
        flow.Pump();
        diagnostics = flow.Core->GetDiagnostics();
        check.Expect(diagnostics.PopupRedirects == 1 && diagnostics.DisplayHost == "accounts.google.com" && diagnostics.NavigationDenials == 2,
            "an allowed popup becomes a main-frame load and is not a denial");
        check.Expect(Contains(flow.Core->StatusLine(), "accounts.google.com") && !Contains(flow.Core->StatusLine(), "client_id"), "the popup notice names the host only");

        {
            // A second panel with the same file starts with the saved host allowed.
            Flow second(fixture.Path(), config);
            check.Expect(second.Fake.GrantedHostCalls.size() == 1 && second.Fake.GrantedHostCalls[0] == std::vector<std::string> { "login.example.net" },
                "the saved host is pushed to the surface at startup");
            check.Expect(second.Core->GrantedHosts().size() == 1 && second.Core->GrantedHosts()[0].Persistent && second.Core->GetDiagnostics().GrantedHostCount == 1,
                "the saved host is listed as remembered");
            second.Fake.ScriptNavigationRequest("https://login.example.net/x", BrowserNavigationKind::TopLevel);
            second.Pump();
            check.Expect(second.Core->GetDiagnostics().NavigationDenials == 0 && second.Core->GetDiagnostics().DisplayHost == "login.example.net",
                "navigation to the saved host needs no consent");
            check.Expect(second.AnyLogContains("1 user-allowed sign-in hosts"), "the startup log counts the saved hosts");
        }

        // Dismiss: nothing granted; the same host is not offered again for a short time.
        flow.Fake.ScriptNavigationRequest("https://stubborn.example.org/", BrowserNavigationKind::TopLevel);
        flow.Pump();
        check.Expect(flow.Core->PendingConsentHost() == "stubborn.example.org", "offered");
        flow.Core->ResolveConsent(SignInConsentChoice::Dismiss);
        check.Expect(flow.Core->PendingConsentHost().empty() && flow.Core->GrantedHosts().size() == 2 && flow.Fake.RetryCalls == 2,
            "dismiss grants nothing and repeats nothing");
        flow.Fake.ScriptNavigationRequest("https://stubborn.example.org/", BrowserNavigationKind::TopLevel);
        flow.Pump();
        check.Expect(flow.Core->PendingConsentHost().empty(), "a dismissed host is not offered again at once");
        flow.Now += SignInHostLimits::kDismissSuppressMilliseconds + 1;
        flow.Fake.ScriptNavigationRequest("https://stubborn.example.org/", BrowserNavigationKind::TopLevel);
        flow.Pump();
        check.Expect(flow.Core->PendingConsentHost() == "stubborn.example.org", "and is offered again after the suppression window");
        // A newer offer replaces the older banner.
        flow.Fake.ScriptNavigationRequest("https://newer.example.org/", BrowserNavigationKind::TopLevel);
        flow.Pump();
        check.Expect(flow.Core->PendingConsentHost() == "newer.example.org", "a newer offer replaces the banner");
        flow.Core->ResolveConsent(SignInConsentChoice::Dismiss);

        // Non-consentable denials never open a banner.
        for (const char* url : { "http://plain.example.org/", "https://127.0.0.1/", "https://user:pw@evil.example/", "javascript:alert(1)" })
        {
            flow.Fake.ScriptNavigationRequest(url, BrowserNavigationKind::TopLevel);
            flow.Pump();
            check.Expect(flow.Core->PendingConsentHost().empty(), std::string("no banner for ") + url);
        }

        // Revoke and clear update the surface and the file.
        flow.Fake.ScriptNavigationRequest("https://second.example.org/", BrowserNavigationKind::TopLevel);
        flow.Pump();
        flow.Core->ResolveConsent(SignInConsentChoice::AllowAlways);
        check.Expect(ReadText(hostsFile) == "{\"schema\":1,\"hosts\":[\"login.example.net\",\"second.example.org\"]}\n", "a second saved host is appended: " + ReadText(hostsFile));
        check.Expect(flow.Core->RevokeGrantedHost("login.example.net") && !flow.Core->RevokeGrantedHost("login.example.net"), "revoke a saved host");
        check.Expect(ReadText(hostsFile) == "{\"schema\":1,\"hosts\":[\"second.example.org\"]}\n", "revoking a saved host rewrites the file: " + ReadText(hostsFile));
        const size_t revokeCalls = flow.Fake.GrantedHostCalls.size();
        check.Expect(flow.Core->RevokeGrantedHost("accounts.example.org") && !flow.Core->RevokeGrantedHost("accounts.example.org"), "revoke a session host");
        check.Expect(flow.Fake.GrantedHostCalls.size() == revokeCalls + 1 && flow.Fake.GrantedHostCalls.back() == std::vector<std::string> { "second.example.org" },
            "the surface list is replaced");
        check.Expect(ReadText(hostsFile) == "{\"schema\":1,\"hosts\":[\"second.example.org\"]}\n", "revoking a session host leaves the file alone");
        flow.Core->ClearGrantedHosts();
        check.Expect(flow.Core->GrantedHosts().empty() && flow.Fake.GrantedHostCalls.back().empty() && ReadText(hostsFile) == "{\"schema\":1,\"hosts\":[]}\n",
            "clearing empties the surface list and the file");
        flow.Fake.ScriptNavigationRequest("https://second.example.org/", BrowserNavigationKind::TopLevel);
        flow.Pump();
        check.Expect(flow.Core->PendingConsentHost() == "second.example.org", "a cleared host is denied again");

        // A page that redirects in a loop is counted in full but logged only up to a cap.
        {
            TempDir floodFixture("flood");
            Flow flood(floodFixture.Path());
            const size_t warningsBefore = flood.Warnings.size();
            for (u32 index = 0; index < BrowserPanelLimits::kMaximumLoggedDenials + 40; ++index)
                flood.Fake.ScriptNavigationRequest("https://loop.example.org/next", BrowserNavigationKind::TopLevel);
            flood.Pump();
            const u32 denials = flood.Core->GetDiagnostics().NavigationDenials;
            check.Expect(denials == BrowserPanelLimits::kMaximumLoggedDenials + 40 && flood.Core->GetDiagnostics().DeniedHosts.size() == 1,
                "every denial is counted, the host list stays distinct");
            check.Expect(flood.Warnings.size() - warningsBefore == BrowserPanelLimits::kMaximumLoggedDenials + 1,
                "the log gets the capped denials plus one notice: " + std::to_string(flood.Warnings.size() - warningsBefore));
        }

        // The cap: a 65th always-allowed host is refused with a reason.
        {
            std::string full = "{\"schema\":1,\"hosts\":[";
            for (size_t index = 0; index < SignInHostLimits::kMaximumPersistentHosts; ++index)
                full += std::string(index == 0 ? "" : ",") + "\"cap" + std::to_string(index) + ".example.org\"";
            full += "]}";
            TempDir capFixture("cap");
            BrowserPanelConfig capConfig;
            capConfig.SignInHostsFile = capFixture.Path() / "SignInHosts.json";
            WriteText(capConfig.SignInHostsFile, full);
            Flow capped(capFixture.Path(), capConfig);
            check.Expect(capped.Core->GrantedHosts().size() == SignInHostLimits::kMaximumPersistentHosts, "64 saved hosts load at startup");
            capped.Fake.ScriptNavigationRequest("https://one-too-many.example.org/", BrowserNavigationKind::TopLevel);
            capped.Pump();
            capped.Core->ResolveConsent(SignInConsentChoice::AllowAlways);
            check.Expect(capped.Core->GrantedHosts().size() == SignInHostLimits::kMaximumPersistentHosts && Contains(capped.Core->SignInHostsNotice(), "full")
                    && ReadText(capConfig.SignInHostsFile) == full,
                "the 65th host is refused, the file is unchanged, and the reason is shown: " + capped.Core->SignInHostsNotice());
        }

        // A malformed file is ignored (with a log line that carries no path); the panel still works.
        {
            TempDir badFixture("bad");
            BrowserPanelConfig badConfig;
            badConfig.SignInHostsFile = badFixture.Path() / "SignInHosts.json";
            WriteText(badConfig.SignInHostsFile, "{\"schema\":1,\"hosts\":[\"127.0.0.1\"]}");
            Flow bad(badFixture.Path(), badConfig);
            check.Expect(bad.Core->Live() && bad.Core->GrantedHosts().empty(), "a malformed list is ignored");
            check.Expect(Contains(bad.Core->SignInHostsNotice(), "ignored") && !bad.AnyLogContains(badFixture.Path().string()), "the notice and the log carry no path");
            bad.Fake.ScriptNavigationRequest("https://fresh.example.org/", BrowserNavigationKind::TopLevel);
            bad.Pump();
            bad.Core->ResolveConsent(SignInConsentChoice::AllowAlways);
            check.Expect(ReadText(badConfig.SignInHostsFile) == "{\"schema\":1,\"hosts\":[\"fresh.example.org\"]}\n", "the next grant replaces the unreadable list");
        }

        // The default location is beside the profile directory, not inside it.
        {
            TempDir derivedFixture("derived");
            BrowserPanelConfig derivedConfig;
            Flow derived(derivedFixture.Path(), derivedConfig);
            check.Expect(derived.Core->SignInHostsFile() == derivedFixture.Path() / "SignInHosts.json", "the default list sits beside the profile directory");
        }

        // Screen geometry is forwarded once per change.
        {
            TempDir screenFixture("screen");
            Flow screen(screenFixture.Path());
            BrowserPanelLayout layout;
            layout.Surface = { 100.0f, 50.0f, 640.0f, 360.0f };
            layout.Screen.Valid = true;
            layout.Screen.MonitorWidth = 2560;
            layout.Screen.MonitorHeight = 1440;
            layout.Screen.WorkWidth = 2560;
            layout.Screen.WorkHeight = 1400;
            for (int frame = 0; frame < 3; ++frame)
            {
                screen.Core->Pump();
                screen.Core->UpdateLayout(layout);
            }
            check.Expect(screen.Fake.ScreenInfos.size() == 1 && screen.Fake.ScreenInfos[0] == layout.Screen, "an unchanged screen is sent once");
            layout.Screen.WorkHeight = 1380;
            screen.Core->Pump();
            screen.Core->UpdateLayout(layout);
            check.Expect(screen.Fake.ScreenInfos.size() == 2 && screen.Fake.ScreenInfos[1].WorkHeight == 1380, "a changed screen is sent again");
        }
        return check.Ok;
    }
}

namespace SpiralTests
{
    bool TestBrowserSigninDefaultHostTable()
    {
        return CheckDefaultHostTable();
    }

    bool TestBrowserSigninConsentAndPopupPolicy()
    {
        return CheckConsentAndPopupPolicy();
    }

    bool TestBrowserSigninAcceptLanguageList()
    {
        return CheckAcceptLanguageList();
    }

    bool TestBrowserSigninHostCodecAndFile()
    {
        return CheckCodecAndFile();
    }

    bool TestBrowserSigninHostCodecProperty()
    {
        return RunProperty("signin-codec", CodecProperty, 800);
    }

    bool TestBrowserSigninHostBookModel()
    {
        return RunProperty("signin-book-model", BookModelProperty, 400);
    }

    bool TestBrowserSigninConsentModel()
    {
        return RunProperty("signin-consent-model", ConsentModelProperty, 600);
    }

    bool TestBrowserSigninPanelFlow()
    {
        return CheckPanelFlow();
    }
}
