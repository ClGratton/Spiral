#include "BrowserInputRouter.h"

#include <algorithm>
#include <cmath>

namespace Fab
{
    namespace
    {
        using Engine::u32;
        using Engine::u64;

        constexpr int kActionRelease = 0;
        constexpr int kActionRepeat = 2;

        u32 ButtonBit(BrowserMouseButton button)
        {
            switch (button)
            {
            case BrowserMouseButton::Left: return BrowserModifier::LeftButton;
            case BrowserMouseButton::Middle: return BrowserModifier::MiddleButton;
            case BrowserMouseButton::Right: return BrowserModifier::RightButton;
            }
            return 0;
        }

        bool IsTypedCodepoint(char32_t codepoint)
        {
            return codepoint >= 0x20 && codepoint <= 0x10FFFF && !(codepoint >= 0x7F && codepoint <= 0x9F)
                && !(codepoint >= 0xD800 && codepoint <= 0xDFFF);
        }
    }

    int TranslateGlfwKeyToWindowsVirtualKey(int key)
    {
        if (key >= 'A' && key <= 'Z')
            return key;
        if (key >= '0' && key <= '9')
            return key;
        if (key >= 290 && key <= 313) // F1..F24
            return 0x70 + (key - 290);
        if (key >= 320 && key <= 329) // KP_0..KP_9
            return 0x60 + (key - 320);
        switch (key)
        {
        case 32: return 0x20;  // space
        case 39: return 0xDE;  // apostrophe, VK_OEM_7
        case 44: return 0xBC;  // comma, VK_OEM_COMMA
        case 45: return 0xBD;  // minus, VK_OEM_MINUS
        case 46: return 0xBE;  // period, VK_OEM_PERIOD
        case 47: return 0xBF;  // slash, VK_OEM_2
        case 59: return 0xBA;  // semicolon, VK_OEM_1
        case 61: return 0xBB;  // equal, VK_OEM_PLUS
        case 91: return 0xDB;  // left bracket, VK_OEM_4
        case 92: return 0xDC;  // backslash, VK_OEM_5
        case 93: return 0xDD;  // right bracket, VK_OEM_6
        case 96: return 0xC0;  // grave accent, VK_OEM_3
        case 161: return 0xE2; // world 1 (ISO extra key), VK_OEM_102
        case 256: return 0x1B; // escape
        case 257: return 0x0D; // enter
        case 258: return 0x09; // tab
        case 259: return 0x08; // backspace
        case 260: return 0x2D; // insert
        case 261: return 0x2E; // delete
        case 262: return 0x27; // right
        case 263: return 0x25; // left
        case 264: return 0x28; // down
        case 265: return 0x26; // up
        case 266: return 0x21; // page up, VK_PRIOR
        case 267: return 0x22; // page down, VK_NEXT
        case 268: return 0x24; // home
        case 269: return 0x23; // end
        case 280: return 0x14; // caps lock
        case 281: return 0x91; // scroll lock
        case 282: return 0x90; // num lock
        case 283: return 0x2C; // print screen, VK_SNAPSHOT
        case 284: return 0x13; // pause
        case 330: return 0x6E; // KP decimal
        case 331: return 0x6F; // KP divide
        case 332: return 0x6A; // KP multiply
        case 333: return 0x6D; // KP subtract
        case 334: return 0x6B; // KP add
        case 335: return 0x0D; // KP enter
        case 340: case 344: return 0x10; // shift
        case 341: case 345: return 0x11; // control
        case 342: case 346: return 0x12; // alt, VK_MENU
        case 343: return 0x5B; // left super, VK_LWIN
        case 347: return 0x5C; // right super, VK_RWIN
        case 348: return 0x5D; // menu, VK_APPS
        default: return 0;
        }
    }

    bool TranslateGlfwMouseButton(int glfwButton, BrowserMouseButton& out)
    {
        switch (glfwButton)
        {
        case 0: out = BrowserMouseButton::Left; return true;
        case 1: out = BrowserMouseButton::Right; return true;
        case 2: out = BrowserMouseButton::Middle; return true;
        default: return false;
        }
    }

    bool BrowserInputRouter::CanRouteMouse() const
    {
        return m_Panel.PanelVisible && !m_Panel.DragDropPayloadActive && !m_Panel.ModalOrPopupOpen;
    }

