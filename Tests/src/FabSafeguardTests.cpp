#include "FabSafeguardTests.h"

#include "BrowserNavigationPolicy.h"
#include "Engine/Platform/ExternalUrl.h"
#include "FabDisclosure.h"
#include "FabLicenseGate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Failure hypotheses, oracles, and non-claims for this file:
// - Units: the Epic-terms safeguards that are pure: the notice dismissal codec,
//   file, and visibility rule (FabDisclosure), the licence-kind gate
//   (FabLicenseGate), the "Open in browser" decision, the kill-switch resolver,
//   and the page-hint tables.
// - Oracles are independent of the code under test: hand-written wire text, a
//   hand-written verdict table, a hand-written host allowlist for the URL
//   property, and direct std::filesystem inspection of the written file.
// - Hypotheses: a decoder that accepts a looser grammar than the one documented;
//   a dismissal that survives a changed notice version; a file that is created
//   group/world readable, follows a symlink, or is written non-atomically; a
//   dismissal stored inside the profile that sign-out deletes; a licence kind
//   that slips through the gate; an open request that carries a query string or
//   a host outside the navigation policy.
// - The mutation sweep is deterministic (fixed byte set, no random source), so a
//   failure is reproduced by rerunning; the first failing mutation is printed.
// - Not claimed: the ImGui drawing of the notice line and the toolbar, the system
//   browser launch itself (Engine::OpenExternalHttpsUrl has its own test), and
//   any Windows permission semantics (the 0600 assertion is POSIX only).

namespace
{
    using namespace Fab;
    namespace fs = std::filesystem;

    struct Checker
    {
        const char* Suite;
        bool Ok = true;

        void Expect(bool condition, const std::string& message)
        {
            if (!condition)
            {
                std::cerr << "Fab safeguard test failed [" << Suite << "]: " << message << '\n';
                Ok = false;
            }
        }
    };

    bool Contains(std::string_view text, std::string_view needle)
    {
        return text.find(needle) != std::string_view::npos;
    }

    class TempDir
    {
    public:
        TempDir()
        {
            static std::atomic<unsigned> counter { 0 };
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_Path = fs::temp_directory_path()
                / ("spiral-fab-safeguards-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
            std::error_code code;
            fs::create_directories(m_Path, code);
            m_Ok = !code && fs::is_directory(m_Path, code);
        }

        ~TempDir()
        {
            std::error_code code;
            fs::remove_all(m_Path, code);
        }

        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;

        const fs::path& Path() const { return m_Path; }
        bool Ok() const { return m_Ok; }

    private:
        fs::path m_Path;
        bool m_Ok = false;
    };

    std::string ReadAll(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }

    bool WriteAll(const fs::path& path, std::string_view text)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(output);
    }

    constexpr Engine::u32 kSentinel = 77;
}

namespace SpiralTests
{
    // ---- 1. codec and visibility ----------------------------------------------------------------------

