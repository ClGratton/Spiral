#pragma once

#include "BrowserNavigationPolicy.h"
#include "Engine/Core/Base.h"

#include <filesystem>
#include <string>
#include <string_view>

// The pure half of the Fab terms safeguards: the non-blocking notice (its text,
// the versioned dismissal codec and file), the kill switch, the "Open in browser"
// decision, and the page-problem hint. No CEF, ImGui, GLFW, or global state. The
// dismissal file holds a version number only, never a URL, cookie, or account
// detail, and is not a legal acceptance on anyone's behalf.
//
// Source claims behind these rules (research of 2026-10-10, not legal advice):
// Fab Terms of Service 1.8 asks that Fab be accessed through interfaces Epic
// provides or approves and bans automated access. The project owner reads that as
// a rule against replacing Epic's interface with a custom one; the panel shows
// Epic's own Fab website unmodified, so the browser starts at once and the notice
// never blocks it. The panel stays hand-driven only and always offers a
// system-browser path.
namespace Fab
{
    namespace FabDisclosureLimits
    {
        constexpr Engine::u32 kSchema = 1;
        // Bump when the notice text changes in substance: it is shown again.
        constexpr Engine::u32 kNoticeVersion = 1;
        constexpr size_t kMaximumFileBytes = 256;
    }

    // Beside the browser profile directory (never inside it: sign-out deletes the
    // profile and must not discard the dismissal; never inside a project).
    inline constexpr std::string_view kFabNoticeDismissalFileName = "FabNoticeDismissed.json";

    // The one-line notice shown above the page until the user dismisses it.
    inline constexpr std::string_view kFabNoticeText =
        "Fab.com is Epic Games' service shown here unmodified. Spiral is not affiliated with Epic. Sign in directly on Epic's pages; "
        "Spiral never sees your password or cookies.";

    // Shown for a load error, a denied navigation, or a likely security check.
    inline constexpr std::string_view kFabPageHintText =
        "If Fab shows a security check that does not work in this panel, use Open in browser, download there, then drop the file "
        "onto the Editor.";

    // ---- dismissal codec and file --------------------------------------------------------

    enum class FabNoticeDismissalStatus
    {
        // The stored notice version equals the current one: the notice stays hidden.
        Dismissed,
        Missing,
        // A well-formed record of a different notice version: the notice is shown again.
        Outdated,
        // Unreadable, not a regular file, or not the exact grammar. Fails closed: the notice is shown.
        Rejected
    };

    // {"schema":1,"notice":1} plus a newline.
    std::string EncodeFabNoticeDismissal(Engine::u32 noticeVersion);

    // The only grammar accepted: one JSON object with exactly the keys "schema"
    // (equal to kSchema) and "notice" (an integer 1..999999999 without leading
    // zeros), in either order, JSON whitespace allowed between tokens. Anything
    // else (unknown or repeated key, string or fractional value, escapes, trailing
    // text, over kMaximumFileBytes) is rejected as a whole and `noticeVersion` is
    // left unchanged. The reason never echoes the content.
    bool DecodeFabNoticeDismissal(std::string_view text, Engine::u32& noticeVersion, std::string& error);

    FabNoticeDismissalStatus ClassifyFabNoticeDismissal(Engine::u32 storedVersion, Engine::u32 currentVersion);

    // Reads `path`. A missing file is Missing, not an error. The reason in `error`
    // (set only for Rejected) never contains the path.
    FabNoticeDismissalStatus LoadFabNoticeDismissalFile(
        const std::filesystem::path& path, Engine::u32 currentVersion, std::string& error);

    // Writes the record through a temporary sibling (owner read/write only, 0600)
    // and a rename. The directory must exist. A non-regular file at `path` (for
    // example a symbolic link) is refused, and a stale temporary is replaced,
    // never followed. On failure the previous file is untouched.
    bool SaveFabNoticeDismissalFile(const std::filesystem::path& path, Engine::u32 noticeVersion, std::string& error);

    // Where the dismissal lives: `explicitFile` when given, else
    // kFabNoticeDismissalFileName in the parent of `profileDirectory` (a sibling of
    // the profile, not inside it). Empty when neither yields a path.
    std::filesystem::path FabNoticeDismissalPath(const std::filesystem::path& profileDirectory, const std::filesystem::path& explicitFile);

    // Whether the notice line is drawn: unless the stored record is for this notice
    // version or the user dismissed it in this session. A rejected record shows it.
    bool FabNoticeVisible(FabNoticeDismissalStatus stored, bool dismissedThisSession);

    // ---- kill switch ----------------------------------------------------------------

    enum class FabBrowserDisabledReason
    {
        None,
        CommandLine,
        Setting
    };

    struct FabBrowserAvailability
    {
        bool Enabled = true;
        FabBrowserDisabledReason Reason = FabBrowserDisabledReason::None;
    };

    // --no-fab-browser always wins; otherwise the persisted Editor setting decides.
    FabBrowserAvailability ResolveFabBrowserAvailability(bool disabledOnCommandLine, bool enabledBySetting);
    // Human text for the menu, the panel, and the log.
    std::string_view FabBrowserDisabledText(FabBrowserDisabledReason reason);
    // Stable typed-control rejection token.
    std::string_view FabBrowserDisabledToken(FabBrowserDisabledReason reason);

    // ---- open in the system browser ---------------------------------------------------

    struct FabOpenDecision
    {
        bool Valid = false;
        // Full https URL and its exact host, as Engine::OpenExternalHttpsUrl takes them.
        std::string Url;
        std::string Host;
        // False when the home page is opened instead of the current page.
        bool UsedCurrentPage = false;
        // Set when Valid is false; never contains a URL.
        std::string Error;
    };

    // Chooses what the "Open in browser" button opens. The current page counts when
    // `scheme` is "https" and "https://" + `currentDisplayAddress` (the host and path
    // the panel reports, without query or fragment) passes `policy` as a top-level
    // navigation and Engine::IsAllowedExternalHttpsUrl for its own host; an address
    // that was truncated, altered for display, or has a port or userinfo does not
    // count. Otherwise `homeUrl` is opened when it passes `policy`. Never reads or
    // returns a query string, so no page identifier or token leaves the panel.
    FabOpenDecision DecideFabOpenInBrowser(const BrowserNavigationPolicy& policy, std::string_view scheme,
        std::string_view currentDisplayAddress, std::string_view homeUrl);

    // ---- page problems ------------------------------------------------------------------

    enum class FabPageHint
    {
        None,
        // A main-frame load error or a browser content failure.
        LoadError,
        // The page asked for a navigation the policy refused.
        NavigationDenied,
        // The main frame finished with HTTP 403 or 503, the status a managed security
        // challenge commonly uses. Spiral never detects, solves, or scripts a challenge.
        SecurityCheckLikely
    };

    bool IsSecurityCheckStatus(int httpStatus);
    // Stable token for diagnostics.
    std::string_view FabPageHintName(FabPageHint hint);
    // A more specific cause replaces a vaguer one; None never replaces anything.
    FabPageHint StrongerFabPageHint(FabPageHint current, FabPageHint incoming);
}