    bool BrowserInputRouter::OverSurface(float x, float y) const
    {
        return CanRouteMouse() && m_Panel.SurfaceHovered && m_Panel.Surface.Contains(x, y);
    }

    BrowserMouse BrowserInputRouter::MakeMouse(float x, float y, u32 glfwMods) const
    {
        return { x - m_Panel.Surface.X, y - m_Panel.Surface.Y, (glfwMods & BrowserModifier::KeyMask) | m_ButtonsDown };
    }

    void BrowserInputRouter::Recompute(BrowserInputRoute& route)
    {
        const bool owns = m_ClickedSinceFocus && m_WindowFocused && m_Panel.PanelFocused && m_Panel.PanelVisible
            && !m_Panel.OtherTextInputActive;
        if (owns == m_OwnsKeyboard)
            return;
        m_OwnsKeyboard = owns;
        route.KeyboardOwnerChanged = true;
        if (!owns)
        {
            route.SyntheticKeyUps.insert(route.SyntheticKeyUps.end(), m_BrowserKeys.begin(), m_BrowserKeys.end());
            m_BrowserKeys.clear();
        }
    }

    void BrowserInputRouter::EndCapture(BrowserInputRoute& route)
    {
        route.SendCaptureLost = true;
        m_MouseCapture = false;
        if (m_ButtonsDown != 0)
            m_EditorGesture = true;
        if (m_PointerInBrowser)
        {
            route.SendMouseLeave = true;
            m_PointerInBrowser = false;
        }
    }

    BrowserInputRoute BrowserInputRouter::UpdatePanel(const BrowserPanelState& state)
    {
        BrowserInputRoute route;
        const bool lostFocus = m_Panel.PanelFocused && !state.PanelFocused;
        m_Panel = state;
        // Typing into another widget, hiding the panel, or leaving it all end the
        // click that granted the keyboard; a later click on the surface regrants it.
        if (lostFocus || !state.PanelVisible || state.OtherTextInputActive)
            m_ClickedSinceFocus = false;
        if (m_MouseCapture && !CanRouteMouse())
            EndCapture(route);
        if (m_PointerInBrowser && !m_MouseCapture && (!CanRouteMouse() || !state.SurfaceHovered))
        {
            route.SendMouseLeave = true;
            m_PointerInBrowser = false;
        }
        Recompute(route);
        return route;
    }

    BrowserInputRoute BrowserInputRouter::OnWindowFocus(bool focused)
    {
        BrowserInputRoute route;
        m_WindowFocused = focused;
        if (!focused)
        {
            if (m_MouseCapture)
                EndCapture(route);
            if (m_PointerInBrowser)
            {
                route.SendMouseLeave = true;
                m_PointerInBrowser = false;
            }
            m_ButtonsDown = 0;
            m_EditorGesture = false;
            m_ClickedSinceFocus = false;
        }
        Recompute(route);
        return route;
    }

    BrowserInputRoute BrowserInputRouter::OnCursorLeftWindow()
    {
        BrowserInputRoute route;
        if (m_PointerInBrowser && !m_MouseCapture)
        {
            route.SendMouseLeave = true;
            m_PointerInBrowser = false;
        }
        return route;
    }

    BrowserInputRoute BrowserInputRouter::ReleaseKeyboard()
    {
        BrowserInputRoute route;
        m_ClickedSinceFocus = false;
        Recompute(route);
        return route;
    }

    BrowserInputRoute BrowserInputRouter::OnMouseMove(float x, float y, u32 glfwMods)
    {
        BrowserInputRoute route;
        route.ImGui = true;
        if (m_MouseCapture || (!m_EditorGesture && OverSurface(x, y)))
        {
            route.Browser = true;
            route.Mouse = MakeMouse(x, y, glfwMods);
            m_PointerInBrowser = true;
            return route;
        }
        route.EditorShortcuts = true;
        if (m_PointerInBrowser)
        {
            route.SendMouseLeave = true;
            m_PointerInBrowser = false;
        }
        return route;
    }