    bool TestFabNoticeDismissalCodecAndVisibility()
    {
        Checker check { "dismissal-codec" };

        // Hand-written wire text (not produced by the encoder).
        check.Expect(EncodeFabNoticeDismissal(1) == "{\"schema\":1,\"notice\":1}\n", "version 1 encodes to the exact documented text");
        check.Expect(EncodeFabNoticeDismissal(41) == "{\"schema\":1,\"notice\":41}\n", "version 41 encodes to the exact documented text");

        struct Accept
        {
            const char* Text;
            Engine::u32 Expected;
        };
        const Accept accepted[] = {
            { "{\"schema\":1,\"notice\":1}\n", 1 },
            { "{\"schema\":1,\"notice\":1}", 1 },
            { "{\"notice\":7,\"schema\":1}", 7 },
            { " { \"schema\" : 1 , \"notice\" : 999999999 } \n", 999999999 },
            { "\t{\r\n\"schema\":1,\r\n\"notice\":12\r\n}\r\n", 12 },
        };
        for (const Accept& row : accepted)
        {
            Engine::u32 version = kSentinel;
            std::string error = "stale";
            check.Expect(DecodeFabNoticeDismissal(row.Text, version, error) && version == row.Expected && error.empty(),
                std::string("accepts: ") + row.Text);
        }

        const std::string tooLarge = "{\"schema\":1,\"notice\":1}" + std::string(260, ' ');
        const std::string embeddedNul("{\"schema\":1,\"notice\":1\0}", 24);
        const std::vector<std::string> rejected = {
            "",
            " ",
            "{}",
            "[]",
            "null",
            "{\"schema\":1}",
            "{\"notice\":1}",
            "{\"schema\":2,\"notice\":1}",
            "{\"schema\":0,\"notice\":1}",
            "{\"schema\":1,\"notice\":0}",
            "{\"schema\":1,\"notice\":01}",
            "{\"schema\":1,\"notice\":1.5}",
            "{\"schema\":1,\"notice\":1e3}",
            "{\"schema\":1,\"notice\":-1}",
            "{\"schema\":1,\"notice\":+1}",
            "{\"schema\":1,\"notice\":\"1\"}",
            "{\"schema\":1,\"notice\":true}",
            "{\"schema\":1,\"notice\":1000000000}",
            "{\"schema\":1,\"notice\":4294967297}",
            "{\"schema\":1,\"schema\":1,\"notice\":1}",
            "{\"schema\":1,\"notice\":1,\"notice\":1}",
            "{\"schema\":1,\"notice\":1,\"extra\":1}",
            "{\"schema\":1,\"notice\":1,}",
            "{\"schema\":1 \"notice\":1}",
            "{\"schema\":1,\"notice\":1}}",
            "{\"schema\":1,\"notice\":1} x",
            "{\"schema\":1,\"notice\":1",
            "{'schema':1,'notice':1}",
            "{\"Schema\":1,\"notice\":1}",
            "{\"sch\\u0065ma\":1,\"notice\":1}",
            "{\"schema\":[1],\"notice\":1}",
            "\xEF\xBB\xBF{\"schema\":1,\"notice\":1}",
            tooLarge,
            embeddedNul,
        };
        for (const std::string& text : rejected)
        {
            Engine::u32 version = kSentinel;
            std::string error;
            const bool accept = DecodeFabNoticeDismissal(text, version, error);
            check.Expect(!accept && version == kSentinel && !error.empty(), "rejects as a whole and leaves the output: " + std::to_string(text.size()) + " bytes");
            check.Expect(text.size() < 4 || error.find(text) == std::string::npos, "the reason does not echo the content");
        }

        // Round trip over a hand-picked spread of versions.
        for (const Engine::u32 value : { 1u, 2u, 9u, 10u, 99u, 100u, 12345u, 999999998u, 999999999u })
        {
            Engine::u32 version = kSentinel;
            std::string error;
            check.Expect(DecodeFabNoticeDismissal(EncodeFabNoticeDismissal(value), version, error) && version == value,
                "round trip of " + std::to_string(value));
        }

        // Deterministic mutation sweep over the canonical text. Oracle: any accepted mutation
        // yields a version in 1..999999999 whose canonical re-encoding decodes to the same value
        // (so the decoder never accepts something it could not have produced), and every strict
        // prefix of the canonical object is rejected.
        const std::string canonical = EncodeFabNoticeDismissal(1);
        const std::string replacements = std::string("0129\"',:{}[] x-.\\+e\n\t") + std::string(1, '\0') + std::string(1, '\x7f') + std::string(1, '\xff');
        size_t mutations = 0;
        const auto sweep = [&](const std::string& text, const std::string& label)
        {
            ++mutations;
            Engine::u32 version = kSentinel;
            std::string error;
            if (!DecodeFabNoticeDismissal(text, version, error))
            {
                check.Expect(version == kSentinel, "a rejected mutation leaves the output: " + label);
                return;
            }
            Engine::u32 reread = kSentinel;
            check.Expect(version >= 1 && version <= 999999999 && DecodeFabNoticeDismissal(EncodeFabNoticeDismissal(version), reread, error)
                    && reread == version,
                "an accepted mutation must be a value the encoder could write: " + label);
        };
        for (size_t index = 0; index < canonical.size(); ++index)
        {
            std::string deleted = canonical;
            deleted.erase(index, 1);
            sweep(deleted, "delete@" + std::to_string(index));
            for (const char replacement : replacements)
            {
                std::string changed = canonical;
                changed[index] = replacement;
                sweep(changed, "replace@" + std::to_string(index));
                std::string inserted = canonical;
                inserted.insert(index, 1, replacement);
                sweep(inserted, "insert@" + std::to_string(index));
            }
        }
        for (size_t length = 0; length + 1 < canonical.size(); ++length)
        {
            Engine::u32 version = kSentinel;
            std::string error;
            check.Expect(!DecodeFabNoticeDismissal(canonical.substr(0, length), version, error), "strict prefix rejected, length " + std::to_string(length));
        }
        check.Expect(mutations > 1000, "the sweep ran: " + std::to_string(mutations));

        // Classification and visibility, hand-written tables.
        check.Expect(ClassifyFabNoticeDismissal(1, 1) == FabNoticeDismissalStatus::Dismissed, "same version is dismissed");
        check.Expect(ClassifyFabNoticeDismissal(1, 2) == FabNoticeDismissalStatus::Outdated, "an older version is outdated");
        check.Expect(ClassifyFabNoticeDismissal(3, 2) == FabNoticeDismissalStatus::Outdated, "a newer stored version is outdated too");

        struct Visibility
        {
            FabNoticeDismissalStatus Stored;
            bool Session;
            bool Visible;
        };
        const Visibility table[] = {
            { FabNoticeDismissalStatus::Dismissed, false, false },
            { FabNoticeDismissalStatus::Dismissed, true, false },
            { FabNoticeDismissalStatus::Missing, false, true },
            { FabNoticeDismissalStatus::Missing, true, false },
            { FabNoticeDismissalStatus::Outdated, false, true },
            { FabNoticeDismissalStatus::Outdated, true, false },
            { FabNoticeDismissalStatus::Rejected, false, true },
            { FabNoticeDismissalStatus::Rejected, true, false },
        };
        for (const Visibility& row : table)
            check.Expect(FabNoticeVisible(row.Stored, row.Session) == row.Visible,
                "visibility row stored=" + std::to_string(static_cast<int>(row.Stored)) + " session=" + (row.Session ? "1" : "0"));

        // The notice text is the exact owner-approved line and the hint the exact approved sentence.
        check.Expect(kFabNoticeText
                == "Fab.com is Epic Games' service shown here unmodified. Spiral is not affiliated with Epic. Sign in directly on Epic's "
                   "pages; Spiral never sees your password or cookies.",
            "the notice text is the approved sentence");
        check.Expect(kFabPageHintText
                == "If Fab shows a security check that does not work in this panel, use Open in browser, download there, then drop the "
                   "file onto the Editor.",
            "the page hint is the approved sentence");
        return check.Ok;
    }

