#include "ShortcutMap.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

namespace SpiralEditor
{
    namespace
    {
        using Engine::InputModifiers;

        struct KeyName
        {
            int Code;
            std::string_view Name;
        };

        constexpr KeyName kKeyNames[] = {
            { 32, "Space" }, { 39, "Apostrophe" }, { 44, "Comma" }, { 45, "Minus" },
            { 46, "Period" }, { 47, "Slash" }, { 48, "0" }, { 49, "1" },
            { 50, "2" }, { 51, "3" }, { 52, "4" }, { 53, "5" },
            { 54, "6" }, { 55, "7" }, { 56, "8" }, { 57, "9" },
            { 59, "Semicolon" }, { 61, "Equal" }, { 65, "A" }, { 66, "B" },
            { 67, "C" }, { 68, "D" }, { 69, "E" }, { 70, "F" },
            { 71, "G" }, { 72, "H" }, { 73, "I" }, { 74, "J" },
            { 75, "K" }, { 76, "L" }, { 77, "M" }, { 78, "N" },
            { 79, "O" }, { 80, "P" }, { 81, "Q" }, { 82, "R" },
            { 83, "S" }, { 84, "T" }, { 85, "U" }, { 86, "V" },
            { 87, "W" }, { 88, "X" }, { 89, "Y" }, { 90, "Z" },
            { 91, "LeftBracket" }, { 92, "Backslash" }, { 93, "RightBracket" }, { 96, "Grave" },
            { 161, "World1" }, { 162, "World2" }, { 256, "Escape" }, { 257, "Enter" },
            { 258, "Tab" }, { 259, "Backspace" }, { 260, "Insert" }, { 261, "Delete" },
            { 262, "Right" }, { 263, "Left" }, { 264, "Down" }, { 265, "Up" },
            { 266, "PageUp" }, { 267, "PageDown" }, { 268, "Home" }, { 269, "End" },
            { 280, "CapsLock" }, { 281, "ScrollLock" }, { 282, "NumLock" }, { 283, "PrintScreen" },
            { 284, "Pause" }, { 290, "F1" }, { 291, "F2" }, { 292, "F3" },
            { 293, "F4" }, { 294, "F5" }, { 295, "F6" }, { 296, "F7" },
            { 297, "F8" }, { 298, "F9" }, { 299, "F10" }, { 300, "F11" },
            { 301, "F12" }, { 302, "F13" }, { 303, "F14" }, { 304, "F15" },
            { 305, "F16" }, { 306, "F17" }, { 307, "F18" }, { 308, "F19" },
            { 309, "F20" }, { 310, "F21" }, { 311, "F22" }, { 312, "F23" },
            { 313, "F24" }, { 314, "F25" }, { 320, "Numpad0" }, { 321, "Numpad1" },
            { 322, "Numpad2" }, { 323, "Numpad3" }, { 324, "Numpad4" }, { 325, "Numpad5" },
            { 326, "Numpad6" }, { 327, "Numpad7" }, { 328, "Numpad8" }, { 329, "Numpad9" },
            { 330, "NumpadDecimal" }, { 331, "NumpadDivide" }, { 332, "NumpadMultiply" }, { 333, "NumpadSubtract" },
            { 334, "NumpadAdd" }, { 335, "NumpadEnter" }, { 336, "NumpadEqual" }, { 348, "Menu" }
        };

        constexpr KeyName kKeyAliases[] = {
            { 256, "Esc" }, { 257, "Return" }, { 260, "Ins" }, { 261, "Del" }, { 266, "PgUp" }, { 267, "PgDn" }
        };

        struct ModifierName
        {
            Engine::InputModifier Bit;
            std::string_view Name;
        };

        // Canonical spellings; the formatter walks this table in order.
        constexpr ModifierName kModifierNames[] = {
            { Engine::InputModifierControl, "Ctrl" }, { Engine::InputModifierShift, "Shift" },
            { Engine::InputModifierAlt, "Alt" }, { Engine::InputModifierSuper, "Super" }
        };

        constexpr ModifierName kModifierAliases[] = {
            { Engine::InputModifierControl, "Control" }, { Engine::InputModifierSuper, "Meta" },
            { Engine::InputModifierSuper, "Win" }, { Engine::InputModifierSuper, "Cmd" }
        };

        char FoldAscii(char c)
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        bool EqualsIgnoreCase(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (FoldAscii(a[i]) != FoldAscii(b[i]))
                    return false;
            return true;
        }

