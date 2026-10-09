#include "PanelVisibility.h"

#include <algorithm>
#include <utility>

namespace SpiralEditor::Layout
{
    namespace
    {
        constexpr std::string_view kMagic = "SpiralEditorPanels";

        bool IsIdCharacter(char character)
        {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9') || character == '.' || character == '_'
                || character == '-';
        }

        bool NextLine(std::string_view& remaining, std::string_view& outLine)
        {
            const size_t newline = remaining.find('\n');
            if (newline == std::string_view::npos)
                return false;
            outLine = remaining.substr(0, newline);
            remaining.remove_prefix(newline + 1);
            return true;
        }
    }

    std::string FormatPanelVisibility(const std::vector<PanelVisibility>& panels)
    {
        std::string text;
        text += kMagic;
        text += ' ';
        text += std::to_string(kPanelVisibilityFormatVersion);
        text += '\n';
        for (const PanelVisibility& panel : panels)
        {
            text += "Panel ";
            text += panel.Id;
            text += panel.Visible ? " 1\n" : " 0\n";
        }
        return text;
    }

    bool ParsePanelVisibility(std::string_view text, const std::vector<std::string>& knownIds,
        std::vector<PanelVisibility>& outPanels, std::string& outError)
    {
        if (text.empty() || text.size() > kMaximumPanelVisibilityBytes)
        {
            outError = text.empty() ? "empty_file" : "oversized_file";
            return false;
        }

        std::string_view remaining = text;
        std::string_view line;
        if (!NextLine(remaining, line))
        {
            outError = "missing_header_line";
            return false;
        }
        if (line != std::string(kMagic) + " " + std::to_string(kPanelVisibilityFormatVersion))
        {
            outError = "unsupported_header";
            return false;
        }

        std::vector<PanelVisibility> parsed;
        parsed.reserve(knownIds.size());
        for (const std::string& id : knownIds)
            parsed.push_back({ id, true });
        std::vector<bool> seen(knownIds.size(), false);

        while (!remaining.empty())
        {
            if (!NextLine(remaining, line))
            {
                outError = "missing_final_newline";
                return false;
            }
            constexpr std::string_view prefix = "Panel ";
            if (line.size() < prefix.size() + 1 + 2 || line.substr(0, prefix.size()) != prefix)
            {
                outError = "malformed_panel_line";
                return false;
            }
            const std::string_view rest = line.substr(prefix.size());
            const size_t separator = rest.rfind(' ');
            if (separator == std::string_view::npos || separator == 0 || separator + 2 != rest.size())
            {
                outError = "malformed_panel_line";
                return false;
            }
            const std::string_view id = rest.substr(0, separator);
            const char flag = rest.back();
            if (!std::all_of(id.begin(), id.end(), IsIdCharacter) || (flag != '0' && flag != '1'))
            {
                outError = "malformed_panel_line";
                return false;
            }
            const auto known = std::find(knownIds.begin(), knownIds.end(), id);
            if (known == knownIds.end())
            {
                outError = "unknown_panel_id";
                return false;
            }
            const size_t index = static_cast<size_t>(known - knownIds.begin());
            if (seen[index])
            {
                outError = "duplicate_panel_id";
                return false;
            }
            seen[index] = true;
            parsed[index].Visible = flag == '1';
        }

        outPanels = std::move(parsed);
        outError.clear();
        return true;
    }
}
