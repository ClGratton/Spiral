#include "CommandsCoreTests.h"

#include "Commands/CommandRegistry.h"
#include "Commands/FuzzyMatch.h"
#include "Commands/ShortcutMap.h"
#include "Core/LogBuffer.h"
#include "Core/Notifications.h"
#include "Core/TextLimits.h"
#include "TestSupport/GeneratedTest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
    using namespace SpiralEditor;
    using Engine::u32;
    using Engine::u64;
    using Engine::u8;

    // Failure hypotheses, oracles, and non-claims for the whole file:
    // - Chord keys are checked against the GLFW key macro list (a stable ABI, extracted from glfw3.h and
    //   named by a mechanical rule); shortcut maps against a linear-scan oracle that classifies overlap by
    //   enumerating focus scopes; the codec against a line-by-line reference parser; the registry against
    //   a map-based model; fuzzy matching against exhaustive enumeration of every alignment; the log ring
    //   against a vector trimmed from the front; notifications against a per-operation deadline model.
    // - Properties run from a fixed seed and replay through SPIRAL_COMMANDS_CORE_SEED /
    //   SPIRAL_COMMANDS_CORE_REPLAY; a failure prints the seed and the original and minimised choice traces
    //   and writes a counterexample JSON under the system temp directory.
    // - Tier: fast, in-process, no GPU, no windowing, no filesystem except the counterexample artifact.
    // - Not claimed: ImGui key delivery, real Engine::Log sink wiring, or throughput numbers; the
    //   ThreadSanitizer run of the concurrent producer test is a separate harness, not this binary.

    struct Checker
    {
        const char* Suite;
        bool Ok = true;

        void Expect(bool condition, const std::string& message)
        {
            if (!condition)
            {
                std::cerr << "Commands core test failed [" << Suite << "]: " << message << '\n';
                Ok = false;
            }
        }
    };

    bool RunProperty(std::string_view name, const Spiral::Tests::Property& property, size_t iterations)
    {
        Spiral::Tests::CampaignOptions options;
        options.Iterations = iterations;
        if (const char* seed = std::getenv("SPIRAL_COMMANDS_CORE_SEED"))
            options.Seed = std::strtoull(seed, nullptr, 10);
        Spiral::Tests::ChoiceTrace replay;
        if (const char* trace = std::getenv("SPIRAL_COMMANDS_CORE_REPLAY"); trace && Spiral::Tests::ParseTrace(trace, replay))
            options.Replay = replay;

        Spiral::Tests::Counterexample failure;
        if (Spiral::Tests::RunCampaign(options, property, failure))
            return true;

        const std::string minimized = Spiral::Tests::SerializeTrace(failure.MinimizedTrace);
        const std::string rerun = "SPIRAL_COMMANDS_CORE_SEED=" + std::to_string(failure.Seed)
            + " SPIRAL_COMMANDS_CORE_REPLAY=\"" + minimized + "\" EngineTests --test <registered name of " + std::string(name) + ">";
        const std::filesystem::path artifact = std::filesystem::temp_directory_path()
            / ("spiral-commands-core-counterexample-" + std::string(name) + ".json");
        std::string artifactError;
        const bool written = Spiral::Tests::WriteCounterexample(artifact, name, failure, rerun, artifactError);
        std::cerr << "Commands core property failed [" << name << "]: " << failure.Message
            << " seed=" << failure.Seed << " iteration=" << failure.Iteration
            << " originalTrace=" << Spiral::Tests::SerializeTrace(failure.OriginalTrace)
            << " minimizedTrace=" << minimized << "\n  rerun: " << rerun << '\n';
        if (written)
            std::cerr << "  counterexample: " << artifact.string() << '\n';
        else
            std::cerr << "  counterexample write failed: " << artifactError << '\n';
        return false;
    }

    template<typename T>
    const T& Pick(Spiral::Tests::ChoiceStream& stream, const std::vector<T>& pool)
    {
        return pool[stream.NextSize(0, pool.size() - 1)];
    }

    // ---- Chord oracle -------------------------------------------------------------------------------------

    struct GlfwKey
    {
        int Code;
        const char* Macro; // GLFW_KEY_<Macro>
        const char* Name;  // expected canonical chord name
    };

    constexpr GlfwKey kGlfwKeys[] = {
            { 32, "SPACE", "Space" }, { 39, "APOSTROPHE", "Apostrophe" }, { 44, "COMMA", "Comma" },
            { 45, "MINUS", "Minus" }, { 46, "PERIOD", "Period" }, { 47, "SLASH", "Slash" },
            { 48, "0", "0" }, { 49, "1", "1" }, { 50, "2", "2" },
            { 51, "3", "3" }, { 52, "4", "4" }, { 53, "5", "5" },
            { 54, "6", "6" }, { 55, "7", "7" }, { 56, "8", "8" },
            { 57, "9", "9" }, { 59, "SEMICOLON", "Semicolon" }, { 61, "EQUAL", "Equal" },
            { 65, "A", "A" }, { 66, "B", "B" }, { 67, "C", "C" },
            { 68, "D", "D" }, { 69, "E", "E" }, { 70, "F", "F" },
            { 71, "G", "G" }, { 72, "H", "H" }, { 73, "I", "I" },
            { 74, "J", "J" }, { 75, "K", "K" }, { 76, "L", "L" },
            { 77, "M", "M" }, { 78, "N", "N" }, { 79, "O", "O" },
            { 80, "P", "P" }, { 81, "Q", "Q" }, { 82, "R", "R" },
            { 83, "S", "S" }, { 84, "T", "T" }, { 85, "U", "U" },
            { 86, "V", "V" }, { 87, "W", "W" }, { 88, "X", "X" },
            { 89, "Y", "Y" }, { 90, "Z", "Z" }, { 91, "LEFT_BRACKET", "LeftBracket" },
            { 92, "BACKSLASH", "Backslash" }, { 93, "RIGHT_BRACKET", "RightBracket" }, { 96, "GRAVE_ACCENT", "Grave" },
            { 161, "WORLD_1", "World1" }, { 162, "WORLD_2", "World2" }, { 256, "ESCAPE", "Escape" },
            { 257, "ENTER", "Enter" }, { 258, "TAB", "Tab" }, { 259, "BACKSPACE", "Backspace" },
            { 260, "INSERT", "Insert" }, { 261, "DELETE", "Delete" }, { 262, "RIGHT", "Right" },
            { 263, "LEFT", "Left" }, { 264, "DOWN", "Down" }, { 265, "UP", "Up" },
            { 266, "PAGE_UP", "PageUp" }, { 267, "PAGE_DOWN", "PageDown" }, { 268, "HOME", "Home" },
            { 269, "END", "End" }, { 280, "CAPS_LOCK", "CapsLock" }, { 281, "SCROLL_LOCK", "ScrollLock" },
            { 282, "NUM_LOCK", "NumLock" }, { 283, "PRINT_SCREEN", "PrintScreen" }, { 284, "PAUSE", "Pause" },
            { 290, "F1", "F1" }, { 291, "F2", "F2" }, { 292, "F3", "F3" },
            { 293, "F4", "F4" }, { 294, "F5", "F5" }, { 295, "F6", "F6" },
            { 296, "F7", "F7" }, { 297, "F8", "F8" }, { 298, "F9", "F9" },
            { 299, "F10", "F10" }, { 300, "F11", "F11" }, { 301, "F12", "F12" },
            { 302, "F13", "F13" }, { 303, "F14", "F14" }, { 304, "F15", "F15" },
            { 305, "F16", "F16" }, { 306, "F17", "F17" }, { 307, "F18", "F18" },
            { 308, "F19", "F19" }, { 309, "F20", "F20" }, { 310, "F21", "F21" },
            { 311, "F22", "F22" }, { 312, "F23", "F23" }, { 313, "F24", "F24" },
            { 314, "F25", "F25" }, { 320, "KP_0", "Numpad0" }, { 321, "KP_1", "Numpad1" },
            { 322, "KP_2", "Numpad2" }, { 323, "KP_3", "Numpad3" }, { 324, "KP_4", "Numpad4" },
            { 325, "KP_5", "Numpad5" }, { 326, "KP_6", "Numpad6" }, { 327, "KP_7", "Numpad7" },
            { 328, "KP_8", "Numpad8" }, { 329, "KP_9", "Numpad9" }, { 330, "KP_DECIMAL", "NumpadDecimal" },
            { 331, "KP_DIVIDE", "NumpadDivide" }, { 332, "KP_MULTIPLY", "NumpadMultiply" }, { 333, "KP_SUBTRACT", "NumpadSubtract" },
            { 334, "KP_ADD", "NumpadAdd" }, { 335, "KP_ENTER", "NumpadEnter" }, { 336, "KP_EQUAL", "NumpadEqual" },
            { 348, "MENU", "Menu" }
    };

    constexpr int kGlfwModifierKeys[] = { 340, 341, 342, 343, 344, 345, 346, 347 };

    // Engine::InputModifier values written as literals: Shift 1, Control 2, Alt 4, Super 8.
    std::string OracleFormatChord(int key, u32 mods)
    {
        std::string text;
        if (mods & 2u) text += "Ctrl+";
        if (mods & 1u) text += "Shift+";
        if (mods & 4u) text += "Alt+";
        if (mods & 8u) text += "Super+";
        for (const GlfwKey& entry : kGlfwKeys)
            if (entry.Code == key)
                return text + entry.Name;
        return {};
    }

    std::string Lower(std::string text)
    {
        for (char& c : text)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return text;
    }

    std::string Upper(std::string text)
    {
        for (char& c : text)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        return text;
    }

    std::string Trim(const std::string& text)
    {
        size_t a = 0;
        size_t b = text.size();
        while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
        while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) --b;
        return text.substr(a, b - a);
    }

    // Reference chord parser written as a split-then-classify pass.
    bool OracleParseChord(const std::string& text, int& key, u32& mods)
    {
        std::vector<std::string> tokens(1);
        for (const char c : text)
        {
            if (c == '+') tokens.emplace_back();
            else tokens.back() += c;
        }
        key = 0;
        mods = 0;
        bool haveKey = false;
        for (const std::string& raw : tokens)
        {
            const std::string token = Lower(Trim(raw));
            if (token.empty()) return false;
            u32 bit = 0;
            if (token == "ctrl" || token == "control") bit = 2;
            else if (token == "shift") bit = 1;
            else if (token == "alt") bit = 4;
            else if (token == "super" || token == "meta" || token == "win" || token == "cmd") bit = 8;
            if (bit != 0)
            {
                if (mods & bit) return false;
                mods |= bit;
                continue;
            }
            int code = 0;
            for (const GlfwKey& entry : kGlfwKeys)
                if (Lower(entry.Name) == token) code = entry.Code;
            if (token == "esc") code = 256;
            if (token == "return") code = 257;
            if (token == "ins") code = 260;
            if (token == "del") code = 261;
            if (token == "pgup") code = 266;
            if (token == "pgdn") code = 267;
            if (code == 0 || haveKey) return false;
            key = code;
            haveKey = true;
        }
        return haveKey;
    }

    bool OracleIdentifier(const std::string& text)
    {
        if (text.empty() || text.size() > 64) return false;
        std::vector<std::string> segments(1);
        for (const char c : text)
        {
            if (c == '.') segments.emplace_back();
            else segments.back() += c;
        }
        for (const std::string& segment : segments)
        {
            if (segment.empty()) return false;
            for (const char c : segment)
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        }
        return true;
    }
}

namespace SpiralTests
{
    bool TestShortcutKeyTableMatchesGlfwAndChordsRoundTrip()
    {
        Checker c { "chord keys" };

        std::set<int> expectedCodes;
        for (const GlfwKey& entry : kGlfwKeys)
            expectedCodes.insert(entry.Code);
        c.Expect(expectedCodes.size() == std::size(kGlfwKeys), "the oracle table lists each GLFW key once");
        const std::span<const int> keys = ChordKeys();
        c.Expect(std::vector<int>(keys.begin(), keys.end()) == std::vector<int>(expectedCodes.begin(), expectedCodes.end()),
            "ChordKeys() is exactly the GLFW key set minus unknown and modifier keys, ascending");

        std::set<std::string> lowered;
        for (const GlfwKey& entry : kGlfwKeys)
        {
            c.Expect(ChordKeyName(entry.Code) == entry.Name, std::string("name of GLFW_KEY_") + entry.Macro);
            c.Expect(lowered.insert(Lower(entry.Name)).second, std::string("name is unique ignoring case: ") + entry.Name);
        }

        for (int key = -8; key < 420; ++key)
        {
            if (expectedCodes.contains(key)) continue;
            c.Expect(ChordKeyName(key).empty(), "no name for non-chord key " + std::to_string(key));
            c.Expect(!IsValidChord({ key, 0 }), "non-chord key is an invalid chord " + std::to_string(key));
        }
        for (const int modifierKey : kGlfwModifierKeys)
            c.Expect(!IsValidChord({ modifierKey, 0 }), "modifier keys cannot be chord keys");
        c.Expect(!IsValidChord({ 0, 0 }) && !IsValidChord({ std::numeric_limits<int>::min(), 0 })
            && !IsValidChord({ std::numeric_limits<int>::max(), 0 }), "extreme key codes are invalid");

        for (const int key : keys)
            for (u32 mods = 0; mods < 16; ++mods)
            {
                const KeyChord chord { key, mods };
                const std::string expected = OracleFormatChord(key, mods);
                c.Expect(IsValidChord(chord), "valid chord " + expected);
                c.Expect(FormatChord(chord) == expected, "format " + expected + " got " + FormatChord(chord));

                KeyChord parsed { -1, 0xFF };
                c.Expect(ParseChord(expected, parsed) && parsed == chord, "round trip " + expected);
                for (const std::string& variant : { Lower(expected), Upper(expected) })
                {
                    parsed = {};
                    c.Expect(ParseChord(variant, parsed) && parsed == chord, "case-insensitive parse " + variant);
                }
                std::string spaced;
                for (const char ch : expected)
                {
                    if (ch == '+') spaced += " + ";
                    else spaced += ch;
                }
                parsed = {};
                c.Expect(ParseChord("\t" + spaced + " ", parsed) && parsed == chord, "blanks around tokens are ignored: " + spaced);
            }

        // Modifier order does not matter on input.
        KeyChord parsed;
        c.Expect(ParseChord("Z+Shift+Ctrl", parsed) && parsed == KeyChord { 'Z', 3 }, "modifiers after the key");
        c.Expect(ParseChord("alt+super+shift+ctrl+f12", parsed) && parsed == KeyChord { 301, 15 }, "all four modifiers");
        c.Expect(FormatChord(parsed) == "Ctrl+Shift+Alt+Super+F12", "canonical modifier order");
        for (const auto& [alias, expected] : std::vector<std::pair<std::string, KeyChord>> {
                 { "Control+Esc", { 256, 2 } }, { "Meta+Return", { 257, 8 } }, { "Win+Del", { 261, 8 } },
                 { "Cmd+Ins", { 260, 8 } }, { "PgUp", { 266, 0 } }, { "shift+PGDN", { 267, 1 } } })
        {
            parsed = {};
            c.Expect(ParseChord(alias, parsed) && parsed == expected, "alias " + alias);
        }

        // Event modifiers carry lock bits that must not leak into chords.
        const u32 locks = Engine::InputModifierCapsLock | Engine::InputModifierNumLock;
        c.Expect(MakeChord('Z', Engine::InputModifierControl | Engine::InputModifierShift | locks) == KeyChord { 'Z', 3 },
            "lock modifiers are stripped");
        c.Expect(!IsValidChord({ 'Z', Engine::InputModifierCapsLock }), "a lock bit is not a chord modifier");
        c.Expect(!IsValidChord({ 'Z', 1u << 6 }) && !IsValidChord({ 'Z', 0x80000000u }), "stray modifier bits are invalid");
        c.Expect(FormatChord({ 'Z', 1u << 6 }).empty() && FormatChord({ 340, 0 }).empty(), "invalid chords format to nothing");
        return c.Ok;
    }

