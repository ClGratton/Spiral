#pragma once

#include "Engine/Core/Window.h"
#include "Engine/Events/Event.h"

namespace Engine
{
    // Values of the GLFW callback constants, duplicated so translation can be
    // unit-tested without GLFW. GLFWWindow.cpp static_asserts each against the
    // real header, so a mismatch fails the build rather than misreporting input.
    namespace GlfwInputValues
    {
        constexpr int ActionRelease = 0;
        constexpr int ActionPress = 1;
        constexpr int ActionRepeat = 2;

        constexpr int ModShift = 0x0001;
        constexpr int ModControl = 0x0002;
        constexpr int ModAlt = 0x0004;
        constexpr int ModSuper = 0x0008;
        constexpr int ModCapsLock = 0x0010;
        constexpr int ModNumLock = 0x0020;
    }

    // Unknown bits are dropped. Lock bits are only reported by GLFW when its
    // lock-key-mods input mode is enabled, which GLFWWindow does.
    InputModifiers TranslateGlfwModifiers(int glfwMods);

    // A Unicode scalar value: nonzero, at most U+10FFFF, not a UTF-16 surrogate.
    bool IsValidInputCodePoint(u32 codePoint);

    // The Dispatch* functions build the event on the stack and invoke the
    // callback exactly once. They return false, calling nothing, when the
    // callback is empty or the raw input is not representable (unknown action,
    // invalid code point, non-finite or non-positive scale).
    bool DispatchGlfwKey(int key, int scancode, int action, int mods, const Window::EventCallbackFn& callback);
    bool DispatchGlfwMouseButton(int button, int action, int mods, const Window::EventCallbackFn& callback);
    bool DispatchGlfwChar(u32 codePoint, const Window::EventCallbackFn& callback);
    bool DispatchGlfwCursorEnter(int entered, const Window::EventCallbackFn& callback);
    bool DispatchGlfwContentScale(float xScale, float yScale, const Window::EventCallbackFn& callback);

    bool IsValidContentScale(float xScale, float yScale);
}
