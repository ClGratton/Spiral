#pragma once

#include "BrowserSurface.h"

#include <vector>

namespace Fab
{
    // GLFW_KEY_* value to Windows virtual-key code (the value CEF's
    // windows_key_code expects), or 0 for keys without one. Modifier keys map
    // to the generic VK_SHIFT/VK_CONTROL/VK_MENU a WM_KEYDOWN carries; the
    // left/right distinction travels in the scancode. GLFW codes are
    // hard-coded here (they are a stable ABI) so this module needs no GLFW
    // include.
    int TranslateGlfwKeyToWindowsVirtualKey(int glfwKey);

    // GLFW_MOUSE_BUTTON_LEFT/RIGHT/MIDDLE (0/1/2) to a browser button.
    bool TranslateGlfwMouseButton(int glfwButton, BrowserMouseButton& out);

    struct BrowserSurfaceRect
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Width = 0.0f;
        float Height = 0.0f;

        bool Contains(float x, float y) const
        {
            return x >= X && y >= Y && x < X + Width && y < Y + Height;
        }
    };

    // Latched once per frame from the UI phase and read by the next frame's
    // events, the same convention as the Scene viewport navigation latch.
    struct BrowserPanelState
    {
        BrowserSurfaceRect Surface;
        bool PanelVisible = false;
        // ImGui: the browser window (root and children) has focus.
        bool PanelFocused = false;
        // ImGui: the pointer is over the surface item and not occluded.
        bool SurfaceHovered = false;
        // ImGui: a text widget elsewhere wants typed text.
        bool OtherTextInputActive = false;
        bool DragDropPayloadActive = false;
        bool ModalOrPopupOpen = false;
    };

    // What the host must do for one input event. ImGui is fed every raw event
    // by its chained GLFW callbacks regardless; the ImGui and EditorShortcuts
    // flags state who is allowed to act on it. Exactly one of Browser and
    // EditorShortcuts handles a key or character; mouse events that reach the
    // browser still let ImGui act (it must see the click to focus the panel)
    // but never reach Editor shortcuts and viewport navigation.
    struct BrowserInputRoute
    {
        bool Browser = false;
        bool ImGui = false;
        bool EditorShortcuts = false;

        bool SendMouseLeave = false;
        bool SendCaptureLost = false;
        // The value of OwnsKeyboard() changed; call IBrowserSurface::SetFocus.
        bool KeyboardOwnerChanged = false;
        // GLFW keys that were delivered down to the browser and must now be
        // released there because the browser lost the keyboard.
        std::vector<int> SyntheticKeyUps;

        // Valid when Browser is true, per event kind.
        BrowserKey Key;
        BrowserMouse Mouse;
        BrowserMouseButton Button = BrowserMouseButton::Left;
        bool ButtonDown = false;
        int ClickCount = 0;
        float WheelDeltaX = 0.0f;
        float WheelDeltaY = 0.0f;
    };

    // Pure state machine deciding which consumer receives each input event.
    // Rules:
    // - The browser owns the keyboard only after a click on its surface, while
    //   the OS window, the panel, and the panel's window are all focused and
    //   no other ImGui text widget is active. Any press outside the surface,
    //   window focus loss, the panel losing focus or visibility, or
    //   ReleaseKeyboard() revokes it. Escape is an ordinary forwarded key.
    // - Mouse events reach the browser while the pointer is over the surface
    //   with no drag-drop payload, modal, or popup active, and from a press
    //   inside the surface until every button is released (capture, so
    //   drag-selects continue past the edge). A press that begins elsewhere
    //   keeps the whole gesture with the Editor until all buttons are up.
    // - A key delivered down to the browser always gets its matching up, from
    //   the key release itself or from SyntheticKeyUps when ownership ends.
    class BrowserInputRouter
    {
    public:
        static constexpr float kPixelsPerWheelNotch = 40.0f;
        static constexpr Engine::u64 kDoubleClickMilliseconds = 500;
        static constexpr float kDoubleClickDistance = 4.0f;

        // Applies the new UI-phase state. The returned route only ever carries
        // effects (capture lost, mouse leave, ownership change, key ups).
        BrowserInputRoute UpdatePanel(const BrowserPanelState& state);

        BrowserInputRoute OnWindowFocus(bool focused);
        BrowserInputRoute OnCursorLeftWindow();
        // The explicit "release keyboard" affordance.
        BrowserInputRoute ReleaseKeyboard();

        BrowserInputRoute OnMouseMove(float x, float y, Engine::u32 glfwMods);
        BrowserInputRoute OnMouseButton(
            BrowserMouseButton button, bool down, float x, float y, Engine::u32 glfwMods, Engine::u64 timeMs);
        BrowserInputRoute OnScroll(float notchesX, float notchesY, float x, float y, Engine::u32 glfwMods);

        // action: 0 release, 1 press, 2 repeat (the GLFW values).
        BrowserInputRoute OnKey(int glfwKey, int scancode, int action, Engine::u32 glfwMods);
        BrowserInputRoute OnChar(char32_t codepoint, Engine::u32 glfwMods);

        bool OwnsKeyboard() const { return m_OwnsKeyboard; }
        bool HasMouseCapture() const { return m_MouseCapture; }
        bool BrowserHoldsKeys() const { return !m_BrowserKeys.empty(); }

    private:
        bool CanRouteMouse() const;
        bool OverSurface(float x, float y) const;
        BrowserMouse MakeMouse(float x, float y, Engine::u32 glfwMods) const;
        void Recompute(BrowserInputRoute& route);
        void EndCapture(BrowserInputRoute& route);

        BrowserPanelState m_Panel;
        bool m_WindowFocused = true;
        bool m_ClickedSinceFocus = false;
        bool m_OwnsKeyboard = false;

        bool m_MouseCapture = false;
        bool m_EditorGesture = false;
        bool m_PointerInBrowser = false;
        Engine::u32 m_ButtonsDown = 0;

        Engine::u64 m_LastClickTime = 0;
        float m_LastClickX = 0.0f;
        float m_LastClickY = 0.0f;
        BrowserMouseButton m_LastClickButton = BrowserMouseButton::Left;
        int m_LastClickCount = 0;

        std::vector<int> m_BrowserKeys;
    };
}