    BrowserInputRoute BrowserInputRouter::OnMouseButton(
        BrowserMouseButton button, bool down, float x, float y, u32 glfwMods, u64 timeMs)
    {
        BrowserInputRoute route;
        route.ImGui = true;
        route.Button = button;
        route.ButtonDown = down;
        const u32 bit = ButtonBit(button);

        if (down)
        {
            const bool anyButtonBefore = m_ButtonsDown != 0;
            m_ButtonsDown |= bit;
            const bool beginsCapture = !m_MouseCapture && !m_EditorGesture && !anyButtonBefore && OverSurface(x, y);
            if (m_MouseCapture || beginsCapture)
            {
                if (beginsCapture)
                {
                    m_MouseCapture = true;
                    m_ClickedSinceFocus = true;
                }
                m_PointerInBrowser = true;
                route.Browser = true;
                route.Mouse = MakeMouse(x, y, glfwMods);
                const bool continuesClick = m_LastClickCount > 0 && m_LastClickButton == button
                    && timeMs >= m_LastClickTime && timeMs - m_LastClickTime <= kDoubleClickMilliseconds
                    && std::fabs(x - m_LastClickX) <= kDoubleClickDistance
                    && std::fabs(y - m_LastClickY) <= kDoubleClickDistance;
                m_LastClickCount = continuesClick ? std::min(m_LastClickCount + 1, 3) : 1;
                m_LastClickButton = button;
                m_LastClickTime = timeMs;
                m_LastClickX = x;
                m_LastClickY = y;
                route.ClickCount = m_LastClickCount;
            }
            else
            {
                m_EditorGesture = true;
                m_ClickedSinceFocus = false;
                route.EditorShortcuts = true;
            }
            Recompute(route);
            return route;
        }

        if ((m_ButtonsDown & bit) == 0)
        {
            route.EditorShortcuts = true;
            return route;
        }
        m_ButtonsDown &= ~bit;
        if (m_MouseCapture)
        {
            route.Browser = true;
            route.Mouse = MakeMouse(x, y, glfwMods);
            if (m_ButtonsDown == 0)
            {
                m_MouseCapture = false;
                if (m_PointerInBrowser && !OverSurface(x, y))
                {
                    route.SendMouseLeave = true;
                    m_PointerInBrowser = false;
                }
            }
            return route;
        }
        route.EditorShortcuts = true;
        if (m_ButtonsDown == 0)
            m_EditorGesture = false;
        return route;
    }

    BrowserInputRoute BrowserInputRouter::OnScroll(float notchesX, float notchesY, float x, float y, u32 glfwMods)
    {
        BrowserInputRoute route;
        route.ImGui = true;
        if (m_MouseCapture || (!m_EditorGesture && OverSurface(x, y)))
        {
            route.Browser = true;
            route.Mouse = MakeMouse(x, y, glfwMods);
            route.WheelDeltaX = notchesX * kPixelsPerWheelNotch;
            route.WheelDeltaY = notchesY * kPixelsPerWheelNotch;
            return route;
        }
        route.EditorShortcuts = true;
        return route;
    }

    BrowserInputRoute BrowserInputRouter::OnKey(int glfwKey, int scancode, int action, u32 glfwMods)
    {
        BrowserInputRoute route;
        if (action < kActionRelease || action > kActionRepeat)
            return route;

        const auto held = std::find(m_BrowserKeys.begin(), m_BrowserKeys.end(), glfwKey);
        if (action == kActionRelease && held != m_BrowserKeys.end())
        {
            m_BrowserKeys.erase(held);
            route.Browser = true;
            route.Key = { BrowserKey::Phase::Up, glfwKey, TranslateGlfwKeyToWindowsVirtualKey(glfwKey), scancode,
                glfwMods & BrowserModifier::KeyMask, 0, false };
            return route;
        }
        if (action != kActionRelease && m_OwnsKeyboard)
        {
            if (held == m_BrowserKeys.end())
                m_BrowserKeys.push_back(glfwKey);
            route.Browser = true;
            route.Key = { BrowserKey::Phase::Down, glfwKey, TranslateGlfwKeyToWindowsVirtualKey(glfwKey), scancode,
                glfwMods & BrowserModifier::KeyMask, 0, action == kActionRepeat };
            return route;
        }
        route.ImGui = true;
        route.EditorShortcuts = true;
        return route;
    }

    BrowserInputRoute BrowserInputRouter::OnChar(char32_t codepoint, u32 glfwMods)
    {
        BrowserInputRoute route;
        if (!IsTypedCodepoint(codepoint))
            return route;
        if (m_OwnsKeyboard)
        {
            route.Browser = true;
            route.Key = { BrowserKey::Phase::Char, 0, 0, 0, glfwMods & BrowserModifier::KeyMask, codepoint, false };
            return route;
        }
        route.ImGui = true;
        route.EditorShortcuts = true;
        return route;
    }
}