    bool TestShortcutChordParsingRejectsMalformedText()
    {
        Checker c { "chord parsing" };
        const KeyChord sentinel { 777, 5 };
        const std::vector<std::string> rejected = {
            "", " ", "+", "++", "Ctrl+", "+A", "Ctrl++A", "Ctrl+ +A", "A+B", "Ctrl+A+B", "Ctrl+Ctrl+A", "Ctrl+Control+A",
            "Ctrl", "Ctrl+Shift", "Shift+Alt+Super", "Foo", "Ctrl+Foo", "A B", "Ctrl+A+", "LeftShift+A",
            "Ctrl+Shift+Z\n", "Ctrl+Z\r", "Ctrl-Z", "Ctrl,Z", "Plus", "Ctrl+Plus", "F26", "Numpad10", "Ctrl+Capslock+"
            , std::string("Ctrl+Z\0", 7), std::string("\0", 1), "\xC3\xA9", "Ctrl+\xE2\x82\xAC"
        };
        for (const std::string& text : rejected)
        {
            KeyChord chord = sentinel;
            const bool ok = ParseChord(text, chord);
            c.Expect(!ok, "rejects '" + text + "'");
            c.Expect(chord == sentinel, "failed parse leaves the output untouched for '" + text + "'");
        }

        KeyChord blanks;
        c.Expect(ParseChord("Ctrl+A ", blanks) && blanks == KeyChord { 'A', 2 }, "trailing blank accepted");
        for (const std::string& text : { std::string("global"), std::string("viewport"), std::string("panel:hierarchy"),
                 std::string("panel:a.b-c_1") })
        {
            ShortcutScope scope;
            c.Expect(ParseScope(text, scope) && FormatScope(scope) == text, "scope round trip " + text);
        }
        for (const std::string& text : { std::string(""), std::string("Global"), std::string("panel"), std::string("panel:"),
                 std::string("panel:Bad"), std::string("panel:a b"), std::string("panel:a..b"), std::string("panel:.a"),
                 std::string("viewport:x"), std::string("global "), std::string("panel:") + std::string(65, 'a') })
        {
            ShortcutScope scope { ShortcutScopeKind::Viewport, {} };
            c.Expect(!ParseScope(text, scope) && scope.Kind == ShortcutScopeKind::Viewport, "rejects scope '" + text + "'");
        }
        c.Expect(!IsValidScope({ ShortcutScopeKind::Viewport, "x" }) && !IsValidScope({ ShortcutScopeKind::Global, "x" })
            && !IsValidScope({ ShortcutScopeKind::Panel, {} }), "scope panel field must match its context");

        // Generated text: acceptance must agree with the reference parser, and accepted chords round-trip.
        const std::vector<std::string> fragments = { "Ctrl", "ctrl", "SHIFT", "Alt", "Super", "Meta", "Z", "z", "F5", "Enter",
            "Esc", "Space", "Numpad3", "Comma", "+", "+", " ", "\t", "", "Foo", "Control", "PgUp" };
        return RunProperty("ShortcutChordParseAgreesWithReference", [&](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            std::string text;
            const size_t pieces = stream.NextSize(0, 7);
            for (size_t i = 0; i < pieces; ++i)
                text += Pick(stream, fragments);
            KeyChord actual = sentinel;
            const bool accepted = ParseChord(text, actual);
            int key = 0;
            u32 mods = 0;
            const bool expected = OracleParseChord(text, key, mods);
            if (accepted != expected)
            {
                message = "parser and reference disagree on '" + text + "'";
                return false;
            }
            if (accepted)
            {
                if (actual != KeyChord { key, mods })
                {
                    message = "different chord for '" + text + "'";
                    return false;
                }
                KeyChord again;
                if (!ParseChord(FormatChord(actual), again) || again != actual)
                {
                    message = "canonical text does not round trip for '" + text + "'";
                    return false;
                }
            }
            else if (actual != sentinel)
            {
                message = "failed parse modified the output for '" + text + "'";
                return false;
            }
            return true;
        }, 4000) && c.Ok;
    }
}

namespace
{
    // ---- Shortcut map oracle ------------------------------------------------------------------------------

    struct OKey
    {
        int Key = 0;
        u32 Mods = 0;
        int Context = 0; // 0 global, 1 viewport, 2 panel
        std::string Panel;

        auto operator<=>(const OKey&) const = default;
    };

    struct OBinding
    {
        std::string Cmd;
        OKey Key;

        auto operator<=>(const OBinding&) const = default;
    };

    bool OracleKeyValid(const OKey& key)
    {
        bool known = false;
        for (const GlfwKey& entry : kGlfwKeys)
            known = known || entry.Code == key.Key;
        if (!known || (key.Mods & ~15u) != 0)
            return false;
        if (key.Context == 2)
            return OracleIdentifier(key.Panel);
        return key.Panel.empty();
    }

    OKey ToOracle(const ShortcutKey& key)
    {
        return { key.Chord.Key, key.Chord.Modifiers, static_cast<int>(key.Scope.Kind), key.Scope.Panel };
    }

    ShortcutKey FromOracle(const OKey& key)
    {
        return { { key.Key, key.Mods }, { static_cast<ShortcutScopeKind>(key.Context), key.Panel } };
    }

    std::vector<OBinding> ToOracle(const ShortcutMap& map)
    {
        std::vector<OBinding> items;
        for (const ShortcutBinding& binding : map.Bindings())
            items.push_back({ binding.CommandId, ToOracle(binding.Key) });
        return items;
    }

    // Does a binding in `scope` fire while `focus` has input focus? Focus 0 is "no particular focus".
    bool ScopeApplies(const OKey& scope, const OKey& focus)
    {
        if (scope.Context == 0) return true;
        if (scope.Context == 1) return focus.Context == 1;
        return focus.Context == 2 && focus.Panel == scope.Panel;
    }

    std::vector<OKey> Foci()
    {
        return { { 0, 0, 0, {} }, { 0, 0, 1, {} }, { 0, 0, 2, "p1" }, { 0, 0, 2, "p2" }, { 0, 0, 2, "p3" } };
    }

    struct OracleMap
    {
        std::vector<OBinding> Items; // insertion order, not sorted

        BindStatus Bind(const std::string& cmd, const OKey& key, std::string* conflicting = nullptr)
        {
            if (!OracleIdentifier(cmd)) return BindStatus::InvalidCommand;
            bool chordOk = false;
            for (const GlfwKey& entry : kGlfwKeys)
                chordOk = chordOk || entry.Code == key.Key;
            if (!chordOk || (key.Mods & ~15u) != 0) return BindStatus::InvalidChord;
            if (!OracleKeyValid(key)) return BindStatus::InvalidScope;
            for (const OBinding& item : Items)
                if (item.Key == key)
                {
                    if (conflicting && item.Cmd != cmd) *conflicting = item.Cmd;
                    return item.Cmd == cmd ? BindStatus::Duplicate : BindStatus::Conflict;
                }
            if (Items.size() >= ShortcutMap::kMaximumBindings) return BindStatus::LimitReached;
            Items.push_back({ cmd, key });
            return BindStatus::Bound;
        }

        bool Unbind(const std::string& cmd, const OKey& key)
        {
            const auto it = std::find(Items.begin(), Items.end(), OBinding { cmd, key });
            if (it == Items.end()) return false;
            Items.erase(it);
            return true;
        }

        std::vector<OBinding> Sorted() const
        {
            std::vector<OBinding> sorted = Items;
            std::sort(sorted.begin(), sorted.end());
            return sorted;
        }

        // Clash kinds: 0 conflict, 1 candidate shadows existing, 2 candidate shadowed by existing.
        std::vector<std::pair<int, OBinding>> Clashes(const OKey& candidate) const
        {
            std::vector<std::pair<int, OBinding>> clashes;
            if (!OracleKeyValid(candidate)) return clashes;
            for (const OBinding& item : Sorted())
            {
                if (item.Key.Key != candidate.Key || item.Key.Mods != candidate.Mods) continue;
                const bool sameScope = item.Key.Context == candidate.Context && item.Key.Panel == candidate.Panel;
                if (sameScope)
                {
                    clashes.push_back({ 0, item });
                    continue;
                }
                bool overlaps = false;
                for (const OKey& focus : Foci())
                    overlaps = overlaps || (ScopeApplies(item.Key, focus) && ScopeApplies(candidate, focus));
                if (overlaps)
                    clashes.push_back({ candidate.Context == 0 ? 2 : 1, item });
            }
            return clashes;
        }

        std::optional<OBinding> Resolve(int key, u32 mods, const OKey& focus) const
        {
            std::optional<OBinding> best;
            for (const OBinding& item : Items)
            {
                if (item.Key.Key != key || item.Key.Mods != mods || !ScopeApplies(item.Key, focus)) continue;
                if (!best || (best->Key.Context == 0 && item.Key.Context != 0)) best = item;
            }
            return best;
        }
    };

    bool SameBindings(const ShortcutMap& map, const OracleMap& oracle)
    {
        return ToOracle(map) == oracle.Sorted();
    }

    OKey RandomKey(Spiral::Tests::ChoiceStream& stream, bool allowInvalid)
    {
        static const std::vector<int> keys = { 'A', 'B', 32, 256, 290 };
        static const std::vector<u32> mods = { 0, 2, 3, 4 };
        OKey key { Pick(stream, keys), Pick(stream, mods), static_cast<int>(stream.NextSize(0, 2)), {} };
        if (key.Context == 2) key.Panel = stream.NextBool() ? "p1" : "p2";
        if (allowInvalid && stream.NextSize(0, 19) == 0)
        {
            switch (stream.NextSize(0, 3))
            {
            case 0: key.Key = 340; break;
            case 1: key.Mods |= 1u << 6; break;
            case 2: key.Context = 2; key.Panel = ""; break;
            default: key.Context = 1; key.Panel = "p1"; break;
            }
        }
        return key;
    }

    std::string RandomCommand(Spiral::Tests::ChoiceStream& stream, bool allowInvalid)
    {
        static const std::vector<std::string> commands = { "c.a", "c.b", "c.c", "c.d" };
        if (allowInvalid && stream.NextSize(0, 29) == 0)
            return stream.NextBool() ? "Bad Id" : "";
        return Pick(stream, commands);
    }

    std::string DescribeStatus(BindStatus status)
    {
        return std::to_string(static_cast<int>(status));
    }
}

namespace SpiralTests
{
    bool TestShortcutMapMatchesBruteForceOracle()
    {
        return RunProperty("ShortcutMapMatchesBruteForceOracle", [](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            ShortcutMap map;
            OracleMap oracle;
            const size_t steps = stream.NextSize(10, 60);
            for (size_t step = 0; step < steps; ++step)
            {
                const size_t op = stream.NextSize(0, 19);
                const std::string cmd = RandomCommand(stream, true);
                const OKey key = RandomKey(stream, true);
                if (op < 12)
                {
                    std::string conflicting;
                    const BindStatus expected = oracle.Bind(cmd, key, &conflicting);
                    const BindResult actual = map.Bind(cmd, FromOracle(key));
                    if (actual.Status != expected || actual.ConflictingCommand != conflicting)
                    {
                        message = "Bind status " + DescribeStatus(actual.Status) + " expected " + DescribeStatus(expected);
                        return false;
                    }
                }
                else if (op < 15)
                {
                    const bool expected = oracle.Unbind(cmd, key);
                    const BindResult actual = map.Unbind(cmd, FromOracle(key));
                    if (actual.Ok() != expected || (!expected && actual.Status != BindStatus::NotBound))
                    {
                        message = "Unbind disagreement";
                        return false;
                    }
                }
                else if (op < 16)
                {
                    const size_t before = oracle.Items.size();
                    std::erase_if(oracle.Items, [&](const OBinding& item) { return item.Cmd == cmd; });
                    if (map.UnbindCommand(cmd) != before - oracle.Items.size())
                    {
                        message = "UnbindCommand count disagreement";
                        return false;
                    }
                }
                else
                {
                    const OKey to = RandomKey(stream, true);
                    OracleMap trial = oracle;
                    BindStatus expected = BindStatus::NotBound;
                    if (trial.Unbind(cmd, key))
                        expected = trial.Bind(cmd, to);
                    if (expected == BindStatus::Bound)
                        oracle = trial;
                    const BindResult actual = map.Rebind(cmd, FromOracle(key), FromOracle(to));
                    if (actual.Status != expected)
                    {
                        message = "Rebind status " + DescribeStatus(actual.Status) + " expected " + DescribeStatus(expected);
                        return false;
                    }
                }
                if (!SameBindings(map, oracle))
                {
                    message = "bindings diverged after step " + std::to_string(step);
                    return false;
                }

                for (int probe = 0; probe < 3; ++probe)
                {
                    const OKey candidate = RandomKey(stream, true);
                    const auto expected = oracle.Clashes(candidate);
                    const std::vector<ChordClash> actual = map.Clashes(FromOracle(candidate));
                    if (actual.size() != expected.size())
                    {
                        message = "clash count " + std::to_string(actual.size()) + " expected " + std::to_string(expected.size());
                        return false;
                    }
                    for (size_t i = 0; i < actual.size(); ++i)
                        if (static_cast<int>(actual[i].Kind) != expected[i].first
                            || actual[i].Existing.CommandId != expected[i].second.Cmd
                            || ToOracle(actual[i].Existing.Key) != expected[i].second.Key)
                        {
                            message = "clash " + std::to_string(i) + " differs";
                            return false;
                        }
                }
                for (const OKey& focus : Foci())
                {
                    const OKey chord = RandomKey(stream, false);
                    const auto expected = oracle.Resolve(chord.Key, chord.Mods, focus);
                    const auto actual = map.Resolve({ chord.Key, chord.Mods }, FromOracle(focus).Scope);
                    if (actual.has_value() != expected.has_value()
                        || (actual && (actual->CommandId != expected->Cmd || ToOracle(actual->Key) != expected->Key)))
                    {
                        message = "Resolve disagreement";
                        return false;
                    }
                }
            }
            return true;
        }, 600);
    }

    bool TestShortcutMapRebindIsAtomic()
    {
        Checker c { "shortcut rebind" };
        const ShortcutKey viewportW { { 'W', 0 }, { ShortcutScopeKind::Viewport, {} } };
        const ShortcutKey viewportE { { 'E', 0 }, { ShortcutScopeKind::Viewport, {} } };
        const ShortcutKey globalZ { { 'Z', 2 }, { ShortcutScopeKind::Global, {} } };
        const ShortcutKey panelDelete { { 261, 0 }, { ShortcutScopeKind::Panel, "hierarchy" } };

        ShortcutMap map;
        c.Expect(map.Bind("tool.translate", viewportW).Ok(), "bind W");
        c.Expect(map.Bind("tool.rotate", viewportE).Ok(), "bind E");
        c.Expect(map.Bind("edit.undo", globalZ).Ok(), "bind Ctrl+Z");
        c.Expect(map.Bind("outliner.delete", panelDelete).Ok(), "bind Delete in the outliner");
        const ShortcutMap before = map;

        BindResult r = map.Rebind("tool.translate", viewportW, viewportE);
        c.Expect(r.Status == BindStatus::Conflict && r.ConflictingCommand == "tool.rotate", "rebind onto a taken chord conflicts");
        c.Expect(map == before, "a conflicting rebind leaves the map unchanged");

        r = map.Rebind("tool.translate", viewportE, viewportW);
        c.Expect(r.Status == BindStatus::NotBound && map == before, "rebinding a chord the command does not hold");
        r = map.Rebind("tool.rotate", viewportE, ShortcutKey { { 340, 0 }, {} });
        c.Expect(r.Status == BindStatus::InvalidChord && map == before, "an invalid target chord is rejected without change");
        r = map.Rebind("tool.rotate", viewportE, ShortcutKey { { 'R', 0 }, { ShortcutScopeKind::Panel, "" } });
        c.Expect(r.Status == BindStatus::InvalidScope && map == before, "an invalid target scope is rejected without change");
        r = map.Rebind("tool.rotate", viewportE, viewportE);
        c.Expect(r.Ok() && map == before, "rebinding a key to itself is a no-op success");

        const ShortcutKey viewportR { { 'R', 0 }, { ShortcutScopeKind::Viewport, {} } };
        r = map.Rebind("tool.rotate", viewportE, viewportR);
        c.Expect(r.Ok() && map.KeysFor("tool.rotate") == std::vector<ShortcutKey> { viewportR }, "a free target rebinds");
        c.Expect(map.Bindings().size() == before.Bindings().size(), "rebind keeps the binding count");

        // Shadowing is reported, never rejected.
        ShortcutMap shadow;
        c.Expect(shadow.Bind("global.w", ShortcutKey { { 'W', 0 }, {} }).Ok(), "bind a Global W");
        c.Expect(shadow.Bind("tool.translate", viewportW).Ok(), "the viewport may shadow a Global chord");
        const auto clashes = shadow.Clashes(viewportW);
        c.Expect(clashes.size() == 2 && clashes[0].Kind == ChordOverlap::Shadows && clashes[1].Kind == ChordOverlap::Conflict,
            "clash list names the shadowed global and the command's own duplicate");
        c.Expect(shadow.Resolve({ 'W', 0 }, { ShortcutScopeKind::Viewport, {} })->CommandId == "tool.translate", "viewport focus prefers the viewport binding");
        c.Expect(shadow.Resolve({ 'W', 0 }, { ShortcutScopeKind::Global, {} })->CommandId == "global.w", "no focus resolves Global");
        c.Expect(shadow.Resolve({ 'W', 0 }, { ShortcutScopeKind::Panel, "console" })->CommandId == "global.w", "another panel falls through to Global");
        c.Expect(!shadow.Resolve({ 340, 0 }, { ShortcutScopeKind::Viewport, {} }).has_value(), "invalid chords never resolve");

        // The binding limit is enforced and leaves the map intact.
        ShortcutMap full;
        size_t bound = 0;
        for (const int key : ChordKeys())
            for (u32 mods = 0; mods < 16 && bound < ShortcutMap::kMaximumBindings; ++mods)
                for (const char* panel : { "a", "b", "c", "d", "e" })
                    if (bound < ShortcutMap::kMaximumBindings
                        && full.Bind("many", ShortcutKey { { key, mods }, { ShortcutScopeKind::Panel, panel } }).Ok())
                        ++bound;
        c.Expect(bound == ShortcutMap::kMaximumBindings, "filled to the limit");
        const ShortcutMap beforeLimit = full;
        c.Expect(full.Bind("many", ShortcutKey { { 'A', 0 }, { ShortcutScopeKind::Viewport, {} } }).Status == BindStatus::LimitReached
            && full == beforeLimit, "the limit rejects without change");
        return c.Ok;
    }
}

