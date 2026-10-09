#include "LayoutFile.h"

#include "Engine/Core/AtomicFile.h"
#include "Engine/Core/Sha256.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <system_error>
#include <unordered_set>

namespace SpiralEditor
{
    namespace
    {
        using Engine::u32;

        constexpr std::string_view kMagic = "SpiralLayout";
        constexpr std::string_view kChecksumKey = "checksum ";
        constexpr size_t kChecksumLineBytes = kChecksumKey.size() + 64 + 1;
        constexpr size_t kMaxLineBytes = 256;
        constexpr std::string_view kNone = "~";

        bool IsAsciiAlnum(char value)
        {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
        }

        char Fold(char value)
        {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        }

        bool EqualsFolded(std::string_view left, std::string_view right)
        {
            return left.size() == right.size()
                && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) { return Fold(a) == Fold(b); });
        }

        bool IsDeviceName(std::string_view name)
        {
            const std::string_view stem = name.substr(0, name.find('.'));
            if (stem.size() == 3)
            {
                for (const std::string_view device : { "con", "prn", "aux", "nul" })
                {
                    if (EqualsFolded(stem, device))
                        return true;
                }
            }
            if (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9')
                return EqualsFolded(stem.substr(0, 3), "com") || EqualsFolded(stem.substr(0, 3), "lpt");
            return false;
        }

        bool IsTokenCharacter(char value)
        {
            return IsAsciiAlnum(value) || value == '_' || value == '.' || value == ':' || value == '#' || value == '+'
                || value == '-';
        }

        bool IsToken(std::string_view value, size_t maximum)
        {
            return value.size() <= maximum && std::all_of(value.begin(), value.end(), IsTokenCharacter);
        }

        bool Fail(std::string& outError, std::string message)
        {
            outError = std::move(message);
            return false;
        }

        // ----- strict token readers -----

        bool ParseU32(std::string_view token, u32& outValue)
        {
            if (token.empty() || token.size() > 10)
                return false;
            u32 value = 0;
            const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (error != std::errc {} || end != token.data() + token.size() || token != std::to_string(value))
                return false;
            outValue = value;
            return true;
        }

        bool ParseI32(std::string_view token, std::int32_t& outValue)
        {
            if (token.empty() || token.size() > 11)
                return false;
            std::int32_t value = 0;
            const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (error != std::errc {} || end != token.data() + token.size() || token != std::to_string(value))
                return false;
            outValue = value;
            return true;
        }

        bool ParseFlag(std::string_view token, bool& outValue)
        {
            if (token != "0" && token != "1")
                return false;
            outValue = token == "1";
            return true;
        }

        // `token` must be exactly "<key>=<value>".
        bool KeyValue(std::string_view token, std::string_view key, std::string_view& outValue)
        {
            if (token.size() <= key.size() + 1 || !token.starts_with(key) || token[key.size()] != '=')
                return false;
            outValue = token.substr(key.size() + 1);
            return true;
        }

        // Single spaces only: no leading, trailing, or doubled separators.
        bool SplitTokens(std::string_view line, std::vector<std::string_view>& outTokens)
        {
            outTokens.clear();
            while (true)
            {
                const size_t space = line.find(' ');
                const std::string_view token = line.substr(0, space);
                if (token.empty())
                    return false;
                outTokens.push_back(token);
                if (space == std::string_view::npos)
                    return true;
                line.remove_prefix(space + 1);
            }
        }

        std::string DockText(std::string_view reference)
        {
            return std::string(reference.empty() ? kNone : reference);
        }

        class Reader
        {
        public:
            explicit Reader(std::string_view text) : m_Text(text) {}

            bool AtEnd() const { return m_Position == m_Text.size(); }

            bool ReadLine(std::string_view& outLine)
            {
                const std::string_view window = m_Text.substr(m_Position, kMaxLineBytes + 1);
                const size_t newline = window.find('\n');
                if (newline == std::string_view::npos)
                    return false;
                outLine = window.substr(0, newline);
                m_Position += newline + 1;
                return true;
            }

            // `count` bytes followed by a newline.
            bool ReadBlock(size_t count, std::string_view& outBytes)
            {
                if (m_Text.size() - m_Position < count + 1 || m_Text[m_Position + count] != '\n')
                    return false;
                outBytes = m_Text.substr(m_Position, count);
                m_Position += count + 1;
                return true;
            }

        private:
            std::string_view m_Text;
            size_t m_Position = 0;
        };

        bool ParseQuotedName(std::string_view line, std::string_view prefix, bool wholeLine, std::string_view& outName,
            std::string_view& outRest)
        {
            if (!line.starts_with(prefix))
                return false;
            line.remove_prefix(prefix.size());
            const size_t close = line.find('"');
            if (close == std::string_view::npos)
                return false;
            outName = line.substr(0, close);
            outRest = line.substr(close + 1);
            return wholeLine ? outRest.empty() : (!outRest.empty() && outRest.front() == ' ');
        }

        bool ParseWorkspaceBody(Reader& reader, std::string_view headerLine, Workspace& outWorkspace, std::string& outError)
        {
            std::string_view name;
            std::string_view rest;
            if (!ParseQuotedName(headerLine, "workspace \"", false, name, rest))
                return Fail(outError, "workspace line is malformed");
            outWorkspace.Name = std::string(name);

            std::vector<std::string_view> tokens;
            if (!SplitTokens(rest.substr(1), tokens) || tokens.size() != 4)
                return Fail(outError, "workspace line has the wrong fields");
            std::string_view restoreText, panelsText, detachedText, iniText;
            u32 panelCount = 0;
            u32 detachedCount = 0;
            u32 iniBytes = 0;
            if (!KeyValue(tokens[0], "restore_detached", restoreText) || !ParseFlag(restoreText, outWorkspace.RestoreDetached)
                || !KeyValue(tokens[1], "panels", panelsText) || !ParseU32(panelsText, panelCount)
                || !KeyValue(tokens[2], "detached", detachedText) || !ParseU32(detachedText, detachedCount)
                || !KeyValue(tokens[3], "ini", iniText) || !ParseU32(iniText, iniBytes))
            {
                return Fail(outError, "workspace line fields are malformed or out of order");
            }
            if (panelCount > kMaxPanelsPerWorkspace || detachedCount > kMaxDetachedPerWorkspace || iniBytes > kMaxImGuiIniBytes)
                return Fail(outError, "workspace exceeds a panel, detached window, or dock layout limit");

            std::string_view line;
            for (u32 index = 0; index < panelCount; ++index)
            {
                std::string_view visibleText, dockText;
                PanelRecord panel;
                if (!reader.ReadLine(line) || !SplitTokens(line, tokens) || tokens.size() != 4 || tokens[0] != "panel"
                    || !KeyValue(tokens[2], "visible", visibleText) || !ParseFlag(visibleText, panel.Visible)
                    || !KeyValue(tokens[3], "dock", dockText))
                {
                    return Fail(outError, "panel line is malformed or missing");
                }
                panel.Id = std::string(tokens[1]);
                panel.DockRef = dockText == kNone ? std::string() : std::string(dockText);
                outWorkspace.Panels.push_back(std::move(panel));
            }

            for (u32 index = 0; index < detachedCount; ++index)
            {
                std::string_view xText, yText, widthText, heightText, monitorText;
                DetachedViewportRecord record;
                if (!reader.ReadLine(line) || !SplitTokens(line, tokens) || tokens.size() != 7 || tokens[0] != "detached"
                    || !KeyValue(tokens[2], "x", xText) || !ParseI32(xText, record.X)
                    || !KeyValue(tokens[3], "y", yText) || !ParseI32(yText, record.Y)
                    || !KeyValue(tokens[4], "w", widthText) || !ParseU32(widthText, record.Width)
                    || !KeyValue(tokens[5], "h", heightText) || !ParseU32(heightText, record.Height)
                    || !KeyValue(tokens[6], "monitor", monitorText))
                {
                    return Fail(outError, "detached window line is malformed or missing");
                }
                record.PanelId = std::string(tokens[1]);
                record.Monitor = monitorText == kNone ? std::string() : std::string(monitorText);
                outWorkspace.Detached.push_back(std::move(record));
            }

            std::string_view ini;
            if (!reader.ReadBlock(iniBytes, ini))
                return Fail(outError, "dock layout block is truncated or unterminated");
            outWorkspace.ImGuiIni = std::string(ini);
            return true;
        }
    }

    bool IsValidWorkspaceName(std::string_view name)
    {
        if (name.empty() || name.size() > kMaxWorkspaceNameBytes || !IsAsciiAlnum(name.front()))
            return false;
        if (!IsAsciiAlnum(name.back()) && name.back() != ')')
            return false;
        for (size_t index = 0; index < name.size(); ++index)
        {
            const char value = name[index];
            const bool allowed = IsAsciiAlnum(value) || value == ' ' || value == '_' || value == '-' || value == '.'
                || value == '(' || value == ')';
            if (!allowed || (value == ' ' && index + 1 < name.size() && name[index + 1] == ' '))
                return false;
        }
        return !IsDeviceName(name);
    }

    bool IsValidPanelId(std::string_view id)
    {
        if (id.empty() || id.size() > kMaxPanelIdBytes || id.front() < 'a' || id.front() > 'z')
            return false;
        return std::all_of(id.begin(), id.end(), [](char value)
        {
            return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '.' || value == '_'
                || value == '-';
        });
    }

    bool IsValidDockRef(std::string_view reference)
    {
        return IsToken(reference, kMaxDockRefBytes);
    }

    bool IsValidMonitorHint(std::string_view hint)
    {
        return IsToken(hint, kMaxMonitorHintBytes);
    }

    std::string SanitizeMonitorHint(std::string_view displayName)
    {
        std::string result;
        for (const char value : displayName.substr(0, kMaxMonitorHintBytes))
            result.push_back(IsTokenCharacter(value) ? value : '_');
        return result;
    }

    bool IsValidImGuiIni(std::string_view ini)
    {
        if (ini.size() > kMaxImGuiIniBytes)
            return false;
        return std::all_of(ini.begin(), ini.end(), [](char value)
        {
            const unsigned char byte = static_cast<unsigned char>(value);
            return byte == '\n' || byte == '\t' || (byte >= 0x20 && byte != 0x7F);
        });
    }

    bool ValidateWorkspace(const Workspace& workspace, std::string& outError)
    {
        if (!IsValidWorkspaceName(workspace.Name))
            return Fail(outError, "workspace name is not valid");
        const std::string where = "workspace \"" + workspace.Name + "\": ";
        if (workspace.Panels.size() > kMaxPanelsPerWorkspace)
            return Fail(outError, where + "too many panels");
        if (workspace.Detached.size() > kMaxDetachedPerWorkspace)
            return Fail(outError, where + "too many detached windows");
        if (!IsValidImGuiIni(workspace.ImGuiIni))
            return Fail(outError, where + "dock layout text is too large or contains control characters");

        std::unordered_set<std::string_view> panelIds;
        for (const PanelRecord& panel : workspace.Panels)
        {
            if (!IsValidPanelId(panel.Id))
                return Fail(outError, where + "panel id is not valid");
            if (!IsValidDockRef(panel.DockRef))
                return Fail(outError, where + "dock reference of panel \"" + panel.Id + "\" is not valid");
            if (!panelIds.insert(panel.Id).second)
                return Fail(outError, where + "duplicate panel \"" + panel.Id + "\"");
        }

        std::unordered_set<std::string_view> detachedIds;
        for (const DetachedViewportRecord& record : workspace.Detached)
        {
            if (!panelIds.contains(record.PanelId))
                return Fail(outError, where + "detached window names unknown panel \"" + record.PanelId + "\"");
            if (!detachedIds.insert(record.PanelId).second)
                return Fail(outError, where + "duplicate detached window for panel \"" + record.PanelId + "\"");
            if (record.X < -kMaxDetachedCoordinate || record.X > kMaxDetachedCoordinate
                || record.Y < -kMaxDetachedCoordinate || record.Y > kMaxDetachedCoordinate)
            {
                return Fail(outError, where + "detached window position is out of range");
            }
            if (record.Width == 0 || record.Width > kMaxDetachedExtent || record.Height == 0 || record.Height > kMaxDetachedExtent)
                return Fail(outError, where + "detached window size is out of range");
            if (!IsValidMonitorHint(record.Monitor))
                return Fail(outError, where + "detached window monitor hint is not valid");
        }
        return true;
    }

    bool ValidateWorkspaceStore(const WorkspaceStore& store, std::string& outError)
    {
        if (store.Workspaces.empty() || store.Workspaces.size() > kMaxWorkspaces)
            return Fail(outError, "a layout file holds between 1 and 16 workspaces");
        for (size_t index = 0; index < store.Workspaces.size(); ++index)
        {
            if (!ValidateWorkspace(store.Workspaces[index], outError))
                return false;
            for (size_t other = 0; other < index; ++other)
            {
                if (EqualsFolded(store.Workspaces[index].Name, store.Workspaces[other].Name))
                    return Fail(outError, "duplicate workspace name \"" + store.Workspaces[index].Name + "\"");
            }
        }
        const bool activeExists = std::any_of(store.Workspaces.begin(), store.Workspaces.end(),
            [&](const Workspace& workspace) { return workspace.Name == store.Active; });
        if (!activeExists)
            return Fail(outError, "the active workspace does not exist");
        return true;
    }

    bool SerializeWorkspaceStore(const WorkspaceStore& store, std::string& outText, std::string& outError)
    {
        std::string error;
        if (!ValidateWorkspaceStore(store, error))
            return Fail(outError, std::move(error));

        std::string text = std::string(kMagic) + ' ' + std::to_string(kLayoutFormatVersion) + '\n';
        text += "active \"" + store.Active + "\"\n";
        text += "workspaces " + std::to_string(store.Workspaces.size()) + '\n';
        for (const Workspace& workspace : store.Workspaces)
        {
            text += "workspace \"" + workspace.Name + "\" restore_detached=" + (workspace.RestoreDetached ? '1' : '0')
                + " panels=" + std::to_string(workspace.Panels.size())
                + " detached=" + std::to_string(workspace.Detached.size())
                + " ini=" + std::to_string(workspace.ImGuiIni.size()) + '\n';
            for (const PanelRecord& panel : workspace.Panels)
            {
                text += "panel " + panel.Id + " visible=" + (panel.Visible ? '1' : '0') + " dock=" + DockText(panel.DockRef) + '\n';
            }
            for (const DetachedViewportRecord& record : workspace.Detached)
            {
                text += "detached " + record.PanelId + " x=" + std::to_string(record.X) + " y=" + std::to_string(record.Y)
                    + " w=" + std::to_string(record.Width) + " h=" + std::to_string(record.Height)
                    + " monitor=" + DockText(record.Monitor) + '\n';
            }
            text += workspace.ImGuiIni;
            text += '\n';
        }
        text += std::string(kChecksumKey) + Engine::Sha256Builder::HashString(text) + '\n';

        if (text.size() > kMaxLayoutFileBytes)
            return Fail(outError, "the layout file would exceed its size limit");
        outText = std::move(text);
        return true;
    }

    bool ParseWorkspaceStore(std::string_view text, WorkspaceStore& outStore, std::string& outError)
    {
        if (text.size() > kMaxLayoutFileBytes)
            return Fail(outError, "layout file exceeds its size limit");

        const std::string header = std::string(kMagic) + ' ';
        if (!text.starts_with(header))
            return Fail(outError, "not a layout file");
        const size_t headerEnd = text.substr(0, 64).find('\n');
        if (headerEnd == std::string_view::npos)
            return Fail(outError, "layout header is unterminated");
        if (text.substr(0, headerEnd) != header + std::to_string(kLayoutFormatVersion))
            return Fail(outError, "unsupported or malformed layout header");

        if (text.size() < kChecksumLineBytes)
            return Fail(outError, "layout file is truncated");
        const std::string_view checksumLine = text.substr(text.size() - kChecksumLineBytes);
        const std::string_view body = text.substr(0, text.size() - kChecksumLineBytes);
        if (!checksumLine.starts_with(kChecksumKey) || checksumLine.back() != '\n')
            return Fail(outError, "layout file is truncated");
        if (checksumLine.substr(kChecksumKey.size(), 64) != Engine::Sha256Builder::HashString(body))
            return Fail(outError, "layout checksum does not match");

        Reader reader(body.substr(headerEnd + 1));
        std::string_view line;
        std::string_view activeName;
        std::string_view activeRest;
        std::vector<std::string_view> tokens;
        u32 workspaceCount = 0;
        if (!reader.ReadLine(line) || !ParseQuotedName(line, "active \"", true, activeName, activeRest))
            return Fail(outError, "active workspace line is malformed");
        if (!reader.ReadLine(line) || !SplitTokens(line, tokens) || tokens.size() != 2 || tokens[0] != "workspaces"
            || !ParseU32(tokens[1], workspaceCount))
        {
            return Fail(outError, "workspace count line is malformed");
        }
        if (workspaceCount == 0 || workspaceCount > kMaxWorkspaces)
            return Fail(outError, "workspace count is out of range");

        WorkspaceStore parsed;
        parsed.Active = std::string(activeName);
        for (u32 index = 0; index < workspaceCount; ++index)
        {
            Workspace workspace;
            if (!reader.ReadLine(line))
                return Fail(outError, "workspace line is missing");
            if (!ParseWorkspaceBody(reader, line, workspace, outError))
                return false;
            parsed.Workspaces.push_back(std::move(workspace));
        }
        if (!reader.AtEnd())
            return Fail(outError, "unexpected data after the last workspace");
        if (!ValidateWorkspaceStore(parsed, outError))
            return false;

        outStore = std::move(parsed);
        return true;
    }

    bool SaveWorkspaceStore(const std::filesystem::path& path, const WorkspaceStore& store, std::string& outError)
    {
        std::string text;
        if (!SerializeWorkspaceStore(store, text, outError))
            return false;
        std::string writeError;
        if (!Engine::WriteFileAtomically(path, text, writeError))
            return Fail(outError, "layout file could not be written: " + writeError);
        return true;
    }

    LayoutLoadStatus LoadWorkspaceStore(const std::filesystem::path& path, WorkspaceStore& outStore, std::string& outError)
    {
        std::error_code error;
        const std::filesystem::file_status status = std::filesystem::status(path, error);
        if (status.type() == std::filesystem::file_type::not_found
            || error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory)
        {
            return LayoutLoadStatus::Missing;
        }
        if (error)
        {
            Fail(outError, "layout file could not be inspected: " + error.message());
            return LayoutLoadStatus::Rejected;
        }
        if (status.type() != std::filesystem::file_type::regular)
        {
            Fail(outError, "layout path is not a regular file");
            return LayoutLoadStatus::Rejected;
        }
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error || size > kMaxLayoutFileBytes)
        {
            Fail(outError, error ? "layout file size could not be read" : "layout file exceeds its size limit");
            return LayoutLoadStatus::Rejected;
        }

        std::ifstream input(path, std::ios::binary);
        std::string text(static_cast<size_t>(size) + 1, '\0');
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (input.bad() || static_cast<size_t>(input.gcount()) != size)
        {
            Fail(outError, "layout file could not be read completely");
            return LayoutLoadStatus::Rejected;
        }
        text.resize(static_cast<size_t>(size));
        return ParseWorkspaceStore(text, outStore, outError) ? LayoutLoadStatus::Loaded : LayoutLoadStatus::Rejected;
    }

    Workspace MakeDefaultWorkspace(std::span<const std::string_view> panelIds)
    {
        Workspace workspace;
        workspace.Name = std::string(kDefaultWorkspaceName);
        ReconcilePanels(workspace, panelIds);
        return workspace;
    }

    WorkspaceStore MakeDefaultStore(std::span<const std::string_view> panelIds)
    {
        WorkspaceStore store;
        store.Active = std::string(kDefaultWorkspaceName);
        store.Workspaces.push_back(MakeDefaultWorkspace(panelIds));
        return store;
    }

    void ResetWorkspaceToDefault(Workspace& workspace, std::span<const std::string_view> panelIds)
    {
        Workspace reset = MakeDefaultWorkspace(panelIds);
        reset.Name = std::move(workspace.Name);
        workspace = std::move(reset);
    }

    const Workspace* FindWorkspace(const WorkspaceStore& store, std::string_view name)
    {
        for (const Workspace& workspace : store.Workspaces)
        {
            if (EqualsFolded(workspace.Name, name))
                return &workspace;
        }
        return nullptr;
    }

    const PanelRecord* FindPanel(const Workspace& workspace, std::string_view panelId)
    {
        for (const PanelRecord& panel : workspace.Panels)
        {
            if (panel.Id == panelId)
                return &panel;
        }
        return nullptr;
    }

    bool UpsertWorkspace(WorkspaceStore& store, Workspace workspace, std::string& outError)
    {
        if (!ValidateWorkspace(workspace, outError))
            return false;
        for (Workspace& existing : store.Workspaces)
        {
            if (!EqualsFolded(existing.Name, workspace.Name))
                continue;
            if (store.Active == existing.Name)
                store.Active = workspace.Name;
            existing = std::move(workspace);
            return true;
        }
        if (store.Workspaces.size() >= kMaxWorkspaces)
            return Fail(outError, "the layout file already holds the maximum number of workspaces");
        store.Workspaces.push_back(std::move(workspace));
        return true;
    }

    bool RemoveWorkspace(WorkspaceStore& store, std::string_view name)
    {
        if (EqualsFolded(name, kDefaultWorkspaceName) || store.Workspaces.size() <= 1)
            return false;
        const auto found = std::find_if(store.Workspaces.begin(), store.Workspaces.end(),
            [&](const Workspace& workspace) { return EqualsFolded(workspace.Name, name); });
        if (found == store.Workspaces.end())
            return false;

        const bool wasActive = found->Name == store.Active;
        store.Workspaces.erase(found);
        if (wasActive)
        {
            const Workspace* fallback = FindWorkspace(store, kDefaultWorkspaceName);
            store.Active = fallback ? fallback->Name : store.Workspaces.front().Name;
        }
        return true;
    }

    bool SetActiveWorkspace(WorkspaceStore& store, std::string_view name)
    {
        const Workspace* workspace = FindWorkspace(store, name);
        if (!workspace)
            return false;
        store.Active = workspace->Name;
        return true;
    }

    bool ReconcilePanels(Workspace& workspace, std::span<const std::string_view> panelIds)
    {
        std::vector<std::string_view> wanted;
        for (const std::string_view id : panelIds)
        {
            if (IsValidPanelId(id) && std::find(wanted.begin(), wanted.end(), id) == wanted.end()
                && wanted.size() < kMaxPanelsPerWorkspace)
            {
                wanted.push_back(id);
            }
        }
        const auto isWanted = [&](std::string_view id)
        {
            return std::find(wanted.begin(), wanted.end(), id) != wanted.end();
        };

        bool changed = false;
        const size_t panelsBefore = workspace.Panels.size();
        std::erase_if(workspace.Panels, [&](const PanelRecord& panel) { return !isWanted(panel.Id); });
        changed |= workspace.Panels.size() != panelsBefore;
        const size_t detachedBefore = workspace.Detached.size();
        std::erase_if(workspace.Detached, [&](const DetachedViewportRecord& record) { return !isWanted(record.PanelId); });
        changed |= workspace.Detached.size() != detachedBefore;

        for (const std::string_view id : wanted)
        {
            if (!FindPanel(workspace, id))
            {
                workspace.Panels.push_back({ std::string(id), true, {} });
                changed = true;
            }
        }
        return changed;
    }

    std::span<const DetachedViewportRecord> DetachedViewportsToRestore(const Workspace& workspace)
    {
        if (!workspace.RestoreDetached)
            return {};
        return workspace.Detached;
    }
}
