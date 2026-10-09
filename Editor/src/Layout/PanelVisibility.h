#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Pure codec for the workspace-global panel visibility file
// (`output/editor/panel-visibility.spiralsettings`, a sibling of the
// navigation-preset file; never part of a project). Standard C++ only so it
// compiles into EngineTests.
//
// Format version 1 is line oriented and strict:
//
//   SpiralEditorPanels 1
//   Panel <id> <0|1>        (one line per panel, each id at most once)
//
// Lines end in a single LF, the file ends in a LF, ids are the stable ASCII
// panel ids the caller supplies, and any deviation (unknown id, duplicate id,
// CRLF, extra token, blank line, other version, oversize) rejects the whole
// file so the caller falls back to its defaults without touching the file.
// A panel missing from a valid file keeps its default (visible), so adding a
// panel in a later release does not discard what a user already hid.
namespace SpiralEditor::Layout
{
    inline constexpr int kPanelVisibilityFormatVersion = 1;
    inline constexpr size_t kMaximumPanelVisibilityBytes = 4096;

    struct PanelVisibility
    {
        std::string Id;
        bool Visible = true;
    };

    // Canonical text for `panels` in the given order.
    std::string FormatPanelVisibility(const std::vector<PanelVisibility>& panels);

    // Parses `text` against the ids the Editor knows. On success `outPanels`
    // holds one entry per known id in knownIds order (missing ids visible); on
    // failure `outPanels` is left unchanged and `outError` names the first
    // violated rule.
    bool ParsePanelVisibility(std::string_view text, const std::vector<std::string>& knownIds,
        std::vector<PanelVisibility>& outPanels, std::string& outError);
}