namespace
{
    // ---- Shortcut codec oracle ----------------------------------------------------------------------------

    constexpr std::string_view kHeader = "spiral-shortcuts 1";

    struct ReferenceDecoded
    {
        bool Ok = false;
        OracleMap Map;
    };

    const std::map<std::string, std::pair<int, u32>>& CanonicalChords()
    {
        static const std::map<std::string, std::pair<int, u32>> chords = []
        {
            std::map<std::string, std::pair<int, u32>> result;
            for (const GlfwKey& entry : kGlfwKeys)
                for (u32 mods = 0; mods < 16; ++mods)
                    result[OracleFormatChord(entry.Code, mods)] = { entry.Code, mods };
            return result;
        }();
        return chords;
    }

    std::vector<std::string> SplitOn(const std::string& text, char separator)
    {
        std::vector<std::string> pieces(1);
        for (const char ch : text)
        {
            if (ch == separator) pieces.emplace_back();
            else pieces.back() += ch;
        }
        return pieces;
    }

    ReferenceDecoded ReferenceDecode(const OracleMap& defaults, const std::string& text)
    {
        ReferenceDecoded result;
        std::vector<std::string> lines = SplitOn(text, '\n');
        if (lines.back().empty()) lines.pop_back();
        if (lines.empty()) return result;
        for (std::string& line : lines)
            if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lines[0] != kHeader) return result;

        OracleMap map = defaults;
        bool sawBind = false;
        for (size_t i = 1; i < lines.size(); ++i)
        {
            const std::vector<std::string> tokens = SplitOn(lines[i], ' ');
            if (tokens.size() != 4) return result;
            for (const std::string& token : tokens)
                if (token.empty()) return result;
            const bool isBind = tokens[0] == "bind";
            if (!isBind && tokens[0] != "unbind") return result;
            if (!isBind && sawBind) return result;
            sawBind = sawBind || isBind;

            OKey key;
            if (tokens[2] == "global") key.Context = 0;
            else if (tokens[2] == "viewport") key.Context = 1;
            else if (tokens[2].rfind("panel:", 0) == 0)
            {
                key.Context = 2;
                key.Panel = tokens[2].substr(6);
            }
            else return result;
            const auto chord = CanonicalChords().find(tokens[3]);
            if (chord == CanonicalChords().end()) return result;
            key.Key = chord->second.first;
            key.Mods = chord->second.second;
            if (!OracleKeyValid(key)) return result;

            if (isBind)
            {
                if (map.Bind(tokens[1], key) != BindStatus::Bound) return result;
            }
            else if (!map.Unbind(tokens[1], key))
                return result;
        }
        result.Ok = true;
        result.Map = map;
        return result;
    }

    ShortcutMap RandomMap(Spiral::Tests::ChoiceStream& stream, size_t maxBindings)
    {
        ShortcutMap map;
        const size_t count = stream.NextSize(0, maxBindings);
        for (size_t i = 0; i < count; ++i)
            map.Bind(RandomCommand(stream, false), FromOracle(RandomKey(stream, false)));
        return map;
    }

    std::string Mutate(Spiral::Tests::ChoiceStream& stream, std::string text)
    {
        static const std::vector<std::string> lines = { "bind c.a global Ctrl+Z", "unbind c.b viewport W", "bind c.d panel:p1 F5",
            "bind c.a  global Ctrl+Z", "bind c.a global ctrl+z", "bind c.a global Ctrl+Z extra", "frobnicate c.a global A",
            "bind c.a panel: A", "bind c.a panel:P1 A", "bind C.A global A", "bind c.a global Ctrl+Shift", "", " ", "spiral-shortcuts 1" };
        switch (stream.NextSize(0, 6))
        {
        case 0: // byte replace
            if (!text.empty())
                text[stream.NextSize(0, text.size() - 1)] = static_cast<char>(stream.NextSize(0, 255));
            break;
        case 1: // delete a range
            if (!text.empty())
            {
                const size_t a = stream.NextSize(0, text.size() - 1);
                text.erase(a, stream.NextSize(1, 12));
            }
            break;
        case 2: // insert a fragment line
            text.insert(stream.NextSize(0, text.size()), Pick(stream, lines) + "\n");
            break;
        case 3: // truncate
            text.resize(stream.NextSize(0, text.size()));
            break;
        case 4: // duplicate a line
        {
            const std::vector<std::string> pieces = SplitOn(text, '\n');
            const std::string line = Pick(stream, pieces);
            text += line + "\n";
            break;
        }
        case 5: // append a fragment without newline
            text += Pick(stream, lines);
            break;
        default: // CRLF conversion of the whole file
        {
            std::string converted;
            for (const char ch : text)
            {
                if (ch == '\n') converted += '\r';
                converted += ch;
            }
            text = converted;
            break;
        }
        }
        return text;
    }
}

namespace SpiralTests
{
    bool TestShortcutCodecRoundTripsAndFailsClosed()
    {
        Checker c { "shortcut codec" };
        const ShortcutKey ctrlY { { 'Y', 2 }, {} };
        const ShortcutKey ctrlShiftZ { { 'Z', 3 }, {} };
        const ShortcutKey viewportW { { 'W', 0 }, { ShortcutScopeKind::Viewport, {} } };

        ShortcutMap current;
        current.Bind("tool.move", viewportW);
        current.Bind("edit.redo", ctrlShiftZ);
        current.Bind("edit.redo", ctrlY);
        const std::string golden = "spiral-shortcuts 1\nbind edit.redo global Ctrl+Y\nbind edit.redo global Ctrl+Shift+Z\n"
                                   "bind tool.move viewport W\n";
        c.Expect(EncodeShortcutOverrides({}, current) == golden, "golden encoding against empty defaults");
        c.Expect(EncodeShortcutOverrides(current, current) == "spiral-shortcuts 1\n", "no overrides encode to the header only");

        // A later version adds a default for a command the user never touched: it must still arrive.
        ShortcutMap oldDefaults;
        oldDefaults.Bind("edit.undo", ShortcutKey { { 'Z', 2 }, {} });
        ShortcutMap userMap = oldDefaults;
        userMap.Unbind("edit.undo", ShortcutKey { { 'Z', 2 }, {} });
        userMap.Bind("edit.undo", ShortcutKey { { 'U', 2 }, {} });
        const std::string saved = EncodeShortcutOverrides(oldDefaults, userMap);
        c.Expect(saved == "spiral-shortcuts 1\nunbind edit.undo global Ctrl+Z\nbind edit.undo global Ctrl+U\n", "unbind precedes bind");
        ShortcutMap newDefaults = oldDefaults;
        newDefaults.Bind("tool.move", viewportW);
        const ShortcutDecodeResult upgraded = DecodeShortcutOverrides(newDefaults, saved);
        c.Expect(upgraded.Ok && upgraded.Map.KeysFor("tool.move") == std::vector<ShortcutKey> { viewportW }
                && upgraded.Map.KeysFor("edit.undo") == std::vector<ShortcutKey> { ShortcutKey { { 'U', 2 }, {} } },
            "defaults added later survive a saved override file");

        struct Case
        {
            std::string Text;
            size_t Line;
            std::string Fragment;
        };
        const std::string h = "spiral-shortcuts 1\n";
        const std::vector<Case> rejected = {
            { "", 1, "header" }, { "\n", 1, "header" }, { "spiral-shortcuts 2\n", 1, "header" }, { "spiral-shortcuts 1 \n", 1, "header" },
            { "bind c.a global Ctrl+Z\n", 1, "header" }, { h + "\n", 2, "expected" }, { h + "bind c.a global\n", 2, "expected" },
            { h + "bind c.a global Ctrl+Z x\n", 2, "expected" }, { h + "bind  c.a global Ctrl+Z\n", 2, "expected" },
            { h + "bind c.a global Ctrl+Z \n", 2, "expected" }, { h + "bind\tc.a global Ctrl+Z\n", 2, "expected" },
            { h + "frob c.a global A\n", 2, "verb" }, { h + "bind c.a global ctrl+z\n", 2, "chord" },
            { h + "bind c.a global Control+Z\n", 2, "chord" }, { h + "bind c.a global Shift+Ctrl+Z\n", 2, "chord" },
            { h + "bind c.a global Ctrl+Shift\n", 2, "chord" }, { h + "bind c.a nowhere A\n", 2, "scope" },
            { h + "bind c.a panel: A\n", 2, "scope" }, { h + "bind c.a panel:Bad A\n", 2, "scope" },
            { h + "bind Bad global A\n", 2, "invalid command" }, { h + "bind c.a global A\nbind c.b global A\n", 3, "c.a" },
            { h + "bind c.a global A\nbind c.a global A\n", 3, "already present" },
            { h + "unbind c.a global A\n", 2, "does not exist" },
            { h + "bind c.a global A\nunbind c.a global A\n", 3, "precede" },
            { h + "bind c.a global A\n\n", 3, "expected" }, { h + "bind c.a global A\r\r\n", 2, "chord" } };
        ShortcutMap defaults;
        defaults.Bind("zz.default", ShortcutKey { { 'Q', 2 }, {} });
        const ShortcutMap defaultsBefore = defaults;
        for (const Case& test : rejected)
        {
            const ShortcutDecodeResult result = DecodeShortcutOverrides(defaults, test.Text);
            c.Expect(!result.Ok && result.Map.empty() && result.ErrorLine == test.Line
                    && result.Error.find(test.Fragment) != std::string::npos,
                "malformed file rejected whole: line " + std::to_string(test.Line) + " '" + test.Fragment + "' got line "
                    + std::to_string(result.ErrorLine) + " '" + result.Error + "' for: " + test.Text);
        }
        c.Expect(defaults == defaultsBefore, "decoding never modifies the defaults");

        for (const std::string& text : { h + "bind c.a global Ctrl+Z", std::string("spiral-shortcuts 1\r\nbind c.a global Ctrl+Z\r\n"), h })
        {
            const ShortcutDecodeResult result = DecodeShortcutOverrides({}, text);
            c.Expect(result.Ok && result.Error.empty() && result.ErrorLine == 0, "accepted form: " + text);
        }
        const ShortcutDecodeResult oversized = DecodeShortcutOverrides({}, h + std::string(kMaximumShortcutFileBytes, 'x'));
        c.Expect(!oversized.Ok && oversized.Map.empty() && oversized.ErrorLine == 0, "an oversized file is rejected before parsing");

        return RunProperty("ShortcutCodecRoundTripAndMutations", [](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            const ShortcutMap generatedDefaults = RandomMap(stream, 10);
            ShortcutMap generated = generatedDefaults;
            const size_t edits = stream.NextSize(0, 8);
            for (size_t i = 0; i < edits; ++i)
            {
                const std::string cmd = RandomCommand(stream, false);
                const ShortcutKey key = FromOracle(RandomKey(stream, false));
                switch (stream.NextSize(0, 3))
                {
                case 0: generated.Bind(cmd, key); break;
                case 1: generated.Unbind(cmd, key); break;
                case 2: generated.UnbindCommand(cmd); break;
                default:
                    if (!generated.KeysFor(cmd).empty())
                        generated.Rebind(cmd, generated.KeysFor(cmd).front(), key);
                    break;
                }
            }
            const std::string text = EncodeShortcutOverrides(generatedDefaults, generated);
            const ShortcutDecodeResult decoded = DecodeShortcutOverrides(generatedDefaults, text);
            if (!decoded.Ok || !(decoded.Map == generated))
            {
                message = "round trip failed: " + decoded.Error + "\n" + text;
                return false;
            }
            if (EncodeShortcutOverrides(generatedDefaults, decoded.Map) != text)
            {
                message = "encoding is not a fixed point";
                return false;
            }

            OracleMap oracleDefaults;
            oracleDefaults.Items = ToOracle(generatedDefaults);
            const ShortcutMap untouched = generatedDefaults;
            std::string mutated = text;
            for (int round = 0; round < 6; ++round)
            {
                mutated = Mutate(stream, mutated);
                const ShortcutDecodeResult actual = DecodeShortcutOverrides(generatedDefaults, mutated);
                const ReferenceDecoded reference = ReferenceDecode(oracleDefaults, mutated);
                if (actual.Ok != reference.Ok)
                {
                    message = std::string("acceptance differs from the reference (actual ") + (actual.Ok ? "ok" : "rejected: " + actual.Error)
                        + ") for:\n" + mutated;
                    return false;
                }
                if (actual.Ok && ToOracle(actual.Map) != reference.Map.Sorted())
                {
                    message = "decoded map differs from the reference for:\n" + mutated;
                    return false;
                }
                if (!actual.Ok && (!actual.Map.empty() || actual.Error.empty()))
                {
                    message = "a rejected file left state behind for:\n" + mutated;
                    return false;
                }
                if (!(generatedDefaults == untouched))
                {
                    message = "defaults were modified";
                    return false;
                }
            }
            return true;
        }, 1500) && c.Ok;
    }
}

namespace
{
    // ---- Command registry model ---------------------------------------------------------------------------

    struct Slot
    {
        bool Enabled = true;
        std::string Reason;
        bool Succeed = true;
        std::string Message;
        int Calls = 0;
        CommandArgument LastArgument;
        CommandSource LastSource = CommandSource::Menu;
    };

    constexpr size_t kSlotCount = 8;

    std::string SlotId(size_t index)
    {
        return "cmd." + std::to_string(index);
    }

    std::string SourceText(CommandSource source)
    {
        switch (source)
        {
        case CommandSource::Menu: return "menu";
        case CommandSource::Shortcut: return "shortcut";
        case CommandSource::Palette: return "palette";
        case CommandSource::Typed: return "typed control";
        }
        return "?";
    }

    std::string_view KindDescription(CommandArgumentKind kind)
    {
        static constexpr std::string_view names[] = { "no argument", "a flag", "an integer", "a number", "text", "a list of numbers", "a list of ids" };
        return names[static_cast<size_t>(kind)];
    }

    // The refusal the registry must give, written from the contract rather than from the implementation.
    std::string ExpectedArgumentRefusal(CommandArgumentKind kind, const CommandArgument& argument)
    {
        const size_t shape = argument.index();
        if (shape != static_cast<size_t>(kind))
            return kind == CommandArgumentKind::None ? "Takes no argument" : "Expects " + std::string(KindDescription(kind));
        switch (kind)
        {
        case CommandArgumentKind::Text: return std::get<std::string>(argument).size() > 1024 ? "Argument too long" : "";
        case CommandArgumentKind::Ids: return std::get<std::vector<u64>>(argument).size() > 4096 ? "Argument too long" : "";
        case CommandArgumentKind::Reals:
        {
            const auto& values = std::get<std::vector<double>>(argument);
            if (values.size() > 4096)
                return "Argument too long";
            for (const double value : values)
                if (value != value || value == std::numeric_limits<double>::infinity() || value == -std::numeric_limits<double>::infinity())
                    return "Argument must be finite";
            return "";
        }
        case CommandArgumentKind::Real:
        {
            const double value = std::get<double>(argument);
            return value != value || value == std::numeric_limits<double>::infinity() || value == -std::numeric_limits<double>::infinity()
                ? "Argument must be finite" : "";
        }
        default: return "";
        }
    }

    CommandArgument RandomArgument(Spiral::Tests::ChoiceStream& stream)
    {
        constexpr double nan = std::numeric_limits<double>::quiet_NaN();
        constexpr double inf = std::numeric_limits<double>::infinity();
        switch (stream.NextSize(0, 6))
        {
        case 0: return std::monostate {};
        case 1: return stream.NextBool();
        case 2: return static_cast<Engine::i64>(stream.NextI64(std::numeric_limits<Engine::i64>::min(), std::numeric_limits<Engine::i64>::max(), { 0, -1, 1 }));
        case 3:
        {
            static const double pool[] = { 0.0, -0.0, 1.5, -2.25, 1e300, nan, inf, -inf };
            return pool[stream.NextSize(0, 7)];
        }
        case 4:
        {
            static const size_t sizes[] = { 0, 1, 15, 1023, 1024, 1025, 2000 };
            return std::string(sizes[stream.NextSize(0, 6)], 'x');
        }
        case 5:
        {
            static const size_t sizes[] = { 0, 1, 3, 4095, 4096, 4097 };
            std::vector<double> values(sizes[stream.NextSize(0, 5)], 0.5);
            if (!values.empty() && stream.NextSize(0, 3) == 0)
            {
                static const double bad[] = { nan, inf, -inf };
                values[stream.NextSize(0, values.size() - 1)] = bad[stream.NextSize(0, 2)];
            }
            return values;
        }
        default:
        {
            static const size_t sizes[] = { 0, 1, 7, 4095, 4096, 4097 };
            return std::vector<u64>(sizes[stream.NextSize(0, 5)], 42u);
        }
        }
    }

