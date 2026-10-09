#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SpiralEditor
{
    constexpr Engine::u32 kLayoutFormatVersion = 1;
    constexpr size_t kMaxLayoutFileBytes = 4ull * 1024ull * 1024ull;
    constexpr size_t kMaxWorkspaces = 16;
    constexpr size_t kMaxPanelsPerWorkspace = 64;
    constexpr size_t kMaxDetachedPerWorkspace = 16;
    constexpr size_t kMaxImGuiIniBytes = 192ull * 1024ull;
    constexpr size_t kMaxWorkspaceNameBytes = 48;
    constexpr size_t kMaxPanelIdBytes = 48;
    constexpr size_t kMaxDockRefBytes = 32;
    constexpr size_t kMaxMonitorHintBytes = 64;
    constexpr std::int32_t kMaxDetachedCoordinate = 1000000;
    constexpr Engine::u32 kMaxDetachedExtent = 32768;
    constexpr std::string_view kDefaultWorkspaceName = "Default";

    struct PanelRecord
    {
        std::string Id; // stable registry id, e.g. "scene.hierarchy"
        bool Visible = true;
        // Opaque reference to the panel's dock node inside ImGuiIni; empty when none.
        std::string DockRef;

        bool operator==(const PanelRecord&) const = default;
    };

    // Advisory placement of a panel that was dragged out into its own OS window. The
    // compositor may clamp it, so it is a hint, never an assertion.
    struct DetachedViewportRecord
    {
        std::string PanelId; // must name a PanelRecord of the same workspace
        std::int32_t X = 0;
        std::int32_t Y = 0;
        Engine::u32 Width = 0;
        Engine::u32 Height = 0;
        std::string Monitor; // OS/compositor display name; empty when unknown

        bool operator==(const DetachedViewportRecord&) const = default;
    };

    struct Workspace
    {
        std::string Name;
        // Detached windows are restored on launch only when the user saved the
        // workspace with this set; see DetachedViewportsToRestore.
        bool RestoreDetached = false;
        std::vector<PanelRecord> Panels;
        std::vector<DetachedViewportRecord> Detached;
        // Opaque ImGui::SaveIniSettingsToMemory text. Empty means "no saved dock
        // layout": the Editor builds its built-in default dock layout.
        std::string ImGuiIni;

        bool operator==(const Workspace&) const = default;
    };

    struct WorkspaceStore
    {
        std::string Active; // names one of Workspaces
        std::vector<Workspace> Workspaces;

        bool operator==(const WorkspaceStore&) const = default;
    };

    // Names are ASCII letters, digits, space, '_', '-', '.', '(' and ')'; they start
    // with a letter or digit, end with a letter, digit, or ')', have no consecutive
    // spaces, and are not Windows device names. They are therefore safe as file names
    // on every supported platform. Names are unique ignoring ASCII case.
    bool IsValidWorkspaceName(std::string_view name);
    // [a-z][a-z0-9._-]*, up to kMaxPanelIdBytes.
    bool IsValidPanelId(std::string_view id);
    // Empty, or up to kMaxDockRefBytes of [A-Za-z0-9_.:#+-].
    bool IsValidDockRef(std::string_view reference);
    // Empty, or up to kMaxMonitorHintBytes of [A-Za-z0-9_.:#+-].
    bool IsValidMonitorHint(std::string_view hint);
    // Maps every other character of an OS display name to '_' and cuts the result to
    // kMaxMonitorHintBytes, so a hint can always be stored.
    std::string SanitizeMonitorHint(std::string_view displayName);
    // At most kMaxImGuiIniBytes of text without control characters other than newline and tab.
    bool IsValidImGuiIni(std::string_view ini);

    // Checks one workspace, then the whole store (names unique ignoring case, active
    // exists, caps, panel ids unique, every detached record names a panel and carries
    // a sane geometry, at most one detached record per panel). Serialization and
    // parsing share these rules.
    bool ValidateWorkspace(const Workspace& workspace, std::string& outError);
    bool ValidateWorkspaceStore(const WorkspaceStore& store, std::string& outError);

    // Canonical, deterministic text; Parse(Serialize(x)) == x and Serialize(Parse(t)) == t
    // for every accepted t. The last line is a SHA-256 over everything before it.
    // Both functions are failure-atomic: on false the output argument is untouched.
    bool SerializeWorkspaceStore(const WorkspaceStore& store, std::string& outText, std::string& outError);
    // Strict: exact grammar and key order, canonical numbers, single spaces, LF only,
    // no trailing bytes, hard size and count limits checked before any allocation
    // they bound. A future or unknown version is rejected, never guessed at.
    bool ParseWorkspaceStore(std::string_view text, WorkspaceStore& outStore, std::string& outError);

    enum class LayoutLoadStatus
    {
        Loaded,
        Missing,
        Rejected
    };

    // Write goes through Engine::WriteFileAtomically, so a failed save leaves the
    // previous file byte for byte. Load reads at most kMaxLayoutFileBytes + 1 bytes.
    // Missing means the file does not exist (first run: migrate or use the default);
    // Rejected means it exists but is unusable (the Editor says so and uses the default).
    bool SaveWorkspaceStore(const std::filesystem::path& path, const WorkspaceStore& store, std::string& outError);
    LayoutLoadStatus LoadWorkspaceStore(const std::filesystem::path& path, WorkspaceStore& outStore, std::string& outError);

    // ----- pure operations on the data -----

    // Every valid, distinct id of `panelIds` (up to kMaxPanelsPerWorkspace) visible,
    // no dock references, no saved dock layout, no detached windows.
    Workspace MakeDefaultWorkspace(std::span<const std::string_view> panelIds);
    // A store holding only the default workspace, which is also the active one.
    WorkspaceStore MakeDefaultStore(std::span<const std::string_view> panelIds);
    // Reset to default: keeps the name, replaces everything else with MakeDefaultWorkspace.
    void ResetWorkspaceToDefault(Workspace& workspace, std::span<const std::string_view> panelIds);

    const Workspace* FindWorkspace(const WorkspaceStore& store, std::string_view name);
    const PanelRecord* FindPanel(const Workspace& workspace, std::string_view panelId);
    // Replaces the workspace of the same name (ignoring case) in place or appends it.
    // False, with the store untouched, when the workspace is invalid or the store is full.
    bool UpsertWorkspace(WorkspaceStore& store, Workspace workspace, std::string& outError);
    // The default workspace, unknown names, and the last workspace cannot be removed. Removing the active
    // workspace makes the default one active.
    bool RemoveWorkspace(WorkspaceStore& store, std::string_view name);
    bool SetActiveWorkspace(WorkspaceStore& store, std::string_view name);

    // Brings a loaded workspace in line with the panels the running Editor has: panels
    // the Editor no longer registers (and their detached records) are dropped, and new
    // panels are appended visible. Returns true when anything changed.
    bool ReconcilePanels(Workspace& workspace, std::span<const std::string_view> panelIds);
    // The detached windows to recreate on launch: none unless the workspace opted in.
    std::span<const DetachedViewportRecord> DetachedViewportsToRestore(const Workspace& workspace);
}