    // ---- 2. file behaviour -----------------------------------------------------------------------------

    bool TestFabNoticeDismissalFileIsOwnerOnlyTransactionalAndBesideTheProfile()
    {
        Checker check { "dismissal-file" };
        TempDir root;
        check.Expect(root.Ok(), "temporary directory");
        if (!root.Ok())
            return false;

        const fs::path browserDir = root.Path() / "FabBrowser";
        const fs::path profile = browserDir / "Profile";
        std::error_code code;
        fs::create_directories(profile, code);

        // Placement: a sibling of the profile, never inside it or a project.
        const fs::path path = FabNoticeDismissalPath(profile, {});
        check.Expect(path == browserDir / "FabNoticeDismissed.json", "default path is beside the profile directory");
        check.Expect(path.parent_path() != profile && path.string().rfind(profile.string() + "/", 0) != 0, "the file is not inside the profile");
        check.Expect(FabNoticeDismissalPath(profile, root.Path() / "x.json") == root.Path() / "x.json", "an explicit file wins");
        check.Expect(FabNoticeDismissalPath({}, {}).empty() && FabNoticeDismissalPath("Profile", {}).empty(), "no parent means no path");

        std::string error;
        check.Expect(LoadFabNoticeDismissalFile(path, 1, error) == FabNoticeDismissalStatus::Missing && error.empty(), "missing file");

        check.Expect(SaveFabNoticeDismissalFile(path, 1, error) && error.empty(), "save: " + error);
        check.Expect(fs::is_regular_file(path, code) && ReadAll(path) == "{\"schema\":1,\"notice\":1}\n", "content is the exact record");
        check.Expect(!fs::exists(fs::path(path).concat(".tmp"), code), "no temporary is left");
#ifndef _WIN32
        check.Expect((fs::status(path, code).permissions() & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write),
            "the file is exactly 0600");
#endif
        check.Expect(LoadFabNoticeDismissalFile(path, 1, error) == FabNoticeDismissalStatus::Dismissed, "dismissed for the current version");
        check.Expect(LoadFabNoticeDismissalFile(path, 2, error) == FabNoticeDismissalStatus::Outdated && error.empty(),
            "a changed notice version asks again");

        // Sign-out deletes the profile directory and only that: the dismissal remains.
        check.Expect(WriteAll(profile / "Cookies", "x"), "profile content");
        fs::remove_all(profile, code);
        check.Expect(!fs::exists(profile, code) && LoadFabNoticeDismissalFile(path, 1, error) == FabNoticeDismissalStatus::Dismissed,
            "removing the profile leaves the dismissal");

        // Replacing a wider file ends owner-only, and a new version replaces the old.
        const fs::path wide = root.Path() / "wide.json";
        check.Expect(WriteAll(wide, "old"), "wide fixture");
#ifndef _WIN32
        fs::permissions(wide, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read,
            fs::perm_options::replace, code);
#endif
        check.Expect(SaveFabNoticeDismissalFile(wide, 2, error) && ReadAll(wide) == "{\"schema\":1,\"notice\":2}\n", "replace an existing file");
#ifndef _WIN32
        check.Expect((fs::status(wide, code).permissions() & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write),
            "a replaced file is owner-only");
#endif
        check.Expect(LoadFabNoticeDismissalFile(wide, 2, error) == FabNoticeDismissalStatus::Dismissed, "the new version is read back");

        // Hostile or damaged content fails closed, with a reason that never contains the path.
        const fs::path damaged = root.Path() / "damaged.json";
        check.Expect(WriteAll(damaged, "not json"), "damaged fixture");
        error.clear();
        check.Expect(LoadFabNoticeDismissalFile(damaged, 1, error) == FabNoticeDismissalStatus::Rejected && !error.empty()
                && !Contains(error, root.Path().string()),
            "damaged content is Rejected with a path-free reason");
        check.Expect(WriteAll(damaged, std::string(300, 'a')), "oversized fixture");
        error.clear();
        check.Expect(LoadFabNoticeDismissalFile(damaged, 1, error) == FabNoticeDismissalStatus::Rejected && !error.empty(), "oversized file is Rejected");

        // A directory at the path is neither read nor replaced.
        const fs::path directoryPath = root.Path() / "dir.json";
        fs::create_directory(directoryPath, code);
        error.clear();
        check.Expect(LoadFabNoticeDismissalFile(directoryPath, 1, error) == FabNoticeDismissalStatus::Rejected && !error.empty(), "a directory is Rejected");
        error.clear();
        check.Expect(!SaveFabNoticeDismissalFile(directoryPath, 1, error) && !error.empty() && fs::is_directory(directoryPath, code),
            "saving over a directory is refused and leaves it");

        // The directory must exist; nothing is created.
        error.clear();
        const fs::path missingParent = root.Path() / "absent" / "n.json";
        check.Expect(!SaveFabNoticeDismissalFile(missingParent, 1, error) && !error.empty() && !fs::exists(root.Path() / "absent", code),
            "a missing directory is refused and not created");

        // Symbolic links: the final component is never followed, and a stale temporary link is replaced, not written through.
        const fs::path victim = root.Path() / "victim.txt";
        check.Expect(WriteAll(victim, "KEEP"), "victim fixture");
        const fs::path link = root.Path() / "link.json";
        fs::create_symlink(victim, link, code);
        if (!code)
        {
            error.clear();
            check.Expect(LoadFabNoticeDismissalFile(link, 1, error) == FabNoticeDismissalStatus::Rejected, "a symlink is Rejected");
            error.clear();
            check.Expect(!SaveFabNoticeDismissalFile(link, 1, error) && ReadAll(victim) == "KEEP", "saving over a symlink is refused and the target is untouched");

            const fs::path staleTarget = root.Path() / "stale-victim.txt";
            check.Expect(WriteAll(staleTarget, "KEEP2"), "stale victim fixture");
            const fs::path real = root.Path() / "real.json";
            fs::create_symlink(staleTarget, fs::path(real).concat(".tmp"), code);
            check.Expect(!code, "stale temporary link fixture");
            error.clear();
            check.Expect(SaveFabNoticeDismissalFile(real, 1, error), "save beside a stale temporary link: " + error);
            check.Expect(ReadAll(staleTarget) == "KEEP2", "the stale link's target was not written");
            check.Expect(fs::is_regular_file(real, code) && !fs::is_symlink(real, code) && ReadAll(real) == "{\"schema\":1,\"notice\":1}\n"
                    && !fs::exists(fs::path(real).concat(".tmp"), code),
                "the record is a regular file and no temporary remains");
        }
        return check.Ok;
    }