    CommandDescriptor MakeDescriptor(std::string id, std::array<Slot, kSlotCount>& slots, size_t slotIndex)
    {
        CommandDescriptor descriptor;
        descriptor.Id = std::move(id);
        descriptor.Title = "Title " + descriptor.Id;
        descriptor.Category = "Category";
        Slot* slot = &slots[slotIndex];
        descriptor.IsEnabled = [slot]
        {
            return slot->Enabled ? CommandAvailability {} : CommandAvailability::Disabled(slot->Reason);
        };
        descriptor.Execute = [slot](const CommandInvocation& invocation)
        {
            ++slot->Calls;
            slot->LastArgument = invocation.Argument;
            slot->LastSource = invocation.Source;
            return CommandOutcome { slot->Succeed, slot->Message };
        };
        return descriptor;
    }

    struct ModelCommand
    {
        bool Registered = false;
        u8 Sources = 0;
        CommandArgumentKind Kind = CommandArgumentKind::None;
    };
}

namespace SpiralTests
{
    bool TestCommandRegistryRegistrationRules()
    {
        Checker c { "registry registration" };
        std::array<Slot, kSlotCount> slots;
        CommandRegistry registry;

        const auto statusOf = [&](CommandDescriptor descriptor) { return registry.Register(std::move(descriptor)).Status; };
        c.Expect(statusOf(MakeDescriptor("edit.undo", slots, 0)) == RegisterStatus::Registered, "register");
        c.Expect(statusOf(MakeDescriptor("edit.undo", slots, 1)) == RegisterStatus::DuplicateId, "duplicate id rejected");
        c.Expect(registry.Find("edit.undo") != nullptr && registry.Commands().size() == 1, "the duplicate did not replace or add");
        for (const char* bad : { "", "Edit.Undo", "edit..undo", ".edit", "edit.", "edit undo", "edit/undo", "edit\xC3\xA9" })
            c.Expect(statusOf(MakeDescriptor(bad, slots, 0)) == RegisterStatus::InvalidId, std::string("invalid id '") + bad + "'");
        c.Expect(statusOf(MakeDescriptor(std::string(65, 'a'), slots, 0)) == RegisterStatus::InvalidId, "65-byte id");
        c.Expect(statusOf(MakeDescriptor(std::string(64, 'a'), slots, 0)) == RegisterStatus::Registered, "64-byte id");

        const auto withTitle = [&](std::string id, std::string title)
        {
            CommandDescriptor d = MakeDescriptor(std::move(id), slots, 0);
            d.Title = std::move(title);
            return d;
        };
        c.Expect(statusOf(withTitle("t.empty", "")) == RegisterStatus::InvalidTitle, "empty title");
        c.Expect(statusOf(withTitle("t.control", std::string("a\nb"))) == RegisterStatus::InvalidTitle, "control character in title");
        c.Expect(statusOf(withTitle("t.del", std::string("a\x7F"))) == RegisterStatus::InvalidTitle, "DEL in title");
        c.Expect(statusOf(withTitle("t.long", std::string(129, 'x'))) == RegisterStatus::InvalidTitle, "129-byte title");
        c.Expect(statusOf(withTitle("t.max", std::string(128, 'x'))) == RegisterStatus::Registered, "128-byte title");
        {
            CommandDescriptor d = MakeDescriptor("t.category", slots, 0);
            d.Category.clear();
            c.Expect(statusOf(std::move(d)) == RegisterStatus::InvalidCategory, "empty category");
            d = MakeDescriptor("t.exec", slots, 0);
            d.Execute = nullptr;
            c.Expect(statusOf(std::move(d)) == RegisterStatus::MissingExecute, "missing execute");
            d = MakeDescriptor("t.sources0", slots, 0);
            d.AllowedSources = 0;
            c.Expect(statusOf(std::move(d)) == RegisterStatus::InvalidSources, "no allowed source");
            d = MakeDescriptor("t.sources1", slots, 0);
            d.AllowedSources = 0x10;
            c.Expect(statusOf(std::move(d)) == RegisterStatus::InvalidSources, "unknown source bit");
        }

        // Default shortcuts register atomically and stay conflict-free.
        const ShortcutKey ctrlZ { { 'Z', 2 }, {} };
        const ShortcutKey ctrlY { { 'Y', 2 }, {} };
        {
            CommandDescriptor d = MakeDescriptor("short.a", slots, 0);
            d.DefaultShortcuts = { ctrlZ, ctrlY };
            c.Expect(statusOf(std::move(d)) == RegisterStatus::Registered, "two default shortcuts");
        }
        const size_t countBefore = registry.Commands().size();
        const ShortcutMap defaultsBefore = registry.DefaultShortcuts();
        {
            CommandDescriptor d = MakeDescriptor("short.b", slots, 0);
            d.DefaultShortcuts = { ShortcutKey { { 'Q', 2 }, {} }, ctrlY };
            const RegisterResult r = registry.Register(std::move(d));
            c.Expect(r.Status == RegisterStatus::ShortcutConflict && r.ConflictingCommand == "short.a", "conflicting default chord names its owner");
        }
        c.Expect(registry.Commands().size() == countBefore && registry.DefaultShortcuts() == defaultsBefore && !registry.Find("short.b"),
            "a conflicting registration changes nothing, including the chords that did fit");
        {
            CommandDescriptor d = MakeDescriptor("short.c", slots, 0);
            d.DefaultShortcuts = { ShortcutKey { { 'Q', 2 }, {} }, ShortcutKey { { 'Q', 2 }, {} } };
            c.Expect(statusOf(std::move(d)) == RegisterStatus::InvalidShortcut && registry.DefaultShortcuts() == defaultsBefore,
                "a command repeating its own chord is invalid and atomic");
            d = MakeDescriptor("short.d", slots, 0);
            d.DefaultShortcuts = { ShortcutKey { { 340, 0 }, {} } };
            c.Expect(statusOf(std::move(d)) == RegisterStatus::InvalidShortcut, "modifier key as a default chord");
            d = MakeDescriptor("short.e", slots, 0);
            d.DefaultShortcuts = { ShortcutKey { { 'W', 0 }, { ShortcutScopeKind::Viewport, {} } } };
            c.Expect(statusOf(std::move(d)) == RegisterStatus::Registered, "scoped default");
            d = MakeDescriptor("short.f", slots, 0);
            d.DefaultShortcuts = { ShortcutKey { { 'W', 0 }, {} } };
            c.Expect(statusOf(std::move(d)) == RegisterStatus::Registered, "a Global default may be shadowed by a viewport one");
        }

        // Enumeration is registration order and survives growth.
        CommandRegistry ordered;
        std::vector<std::string> expectedOrder;
        for (size_t i = 0; i < 300; ++i)
        {
            const std::string id = "order." + std::to_string((i * 7919) % 1000);
            if (ordered.Register(MakeDescriptor(id, slots, 0)).Ok())
                expectedOrder.push_back(id);
        }
        std::vector<std::string> actualOrder;
        for (const CommandDescriptor& d : ordered.Commands())
            actualOrder.push_back(d.Id);
        c.Expect(actualOrder == expectedOrder && !expectedOrder.empty(), "enumeration is registration order");
        for (const std::string& id : expectedOrder)
            c.Expect(ordered.Find(id) && ordered.Find(id)->Id == id, "Find after growth " + id);

        CommandRegistry limited;
        size_t registered = 0;
        for (size_t i = 0; i < CommandRegistry::kMaximumCommands + 5; ++i)
            if (limited.Register(MakeDescriptor("lim." + std::to_string(i), slots, 0)).Ok())
                ++registered;
        c.Expect(registered == CommandRegistry::kMaximumCommands, "the command limit is enforced");
        c.Expect(limited.Register(MakeDescriptor("lim.extra", slots, 0)).Status == RegisterStatus::LimitReached, "LimitReached status");
        return c.Ok;
    }

    bool TestCommandRegistryDispatchMatchesModel()
    {
        return RunProperty("CommandRegistryDispatchMatchesModel", [](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            std::array<Slot, kSlotCount> slots;
            CommandRegistry registry;
            std::array<ModelCommand, kSlotCount> model;
            std::vector<std::string> registrationOrder;
            OracleMap defaultsOracle;
            std::vector<DispatchResult> observed;
            registry.SetObserver([&](const DispatchResult& result) { observed.push_back(result); });
            size_t expectedObserved = 0;

            const size_t steps = stream.NextSize(20, 80);
            for (size_t step = 0; step < steps; ++step)
            {
                const size_t op = stream.NextSize(0, 9);
                if (op < 3)
                {
                    const size_t index = stream.NextSize(0, kSlotCount - 1);
                    CommandDescriptor descriptor = MakeDescriptor(SlotId(index), slots, index);
                    RegisterStatus expected = RegisterStatus::Registered;
                    switch (stream.NextSize(0, 11))
                    {
                    case 0: descriptor.Id = "Bad Id"; expected = RegisterStatus::InvalidId; break;
                    case 1: descriptor.Title.clear(); expected = RegisterStatus::InvalidTitle; break;
                    case 2: descriptor.Category.clear(); expected = RegisterStatus::InvalidCategory; break;
                    case 3: descriptor.Execute = nullptr; expected = RegisterStatus::MissingExecute; break;
                    case 4: descriptor.AllowedSources = 0; expected = RegisterStatus::InvalidSources; break;
                    case 5: descriptor.ArgumentKind = static_cast<CommandArgumentKind>(7); expected = RegisterStatus::InvalidArgumentKind; break;
                    default:
                        descriptor.AllowedSources = static_cast<u8>(stream.NextSize(1, 15));
                        descriptor.ArgumentKind = static_cast<CommandArgumentKind>(stream.NextSize(0, 6));
                        break;
                    }
                    std::vector<OKey> shortcuts;
                    const size_t shortcutCount = stream.NextSize(0, 2);
                    for (size_t i = 0; i < shortcutCount; ++i)
                    {
                        shortcuts.push_back(RandomKey(stream, true));
                        descriptor.DefaultShortcuts.push_back(FromOracle(shortcuts.back()));
                    }
                    std::string expectedConflict;
                    OracleMap trial = defaultsOracle;
                    if (expected == RegisterStatus::Registered && model[index].Registered)
                        expected = RegisterStatus::DuplicateId;
                    if (expected == RegisterStatus::Registered)
                        for (const OKey& key : shortcuts)
                        {
                            std::string conflicting;
                            const BindStatus bound = trial.Bind(descriptor.Id, key, &conflicting);
                            if (bound == BindStatus::Bound) continue;
                            expected = bound == BindStatus::Conflict ? RegisterStatus::ShortcutConflict : RegisterStatus::InvalidShortcut;
                            expectedConflict = conflicting;
                            break;
                        }
                    const u8 sources = descriptor.AllowedSources;
                    const CommandArgumentKind kind = descriptor.ArgumentKind;
                    const RegisterResult actual = registry.Register(std::move(descriptor));
                    if (actual.Status != expected || actual.ConflictingCommand != expectedConflict)
                    {
                        message = "register status " + std::to_string(static_cast<int>(actual.Status)) + " expected "
                            + std::to_string(static_cast<int>(expected));
                        return false;
                    }
                    if (expected == RegisterStatus::Registered)
                    {
                        model[index] = { true, sources, kind };
                        registrationOrder.push_back(SlotId(index));
                        defaultsOracle = trial;
                    }
                }
                else if (op == 3)
                {
                    Slot& slot = slots[stream.NextSize(0, kSlotCount - 1)];
                    slot.Enabled = stream.NextBool();
                    slot.Reason = stream.NextBool() ? "because " + std::to_string(step) : std::string();
                    slot.Succeed = stream.NextSize(0, 3) != 0;
                    slot.Message = stream.NextBool() ? "msg " + std::to_string(step) : std::string();
                }
                else
                {
                    const bool known = stream.NextSize(0, 7) != 0;
                    const size_t index = stream.NextSize(0, kSlotCount - 1);
                    const std::string id = known ? SlotId(index) : "missing.command";
                    const auto source = static_cast<CommandSource>(stream.NextSize(0, 3));
                    CommandArgument argument = RandomArgument(stream);
                    if (known && model[index].Registered && stream.NextBool())
                    {
                        // Half of the calls pass the shape the command declared, so valid arguments are exercised too.
                        for (size_t attempt = 0; attempt < 8 && ArgumentKindOf(argument) != model[index].Kind; ++attempt)
                            argument = RandomArgument(stream);
                    }

                    // Query first: it must agree with what Dispatch is about to do and must not run Execute.
                    const int callsBefore = known ? slots[index].Calls : 0;
                    const CommandAvailability availability = registry.Query(id, source);
                    if (known && slots[index].Calls != callsBefore)
                    {
                        message = "Query executed a command";
                        return false;
                    }

                    DispatchStatus expected = DispatchStatus::Executed;
                    std::string expectedReason;
                    std::string expectedMessage;
                    bool expectCall = false;
                    if (!known || !model[index].Registered)
                    {
                        expected = DispatchStatus::NotFound;
                        expectedReason = "Unknown command";
                    }
                    else if ((model[index].Sources & (1u << static_cast<u8>(source))) == 0)
                    {
                        expected = DispatchStatus::SourceNotAllowed;
                        expectedReason = "Not available from " + SourceText(source);
                    }
                    else if (!ExpectedArgumentRefusal(model[index].Kind, argument).empty())
                    {
                        expected = DispatchStatus::InvalidArgument;
                        expectedReason = ExpectedArgumentRefusal(model[index].Kind, argument);
                    }
                    else if (!slots[index].Enabled)
                    {
                        expected = DispatchStatus::Disabled;
                        expectedReason = slots[index].Reason.empty() ? "Unavailable" : slots[index].Reason;
                    }
                    else if (slots[index].Succeed)
                    {
                        expectCall = true;
                        expectedMessage = slots[index].Message;
                    }
                    else
                    {
                        expected = DispatchStatus::Failed;
                        expectCall = true;
                        expectedReason = slots[index].Message.empty() ? "Command failed" : slots[index].Message;
                    }

                    CommandAvailability expectedAvailability;
                    if (!known || !model[index].Registered)
                        expectedAvailability = CommandAvailability::Disabled("Unknown command");
                    else if ((model[index].Sources & (1u << static_cast<u8>(source))) == 0)
                        expectedAvailability = CommandAvailability::Disabled("Not available from " + SourceText(source));
                    else if (!slots[index].Enabled)
                        expectedAvailability = CommandAvailability::Disabled(slots[index].Reason.empty() ? "Unavailable" : slots[index].Reason);
                    if (availability.Enabled != expectedAvailability.Enabled || availability.Reason != expectedAvailability.Reason)
                    {
                        message = "Query disagrees with the model for " + id + ": '" + availability.Reason + "'";
                        return false;
                    }

                    const DispatchResult actual = registry.Dispatch(id, source, argument);
                    ++expectedObserved;
                    if (actual.Status != expected || actual.Reason != expectedReason || actual.Message != expectedMessage
                        || actual.CommandId != id || actual.Source != source)
                    {
                        message = "dispatch of " + id + " got status " + std::to_string(static_cast<int>(actual.Status)) + " reason '"
                            + actual.Reason + "' message '" + actual.Message + "' expected status "
                            + std::to_string(static_cast<int>(expected)) + " reason '" + expectedReason + "'";
                        return false;
                    }
                    if (known)
                    {
                        if (slots[index].Calls != callsBefore + (expectCall ? 1 : 0))
                        {
                            message = "Execute call count wrong for " + id;
                            return false;
                        }
                        if (expectCall && (!(slots[index].LastArgument == argument) || slots[index].LastSource != source))
                        {
                            message = "Execute received a different invocation";
                            return false;
                        }
                    }
                    if (observed.size() != expectedObserved || observed.back().Status != actual.Status
                        || observed.back().CommandId != id || observed.back().Reason != actual.Reason)
                    {
                        message = "observer was not called exactly once with the final result";
                        return false;
                    }
                }

                std::vector<std::string> order;
                for (const CommandDescriptor& d : registry.Commands())
                    order.push_back(d.Id);
                if (order != registrationOrder)
                {
                    message = "enumeration order diverged";
                    return false;
                }
                if (ToOracle(registry.DefaultShortcuts()) != defaultsOracle.Sorted())
                {
                    message = "default shortcut map diverged";
                    return false;
                }
            }
            return true;
        }, 500);
    }