        std::string_view TrimBlanks(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
                text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
                text.remove_suffix(1);
            return text;
        }

        template<size_t N>
        bool FindIn(const KeyName (&table)[N], std::string_view name, int& code)
        {
            for (const KeyName& key : table)
                if (EqualsIgnoreCase(key.Name, name))
                {
                    code = key.Code;
                    return true;
                }
            return false;
        }

        bool FindKeyCode(std::string_view name, int& code)
        {
            return FindIn(kKeyNames, name, code) || FindIn(kKeyAliases, name, code);
        }

        bool FindModifierBit(std::string_view name, InputModifiers& bit)
        {
            for (const ModifierName& modifier : kModifierNames)
                if (EqualsIgnoreCase(modifier.Name, name))
                {
                    bit = modifier.Bit;
                    return true;
                }
            for (const ModifierName& modifier : kModifierAliases)
                if (EqualsIgnoreCase(modifier.Name, name))
                {
                    bit = modifier.Bit;
                    return true;
                }
            return false;
        }

        constexpr std::string_view kShortcutHeader = "spiral-shortcuts 1";

        const char* BindStatusText(BindStatus status)
        {
            switch (status)
            {
            case BindStatus::Bound: return "bound";
            case BindStatus::InvalidCommand: return "invalid command id";
            case BindStatus::InvalidChord: return "invalid chord";
            case BindStatus::InvalidScope: return "invalid scope";
            case BindStatus::Duplicate: return "binding already present";
            case BindStatus::Conflict: return "chord already used by another command in this scope";
            case BindStatus::NotBound: return "binding does not exist";
            case BindStatus::LimitReached: return "too many bindings";
            }
            return "unknown error";
        }

        std::string FormatLine(std::string_view verb, const ShortcutBinding& binding)
        {
            std::string line(verb);
            line += ' ';
            line += binding.CommandId;
            line += ' ';
            line += FormatScope(binding.Key.Scope);
            line += ' ';
            line += FormatChord(binding.Key.Chord);
            line += '\n';
            return line;
        }
    }

