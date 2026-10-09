#include "Engine/Platform/InputTranslation.h"

#include "Engine/Events/ApplicationEvent.h"
#include "Engine/Events/KeyEvent.h"
#include "Engine/Events/MouseEvent.h"

#include <cmath>

namespace Engine
{
    InputModifiers TranslateGlfwModifiers(int glfwMods)
    {
        using namespace GlfwInputValues;

        InputModifiers modifiers = InputModifierNone;
        if ((glfwMods & ModShift) != 0)
            modifiers |= InputModifierShift;
        if ((glfwMods & ModControl) != 0)
            modifiers |= InputModifierControl;
        if ((glfwMods & ModAlt) != 0)
            modifiers |= InputModifierAlt;
        if ((glfwMods & ModSuper) != 0)
            modifiers |= InputModifierSuper;
        if ((glfwMods & ModCapsLock) != 0)
            modifiers |= InputModifierCapsLock;
        if ((glfwMods & ModNumLock) != 0)
            modifiers |= InputModifierNumLock;
        return modifiers;
    }

    bool IsValidInputCodePoint(u32 codePoint)
    {
        return codePoint != 0 && codePoint <= 0x10FFFFu && (codePoint < 0xD800u || codePoint > 0xDFFFu);
    }

    bool IsValidContentScale(float xScale, float yScale)
    {
        return std::isfinite(xScale) && std::isfinite(yScale) && xScale > 0.0f && yScale > 0.0f;
    }

    bool DispatchGlfwKey(int key, int scancode, int action, int mods, const Window::EventCallbackFn& callback)
    {
        if (!callback)
            return false;

        const InputModifiers modifiers = TranslateGlfwModifiers(mods);
        if (action == GlfwInputValues::ActionPress)
        {
            KeyPressedEvent event(key, false, scancode, modifiers);
            callback(event);
        }
        else if (action == GlfwInputValues::ActionRelease)
        {
            KeyReleasedEvent event(key, scancode, modifiers);
            callback(event);
        }
        else if (action == GlfwInputValues::ActionRepeat)
        {
            KeyPressedEvent event(key, true, scancode, modifiers);
            callback(event);
        }
        else
        {
            return false;
        }
        return true;
    }

    bool DispatchGlfwMouseButton(int button, int action, int mods, const Window::EventCallbackFn& callback)
    {
        if (!callback)
            return false;

        const InputModifiers modifiers = TranslateGlfwModifiers(mods);
        if (action == GlfwInputValues::ActionPress)
        {
            MouseButtonPressedEvent event(button, modifiers);
            callback(event);
        }
        else if (action == GlfwInputValues::ActionRelease)
        {
            MouseButtonReleasedEvent event(button, modifiers);
            callback(event);
        }
        else
        {
            return false;
        }
        return true;
    }

    bool DispatchGlfwChar(u32 codePoint, const Window::EventCallbackFn& callback)
    {
        if (!callback || !IsValidInputCodePoint(codePoint))
            return false;

        CharTypedEvent event(codePoint);
        callback(event);
        return true;
    }

    bool DispatchGlfwCursorEnter(int entered, const Window::EventCallbackFn& callback)
    {
        if (!callback)
            return false;

        CursorEnterEvent event(entered != 0);
        callback(event);
        return true;
    }

    bool DispatchGlfwContentScale(float xScale, float yScale, const Window::EventCallbackFn& callback)
    {
        if (!callback || !IsValidContentScale(xScale, yScale))
            return false;

        WindowContentScaleEvent event(xScale, yScale);
        callback(event);
        return true;
    }
}