    bool TestCommandRegistryReentrancyAndSources()
    {
        Checker c { "registry reentrancy" };

        // A command that dispatches itself runs exactly kMaximumDispatchDepth times, then the guard refuses.
        {
            CommandRegistry registry;
            int executions = 0;
            DispatchStatus innermost = DispatchStatus::Executed;
            CommandDescriptor d;
            d.Id = "re.self";
            d.Title = "Self";
            d.Category = "Test";
            d.Execute = [&](const CommandInvocation&)
            {
                ++executions;
                innermost = registry.Dispatch("re.self", CommandSource::Palette).Status;
                return CommandOutcome {};
            };
            c.Expect(registry.Register(std::move(d)).Ok(), "register recursive command");
            size_t observerCalls = 0;
            registry.SetObserver([&](const DispatchResult&) { ++observerCalls; });
            const DispatchResult result = registry.Dispatch("re.self", CommandSource::Menu);
            c.Expect(result.Executed(), "the outer dispatch still succeeds");
            c.Expect(executions == static_cast<int>(CommandRegistry::kMaximumDispatchDepth), "recursion stops at the depth limit");
            c.Expect(innermost == DispatchStatus::Executed, "the second-innermost dispatch executed");
            c.Expect(observerCalls == CommandRegistry::kMaximumDispatchDepth, "observer saw every executed nested result once and not the TooDeep refusal");
            c.Expect(registry.Dispatch("re.self", CommandSource::Menu).Executed(), "depth is restored after unwinding");
        }

        // The refusal itself is reported as TooDeep.
        {
            CommandRegistry registry;
            std::vector<DispatchStatus> statuses;
            CommandDescriptor d;
            d.Id = "re.chain";
            d.Title = "Chain";
            d.Category = "Test";
            d.Execute = [&](const CommandInvocation&)
            {
                statuses.push_back(registry.Dispatch("re.chain", CommandSource::Menu).Status);
                return CommandOutcome {};
            };
            registry.Register(std::move(d));
            registry.Dispatch("re.chain", CommandSource::Menu);
            c.Expect(!statuses.empty() && statuses.front() == DispatchStatus::TooDeep, "the deepest dispatch returns TooDeep");
        }

        // Registering from inside Execute may reallocate the command vector; the running call must survive.
        {
            CommandRegistry registry;
            int afterRegister = 0;
            CommandDescriptor d;
            d.Id = "re.grow";
            d.Title = "Grow";
            d.Category = "Test";
            d.Execute = [&](const CommandInvocation&)
            {
                for (int i = 0; i < 64; ++i)
                {
                    CommandDescriptor extra;
                    extra.Id = "re.extra." + std::to_string(i);
                    extra.Title = "Extra";
                    extra.Category = "Test";
                    extra.Execute = [](const CommandInvocation&) { return CommandOutcome {}; };
                    registry.Register(std::move(extra));
                }
                ++afterRegister;
                return CommandOutcome { true, "grown" };
            };
            registry.Register(std::move(d));
            const DispatchResult result = registry.Dispatch("re.grow", CommandSource::Menu);
            c.Expect(result.Executed() && result.Message == "grown" && afterRegister == 1 && registry.Commands().size() == 65,
                "a command can register commands while it runs");
        }

        // An observer that dispatches is bounded by the same guard and may replace itself.
        {
            CommandRegistry registry;
            int calls = 0;
            CommandDescriptor d;
            d.Id = "obs.cmd";
            d.Title = "Observed";
            d.Category = "Test";
            d.Execute = [&](const CommandInvocation&) { ++calls; return CommandOutcome {}; };
            registry.Register(std::move(d));
            size_t observed = 0;
            registry.SetObserver([&](const DispatchResult&)
            {
                ++observed;
                registry.Dispatch("obs.cmd", CommandSource::Menu);
            });
            registry.Dispatch("obs.cmd", CommandSource::Menu);
            c.Expect(observed >= 1 && observed <= CommandRegistry::kMaximumDispatchDepth && calls >= 1, "a dispatching observer terminates within the depth limit");
            registry.SetObserver({});
            const int before = calls;
            registry.Dispatch("obs.cmd", CommandSource::Menu);
            c.Expect(calls == before + 1 && observed <= CommandRegistry::kMaximumDispatchDepth, "clearing the observer stops notifications");
        }

        // Typed control must opt in, and menus must not reach a typed-only command.
        {
            CommandRegistry registry;
            const auto make = [](const char* id, u8 sources)
            {
                CommandDescriptor d;
                d.Id = id;
                d.Title = id;
                d.Category = "Test";
                d.AllowedSources = sources;
                d.ArgumentKind = CommandArgumentKind::Text;
                d.Execute = [](const CommandInvocation& invocation) { return CommandOutcome { true, std::get<std::string>(invocation.Argument) }; };
                return d;
            };
            registry.Register(make("src.default", kDefaultCommandSources));
            registry.Register(make("src.all", kAllCommandSources));
            registry.Register(make("src.typed", CommandSourceBit(CommandSource::Typed)));
            c.Expect(registry.Dispatch("src.default", CommandSource::Typed, std::string("x")).Status == DispatchStatus::SourceNotAllowed,
                "typed control cannot reach a command that did not opt in");
            c.Expect(registry.Dispatch("src.default", CommandSource::Typed, std::string("x")).Reason == "Not available from typed control", "refusal reason");
            c.Expect(!registry.Query("src.default", CommandSource::Typed).Enabled && registry.Query("src.default", CommandSource::Menu).Enabled,
                "Query is source aware");
            c.Expect(registry.Dispatch("src.all", CommandSource::Typed, std::string("panel.fab")).Message == "panel.fab", "the argument reaches the command unchanged");
            c.Expect(registry.Dispatch("src.typed", CommandSource::Menu, std::string("x")).Status == DispatchStatus::SourceNotAllowed, "typed-only commands refuse menus");
            c.Expect(registry.Dispatch("src.typed", CommandSource::Typed, std::string("x")).Executed(), "typed-only commands accept typed control");
            c.Expect(registry.Dispatch("src.typed", CommandSource::Typed, std::string(1025, 'x')).Status == DispatchStatus::InvalidArgument,
                "an oversized argument is refused");
        }

        // Disabled without a stated reason and failure without a message still explain themselves.
        {
            CommandRegistry registry;
            CommandDescriptor d;
            d.Id = "msg.silent";
            d.Title = "Silent";
            d.Category = "Test";
            d.IsEnabled = [] { return CommandAvailability { false, {} }; };
            d.Execute = [](const CommandInvocation&) { return CommandOutcome { false, {} }; };
            registry.Register(d);
            c.Expect(registry.Dispatch("msg.silent", CommandSource::Menu).Reason == "Unavailable", "default disabled reason");
            c.Expect(registry.Query("msg.silent").Reason == "Unavailable", "Query default disabled reason");
            d.Id = "msg.fail";
            d.IsEnabled = nullptr;
            registry.Register(d);
            const DispatchResult failed = registry.Dispatch("msg.fail", CommandSource::Menu);
            c.Expect(failed.Status == DispatchStatus::Failed && failed.Reason == "Command failed" && failed.Message.empty(), "default failure reason");
        }
        return c.Ok;
    }

    bool TestCommandRegistryTypedArguments()
    {
        Checker c { "registry typed arguments" };
        CommandRegistry registry;
        CommandArgument received;
        int calls = 0;
        const auto add = [&](const char* id, CommandArgumentKind kind)
        {
            CommandDescriptor d;
            d.Id = id;
            d.Title = id;
            d.Category = "Test";
            d.AllowedSources = kAllCommandSources;
            d.ArgumentKind = kind;
            d.Execute = [&](const CommandInvocation& invocation)
            {
                ++calls;
                received = invocation.Argument;
                return CommandOutcome {};
            };
            c.Expect(registry.Register(std::move(d)).Ok(), std::string("register ") + id);
        };
        add("arg.none", CommandArgumentKind::None);
        add("arg.flag", CommandArgumentKind::Flag);
        add("arg.int", CommandArgumentKind::Integer);
        add("arg.real", CommandArgumentKind::Real);
        add("arg.text", CommandArgumentKind::Text);
        add("arg.reals", CommandArgumentKind::Reals);
        add("arg.ids", CommandArgumentKind::Ids);

        const auto accepts = [&](const char* id, CommandArgument value, const char* what)
        {
            const int before = calls;
            const DispatchResult result = registry.Dispatch(id, CommandSource::Typed, value);
            c.Expect(result.Executed() && calls == before + 1 && received == value, std::string("accepts ") + what);
        };
        const auto refuses = [&](const char* id, CommandArgument value, const char* reason, const char* what)
        {
            const int before = calls;
            const DispatchResult result = registry.Dispatch(id, CommandSource::Typed, value);
            c.Expect(result.Status == DispatchStatus::InvalidArgument && result.Reason == reason && calls == before,
                std::string("refuses ") + what + ": got '" + result.Reason + "'");
        };
        constexpr double inf = std::numeric_limits<double>::infinity();
        constexpr double nan = std::numeric_limits<double>::quiet_NaN();

        accepts("arg.none", std::monostate {}, "no argument");
        accepts("arg.flag", true, "true");
        accepts("arg.flag", false, "false");
        accepts("arg.int", std::numeric_limits<Engine::i64>::min(), "i64 minimum");
        accepts("arg.int", std::numeric_limits<Engine::i64>::max(), "i64 maximum");
        accepts("arg.real", -2.5, "a finite number");
        accepts("arg.text", std::string(), "empty text");
        accepts("arg.text", std::string(1024, 't'), "1024 bytes of text");
        accepts("arg.reals", std::vector<double> {}, "an empty list");
        accepts("arg.reals", std::vector<double>(4096, 1.0), "4096 numbers");
        accepts("arg.ids", std::vector<u64> { 1, std::numeric_limits<u64>::max() }, "ids");
        accepts("arg.ids", std::vector<u64>(4096, 7), "4096 ids");
        accepts("arg.real", -0.0, "negative zero");
        c.Expect(std::signbit(std::get<double>(received)), "negative zero keeps its sign through dispatch");

        refuses("arg.none", true, "Takes no argument", "an argument for a no-argument command");
        refuses("arg.flag", std::monostate {}, "Expects a flag", "a missing flag");
        refuses("arg.flag", Engine::i64 { 1 }, "Expects a flag", "an integer for a flag");
        refuses("arg.int", true, "Expects an integer", "a flag for an integer");
        refuses("arg.int", 1.0, "Expects an integer", "a real for an integer");
        refuses("arg.real", Engine::i64 { 1 }, "Expects a number", "an integer for a real");
        refuses("arg.text", std::monostate {}, "Expects text", "missing text");
        refuses("arg.reals", std::vector<u64> {}, "Expects a list of numbers", "ids for reals");
        refuses("arg.ids", std::vector<double> {}, "Expects a list of ids", "reals for ids");
        refuses("arg.text", std::string(1025, 't'), "Argument too long", "1025 bytes of text");
        refuses("arg.reals", std::vector<double>(4097, 1.0), "Argument too long", "4097 numbers");
        refuses("arg.ids", std::vector<u64>(4097, 7), "Argument too long", "4097 ids");
        refuses("arg.real", nan, "Argument must be finite", "NaN");
        refuses("arg.real", inf, "Argument must be finite", "infinity");
        refuses("arg.reals", std::vector<double> { 1.0, 2.0, -inf }, "Argument must be finite", "a list holding -infinity");
        refuses("arg.reals", std::vector<double> { nan }, "Argument must be finite", "a list holding NaN");

        // The argument is checked before the enabled predicate, so a caller learns about a malformed call even
        // when the command is also unavailable.
        CommandDescriptor off;
        off.Id = "arg.off";
        off.Title = "Off";
        off.Category = "Test";
        off.ArgumentKind = CommandArgumentKind::Flag;
        off.IsEnabled = [] { return CommandAvailability::Disabled("not now"); };
        off.Execute = [](const CommandInvocation&) { return CommandOutcome {}; };
        registry.Register(std::move(off));
        c.Expect(registry.Dispatch("arg.off", CommandSource::Menu, true).Reason == "not now", "a well-formed call to a disabled command reports why");
        c.Expect(registry.Dispatch("arg.off", CommandSource::Menu).Status == DispatchStatus::InvalidArgument, "a malformed call is refused for its argument first");
        return c.Ok;
    }
}

namespace
{
    // ---- Fuzzy oracle -------------------------------------------------------------------------------------

    struct Atom
    {
        const char* Bytes;
        char Ascii; // the character for ASCII atoms, 0 otherwise
    };

    const std::vector<Atom>& Alphabet()
    {
        static const std::vector<Atom> atoms = { { "a", 'a' }, { "b", 'b' }, { "c", 'c' }, { "A", 'A' }, { "B", 'B' },
            { "C", 'C' }, { " ", ' ' }, { "-", '-' }, { "_", '_' }, { ".", '.' }, { "1", '1' }, { "a", 'a' }, { "b", 'b' },
            { "\xC3\xA9", 0 }, { "\xE6\x97\xA5", 0 }, { "\xF0\x9F\x98\x80", 0 } };
        return atoms;
    }

    struct Word
    {
        std::vector<size_t> Atoms;
        std::string Bytes;
        std::vector<size_t> Offsets;
    };

    Word MakeWord(Spiral::Tests::ChoiceStream& stream, size_t minimum, size_t maximum)
    {
        Word word;
        const size_t length = stream.NextSize(minimum, maximum);
        for (size_t i = 0; i < length; ++i)
        {
            const size_t atom = stream.NextSize(0, Alphabet().size() - 1);
            word.Atoms.push_back(atom);
            word.Offsets.push_back(word.Bytes.size());
            word.Bytes += Alphabet()[atom].Bytes;
        }
        return word;
    }

    bool AtomMatches(const Atom& query, const Atom& text)
    {
        if (query.Ascii >= 'a' && query.Ascii <= 'z')
            return text.Ascii == query.Ascii || text.Ascii == query.Ascii - 'a' + 'A';
        return std::string_view(query.Bytes) == text.Bytes;
    }

    int OracleBoundary(const Word& text, size_t p)
    {
        if (p == 0) return 24;
        const char previous = Alphabet()[text.Atoms[p - 1]].Ascii;
        const char current = Alphabet()[text.Atoms[p]].Ascii;
        const bool previousAlnum = (previous >= 'a' && previous <= 'z') || (previous >= 'A' && previous <= 'Z') || (previous >= '0' && previous <= '9');
        if (previous != 0 && !previousAlnum) return 20;
        if (previous >= 'a' && previous <= 'z' && current >= 'A' && current <= 'Z') return 16;
        return 0;
    }

    int OracleScore(const Word& text, const std::vector<size_t>& positions)
    {
        int score = -static_cast<int>(std::min<size_t>(positions[0], 6));
        for (size_t j = 0; j < positions.size(); ++j)
        {
            score += 16 + OracleBoundary(text, positions[j]);
            if (j > 0)
            {
                const size_t gap = positions[j] - positions[j - 1] - 1;
                score += gap == 0 ? 14 : -static_cast<int>(gap + 3);
            }
        }
        return score;
    }

    struct OracleFuzzy
    {
        bool Matched = false;
        FuzzyTier Tier = FuzzyTier::Subsequence;
        int Score = 0;
        std::vector<FuzzyHighlight> Highlights;
    };

    void EnumerateAlignments(const Word& query, const Word& text, std::vector<size_t>& current, size_t from,
        std::vector<std::vector<size_t>>& out)
    {
        if (current.size() == query.Atoms.size())
        {
            out.push_back(current);
            return;
        }
        for (size_t p = from; p < text.Atoms.size(); ++p)
            if (AtomMatches(Alphabet()[query.Atoms[current.size()]], Alphabet()[text.Atoms[p]]))
            {
                current.push_back(p);
                EnumerateAlignments(query, text, current, p + 1, out);
                current.pop_back();
            }
    }