    // ---- 3. licence-kind gate ---------------------------------------------------------------------------

    bool TestFabLicenseGateTable()
    {
        Checker check { "license-gate" };
        constexpr std::string_view kRefusalPhrase = "cannot be used as importable source content in Spiral";

        struct Row
        {
            FabLicenseChoice Choice;
            const char* Attribution;
            FabLicenseVerdict Verdict;
            const char* MessageContains; // nullptr when the message must be empty
        };
        // Hand-written: Standard Personal/Professional import; CC-BY imports only with non-blank
        // attribution text; Reference-Only, code plugin, legacy UE-only and Other are refused.
        const Row table[] = {
            { FabLicenseChoice::NotChosen, "", FabLicenseVerdict::Incomplete, "Choose the licence" },
            { FabLicenseChoice::StandardPersonal, "", FabLicenseVerdict::Allowed, nullptr },
            { FabLicenseChoice::StandardPersonal, "ignored", FabLicenseVerdict::Allowed, nullptr },
            { FabLicenseChoice::StandardProfessional, "", FabLicenseVerdict::Allowed, nullptr },
            { FabLicenseChoice::CcBy, "", FabLicenseVerdict::Incomplete, "attribution" },
            { FabLicenseChoice::CcBy, "   \t\r\n", FabLicenseVerdict::Incomplete, "attribution" },
            { FabLicenseChoice::CcBy, "Rock by Someone", FabLicenseVerdict::Allowed, nullptr },
            { FabLicenseChoice::CcBy, " x ", FabLicenseVerdict::Allowed, nullptr },
            { FabLicenseChoice::PersonalReferenceOnly, "", FabLicenseVerdict::Refused, "Reference Only" },
            { FabLicenseChoice::PersonalReferenceOnly, "Rock by Someone", FabLicenseVerdict::Refused, "Reference Only" },
            { FabLicenseChoice::CodePlugin, "", FabLicenseVerdict::Refused, "Code plugins" },
            { FabLicenseChoice::LegacyUeOnly, "", FabLicenseVerdict::Refused, "Unreal Engine use only" },
            { FabLicenseChoice::Other, "", FabLicenseVerdict::Refused, "other licences" },
            { FabLicenseChoice::Other, "Rock by Someone", FabLicenseVerdict::Refused, "other licences" },
        };
        for (const Row& row : table)
        {
            for (const Engine::FabMetadataFlag noAi : { Engine::FabMetadataFlag::Unknown, Engine::FabMetadataFlag::No, Engine::FabMetadataFlag::Yes })
            {
                const std::string label = std::string(FabLicenseChoiceLabel(row.Choice)) + " noAi=" + std::to_string(static_cast<int>(noAi));
                const FabLicenseGateResult result = EvaluateFabLicenseGate(row.Choice, row.Attribution, noAi);
                check.Expect(result.Verdict == row.Verdict, "verdict: " + label);
                check.Expect(result.NoAiNoticeRequired == (noAi == Engine::FabMetadataFlag::Yes), "the NoAI notice follows the declared flag: " + label);
                if (row.MessageContains == nullptr)
                    check.Expect(result.Message.empty(), "an allowed choice has no message: " + label);
                else
                    check.Expect(Contains(result.Message, row.MessageContains), "message names the reason: " + label + " -> " + result.Message);
                check.Expect((row.Verdict == FabLicenseVerdict::Refused) == Contains(result.Message, kRefusalPhrase),
                    "a refusal says the content cannot be used as importable source content in Spiral, and nothing else does: " + label);
            }
        }

        // Every choice has a distinct, non-empty label, and the list is complete.
        std::vector<std::string> labels;
        for (const FabLicenseChoice choice : kFabLicenseChoices)
            labels.emplace_back(FabLicenseChoiceLabel(choice));
        check.Expect(kFabLicenseChoices.size() == 8 && kFabLicenseChoices[0] == FabLicenseChoice::NotChosen, "eight choices, Not chosen first");
        for (size_t left = 0; left < labels.size(); ++left)
        {
            check.Expect(!labels[left].empty(), "label present");
            for (size_t right = left + 1; right < labels.size(); ++right)
                check.Expect(labels[left] != labels[right], "labels are distinct: " + labels[left]);
        }
        check.Expect(IsPanelOnlyLicenseChoice(FabLicenseChoice::CodePlugin) && IsPanelOnlyLicenseChoice(FabLicenseChoice::Other)
                && !IsPanelOnlyLicenseChoice(FabLicenseChoice::LegacyUeOnly) && !IsPanelOnlyLicenseChoice(FabLicenseChoice::CcBy)
                && !IsPanelOnlyLicenseChoice(FabLicenseChoice::PersonalReferenceOnly),
            "only the choices the receipt cannot represent are panel-only");

        // Receipt fields per choice, hand-written.
        struct Fields
        {
            FabLicenseChoice Choice;
            Engine::FabLicenseFamily Family;
            Engine::FabLicenseTier Tier;
        };
        const Fields fields[] = {
            { FabLicenseChoice::NotChosen, Engine::FabLicenseFamily::Unknown, Engine::FabLicenseTier::Unknown },
            { FabLicenseChoice::StandardPersonal, Engine::FabLicenseFamily::FabStandard, Engine::FabLicenseTier::Personal },
            { FabLicenseChoice::StandardProfessional, Engine::FabLicenseFamily::FabStandard, Engine::FabLicenseTier::Professional },
            { FabLicenseChoice::CcBy, Engine::FabLicenseFamily::CreativeCommonsAttribution, Engine::FabLicenseTier::NotApplicable },
            { FabLicenseChoice::PersonalReferenceOnly, Engine::FabLicenseFamily::ReferenceOnly, Engine::FabLicenseTier::NotApplicable },
            { FabLicenseChoice::CodePlugin, Engine::FabLicenseFamily::Unknown, Engine::FabLicenseTier::Unknown },
            { FabLicenseChoice::LegacyUeOnly, Engine::FabLicenseFamily::LegacyUnrealMarketplace, Engine::FabLicenseTier::NotApplicable },
            { FabLicenseChoice::Other, Engine::FabLicenseFamily::Unknown, Engine::FabLicenseTier::Unknown },
        };
        for (const Fields& row : fields)
        {
            const FabLicenseFields mapped = FabLicenseFieldsForChoice(row.Choice);
            check.Expect(mapped.Family == row.Family && mapped.Tier == row.Tier, std::string("fields for ") + std::string(FabLicenseChoiceLabel(row.Choice)));
        }

        // The inverse, and the gate applied to a controller-held provenance.
        for (const FabLicenseChoice choice : kFabLicenseChoices)
        {
            if (IsPanelOnlyLicenseChoice(choice) || choice == FabLicenseChoice::NotChosen)
                continue;
            FabProvenance provenance;
            const FabLicenseFields mapped = FabLicenseFieldsForChoice(choice);
            provenance.LicenseFamily = mapped.Family;
            provenance.LicenseTier = mapped.Tier;
            check.Expect(FabLicenseChoiceFromProvenance(provenance) == choice, std::string("inverse of ") + std::string(FabLicenseChoiceLabel(choice)));
        }
        FabProvenance noTier;
        noTier.LicenseFamily = Engine::FabLicenseFamily::FabStandard;
        noTier.LicenseTier = Engine::FabLicenseTier::Unknown;
        check.Expect(FabLicenseChoiceFromProvenance(noTier) == FabLicenseChoice::NotChosen
                && EvaluateFabLicenseGate(noTier).Verdict == FabLicenseVerdict::Incomplete,
            "Fab Standard without a tier is not chosen");
        FabProvenance legacy;
        legacy.LicenseFamily = Engine::FabLicenseFamily::LegacyUnrealMarketplace;
        legacy.LicenseTier = Engine::FabLicenseTier::NotApplicable;
        check.Expect(EvaluateFabLicenseGate(legacy).Verdict == FabLicenseVerdict::Refused, "a held legacy UE provenance is refused");
        FabProvenance reference;
        reference.LicenseFamily = Engine::FabLicenseFamily::ReferenceOnly;
        reference.LicenseTier = Engine::FabLicenseTier::NotApplicable;
        check.Expect(EvaluateFabLicenseGate(reference).Verdict == FabLicenseVerdict::Refused, "a held Reference-Only provenance is refused");
        FabProvenance ccBy;
        ccBy.LicenseFamily = Engine::FabLicenseFamily::CreativeCommonsAttribution;
        ccBy.LicenseTier = Engine::FabLicenseTier::NotApplicable;
        check.Expect(EvaluateFabLicenseGate(ccBy).Verdict == FabLicenseVerdict::Incomplete, "held CC-BY without attribution is incomplete");
        ccBy.AttributionText = "Rock by Someone";
        check.Expect(EvaluateFabLicenseGate(ccBy).Verdict == FabLicenseVerdict::Allowed, "held CC-BY with attribution is allowed");
        return check.Ok;
    }

