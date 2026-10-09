#include "ShortcutDispatch.h"

namespace EditorHistory
{
    ShortcutDecision ResolveShortcut(const ShortcutChord& chord, const ShortcutContext& context)
    {
        ShortcutAction action = ShortcutAction::None;
        if (chord.Ctrl && !chord.Alt)
        {
            if (chord.Key == ShortcutKey::Z)
                action = chord.Shift ? ShortcutAction::Redo : ShortcutAction::Undo;
            else if (chord.Key == ShortcutKey::Y && !chord.Shift)
                action = ShortcutAction::Redo;
        }
        if (action == ShortcutAction::None)
            return { ShortcutAction::None, ShortcutReason::NotAHistoryChord };

        ShortcutReason blocker = ShortcutReason::Allowed;
        if (!context.WindowFocused)
            blocker = ShortcutReason::WindowNotFocused;
        else if (context.BrowserOwnsKeyboard)
            blocker = ShortcutReason::BrowserOwnsKeyboard;
        else if (context.TextInputActive)
            blocker = ShortcutReason::TextInputActive;
        else if (context.ModalOpen)
            blocker = ShortcutReason::ModalOpen;
        else if (context.DragActive)
            blocker = ShortcutReason::DragActive;
        else if (chord.Repeat)
            blocker = ShortcutReason::AutoRepeat;

        if (blocker != ShortcutReason::Allowed)
            return { ShortcutAction::None, blocker };
        return { action, ShortcutReason::Allowed };
    }

    const char* DescribeShortcutBlock(ShortcutReason reason)
    {
        switch (reason)
        {
        case ShortcutReason::WindowNotFocused: return "No editor window has focus";
        case ShortcutReason::BrowserOwnsKeyboard: return "The web page owns the keyboard";
        case ShortcutReason::TextInputActive: return "A text field is being edited";
        case ShortcutReason::ModalOpen: return "Close the dialog first";
        case ShortcutReason::DragActive: return "Finish the current edit first";
        case ShortcutReason::AutoRepeat: return "Key repeat is ignored";
        case ShortcutReason::NotAHistoryChord:
        case ShortcutReason::Allowed:
            break;
        }
        return "";
    }
}