    OracleFuzzy OracleMatch(const Word& query, const Word& text)
    {
        OracleFuzzy result;
        if (query.Atoms.empty())
        {
            result.Matched = true;
            result.Tier = FuzzyTier::Prefix;
            return result;
        }
        std::vector<std::vector<size_t>> alignments; // lexicographic order by construction
        std::vector<size_t> scratch;
        EnumerateAlignments(query, text, scratch, 0, alignments);
        if (alignments.empty()) return result;

        const auto contiguous = [](const std::vector<size_t>& a)
        {
            for (size_t j = 1; j < a.size(); ++j)
                if (a[j] != a[j - 1] + 1) return false;
            return true;
        };
        const std::vector<size_t>* chosen = nullptr;
        int chosenScore = 0;
        const auto consider = [&](const std::vector<size_t>& a)
        {
            const int score = OracleScore(text, a);
            if (!chosen || score > chosenScore)
            {
                chosen = &a;
                chosenScore = score;
            }
        };
        bool anyContiguous = false;
        bool prefix = false;
        for (const auto& a : alignments)
        {
            if (!contiguous(a)) continue;
            anyContiguous = true;
            prefix = prefix || a[0] == 0;
        }
        result.Matched = true;
        if (prefix)
        {
            result.Tier = query.Atoms.size() == text.Atoms.size() ? FuzzyTier::Exact : FuzzyTier::Prefix;
            for (const auto& a : alignments)
                if (contiguous(a) && a[0] == 0) consider(a);
        }
        else if (anyContiguous)
        {
            result.Tier = FuzzyTier::Substring;
            for (const auto& a : alignments)
                if (contiguous(a)) consider(a);
        }
        else
        {
            result.Tier = FuzzyTier::Subsequence;
            for (const auto& a : alignments)
                consider(a);
        }
        result.Score = chosenScore;
        for (const size_t p : *chosen)
            result.Highlights.push_back({ static_cast<u32>(text.Offsets[p]), static_cast<u32>(std::string_view(Alphabet()[text.Atoms[p]].Bytes).size()) });
        return result;
    }

    std::string DescribeFuzzy(bool matched, FuzzyTier tier, int score, const std::vector<FuzzyHighlight>& highlights)
    {
        std::string text = matched ? "tier " + std::to_string(static_cast<int>(tier)) + " score " + std::to_string(score) + " at" : "no match";
        for (const FuzzyHighlight& h : highlights)
            text += " " + std::to_string(h.Offset) + "+" + std::to_string(h.Length);
        return text;
    }

    void ExpectFuzzy(Checker& c, std::string_view query, std::string_view text, bool matched, FuzzyTier tier, int score,
        std::vector<FuzzyHighlight> highlights)
    {
        const FuzzyMatchResult actual = FuzzyMatch(query, text);
        const std::string label = "'" + std::string(query) + "' in '" + std::string(text) + "': ";
        if (!matched)
        {
            c.Expect(!actual.Matched && actual.Highlights.empty(), label + "expected no match, got "
                + DescribeFuzzy(actual.Matched, actual.Tier, actual.Score, actual.Highlights));
            return;
        }
        c.Expect(actual.Matched && actual.Tier == tier && actual.Score == score && actual.Highlights == highlights,
            label + "expected " + DescribeFuzzy(true, tier, score, highlights) + " got "
                + DescribeFuzzy(actual.Matched, actual.Tier, actual.Score, actual.Highlights));
    }
}

namespace SpiralTests
{
    bool TestFuzzyMatchGoldenCases()
    {
        Checker c { "fuzzy golden" };
        using T = FuzzyTier;
        // Hand-computed from the scoring table in FuzzyMatch.h.
        ExpectFuzzy(c, "cu", "Cube", true, T::Prefix, 70, { { 0, 1 }, { 1, 1 } });           // (16+24) + (16+0+14)
        ExpectFuzzy(c, "Cu", "Cube", true, T::Prefix, 70, { { 0, 1 }, { 1, 1 } });           // uppercase query letter hits uppercase text
        ExpectFuzzy(c, "cube", "Cube", true, T::Exact, 130, { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } });
        ExpectFuzzy(c, "Cube", "cube", false, T::Exact, 0, {});                              // uppercase query letter rejects lowercase text
        ExpectFuzzy(c, "CUBE", "Cube", false, T::Exact, 0, {});
        ExpectFuzzy(c, "cb", "Cube", true, T::Subsequence, 52, { { 0, 1 }, { 2, 1 } });     // 40 + (16+0-4)
        ExpectFuzzy(c, "be", "Cube", true, T::Substring, 44, { { 2, 1 }, { 3, 1 } });        // -2 + 16 + (16+14)
        ExpectFuzzy(c, "mc", "Move Cube", true, T::Subsequence, 69, { { 0, 1 }, { 5, 1 } }); // 40 + (16+20-7)
        ExpectFuzzy(c, "sr", "SceneRenderer", true, T::Subsequence, 65, { { 0, 1 }, { 5, 1 } }); // camelCase step beats later r
        ExpectFuzzy(c, "abc", "a-bxc-b-c", true, T::Subsequence, 100, { { 0, 1 }, { 2, 1 }, { 8, 1 } }); // greedy (0,2,4) scores 84
        ExpectFuzzy(c, "a", "aa", true, T::Prefix, 40, { { 0, 1 } });
        ExpectFuzzy(c, "a", "ba", true, T::Substring, 15, { { 1, 1 } });
        ExpectFuzzy(c, "ab", "xab ab", true, T::Substring, 62, { { 4, 1 }, { 5, 1 } });      // word-start occurrence -4+36+30 beats the inner one (45)

        // UTF-8: highlights are byte ranges of whole code points; bytes of a character never match alone.
        ExpectFuzzy(c, "\xC3\xA9", "x\xC3\xA9", true, T::Substring, 15, { { 1, 2 } });
        ExpectFuzzy(c, "\xE6\x97\xA5", "a\xE6\x97\xA5", true, T::Substring, 15, { { 1, 3 } });
        ExpectFuzzy(c, "e", "\xC3\xA9", false, T::Exact, 0, {});
        ExpectFuzzy(c, "\xC3", "\xC3\xA9", false, T::Exact, 0, {});
        ExpectFuzzy(c, "a", "\xFF" "a", true, T::Substring, 15, { { 1, 1 } });
        ExpectFuzzy(c, "\xFF", "a\xFF", true, T::Substring, 15, { { 1, 1 } });
        ExpectFuzzy(c, "\xE6", "\xE6\x97", true, T::Prefix, 40, { { 0, 1 } });               // truncated sequence: each byte is its own unit
        ExpectFuzzy(c, "\xC3", "\xC3\x83", false, T::Exact, 0, {});                       // an invalid byte never equals the code point with the same value
        ExpectFuzzy(c, "\x80", "\xC2\x80", false, T::Exact, 0, {});
        ExpectFuzzy(c, "\xE0\x80\x80", "\xE0\x80\x80", true, T::Exact, 100, { { 0, 1 }, { 1, 1 }, { 2, 1 } }); // overlong U+0000
        ExpectFuzzy(c, "\xF0\x80\x80\x80", "\xF0\x80\x80\x80", true, T::Exact, 130, { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } }); // overlong 4-byte
        ExpectFuzzy(c, "\xF4\x90\x80\x80", "\xF4\x90\x80\x80", true, T::Exact, 130, { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } }); // past U+10FFFF
        ExpectFuzzy(c, "\xC0\x80", "\xC0\x80", true, T::Exact, 70, { { 0, 1 }, { 1, 1 } });  // overlong NUL encoding is two invalid bytes
        ExpectFuzzy(c, "\xED\xA0\x80", "\xED\xA0\x80", true, T::Exact, 100, { { 0, 1 }, { 1, 1 }, { 2, 1 } }); // UTF-16 surrogate

        // Empty and over-limit inputs.
        ExpectFuzzy(c, "", "anything", true, T::Prefix, 0, {});
        ExpectFuzzy(c, "", "", true, T::Prefix, 0, {});
        ExpectFuzzy(c, "a", "", false, T::Exact, 0, {});
        ExpectFuzzy(c, std::string(65, 'a'), std::string(100, 'a'), false, T::Exact, 0, {});
        c.Expect(FuzzyMatch(std::string(64, 'a'), std::string(64, 'a')).Tier == T::Exact, "a 64-unit query is within the limit");
        c.Expect(FuzzyMatch("a", std::string(1024, 'a')).Matched, "a 1024-unit text is within the limit");
        c.Expect(!FuzzyMatch("a", std::string(1025, 'a')).Matched, "a 1025-unit text never matches");

        // Ranking: tier, then score, then length, then input order.
        const std::vector<std::string_view> candidates = { "Camera", "Scale", "Place", "Frame Cache", "Copy Area" };
        const std::vector<FuzzyRanked> ranked = FuzzyRank("ca", candidates);
        c.Expect(ranked.size() == 4 && ranked[0].Index == 0 && ranked[1].Index == 3 && ranked[2].Index == 1 && ranked[3].Index == 4,
            "ranking by tier then score: Camera (prefix), Frame Cache (60), Scale (45), Copy Area (subsequence)");
        c.Expect(ranked.size() == 4 && ranked[1].Score == 60 && ranked[2].Score == 45 && ranked[3].Score == 69, "hand-computed scores");
        c.Expect(FuzzyRank("ca", candidates, 2).size() == 2 && FuzzyRank("ca", candidates, 0).empty(), "limit truncates the ranking");
        const std::vector<FuzzyRanked> everything = FuzzyRank("", candidates);
        c.Expect(everything.size() == 5 && everything[0].Index == 0 && everything[4].Index == 4, "empty query keeps input order");
        const std::vector<std::string_view> tied = { "Undo", "Redo", "Undo", "undo" };
        const std::vector<FuzzyRanked> tiedRanked = FuzzyRank("un", tied);
        c.Expect(tiedRanked.size() == 3 && tiedRanked[0].Index == 0 && tiedRanked[1].Index == 2 && tiedRanked[2].Index == 3,
            "equal keys keep input order and case does not change a lowercase query's score");
        return c.Ok;
    }

    bool TestFuzzyMatchAgreesWithBruteForceOracle()
    {
        return RunProperty("FuzzyMatchAgreesWithBruteForceOracle", [](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            const Word query = MakeWord(stream, 0, 4);
            const Word text = MakeWord(stream, 0, 11);
            const OracleFuzzy expected = OracleMatch(query, text);
            const FuzzyMatchResult actual = FuzzyMatch(query.Bytes, text.Bytes);
            if (actual.Matched != expected.Matched
                || (expected.Matched && (actual.Tier != expected.Tier || actual.Score != expected.Score || actual.Highlights != expected.Highlights))
                || (!expected.Matched && !actual.Highlights.empty()))
            {
                message = "query '" + query.Bytes + "' text '" + text.Bytes + "': oracle "
                    + DescribeFuzzy(expected.Matched, expected.Tier, expected.Score, expected.Highlights) + ", actual "
                    + DescribeFuzzy(actual.Matched, actual.Tier, actual.Score, actual.Highlights);
                return false;
            }
            return true;
        }, 6000);
    }

    bool TestFuzzyRankProperties()
    {
        return RunProperty("FuzzyRankProperties", [](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            const Word query = MakeWord(stream, 0, 3);
            std::vector<Word> words;
            const size_t count = stream.NextSize(0, 12);
            for (size_t i = 0; i < count; ++i)
            {
                if (!words.empty() && stream.NextSize(0, 3) == 0)
                    words.push_back(words[stream.NextSize(0, words.size() - 1)]); // duplicates exercise stability
                else
                    words.push_back(MakeWord(stream, 0, 8));
            }
            std::vector<std::string_view> candidates;
            for (const Word& w : words)
                candidates.push_back(w.Bytes);

            // Oracle ranking: stable sort of the matching candidates by (tier, -score, length).
            struct Row
            {
                size_t Index;
                OracleFuzzy Match;
            };
            std::vector<Row> rows;
            for (size_t i = 0; i < words.size(); ++i)
            {
                OracleFuzzy m = OracleMatch(query, words[i]);
                if (m.Matched) rows.push_back({ i, std::move(m) });
            }
            if (!query.Atoms.empty())
                std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b)
                {
                    if (a.Match.Tier != b.Match.Tier) return a.Match.Tier < b.Match.Tier;
                    if (a.Match.Score != b.Match.Score) return a.Match.Score > b.Match.Score;
                    return words[a.Index].Atoms.size() < words[b.Index].Atoms.size();
                });

            const size_t limit = stream.NextBool() ? std::numeric_limits<size_t>::max() : stream.NextSize(0, 6);
            const std::vector<FuzzyRanked> actual = FuzzyRank(query.Bytes, candidates, limit);
            const size_t expectedCount = std::min(limit, rows.size());
            if (actual.size() != expectedCount)
            {
                message = "ranked " + std::to_string(actual.size()) + " expected " + std::to_string(expectedCount);
                return false;
            }
            for (size_t i = 0; i < actual.size(); ++i)
                if (actual[i].Index != rows[i].Index || actual[i].Tier != rows[i].Match.Tier || actual[i].Score != rows[i].Match.Score
                    || actual[i].Highlights != rows[i].Match.Highlights)
                {
                    message = "rank " + std::to_string(i) + " is candidate " + std::to_string(actual[i].Index) + ", oracle says "
                        + std::to_string(rows[i].Index);
                    return false;
                }
            for (size_t i = 1; i < actual.size(); ++i)
                if (actual[i - 1].Tier > actual[i].Tier)
                {
                    message = "a lower tier outranked a higher one";
                    return false;
                }

            const std::vector<FuzzyRanked> again = FuzzyRank(query.Bytes, candidates, limit);
            if (again.size() != actual.size())
            {
                message = "ranking is not deterministic";
                return false;
            }
            for (size_t i = 0; i < actual.size(); ++i)
                if (again[i].Index != actual[i].Index || again[i].Score != actual[i].Score || again[i].Highlights != actual[i].Highlights)
                {
                    message = "ranking is not deterministic";
                    return false;
                }

            // Extending the query can only remove matches, and lowercasing it can only add them.
            const Word extension = MakeWord(stream, 1, 1);
            for (const Word& w : words)
            {
                if (FuzzyMatch(query.Bytes + extension.Bytes, w.Bytes).Matched && !FuzzyMatch(query.Bytes, w.Bytes).Matched)
                {
                    message = "a longer query matched where its prefix did not: '" + query.Bytes + extension.Bytes + "' in '" + w.Bytes + "'";
                    return false;
                }
                std::string lowered = query.Bytes;
                for (char& ch : lowered)
                    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
                if (FuzzyMatch(query.Bytes, w.Bytes).Matched && !FuzzyMatch(lowered, w.Bytes).Matched)
                {
                    message = "lowercasing the query lost a match";
                    return false;
                }
            }
            return true;
        }, 3000);
    }
}

namespace
{
    // ---- Log buffer model ---------------------------------------------------------------------------------

    struct ModelEntry
    {
        u64 Id;
        LogSeverity Severity;
        std::string Source;
        std::string Message;
        u64 Timestamp;
    };

    bool NaiveContains(const std::string& haystack, const std::string& needle)
    {
        if (needle.empty()) return true;
        const auto fold = [](std::string text)
        {
            for (char& ch : text)
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
            return text;
        };
        return fold(haystack).find(fold(needle)) != std::string::npos;
    }

    struct ModelLog
    {
        size_t Capacity;
        std::vector<ModelEntry> Entries;
        u64 NextId = 1;
        u64 Evicted = 0;

        explicit ModelLog(size_t capacity) : Capacity(capacity) {}

        u64 Append(LogSeverity severity, const std::string& source, const std::string& message, u64 timestamp)
        {
            Entries.push_back({ NextId, severity, source.substr(0, kMaximumLogSourceBytes), message.substr(0, kMaximumLogMessageBytes), timestamp });
            if (Entries.size() > Capacity)
            {
                Entries.erase(Entries.begin());
                ++Evicted;
            }
            return NextId++;
        }

        std::vector<ModelEntry> Filtered(const LogFilter& filter) const
        {
            std::vector<ModelEntry> result;
            for (const ModelEntry& entry : Entries)
            {
                if ((filter.SeverityMask & (1u << static_cast<unsigned>(entry.Severity))) == 0) continue;
                if (!NaiveContains(entry.Message, filter.Text) && !NaiveContains(entry.Source, filter.Text)) continue;
                result.push_back(entry);
            }
            return result;
        }
    };

    bool SnapshotMatchesModel(const LogBuffer& buffer, const ModelLog& model, const LogFilter& filter, std::string& problem)
    {
        const LogSnapshot snapshot = buffer.Snapshot(filter);
        const std::vector<ModelEntry> expected = model.Filtered(filter);
        if (snapshot.Entries.size() != expected.size())
        {
            problem = "snapshot holds " + std::to_string(snapshot.Entries.size()) + " entries, model " + std::to_string(expected.size());
            return false;
        }
        for (size_t i = 0; i < expected.size(); ++i)
        {
            const LogEntry& entry = *snapshot.Entries[i];
            if (entry.Id != expected[i].Id || entry.Severity != expected[i].Severity || entry.Source != expected[i].Source
                || entry.Message != expected[i].Message || entry.TimestampNanoseconds != expected[i].Timestamp)
            {
                problem = "entry " + std::to_string(i) + " differs (id " + std::to_string(entry.Id) + " vs " + std::to_string(expected[i].Id) + ")";
                return false;
            }
        }
        std::array<u64, kLogSeverityCount> retained {};
        for (const ModelEntry& entry : model.Entries)
            ++retained[static_cast<size_t>(entry.Severity)];
        if (snapshot.LastId != model.NextId - 1 || snapshot.Evicted != model.Evicted || snapshot.RetainedBySeverity != retained)
        {
            problem = "counters differ: last " + std::to_string(snapshot.LastId) + " evicted " + std::to_string(snapshot.Evicted);
            return false;
        }
        return true;
    }
}

