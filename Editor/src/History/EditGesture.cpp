#include "EditGesture.h"

namespace EditorHistory
{
    EditGestureResult EditGestureTracker::OnItem(const EditItemFrame& frame)
    {
        const bool released = frame.Deactivated || frame.DeactivatedAfterEdit;
        const bool changed = frame.Edited || frame.DeactivatedAfterEdit;

        if (!m_Open)
        {
            if (frame.Activated)
            {
                if (released)
                    return changed ? EditGestureResult { EditGestureAction::Discrete, frame.Key, true } : EditGestureResult {};
                m_Open = true;
                m_Edited = frame.Edited;
                m_Key = frame.Key;
                return { EditGestureAction::Begin, frame.Key, frame.Edited };
            }
            if (frame.Edited)
                return { EditGestureAction::Discrete, frame.Key, true };
            return {};
        }

        if (frame.Key != m_Key)
        {
            if (frame.Activated || frame.Edited)
                return { EditGestureAction::Rejected, frame.Key, false };
            return {};
        }

        m_Edited = m_Edited || changed;
        if (!released)
            return {};

        const EditGestureResult result { EditGestureAction::End, m_Key, m_Edited };
        m_Open = false;
        m_Edited = false;
        return result;
    }

    EditGestureResult EditGestureTracker::OnFrameEnd(bool anyItemActive)
    {
        if (!m_Open || anyItemActive)
            return {};
        const EditGestureResult result { EditGestureAction::End, m_Key, m_Edited };
        Reset();
        return result;
    }

    EditGestureResult EditGestureTracker::Cancel()
    {
        if (!m_Open)
            return {};
        const EditGestureResult result { EditGestureAction::Cancel, m_Key, m_Edited };
        Reset();
        return result;
    }

    void EditGestureTracker::Reset()
    {
        m_Open = false;
        m_Edited = false;
        m_Key = {};
    }
}
