#pragma once

#include "Engine/Core/Base.h"

namespace EditorHistory
{
    // Identity of one continuous edit. Item is the UI widget identity (an
    // ImGui item id or a gizmo handle id), Entity the stable entity id the edit
    // targets (0 for non-entity state), Property a caller-defined property id.
    // Two gestures are the same gesture only when all three match.
    struct EditGestureKey
    {
        Engine::u64 Item = 0;
        Engine::u64 Entity = 0;
        Engine::u32 Property = 0;

        friend bool operator==(const EditGestureKey&, const EditGestureKey&) = default;
    };

    // One widget's state for one UI frame, copied from the widget's
    // activated/edited/deactivated queries after it was submitted. ImGui reports
    // a composite widget (DragFloat3, ColorEdit3) as one item, so one key per
    // widget is enough.
    struct EditItemFrame
    {
        EditGestureKey Key;
        // The widget became active this frame.
        bool Activated = false;
        // The widget changed its value this frame (its function returned true).
        bool Edited = false;
        // The widget stopped being active this frame.
        bool Deactivated = false;
        // The widget stopped being active this frame and changed its value at
        // some point while it was active.
        bool DeactivatedAfterEdit = false;
    };

    enum class EditGestureAction
    {
        None,
        // Capture the Before state NOW, before the widget's value is applied to
        // live state, then open the history gesture for Key.
        Begin,
        // Close the history gesture for Key; Edited tells whether the value
        // changed at any point (the history still compares Before and After).
        End,
        // Restore the Before state and close the gesture without an entry.
        Cancel,
        // A discrete change with no activation (checkbox, combo): capture
        // Before, apply, capture After, record one entry.
        Discrete,
        // Another gesture is open: the edit must not be applied and no history
        // call may be made for Key. The open gesture is unchanged.
        Rejected
    };

    struct EditGestureResult
    {
        EditGestureAction Action = EditGestureAction::None;
        EditGestureKey Key;
        bool Edited = false;
    };

    // ImGui-free per-frame state machine that turns widget states into history
    // gesture calls, so a continuous drag or a text edit is one entry. It owns
    // no snapshots: the caller captures Before when told to Begin. Only one
    // gesture is open at a time; the widget that opened it is the only widget
    // that can end it.
    class EditGestureTracker
    {
    public:
        // Feed every widget that reported any of the flags, in submission order.
        // A frame with no flag set and no open gesture may be skipped.
        EditGestureResult OnItem(const EditItemFrame& frame);

        // End of the UI frame. A gesture whose widget is no longer submitted (a
        // closed panel, a changed selection, a deleted entity) and where no item
        // is active ends here, so a gesture is never left open across frames
        // that cannot finish it.
        EditGestureResult OnFrameEnd(bool anyItemActive);

        // Explicit cancel (Escape during a gizmo drag).
        EditGestureResult Cancel();

        // Drops the open gesture without an action; for a project switch that
        // re-bases the history, which already discarded the gesture.
        void Reset();

        bool Open() const { return m_Open; }
        const EditGestureKey& ActiveKey() const { return m_Key; }

    private:
        bool m_Open = false;
        bool m_Edited = false;
        EditGestureKey m_Key;
    };
}