namespace SpiralTests
{
    bool TestLogBufferRingBoundaries()
    {
        Checker c { "log ring boundaries" };

        LogBuffer buffer(5);
        c.Expect(buffer.Capacity() == 5 && buffer.Snapshot().Entries.empty() && buffer.Snapshot().LastId == 0, "empty buffer");
        const auto idsOf = [&](const LogFilter& filter = {})
        {
            std::vector<u64> ids;
            for (const auto& entry : buffer.Snapshot(filter).Entries)
                ids.push_back(entry->Id);
            return ids;
        };
        for (u64 n = 1; n <= 8; ++n)
        {
            const u64 id = buffer.Append({ LogSeverity::Info, "Test", "line " + std::to_string(n) });
            c.Expect(id == n, "ids count up from 1");
            std::vector<u64> expected;
            for (u64 k = n > 5 ? n - 4 : 1; k <= n; ++k)
                expected.push_back(k);
            c.Expect(idsOf() == expected, "retained ids after " + std::to_string(n) + " appends (capacity boundary B-1, B, B+1)");
            c.Expect(buffer.Snapshot().Evicted == (n > 5 ? n - 5 : 0), "eviction count after " + std::to_string(n));
        }

        const u64 revision = buffer.Revision();
        buffer.Snapshot();
        c.Expect(buffer.Revision() == revision, "reading does not change the revision");
        buffer.Clear();
        c.Expect(buffer.Revision() > revision && idsOf().empty() && buffer.Snapshot().Evicted == 3, "Clear empties without counting evictions");
        c.Expect(buffer.Append({ LogSeverity::Error, "Test", "after clear" }) == 9 && idsOf() == std::vector<u64> { 9 }, "ids survive Clear");
        const u64 afterAppend = buffer.Revision();
        buffer.Append({ LogSeverity::Warn, "Test", "x" });
        c.Expect(buffer.Revision() == afterAppend + 1, "each append bumps the revision once");
        buffer.Clear();
        buffer.Clear();
        c.Expect(idsOf().empty(), "Clear is idempotent");

        // Capacity extremes.
        LogBuffer one(1);
        one.Append({ LogSeverity::Info, "a", "first" });
        one.Append({ LogSeverity::Info, "a", "second" });
        const LogSnapshot oneSnapshot = one.Snapshot();
        c.Expect(oneSnapshot.Entries.size() == 1 && oneSnapshot.Entries[0]->Message == "second" && oneSnapshot.Evicted == 1, "capacity one keeps the newest");
        c.Expect(LogBuffer(0).Capacity() == 1, "capacity zero is clamped to one");
        c.Expect(LogBuffer(~size_t { 0 }).Capacity() == LogBuffer::kMaximumCapacity, "capacity is clamped to the maximum");

        // Filtering by severity mask and case-insensitive text over message and source.
        LogBuffer filtered(16);
        filtered.Append({ LogSeverity::Trace, "Renderer", "frame begin" });
        filtered.Append({ LogSeverity::Info, "Editor", "Opened scene Demo" });
        filtered.Append({ LogSeverity::Warn, "Assets", "Missing texture DEMO.png" });
        filtered.Append({ LogSeverity::Error, "Renderer", "Device lost" });
        const auto count = [&](u8 mask, const char* text)
        {
            LogFilter filter;
            filter.SeverityMask = mask;
            filter.Text = text;
            return filtered.Snapshot(filter).Entries.size();
        };
        c.Expect(count(kAllLogSeverities, "") == 4 && count(0, "") == 0, "all severities or none");
        c.Expect(count(LogSeverityBit(LogSeverity::Error), "") == 1 && count(LogSeverityBit(LogSeverity::Warn) | LogSeverityBit(LogSeverity::Error), "") == 2,
            "severity masks");
        c.Expect(count(kAllLogSeverities, "demo") == 2 && count(kAllLogSeverities, "DEMO") == 2, "text filter ignores case");
        c.Expect(count(kAllLogSeverities, "renderer") == 2, "text filter also searches the source");
        c.Expect(count(LogSeverityBit(LogSeverity::Warn), "demo") == 1 && count(LogSeverityBit(LogSeverity::Error), "demo") == 0, "mask and text combine with AND");
        c.Expect(count(kAllLogSeverities, "no such text") == 0, "no match");
        const LogSnapshot counts = filtered.Snapshot({ LogSeverityBit(LogSeverity::Error), "" });
        c.Expect(counts.RetainedBySeverity[0] == 1 && counts.RetainedBySeverity[1] == 1 && counts.RetainedBySeverity[2] == 1
            && counts.RetainedBySeverity[3] == 1, "retained counts ignore the filter");

        // Truncation and sanitising.
        LogBuffer limits(4);
        limits.Append({ LogSeverity::Info, std::string(64, 's'), std::string(4096, 'm'), 77 });
        limits.Append({ LogSeverity::Info, std::string(65, 's'), std::string(4097, 'm'), 78 });
        limits.Append({ static_cast<LogSeverity>(9), "bad", "unknown severity", 79 });
        const LogSnapshot limitSnapshot = limits.Snapshot();
        c.Expect(limitSnapshot.Entries.size() == 3, "three entries");
        c.Expect(!limitSnapshot.Entries[0]->Truncated && limitSnapshot.Entries[0]->Message.size() == 4096 && limitSnapshot.Entries[0]->TimestampNanoseconds == 77,
            "a message at the limit is intact");
        c.Expect(limitSnapshot.Entries[1]->Truncated && limitSnapshot.Entries[1]->Message.size() == 4096 && limitSnapshot.Entries[1]->Source.size() == 64,
            "an oversized record is cut and flagged");
        limits.Append({ LogSeverity::Info, std::string(65, 's'), "short" });
        c.Expect(limits.Snapshot().Entries.back()->Truncated && limits.Snapshot().Entries.back()->Source.size() == 64, "an over-long source alone sets Truncated");
        c.Expect(limitSnapshot.Entries[2]->Severity == LogSeverity::Error && limitSnapshot.RetainedBySeverity[3] == 1,
            "an out-of-range severity is recorded as Error");

        // A snapshot stays valid and unchanged after its entries are evicted.
        LogBuffer shortLived(3);
        for (int i = 0; i < 3; ++i)
            shortLived.Append({ LogSeverity::Info, "src", "original " + std::to_string(i) });
        const LogSnapshot held = shortLived.Snapshot();
        for (int i = 0; i < 20; ++i)
            shortLived.Append({ LogSeverity::Info, "src", "later " + std::to_string(i) });
        shortLived.Clear();
        c.Expect(held.Entries.size() == 3 && held.Entries[0]->Message == "original 0" && held.Entries[2]->Message == "original 2" && held.Entries[2]->Id == 3,
            "a held snapshot is immutable");
        return c.Ok;
    }

    bool TestLogBufferMatchesNaiveModel()
    {
        return RunProperty("LogBufferMatchesNaiveModel", [](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            const size_t capacity = stream.NextSize(1, 8);
            LogBuffer buffer(capacity);
            ModelLog model(capacity);
            const std::vector<std::string> sources = { "Editor", "Renderer", "assets", "" };
            const std::vector<std::string> messages = { "Opened Scene", "device LOST", "frame 12", "scene saved", "", "Missing texture" };
            const std::vector<std::string> needles = { "", "scene", "SCENE", "ren", "lost", "zzz", "12" };
            const size_t steps = stream.NextSize(5, 60);
            u64 revision = buffer.Revision();
            for (size_t step = 0; step < steps; ++step)
            {
                const size_t op = stream.NextSize(0, 9);
                if (op < 6)
                {
                    const auto severity = static_cast<LogSeverity>(stream.NextSize(0, 3));
                    const std::string& source = Pick(stream, sources);
                    std::string text = Pick(stream, messages);
                    if (stream.NextSize(0, 9) == 0) text.append(stream.NextSize(4090, 4100), 'x');
                    const u64 timestamp = stream.Next() & 0xFFFFFF;
                    const u64 id = buffer.Append({ severity, source, text, timestamp });
                    const u64 expectedId = model.Append(severity, source, text, timestamp);
                    if (id != expectedId || buffer.Revision() != revision + 1)
                    {
                        message = "append id or revision wrong";
                        return false;
                    }
                    ++revision;
                }
                else if (op == 6)
                {
                    buffer.Clear();
                    model.Entries.clear();
                    ++revision;
                    if (buffer.Revision() != revision)
                    {
                        message = "Clear revision wrong";
                        return false;
                    }
                }
                LogFilter filter;
                filter.SeverityMask = static_cast<u8>(stream.NextSize(0, 15));
                filter.Text = Pick(stream, needles);
                std::string problem;
                if (!SnapshotMatchesModel(buffer, model, filter, problem))
                {
                    message = problem + " (step " + std::to_string(step) + ")";
                    return false;
                }
            }
            return true;
        }, 800);
    }

    bool TestLogBufferConcurrentProducers()
    {
        Checker c { "log concurrency" };
        constexpr size_t kProducers = 4;
        constexpr size_t kPerProducer = 4000;
        constexpr size_t kCapacity = 257;
        LogBuffer buffer(kCapacity);
        std::atomic<bool> producersDone { false };
        std::atomic<size_t> readerFailures { 0 };
        std::atomic<size_t> snapshotsTaken { 0 };
        std::string readerProblem;
        std::mutex problemMutex;

        const auto reader = [&]
        {
            while (!producersDone.load(std::memory_order_acquire) || snapshotsTaken.load() < 20)
            {
                const LogSnapshot snapshot = buffer.Snapshot();
                snapshotsTaken.fetch_add(1);
                std::array<u64, kProducers> lastSequence {};
                std::array<bool, kProducers> seen {};
                u64 total = 0;
                for (const u64 n : snapshot.RetainedBySeverity)
                    total += n;
                std::string problem;
                if (snapshot.Entries.size() > kCapacity || total != snapshot.Entries.size())
                    problem = "retained counters disagree with the entries";
                for (size_t i = 0; problem.empty() && i < snapshot.Entries.size(); ++i)
                {
                    const LogEntry& entry = *snapshot.Entries[i];
                    if (i > 0 && entry.Id != snapshot.Entries[i - 1]->Id + 1)
                        problem = "ids are not consecutive";
                    else if (entry.Source.size() != 2 || entry.Source[0] != 'p')
                        problem = "corrupt source";
                    else
                    {
                        const size_t producer = static_cast<size_t>(entry.Source[1] - '0');
                        const u64 sequence = std::strtoull(entry.Message.c_str(), nullptr, 10);
                        if (producer >= kProducers || (seen[producer] && sequence <= lastSequence[producer]))
                            problem = "a producer's entries are out of order";
                        seen[producer] = true;
                        lastSequence[producer] = sequence;
                    }
                }
                if (problem.empty() && !snapshot.Entries.empty() && snapshot.LastId < snapshot.Entries.back()->Id)
                    problem = "LastId is older than the newest entry";
                if (!problem.empty())
                {
                    readerFailures.fetch_add(1);
                    std::lock_guard lock(problemMutex);
                    readerProblem = problem;
                    return;
                }
            }
        };

        std::vector<std::vector<u64>> returned(kProducers);
        std::vector<std::thread> threads;
        for (size_t p = 0; p < kProducers; ++p)
            threads.emplace_back([&, p]
            {
                const std::string source = "p" + std::to_string(p);
                for (size_t n = 0; n < kPerProducer; ++n)
                    returned[p].push_back(buffer.Append({ LogSeverity::Info, source, std::to_string(n + 1), n }));
            });
        std::thread readerA(reader);
        std::thread readerB(reader);
        for (std::thread& thread : threads)
            thread.join();
        producersDone.store(true, std::memory_order_release);
        readerA.join();
        readerB.join();

        c.Expect(readerFailures.load() == 0, "a reader saw a torn snapshot: " + readerProblem);
        std::set<u64> allIds;
        for (size_t p = 0; p < kProducers; ++p)
        {
            c.Expect(std::is_sorted(returned[p].begin(), returned[p].end()) && std::adjacent_find(returned[p].begin(), returned[p].end()) == returned[p].end(),
                "a producer's ids increase");
            allIds.insert(returned[p].begin(), returned[p].end());
        }
        const u64 total = kProducers * kPerProducer;
        c.Expect(allIds.size() == total && *allIds.begin() == 1 && *allIds.rbegin() == total, "ids are unique and exactly 1..N");
        const LogSnapshot final = buffer.Snapshot();
        c.Expect(final.LastId == total && final.Entries.size() == kCapacity && final.Evicted == total - kCapacity, "final counters");
        c.Expect(final.Entries.back()->Id == total && final.Entries.front()->Id == total - kCapacity + 1, "the newest capacity-many entries remain");

        // Clear racing with appends and reads must stay consistent (no tearing, no crash).
        LogBuffer racing(64);
        std::atomic<bool> stop { false };
        std::atomic<size_t> racingFailures { 0 };
        std::thread clearer([&]
        {
            while (!stop.load(std::memory_order_acquire))
            {
                racing.Clear();
                std::this_thread::yield();
            }
        });
        std::thread snapshotter([&]
        {
            while (!stop.load(std::memory_order_acquire))
            {
                const LogSnapshot s = racing.Snapshot();
                for (size_t i = 1; i < s.Entries.size(); ++i)
                    if (s.Entries[i]->Id != s.Entries[i - 1]->Id + 1) racingFailures.fetch_add(1);
            }
        });
        std::vector<std::thread> writers;
        for (size_t p = 0; p < 2; ++p)
            writers.emplace_back([&] { for (int n = 0; n < 5000; ++n) racing.Append({ LogSeverity::Warn, "w", "x" }); });
        for (std::thread& writer : writers)
            writer.join();
        stop.store(true, std::memory_order_release);
        clearer.join();
        snapshotter.join();
        c.Expect(racingFailures.load() == 0 && racing.Snapshot().LastId == 10000, "Clear racing with appends keeps ids consecutive and complete");
        return c.Ok;
    }
}

namespace
{
    // ---- Notifications ------------------------------------------------------------------------------------

    struct FakeClock
    {
        std::shared_ptr<u64> Now = std::make_shared<u64>(0);

        NotificationClock Get() const
        {
            const std::shared_ptr<u64> now = Now;
            return [now] { return *now; };
        }
    };

    NotificationSpec Spec(NotificationSeverity severity, std::string message, std::optional<u32> duration = std::nullopt,
        std::string label = {}, std::string command = {})
    {
        NotificationSpec spec;
        spec.Severity = severity;
        spec.Message = std::move(message);
        spec.DurationMs = duration;
        spec.ActionLabel = std::move(label);
        spec.ActionCommandId = std::move(command);
        return spec;
    }

    std::vector<u64> ActiveIds(Notifications& notifications)
    {
        std::vector<u64> ids;
        for (const Notification& n : notifications.Active())
            ids.push_back(n.Id);
        return ids;
    }

    struct ModelToast
    {
        u64 Id;
        NotificationSeverity Severity;
        std::string Message;
        std::string Label;
        std::string Command;
        u32 Count = 1;
        bool Sticky = false;
        bool Held = false;
        u64 Deadline = 0;
        u64 Remaining = 0;
    };

    struct ModelToasts
    {
        std::vector<ModelToast> Items;
        u64 NextId = 1;
        u64 Now = 0;
        size_t Maximum;

        explicit ModelToasts(size_t maximum) : Maximum(maximum) {}

        void Tick(u64 clock)
        {
            Now = std::max(Now, clock);
            for (size_t i = 0; i < Items.size();)
            {
                const ModelToast& t = Items[i];
                if (!t.Sticky && !t.Held && Now >= t.Deadline) Items.erase(Items.begin() + static_cast<std::ptrdiff_t>(i));
                else ++i;
            }
        }

        std::pair<u64, bool> Post(const NotificationSpec& spec)
        {
            u32 duration = spec.DurationMs ? *spec.DurationMs
                : spec.Severity == NotificationSeverity::Info ? 4000u : spec.Severity == NotificationSeverity::Warning ? 8000u : 0u;
            duration = std::min<u32>(duration, 600000);
            for (ModelToast& t : Items)
                if (t.Severity == spec.Severity && t.Message == spec.Message && t.Label == spec.ActionLabel && t.Command == spec.ActionCommandId)
                {
                    t.Count = std::min<u32>(t.Count + 1, 999);
                    t.Sticky = duration == 0;
                    t.Deadline = Now + duration;
                    t.Remaining = duration;
                    return { t.Id, true };
                }
            ModelToast t;
            t.Id = NextId++;
            t.Severity = spec.Severity;
            t.Message = spec.Message;
            t.Label = spec.ActionLabel;
            t.Command = spec.ActionCommandId;
            t.Sticky = duration == 0;
            t.Deadline = Now + duration;
            t.Remaining = duration;
            Items.push_back(t);
            if (Items.size() > Maximum) Items.erase(Items.begin());
            return { t.Id, false };
        }
    };
}