    bool IsValidEditorIdentifier(std::string_view text)
    {
        if (text.empty() || text.size() > 64)
            return false;
        bool segmentEmpty = true;
        for (const char c : text)
        {
            if (c == '.')
            {
                if (segmentEmpty)
                    return false;
                segmentEmpty = true;
                continue;
            }
            const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!allowed)
                return false;
            segmentEmpty = false;
        }
        return !segmentEmpty;
    }

    bool IsValidChord(const KeyChord& chord)
    {
        return (chord.Modifiers & ~kChordModifierMask) == 0 && !ChordKeyName(chord.Key).empty();
    }

    KeyChord MakeChord(int glfwKey, Engine::InputModifiers eventModifiers)
    {
        return KeyChord { glfwKey, eventModifiers & kChordModifierMask };
    }

    std::span<const int> ChordKeys()
    {
        static const std::vector<int> keys = []
        {
            std::vector<int> result;
            result.reserve(std::size(kKeyNames));
            for (const KeyName& key : kKeyNames)
                result.push_back(key.Code);
            return result;
        }();
        return keys;
    }

    std::string_view ChordKeyName(int glfwKey)
    {
        const auto it = std::lower_bound(std::begin(kKeyNames), std::end(kKeyNames), glfwKey,
            [](const KeyName& key, int code) { return key.Code < code; });
        return it != std::end(kKeyNames) && it->Code == glfwKey ? it->Name : std::string_view();
    }

    std::string FormatChord(const KeyChord& chord)
    {
        if (!IsValidChord(chord))
            return {};
        std::string text;
        for (const ModifierName& modifier : kModifierNames)
            if ((chord.Modifiers & modifier.Bit) != 0)
            {
                text += modifier.Name;
                text += '+';
            }
        text += ChordKeyName(chord.Key);
        return text;
    }

    bool ParseChord(std::string_view text, KeyChord& chord)
    {
        KeyChord parsed;
        bool haveKey = false;
        size_t start = 0;
        while (true)
        {
            const size_t plus = text.find('+', start);
            const std::string_view token = TrimBlanks(text.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start));
            if (token.empty())
                return false;
            InputModifiers bit = 0;
            int code = 0;
            if (FindModifierBit(token, bit))
            {
                if ((parsed.Modifiers & bit) != 0)
                    return false;
                parsed.Modifiers |= bit;
            }
            else if (FindKeyCode(token, code))
            {
                if (haveKey)
                    return false;
                parsed.Key = code;
                haveKey = true;
            }
            else
                return false;
            if (plus == std::string_view::npos)
                break;
            start = plus + 1;
        }
        if (!haveKey)
            return false;
        chord = parsed;
        return true;
    }

    bool IsValidScope(const ShortcutScope& scope)
    {
        switch (scope.Kind)
        {
        case ShortcutScopeKind::Global:
        case ShortcutScopeKind::Viewport: return scope.Panel.empty();
        case ShortcutScopeKind::Panel: return IsValidEditorIdentifier(scope.Panel);
        }
        return false;
    }

    std::string FormatScope(const ShortcutScope& scope)
    {
        if (!IsValidScope(scope))
            return {};
        switch (scope.Kind)
        {
        case ShortcutScopeKind::Global: return "global";
        case ShortcutScopeKind::Viewport: return "viewport";
        case ShortcutScopeKind::Panel: return "panel:" + scope.Panel;
        }
        return {};
    }

    bool ParseScope(std::string_view text, ShortcutScope& scope)
    {
        ShortcutScope parsed;
        if (text == "global")
            parsed.Kind = ShortcutScopeKind::Global;
        else if (text == "viewport")
            parsed.Kind = ShortcutScopeKind::Viewport;
        else if (text.starts_with("panel:"))
        {
            parsed.Kind = ShortcutScopeKind::Panel;
            parsed.Panel = std::string(text.substr(6));
        }
        else
            return false;
        if (!IsValidScope(parsed))
            return false;
        scope = std::move(parsed);
        return true;
    }

    BindResult ShortcutMap::Bind(std::string_view commandId, const ShortcutKey& key)
    {
        if (!IsValidEditorIdentifier(commandId))
            return { BindStatus::InvalidCommand, {} };
        if (!IsValidChord(key.Chord))
            return { BindStatus::InvalidChord, {} };
        if (!IsValidScope(key.Scope))
            return { BindStatus::InvalidScope, {} };
        for (const ShortcutBinding& existing : m_Bindings)
            if (existing.Key == key)
                return existing.CommandId == commandId ? BindResult { BindStatus::Duplicate, {} }
                                                       : BindResult { BindStatus::Conflict, existing.CommandId };
        if (m_Bindings.size() >= kMaximumBindings)
            return { BindStatus::LimitReached, {} };
        ShortcutBinding binding { std::string(commandId), key };
        m_Bindings.insert(std::lower_bound(m_Bindings.begin(), m_Bindings.end(), binding), std::move(binding));
        return {};
    }

    BindResult ShortcutMap::Unbind(std::string_view commandId, const ShortcutKey& key)
    {
        const auto it = std::find_if(m_Bindings.begin(), m_Bindings.end(),
            [&](const ShortcutBinding& b) { return b.CommandId == commandId && b.Key == key; });
        if (it == m_Bindings.end())
            return { BindStatus::NotBound, {} };
        m_Bindings.erase(it);
        return {};
    }

    size_t ShortcutMap::UnbindCommand(std::string_view commandId)
    {
        return std::erase_if(m_Bindings, [&](const ShortcutBinding& b) { return b.CommandId == commandId; });
    }

    BindResult ShortcutMap::Rebind(std::string_view commandId, const ShortcutKey& from, const ShortcutKey& to)
    {
        ShortcutMap candidate = *this;
        if (const BindResult removed = candidate.Unbind(commandId, from); !removed.Ok())
            return removed;
        if (const BindResult added = candidate.Bind(commandId, to); !added.Ok())
            return added;
        *this = std::move(candidate);
        return {};
    }

    std::vector<ChordClash> ShortcutMap::Clashes(const ShortcutKey& candidate) const
    {
        std::vector<ChordClash> clashes;
        if (!IsValidChord(candidate.Chord) || !IsValidScope(candidate.Scope))
            return clashes;
        const bool candidateGlobal = candidate.Scope.Kind == ShortcutScopeKind::Global;
        for (const ShortcutBinding& existing : m_Bindings)
        {
            if (existing.Key.Chord != candidate.Chord)
                continue;
            const bool existingGlobal = existing.Key.Scope.Kind == ShortcutScopeKind::Global;
            if (existing.Key.Scope == candidate.Scope)
                clashes.push_back({ ChordOverlap::Conflict, existing });
            else if (candidateGlobal && !existingGlobal)
                clashes.push_back({ ChordOverlap::ShadowedBy, existing });
            else if (!candidateGlobal && existingGlobal)
                clashes.push_back({ ChordOverlap::Shadows, existing });
        }
        return clashes;
    }

    std::optional<ShortcutBinding> ShortcutMap::Resolve(const KeyChord& chord, const ShortcutScope& focus) const
    {
        const ShortcutBinding* global = nullptr;
        for (const ShortcutBinding& binding : m_Bindings)
        {
            if (binding.Key.Chord != chord)
                continue;
            if (binding.Key.Scope == focus)
                return binding;
            if (binding.Key.Scope.Kind == ShortcutScopeKind::Global)
                global = &binding;
        }
        return global ? std::optional<ShortcutBinding>(*global) : std::nullopt;
    }

    std::vector<ShortcutKey> ShortcutMap::KeysFor(std::string_view commandId) const
    {
        std::vector<ShortcutKey> keys;
        for (const ShortcutBinding& binding : m_Bindings)
            if (binding.CommandId == commandId)
                keys.push_back(binding.Key);
        return keys;
    }

    std::string EncodeShortcutOverrides(const ShortcutMap& defaults, const ShortcutMap& current)
    {
        std::vector<ShortcutBinding> removed;
        std::vector<ShortcutBinding> added;
        std::set_difference(defaults.Bindings().begin(), defaults.Bindings().end(), current.Bindings().begin(),
            current.Bindings().end(), std::back_inserter(removed));
        std::set_difference(current.Bindings().begin(), current.Bindings().end(), defaults.Bindings().begin(),
            defaults.Bindings().end(), std::back_inserter(added));
        std::string text(kShortcutHeader);
        text += '\n';
        for (const ShortcutBinding& binding : removed)
            text += FormatLine("unbind", binding);
        for (const ShortcutBinding& binding : added)
            text += FormatLine("bind", binding);
        return text;
    }

    ShortcutDecodeResult DecodeShortcutOverrides(const ShortcutMap& defaults, std::string_view text)
    {
        ShortcutDecodeResult result;
        const auto fail = [&](size_t line, std::string message)
        {
            result.Ok = false;
            result.Map = {};
            result.ErrorLine = line;
            result.Error = std::move(message);
            return result;
        };
        if (text.size() > kMaximumShortcutFileBytes)
            return fail(0, "shortcut file is too large");

        ShortcutMap map = defaults;
        size_t lineNumber = 0;
        bool sawBind = false;
        do
        {
            ++lineNumber;
            const size_t newline = text.find('\n');
            std::string_view line = text.substr(0, newline);
            text = newline == std::string_view::npos ? std::string_view() : text.substr(newline + 1);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);

            if (lineNumber == 1)
            {
                if (line != kShortcutHeader)
                    return fail(1, "missing or unsupported shortcut file header");
                continue;
            }

            constexpr const char* kShape = "expected '<bind|unbind> <command> <scope> <chord>'";
            std::array<std::string_view, 4> tokens;
            size_t count = 0;
            size_t start = 0;
            while (true)
            {
                const size_t space = line.find(' ', start);
                const std::string_view token = line.substr(start, space == std::string_view::npos ? std::string_view::npos : space - start);
                if (token.empty() || count == tokens.size())
                    return fail(lineNumber, kShape);
                tokens[count++] = token;
                if (space == std::string_view::npos)
                    break;
                start = space + 1;
            }
            if (count != tokens.size())
                return fail(lineNumber, kShape);

            const bool isBind = tokens[0] == "bind";
            if (!isBind && tokens[0] != "unbind")
                return fail(lineNumber, "unknown verb '" + std::string(tokens[0]) + "'");
            if (isBind)
                sawBind = true;
            else if (sawBind)
                return fail(lineNumber, "unbind lines must precede bind lines");

            ShortcutKey key;
            if (!ParseScope(tokens[2], key.Scope))
                return fail(lineNumber, "invalid scope '" + std::string(tokens[2]) + "'");
            if (!ParseChord(tokens[3], key.Chord) || tokens[3] != FormatChord(key.Chord))
                return fail(lineNumber, "invalid chord '" + std::string(tokens[3]) + "'");
            const BindResult applied = isBind ? map.Bind(tokens[1], key) : map.Unbind(tokens[1], key);
            if (!applied.Ok())
            {
                std::string message = BindStatusText(applied.Status);
                if (!applied.ConflictingCommand.empty())
                    message += " (" + applied.ConflictingCommand + ")";
                return fail(lineNumber, std::move(message));
            }
        } while (!text.empty());

        result.Ok = true;
        result.Map = std::move(map);
        return result;
    }
}
