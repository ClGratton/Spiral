#pragma once

#include "LayoutFile.h"

#include <span>
#include <string>
#include <string_view>

namespace SpiralEditor
{
    struct LayoutMigrationResult
    {
        // Always a valid store: the default workspace, carrying the legacy dock layout
        // when one was adopted.
        WorkspaceStore Store;
        bool AdoptedImGuiIni = false;
        // True when the legacy settings text matched the format-1 grammar exactly.
        bool RecognisedSettings = false;
        // The ViewportNavigationPreset token of recognised settings (still to be
        // validated by the settings owner); empty otherwise.
        std::string NavigationPreset;
    };

    // One-time import from the two files the Editor persisted before layouts existed:
    //  - output/editor/engine-settings.spiralsettings (format 1), which holds only
    //    "SpiralEditorSettings 1 ViewportNavigationPreset <preset>" and no layout;
    //  - imgui.ini, which ImGui autosaves into the working directory.
    // Pass empty text for an absent file. The sources are read-only inputs; nothing
    // here touches the filesystem. A dock layout that fails IsValidImGuiIni (control
    // characters or over the size limit) is ignored and the built-in default layout
    // is used, so a damaged legacy file can never block start-up.
    LayoutMigrationResult MigrateLegacyEditorState(std::string_view legacySettingsText,
        std::string_view legacyImGuiIni, std::span<const std::string_view> panelIds);
}