namespace SpiralTests
{
    bool TestTextTruncationNeverSplitsUtf8()
    {
        Checker c { "utf8 truncation" };
        static const char* const kAtoms[] = { "a", "\xC3\xA9", "\xE6\x97\xA5", "\xF0\x9F\x98\x80" };
        const bool property = RunProperty("TextTruncationNeverSplitsUtf8", [](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            std::string text;
            std::vector<size_t> boundaries { 0 };
            const size_t atoms = stream.NextSize(0, 40);
            for (size_t i = 0; i < atoms; ++i)
            {
                text += kAtoms[stream.NextSize(0, 3)];
                boundaries.push_back(text.size());
            }
            const size_t limit = stream.NextSize(0, text.size() + 4);
            size_t expected = 0;
            for (const size_t boundary : boundaries)
                if (boundary <= limit)
                    expected = boundary;
            const std::string_view actual = TruncateUtf8(text, limit);
            if (actual != std::string_view(text).substr(0, expected))
            {
                message = "limit " + std::to_string(limit) + " of " + std::to_string(text.size()) + " bytes gave " + std::to_string(actual.size())
                    + " expected " + std::to_string(expected);
                return false;
            }
            return true;
        }, 800);
        c.Expect(property, "TruncateUtf8 returns the longest whole-sequence prefix within the limit");

        c.Expect(TruncateUtf8("abc", 3) == "abc" && TruncateUtf8("abc", 99) == "abc" && TruncateUtf8("abc", 0).empty(), "ASCII and the limits themselves");
        c.Expect(TruncateUtf8(std::string(5, '\x80'), 3).empty(), "stray continuation bytes are dropped rather than split");

        // The same rule reaches the two bounded stores.
        std::string threeByte;
        for (int i = 0; i < 1400; ++i)
            threeByte += "\xE6\x97\xA5";
        LogBuffer log(4);
        log.Append({ LogSeverity::Info, std::string_view(threeByte).substr(0, 66), threeByte, 1 });
        const LogSnapshot snapshot = log.Snapshot();
        c.Expect(snapshot.Entries.size() == 1 && snapshot.Entries[0]->Message.size() == 4095 && snapshot.Entries[0]->Source.size() == 63
            && snapshot.Entries[0]->Truncated, "log fields are cut at the last whole code point");
        Notifications notifications;
        const PostResult posted = notifications.Post(NotificationSpec { NotificationSeverity::Info, threeByte, std::nullopt, {}, {} });
        c.Expect(posted.Id != 0 && notifications.Active()[0].Message.size() == 510, "a toast message is cut at the last whole code point");
        c.Expect(notifications.Post(NotificationSpec { NotificationSeverity::Info, std::string(600, '\x80'), std::nullopt, {}, {} }).Id == 0,
            "a message that truncates to nothing is rejected");
        return c.Ok;
    }

    bool TestNotificationsExpiryUsesInjectedClock()
    {
        Checker c { "notifications" };
        FakeClock clock;
        const auto at = [&](u64 now) { *clock.Now = now; };

        {
            Notifications n(clock.Get());
            at(1000);
            const PostResult info = n.Post(Spec(NotificationSeverity::Info, "Saved"));
            c.Expect(info.Id == 1 && !info.Deduplicated, "first id is 1");
            at(4999);
            c.Expect(ActiveIds(n) == std::vector<u64> { 1 }, "an Info toast is visible 1 ms before its 4000 ms deadline");
            at(5000);
            c.Expect(ActiveIds(n).empty(), "and gone exactly at the deadline");
            at(1000000);
            c.Expect(n.Dismiss(1) == false, "an expired toast cannot be dismissed");
        }
        {
            Notifications n(clock.Get());
            at(0);
            const u64 warning = n.Post(Spec(NotificationSeverity::Warning, "Careful")).Id;
            const u64 error = n.Post(Spec(NotificationSeverity::Error, "Failed")).Id;
            const u64 explicitSticky = n.Post(Spec(NotificationSeverity::Info, "Pinned", 0u)).Id;
            const u64 brief = n.Post(Spec(NotificationSeverity::Info, "Brief", 1u)).Id;
            const u64 clamped = n.Post(Spec(NotificationSeverity::Info, "Long", 4000000000u)).Id;
            at(1);
            c.Expect(ActiveIds(n) == std::vector<u64> { warning, error, explicitSticky, clamped }, "a 1 ms toast is gone after 1 ms");
            at(7999);
            c.Expect(ActiveIds(n).size() == 4, "warning visible before 8000 ms");
            at(8000);
            c.Expect(ActiveIds(n) == std::vector<u64> { error, explicitSticky, clamped }, "warning gone at 8000 ms; errors and zero-duration toasts stay");
            at(599999);
            c.Expect(ActiveIds(n).size() == 3, "the maximum duration is 600000 ms: still visible at 599999");
            at(600000);
            c.Expect(ActiveIds(n) == std::vector<u64> { error, explicitSticky }, "an over-long duration is clamped to ten minutes");
            at(1u << 30);
            c.Expect(ActiveIds(n).size() == 2 && n.Dismiss(error) && n.Dismiss(explicitSticky) && ActiveIds(n).empty(), "sticky toasts persist until dismissed");
            (void)brief;
        }
        {
            Notifications n(clock.Get());
            at(0);
            const PostResult first = n.Post(Spec(NotificationSeverity::Info, "Moved Cube", std::nullopt, "Undo: Move Cube", "edit.undo"));
            at(3000);
            const PostResult again = n.Post(Spec(NotificationSeverity::Info, "Moved Cube", std::nullopt, "Undo: Move Cube", "edit.undo"));
            c.Expect(again.Deduplicated && again.Id == first.Id, "an identical active toast is folded");
            const std::vector<Notification> active = n.Active();
            c.Expect(active.size() == 1 && active[0].Count == 2 && active[0].ExpiresAtMs == 7000, "folding bumps the count and restarts the timer");
            at(6999);
            c.Expect(ActiveIds(n).size() == 1, "the restarted timer governs");
            const PostResult other = n.Post(Spec(NotificationSeverity::Info, "Moved Cube", std::nullopt, "Redo", "edit.redo"));
            const PostResult otherSeverity = n.Post(Spec(NotificationSeverity::Warning, "Moved Cube", std::nullopt, "Undo: Move Cube", "edit.undo"));
            c.Expect(!other.Deduplicated && !otherSeverity.Deduplicated && n.Active().size() == 3, "a different action or severity is a different toast");
            at(7000);
            c.Expect(ActiveIds(n) == std::vector<u64> { other.Id, otherSeverity.Id }, "the folded toast expired at its restarted deadline");
            const PostResult reposted = n.Post(Spec(NotificationSeverity::Info, "Moved Cube", std::nullopt, "Undo: Move Cube", "edit.undo"));
            c.Expect(!reposted.Deduplicated && reposted.Id > otherSeverity.Id, "after expiry the same toast is new again");
            for (int i = 0; i < 2000; ++i)
                n.Post(Spec(NotificationSeverity::Info, "Moved Cube", std::nullopt, "Undo: Move Cube", "edit.undo"));
            u32 maxCount = 0;
            for (const Notification& toast : n.Active())
                maxCount = std::max(maxCount, toast.Count);
            c.Expect(maxCount == Notifications::kMaximumCount, "the fold count saturates");
        }
        {
            Notifications n(clock.Get());
            at(0);
            const u64 id = n.Post(Spec(NotificationSeverity::Info, "Hover")).Id;
            at(1000);
            c.Expect(n.Hold(id, true) && !n.Hold(999, true), "hold known and unknown ids");
            at(100000);
            c.Expect(ActiveIds(n) == std::vector<u64> { id }, "a held toast does not expire");
            c.Expect(n.Hold(id, true), "holding twice is harmless");
            c.Expect(n.Hold(id, false), "release");
            at(102999);
            c.Expect(ActiveIds(n) == std::vector<u64> { id }, "after release exactly the remaining 3000 ms are left");
            at(103000);
            c.Expect(ActiveIds(n).empty(), "expires when the remainder is used");
            const u64 sticky = n.Post(Spec(NotificationSeverity::Error, "Stuck")).Id;
            c.Expect(n.Hold(sticky, true) && n.Hold(sticky, false), "holding a sticky toast is harmless");
            at(1u << 30);
            c.Expect(ActiveIds(n) == std::vector<u64> { sticky }, "the sticky toast is still there");
        }
        {
            Notifications n(clock.Get(), 3);
            at(0);
            std::vector<u64> ids;
            for (int i = 0; i < 4; ++i)
                ids.push_back(n.Post(Spec(NotificationSeverity::Error, "e" + std::to_string(i))).Id);
            c.Expect(ActiveIds(n) == std::vector<u64> { ids[1], ids[2], ids[3] }, "the oldest toast is dropped past the limit");
            c.Expect(ids == std::vector<u64> { 1, 2, 3, 4 }, "ids are never reused");
            c.Expect(Notifications(clock.Get(), 0).Post(Spec(NotificationSeverity::Info, "x")).Id == 1, "a zero limit is clamped to one");
        }
        {
            Notifications n(clock.Get());
            at(0);
            const u64 withAction = n.Post(Spec(NotificationSeverity::Info, "Deleted", 4000u, "Undo: Delete", "edit.undo")).Id;
            const u64 plain = n.Post(Spec(NotificationSeverity::Info, "Plain")).Id;
            c.Expect(!n.TakeAction(plain).has_value() && ActiveIds(n).size() == 2, "a toast without an action yields nothing and stays");
            const auto taken = n.TakeAction(withAction);
            c.Expect(taken && *taken == "edit.undo" && ActiveIds(n) == std::vector<u64> { plain }, "TakeAction returns the command and dismisses");
            c.Expect(!n.TakeAction(withAction).has_value() && !n.TakeAction(12345).has_value(), "the action runs at most once");
        }
        {
            Notifications n(clock.Get());
            at(5000);
            const u64 id = n.Post(Spec(NotificationSeverity::Info, "Clock")).Id;
            at(1000);
            c.Expect(ActiveIds(n) == std::vector<u64> { id }, "a clock that jumps back does not resurrect or kill toasts");
            const u64 second = n.Post(Spec(NotificationSeverity::Info, "Clock 2")).Id;
            c.Expect(n.Active().back().ExpiresAtMs == 9000, "posts use the clamped, non-decreasing time");
            at(8999);
            c.Expect(ActiveIds(n) == std::vector<u64> { id, second }, "still visible at 8999");
            at(9000);
            c.Expect(ActiveIds(n).empty(), "both gone at 9000");
        }
        {
            Notifications n(clock.Get());
            at(0);
            const std::vector<NotificationSpec> invalid = {
                Spec(NotificationSeverity::Info, ""), Spec(NotificationSeverity::Info, "x", std::nullopt, "label", ""),
                Spec(NotificationSeverity::Info, "x", std::nullopt, "", "edit.undo"),
                Spec(NotificationSeverity::Info, "x", std::nullopt, std::string(65, 'l'), "edit.undo"),
                Spec(NotificationSeverity::Info, "x", std::nullopt, "label", std::string(65, 'c')),
                Spec(static_cast<NotificationSeverity>(7), "x") };
            for (const NotificationSpec& spec : invalid)
                c.Expect(n.Post(spec).Id == 0, "rejects an invalid spec");
            c.Expect(n.Active().empty(), "rejected posts leave no toast");
            c.Expect(n.Post(Spec(NotificationSeverity::Info, std::string(600, 'm'), std::nullopt, std::string(64, 'l'), std::string(64, 'c'))).Id == 1,
                "rejected posts do not consume ids; a 64-byte action is accepted");
            c.Expect(n.Active()[0].Message.size() == Notifications::kMaximumMessageBytes, "long messages are cut");
            n.Clear();
            c.Expect(n.Active().empty(), "Clear empties");
        }
        {
            Notifications n;
            n.Post(Spec(NotificationSeverity::Error, "default clock"));
            c.Expect(n.Active().size() == 1, "a null clock selects the steady clock");
            c.Expect(SteadyMillisecondClock() <= SteadyMillisecondClock() + 1, "steady clock is usable");
        }
        return c.Ok;
    }

    bool TestNotificationsMatchModel()
    {
        return RunProperty("NotificationsMatchModel", [](Spiral::Tests::ChoiceStream& stream, std::string& message)
        {
            FakeClock clock;
            const size_t maximum = stream.NextSize(1, 4);
            Notifications notifications(clock.Get(), maximum);
            ModelToasts model(maximum);
            const std::vector<std::string> messages = { "A", "B", "C" };
            const std::vector<std::optional<u32>> durations = { std::nullopt, 0u, 1u, 50u, 4000u };
            const std::vector<NotificationSeverity> severities = { NotificationSeverity::Info, NotificationSeverity::Warning, NotificationSeverity::Error };
            u64 maxId = 0;

            const size_t steps = stream.NextSize(10, 80);
            for (size_t step = 0; step < steps; ++step)
            {
                switch (stream.NextSize(0, 6))
                {
                case 0:
                case 1:
                {
                    const NotificationSpec spec = Spec(Pick(stream, severities), Pick(stream, messages), Pick(stream, durations),
                        stream.NextBool() ? "Undo" : "", "");
                    NotificationSpec fixedSpec = spec;
                    if (!fixedSpec.ActionLabel.empty()) fixedSpec.ActionCommandId = "edit.undo";
                    model.Tick(*clock.Now);
                    const auto expected = model.Post(fixedSpec);
                    const PostResult actual = notifications.Post(fixedSpec);
                    if (actual.Id != expected.first || actual.Deduplicated != expected.second)
                    {
                        message = "post id/dedup differ";
                        return false;
                    }
                    maxId = std::max(maxId, actual.Id);
                    break;
                }
                case 2:
                    *clock.Now += stream.NextSize(0, 3000);
                    break;
                case 3:
                    if (stream.NextSize(0, 4) == 0 && *clock.Now > 500) *clock.Now -= stream.NextSize(1, 500);
                    break;
                case 4:
                {
                    const u64 id = stream.NextSize(1, static_cast<size_t>(maxId + 2));
                    const bool hold = stream.NextBool();
                    model.Tick(*clock.Now);
                    bool expected = false;
                    for (ModelToast& t : model.Items)
                        if (t.Id == id)
                        {
                            expected = true;
                            if (t.Held != hold && !t.Sticky)
                            {
                                if (hold) t.Remaining = t.Deadline - model.Now;
                                else t.Deadline = model.Now + t.Remaining;
                            }
                            t.Held = hold;
                        }
                    if (notifications.Hold(id, hold) != expected)
                    {
                        message = "Hold result differs";
                        return false;
                    }
                    break;
                }
                case 5:
                {
                    const u64 id = stream.NextSize(1, static_cast<size_t>(maxId + 2));
                    model.Tick(*clock.Now);
                    const bool expected = std::erase_if(model.Items, [&](const ModelToast& t) { return t.Id == id; }) != 0;
                    if (notifications.Dismiss(id) != expected)
                    {
                        message = "Dismiss result differs";
                        return false;
                    }
                    break;
                }
                default:
                {
                    const u64 id = stream.NextSize(1, static_cast<size_t>(maxId + 2));
                    model.Tick(*clock.Now);
                    std::optional<std::string> expected;
                    for (size_t i = 0; i < model.Items.size(); ++i)
                        if (model.Items[i].Id == id && !model.Items[i].Command.empty())
                        {
                            expected = model.Items[i].Command;
                            model.Items.erase(model.Items.begin() + static_cast<std::ptrdiff_t>(i));
                            break;
                        }
                    if (notifications.TakeAction(id) != expected)
                    {
                        message = "TakeAction result differs";
                        return false;
                    }
                    break;
                }
                }

                model.Tick(*clock.Now);
                const std::vector<Notification> actual = notifications.Active();
                if (actual.size() != model.Items.size())
                {
                    message = "active count " + std::to_string(actual.size()) + " expected " + std::to_string(model.Items.size());
                    return false;
                }
                for (size_t i = 0; i < actual.size(); ++i)
                {
                    const ModelToast& t = model.Items[i];
                    const Notification& a = actual[i];
                    if (a.Id != t.Id || a.Count != t.Count || a.Sticky != t.Sticky || a.Held != t.Held || a.Message != t.Message
                        || a.ActionCommandId != t.Command || (!t.Sticky && !t.Held && a.ExpiresAtMs != t.Deadline)
                        || (!t.Sticky && !t.Held && a.ExpiresAtMs <= model.Now))
                    {
                        message = "toast " + std::to_string(i) + " differs from the model";
                        return false;
                    }
                    if (i > 0 && actual[i - 1].Id >= a.Id)
                    {
                        message = "toasts are not oldest first";
                        return false;
                    }
                }
            }
            return true;
        }, 800);
    }
}
