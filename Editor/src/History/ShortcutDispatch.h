#pragma once

namespace EditorHistory
{
    // Keys the history shortcuts care about; the host maps its own key codes
    // (ImGuiKey, GLFW) to these and passes Other for everything else.
    enum class ShortcutKey
    {
        None,
        Z,
        Y,
        Other
    };

    // Ctrl means the platform shortcut modifier (Cmd on macOS).
    struct ShortcutChord
    {
        ShortcutKey Key = ShortcutKey::None;
        bool Ctrl = false;
        bool Shift = false;
        bool Alt = false;
        // The key event is an OS auto-repeat, not the initial press.
        bool Repeat = false;
    };

    // Latched once per UI frame so the GLFW event phase and the ImGui phase
    // decide from the same facts.
    struct ShortcutContext
    {
        // Some editor window (main or detached) has OS focus.
        bool WindowFocused = true;
        // The Fab web page owns the keyboard (BrowserInputRouter::OwnsKeyboard).
        bool BrowserOwnsKeyboard = false;
        // A text widget is active or wants typed characters.
        bool TextInputActive = false;
        // A modal or popup is open.
        bool ModalOpen = false;
        // A drag, slider, or gizmo gesture is in progress.
        bool DragActive = false;
    };

    enum class ShortcutAction
    {
        None,
        Undo,
        Redo
    };

    // Why the action is what it is. When several blockers hold, the first in
    // this order is reported.
    enum class ShortcutReason
    {
        // The chord is not a history shortcut.
        NotAHistoryChord,
        WindowNotFocused,
        BrowserOwnsKeyboard,
        TextInputActive,
        ModalOpen,
        DragActive,
        AutoRepeat,
        // The shortcut fires.
        Allowed
    };

    struct ShortcutDecision
    {
        ShortcutAction Action = ShortcutAction::None;
        ShortcutReason Reason = ShortcutReason::NotAHistoryChord;
    };

    // Ctrl+Z is undo; Ctrl+Y and Ctrl+Shift+Z are redo; any other modifier
    // combination or key is not a history chord. A history chord acts only when
    // an editor window is focused, the web page does not own the keyboard, no
    // text field is being typed in, no modal is open, no drag is in progress,
    // and the event is not an auto-repeat (every restore copies and republishes
    // the project state).
    ShortcutDecision ResolveShortcut(const ShortcutChord& chord, const ShortcutContext& context);

    // A sentence for the status text when a recognised chord is blocked, or an
    // empty string for Allowed and NotAHistoryChord.
    const char* DescribeShortcutBlock(ShortcutReason reason);
}