    // ---- 4. open in the system browser ---------------------------------------------------------------------

    bool TestFabOpenInBrowserDecision()
    {
        Checker check { "open-in-browser" };
        const BrowserNavigationPolicy policy;
        constexpr std::string_view home = "https://www.fab.com/";

        struct Row
        {
            const char* Label;
            const char* Scheme;
            std::string Address;
            const char* Home;
            bool Valid;
            bool Current;
            const char* Url;
        };
        const std::string longPath = "www.fab.com/" + std::string(220, 'a');
        const std::string capPath = "www.fab.com/" + std::string(188, 'a'); // exactly 200 bytes: may have been truncated
        const Row table[] = {
            { "current Fab page", "https", "www.fab.com/listings/abc-123", home.data(), true, true, "https://www.fab.com/listings/abc-123" },
            { "Fab root", "https", "www.fab.com/", home.data(), true, true, "https://www.fab.com/" },
            { "current Epic sign-in page", "https", "www.epicgames.com/id/login", home.data(), true, true, "https://www.epicgames.com/id/login" },
            { "foreign host falls back to home", "https", "evil.example/x", home.data(), true, false, "https://www.fab.com/" },
            { "look-alike host", "https", "www.fab.com.evil.example/x", home.data(), true, false, "https://www.fab.com/" },
            { "http scheme", "http", "www.fab.com/x", home.data(), true, false, "https://www.fab.com/" },
            { "about page", "about", "blank", home.data(), true, false, "https://www.fab.com/" },
            { "no scheme yet", "", "", home.data(), true, false, "https://www.fab.com/" },
            { "empty address", "https", "", home.data(), true, false, "https://www.fab.com/" },
            { "port", "https", "www.fab.com:8443/x", home.data(), true, false, "https://www.fab.com/" },
            { "userinfo", "https", "user@www.fab.com/x", home.data(), true, false, "https://www.fab.com/" },
            { "query is never forwarded", "https", "www.fab.com/x?token=1", home.data(), true, false, "https://www.fab.com/" },
            { "fragment", "https", "www.fab.com/x#frag", home.data(), true, false, "https://www.fab.com/" },
            { "backslash", "https", "www.fab.com\\@evil.example/", home.data(), true, false, "https://www.fab.com/" },
            { "space", "https", "www.fab.com/a b", home.data(), true, false, "https://www.fab.com/" },
            { "control byte", "https", std::string("www.fab.com/a\nb"), home.data(), true, false, "https://www.fab.com/" },
            { "truncated display address", "https", longPath.substr(0, 200), home.data(), true, false, "https://www.fab.com/" },
            { "address at the cap", "https", capPath, home.data(), true, false, "https://www.fab.com/" },
            { "uppercase scheme text is not https", "HTTPS", "www.fab.com/x", home.data(), true, false, "https://www.fab.com/" },
            { "home outside the policy and no page", "https", "", "https://evil.example/", false, false, "" },
            { "home is http", "https", "", "http://www.fab.com/", false, false, "" },
            { "home is empty", "", "", "", false, false, "" },
            { "page falls back but home is invalid", "https", "evil.example/", "https://www.fab.com.evil.example/", false, false, "" },
        };
        for (const Row& row : table)
        {
            const FabOpenDecision decision = DecideFabOpenInBrowser(policy, row.Scheme, row.Address, row.Home);
            check.Expect(decision.Valid == row.Valid, std::string("validity: ") + row.Label);
            if (row.Valid)
            {
                check.Expect(decision.Url == row.Url && decision.UsedCurrentPage == row.Current && decision.Error.empty(),
                    std::string("url and source: ") + row.Label + " -> " + decision.Url);
                // The host handed to Engine::OpenExternalHttpsUrl is the exact host of the URL and passes its policy.
                check.Expect(Engine::IsAllowedExternalHttpsUrl(decision.Url, decision.Host),
                    std::string("Engine::OpenExternalHttpsUrl would accept it: ") + row.Label);
            }
            else
            {
                check.Expect(decision.Url.empty() && !decision.Error.empty() && !Contains(decision.Error, "http"),
                    std::string("an invalid decision explains itself without a URL: ") + row.Label);
            }
        }

        // A host the user granted for sign-in is a valid page to open; a revoked one falls back.
        BrowserNavigationPolicy granted;
        std::string error;
        check.Expect(granted.AddProviderHost("accounts.google.com", error), "grant fixture: " + error);
        FabOpenDecision decision = DecideFabOpenInBrowser(granted, "https", "accounts.google.com/o/oauth2/v2/auth", home);
        check.Expect(decision.Valid && decision.UsedCurrentPage && decision.Host == "accounts.google.com", "a granted sign-in host may be opened");
        decision = DecideFabOpenInBrowser(policy, "https", "accounts.google.com/o/oauth2/v2/auth", home);
        check.Expect(decision.Valid && !decision.UsedCurrentPage && decision.Host == "www.fab.com", "without the grant the home page opens");

        // Property over a deterministic spread of generated addresses. Oracle: independent of the
        // implementation, a valid result is an https URL with no query, fragment, backslash, port,
        // userinfo, whitespace or control byte whose host is one of the two built-in hosts.
        const std::vector<std::string> hosts = { "www.fab.com", "www.epicgames.com", "fab.com", "www.fab.com.evil.example", "evil.example",
            "WWW.FAB.COM", "www.fab.com:80", "u@www.fab.com", "www.epicgames.com." };
        const std::vector<std::string> paths = { "", "/", "/listings/a-b", "/id/login", "/a?b=c", "/a#b", "/a b", "/a\\b", "/\x01", "/%2f..", "//x", "/a/b/c" };
        size_t cases = 0;
        for (const char* scheme : { "https", "http", "", "ftp" })
        {
            for (const std::string& host : hosts)
            {
                for (const std::string& path : paths)
                {
                    ++cases;
                    const FabOpenDecision generated = DecideFabOpenInBrowser(policy, scheme, host + path, home);
                    check.Expect(generated.Valid, "the home page always remains a valid fallback");
                    const std::string& url = generated.Url;
                    check.Expect(url.rfind("https://", 0) == 0, "always https: " + url);
                    const std::string authorityAndPath = url.substr(8);
                    const size_t slash = authorityAndPath.find('/');
                    const std::string authority = authorityAndPath.substr(0, slash);
                    std::string lowered = authority;
                    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char character)
                    {
                        return static_cast<char>(character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
                    });
                    check.Expect(lowered == "www.fab.com" || lowered == "www.epicgames.com", "host is a built-in host: " + url);
                    check.Expect(url.find_first_of("?#\\ @") == std::string::npos, "no query, fragment, backslash, space or userinfo: " + url);
                    for (const char character : url)
                        check.Expect(static_cast<unsigned char>(character) >= 0x21 && static_cast<unsigned char>(character) < 0x7f, "printable ASCII only");
                    std::string host = generated.Host;
                    std::transform(host.begin(), host.end(), host.begin(), [](char character)
                    {
                        return static_cast<char>(character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
                    });
                    check.Expect(host == lowered, "Host is the URL's authority");
                }
            }
        }
        check.Expect(cases == 4 * hosts.size() * paths.size(), "the generated spread ran");
        return check.Ok;
    }

