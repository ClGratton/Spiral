#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Events/Event.h"

#include <compare>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SpiralEditor
{
    // Lowercase dot-separated segments of [a-z0-9_-], at most 64 bytes in total.
    // Command ids and panel ids share this grammar so a persisted shortcut line
    // never needs quoting.
    bool IsValidEditorIdentifier(std::string_view text);

    // A key plus the four chord modifiers. Key is a GLFW_KEY_* value; the
    // values are a stable ABI and are tabulated in ShortcutMap.cpp so that no
    // GLFW header is needed. Caps Lock and Num Lock are not chord modifiers.
    struct KeyChord
    {
        int Key = 0;
        Engine::InputModifiers Modifiers = Engine::InputModifierNone;

        auto operator<=>(const KeyChord&) const = default;
    };

    constexpr Engine::InputModifiers kChordModifierMask = Engine::InputModifierShift | Engine::InputModifierControl
        | Engine::InputModifierAlt | Engine::InputModifierSuper;

    // True when Key is a nameable non-modifier key and Modifiers holds only chord modifiers.
    bool IsValidChord(const KeyChord& chord);

    // Strips the non-chord modifier bits (Caps Lock, Num Lock) from an event's modifiers.
    KeyChord MakeChord(int glfwKey, Engine::InputModifiers eventModifiers);

    // Every key a chord may use, ascending by key code.
    std::span<const int> ChordKeys();

    // Canonical name of a chord key ("A", "F5", "PageUp", "Numpad3"), or empty when the key has none.
    std::string_view ChordKeyName(int glfwKey);

    // Canonical text: modifiers in the order Ctrl, Shift, Alt, Super, then the key, joined by '+'
    // ("Ctrl+Shift+Z"). Empty for an invalid chord.
    std::string FormatChord(const KeyChord& chord);

    // Parses modifiers and one key separated by '+', in any order, case-insensitively, with optional
    // blanks around each token. Accepts the aliases Control, Meta, Win, Cmd, Esc, Return, Del, Ins,
    // PgUp, PgDn. Rejects empty tokens, repeated modifiers, zero or several keys, and unknown names.
    // `chord` is written only on success.
    bool ParseChord(std::string_view text, KeyChord& chord);

    enum class ShortcutScopeKind : Engine::u8
    {
        Global,
        Viewport,
        Panel
    };

    // Where a binding is live. Panel scopes name their panel; Global and Viewport carry no panel.
    struct ShortcutScope
    {
        ShortcutScopeKind Kind = ShortcutScopeKind::Global;
        std::string Panel;

        auto operator<=>(const ShortcutScope&) const = default;
    };

    bool IsValidScope(const ShortcutScope& scope);
    // "global", "viewport", "panel:<id>"; empty for an invalid scope.
    std::string FormatScope(const ShortcutScope& scope);
    bool ParseScope(std::string_view text, ShortcutScope& scope);

    struct ShortcutKey
    {
        KeyChord Chord;
        ShortcutScope Scope;

        auto operator<=>(const ShortcutKey&) const = default;
    };

    struct ShortcutBinding
    {
        std::string CommandId;
        ShortcutKey Key;

        auto operator<=>(const ShortcutBinding&) const = default;
    };

    enum class BindStatus : Engine::u8
    {
        Bound,
        InvalidCommand,
        InvalidChord,
        InvalidScope,
        Duplicate, // the same command already holds this chord in this scope
        Conflict,  // another command holds this chord in this scope
        NotBound,  // Unbind/Rebind of a binding that does not exist
        LimitReached
    };

    struct BindResult
    {
        BindStatus Status = BindStatus::Bound;
        std::string ConflictingCommand;

        bool Ok() const { return Status == BindStatus::Bound; }
    };

    enum class ChordOverlap : Engine::u8
    {
        Conflict,  // same chord and same scope: ambiguous, never allowed
        Shadows,   // a narrower scope hides an existing Global binding while it has focus
        ShadowedBy // an existing narrower binding hides a candidate Global binding while it has focus
    };

    struct ChordClash
    {
        ChordOverlap Kind = ChordOverlap::Conflict;
        ShortcutBinding Existing;
    };

    // One chord may hold at most one command per scope. A Viewport or Panel binding shadows a Global
    // binding of the same chord while that scope has focus; that is permitted and reported, not rejected.
    // Bindings are kept sorted by (command, chord, scope), so enumeration and equality are deterministic.
    // Main thread only.
    class ShortcutMap
    {
    public:
        static constexpr size_t kMaximumBindings = 4096;

        BindResult Bind(std::string_view commandId, const ShortcutKey& key);
        BindResult Unbind(std::string_view commandId, const ShortcutKey& key);
        // Removes every binding of the command; returns how many were removed.
        size_t UnbindCommand(std::string_view commandId);
        // Atomic replace: on any failure the map is unchanged. Rebinding a key to itself succeeds.
        BindResult Rebind(std::string_view commandId, const ShortcutKey& from, const ShortcutKey& to);

        // Existing bindings that overlap `candidate`, in map order. Includes the command's own duplicate.
        std::vector<ChordClash> Clashes(const ShortcutKey& candidate) const;

        // The binding that a key press resolves to when `focus` has input focus: the binding in the
        // focus scope if any, else the Global one. A Global focus resolves Global bindings only.
        std::optional<ShortcutBinding> Resolve(const KeyChord& chord, const ShortcutScope& focus) const;

        std::vector<ShortcutKey> KeysFor(std::string_view commandId) const;
        const std::vector<ShortcutBinding>& Bindings() const { return m_Bindings; }
        bool empty() const { return m_Bindings.empty(); }

        bool operator==(const ShortcutMap&) const = default;

    private:
        std::vector<ShortcutBinding> m_Bindings;
    };

    // Persistence. The file stores only the difference from a defaults map so that a command added with a
    // default chord in a later version still receives it:
    //   spiral-shortcuts 1
    //   unbind <command-id> <scope> <chord>
    //   bind <command-id> <scope> <chord>
    // Lines end with '\n' (a trailing '\r' is tolerated); unbind lines precede bind lines and each group
    // is sorted. Any other line, header, token, duplicate, or conflicting result rejects the whole file.
    inline constexpr size_t kMaximumShortcutFileBytes = 1024 * 1024;

    std::string EncodeShortcutOverrides(const ShortcutMap& defaults, const ShortcutMap& current);

    struct ShortcutDecodeResult
    {
        bool Ok = false;
        ShortcutMap Map;      // defaults with the file applied; empty when !Ok
        std::string Error;    // empty when Ok
        size_t ErrorLine = 0; // 1-based; 0 when Ok or when the error is not tied to a line
    };

    // Transactional: on failure nothing is applied and `defaults` is untouched.
    ShortcutDecodeResult DecodeShortcutOverrides(const ShortcutMap& defaults, std::string_view text);
}
