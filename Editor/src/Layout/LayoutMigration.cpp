#include "LayoutMigration.h"

#include <algorithm>
#include <vector>

namespace SpiralEditor
{
    namespace
    {
        bool IsSpace(char value)
        {
            return value == ' ' || value == '\t' || value == '\n' || value == '\r';
        }

        std::vector<std::string_view> SplitWhitespace(std::string_view text)
        {
            std::vector<std::string_view> tokens;
            size_t index = 0;
            while (index < text.size())
            {
                while (index < text.size() && IsSpace(text[index]))
                    ++index;
                const size_t start = index;
                while (index < text.size() && !IsSpace(text[index]))
                    ++index;
                if (index > start)
                    tokens.push_back(text.substr(start, index - start));
            }
            return tokens;
        }

        bool IsPresetToken(std::string_view token)
        {
            return !token.empty() && token.size() <= 32 && std::all_of(token.begin(), token.end(), [](char value)
            {
                return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9')
                    || value == '_';
            });
        }
    }

    LayoutMigrationResult MigrateLegacyEditorState(std::string_view legacySettingsText,
        std::string_view legacyImGuiIni, std::span<const std::string_view> panelIds)
    {
        LayoutMigrationResult result;
        result.Store = MakeDefaultStore(panelIds);

        // Mirrors the Editor's format-1 reader: exactly four whitespace-separated tokens.
        if (legacySettingsText.size() <= 4096)
        {
            const std::vector<std::string_view> tokens = SplitWhitespace(legacySettingsText);
            if (tokens.size() == 4 && tokens[0] == "SpiralEditorSettings" && tokens[1] == "1"
                && tokens[2] == "ViewportNavigationPreset" && IsPresetToken(tokens[3]))
            {
                result.RecognisedSettings = true;
                result.NavigationPreset = std::string(tokens[3]);
            }
        }

        if (!legacyImGuiIni.empty() && IsValidImGuiIni(legacyImGuiIni))
        {
            result.Store.Workspaces.front().ImGuiIni = std::string(legacyImGuiIni);
            result.AdoptedImGuiIni = true;
        }
        return result;
    }
}