    // ---- 5. kill switch, page hints ----------------------------------------------------------------------------

    bool TestFabKillSwitchAndPageHintTables()
    {
        Checker check { "kill-switch-and-hints" };

        struct Switch
        {
            bool CommandLine;
            bool SettingEnabled;
            bool Enabled;
            FabBrowserDisabledReason Reason;
        };
        // --no-fab-browser always wins; otherwise the persisted setting decides.
        const Switch switches[] = {
            { false, true, true, FabBrowserDisabledReason::None },
            { false, false, false, FabBrowserDisabledReason::Setting },
            { true, true, false, FabBrowserDisabledReason::CommandLine },
            { true, false, false, FabBrowserDisabledReason::CommandLine },
        };
        for (const Switch& row : switches)
        {
            const FabBrowserAvailability availability = ResolveFabBrowserAvailability(row.CommandLine, row.SettingEnabled);
            check.Expect(availability.Enabled == row.Enabled && availability.Reason == row.Reason,
                std::string("resolution cli=") + (row.CommandLine ? "1" : "0") + " setting=" + (row.SettingEnabled ? "1" : "0"));
        }
        check.Expect(FabBrowserDisabledToken(FabBrowserDisabledReason::CommandLine) == "fab_browser_disabled_by_command_line"
                && FabBrowserDisabledToken(FabBrowserDisabledReason::Setting) == "fab_browser_disabled_by_setting"
                && FabBrowserDisabledToken(FabBrowserDisabledReason::None).empty(),
            "typed-control tokens");
        check.Expect(Contains(FabBrowserDisabledText(FabBrowserDisabledReason::CommandLine), "--no-fab-browser")
                && Contains(FabBrowserDisabledText(FabBrowserDisabledReason::Setting), "Settings")
                && FabBrowserDisabledText(FabBrowserDisabledReason::None).empty(),
            "menu and log text names the cause");

        for (int status = 0; status <= 600; ++status)
            check.Expect(IsSecurityCheckStatus(status) == (status == 403 || status == 503), "status " + std::to_string(status));
        check.Expect(!IsSecurityCheckStatus(-1) && !IsSecurityCheckStatus(100000), "out-of-range statuses are not security checks");

        const FabPageHint order[] = { FabPageHint::None, FabPageHint::LoadError, FabPageHint::NavigationDenied, FabPageHint::SecurityCheckLikely };
        for (size_t current = 0; current < 4; ++current)
        {
            for (size_t incoming = 0; incoming < 4; ++incoming)
                check.Expect(StrongerFabPageHint(order[current], order[incoming]) == order[std::max(current, incoming)],
                    "stronger hint " + std::to_string(current) + " vs " + std::to_string(incoming));
        }
        check.Expect(FabPageHintName(FabPageHint::None) == "none" && FabPageHintName(FabPageHint::LoadError) == "load-error"
                && FabPageHintName(FabPageHint::NavigationDenied) == "navigation-denied"
                && FabPageHintName(FabPageHint::SecurityCheckLikely) == "security-check-likely",
            "hint tokens");
        return check.Ok;
    }
}
