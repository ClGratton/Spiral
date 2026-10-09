#include "InputEventTests.h"

#include "TestSupport/GeneratedTest.h"

#include "Engine/Core/Window.h"
#include "Engine/Events/ApplicationEvent.h"
#include "Engine/Events/Event.h"
#include "Engine/Events/KeyEvent.h"
#include "Engine/Events/MouseEvent.h"
#include "Engine/Platform/Headless/HeadlessWindow.h"
#include "Engine/Platform/InputTranslation.h"

#include <cmath>
#include <iterator>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SpiralTests
{
    namespace
    {
        using namespace Engine;

        // Everything observable about one delivered event, copied inside the callback.
        struct Recorded
        {
            EventType Type = EventType::None;
            u32 Categories = 0;
            int Key = 0;
            int Scancode = 0;
            InputModifiers Mods = 0;
            bool Repeat = false;
            int Button = 0;
            u32 CodePoint = 0;
            bool Entered = false;
            float ScaleX = 0.0f;
            float ScaleY = 0.0f;
            std::string Text;
        };

        struct Recorder
        {
            std::vector<Recorded> Events;

            Window::EventCallbackFn Callback()
            {
                return [this](Event& event)
                {
                    Recorded record;
                    record.Type = event.GetEventType();
                    record.Categories = event.GetCategoryFlags();
                    record.Text = event.ToString();
                    switch (record.Type)
                    {
                    case EventType::KeyPressed:
                    {
                        const auto& key = static_cast<const KeyPressedEvent&>(event);
                        record.Key = key.GetKeyCode();
                        record.Scancode = key.GetScancode();
                        record.Mods = key.GetModifiers();
                        record.Repeat = key.IsRepeat();
                        break;
                    }
                    case EventType::KeyReleased:
                    {
                        const auto& key = static_cast<const KeyReleasedEvent&>(event);
                        record.Key = key.GetKeyCode();
                        record.Scancode = key.GetScancode();
                        record.Mods = key.GetModifiers();
                        break;
                    }
                    case EventType::MouseButtonPressed:
                    {
                        const auto& button = static_cast<const MouseButtonPressedEvent&>(event);
                        record.Button = button.GetMouseButton();
                        record.Mods = button.GetModifiers();
                        break;
                    }
                    case EventType::MouseButtonReleased:
                    {
                        const auto& button = static_cast<const MouseButtonReleasedEvent&>(event);
                        record.Button = button.GetMouseButton();
                        record.Mods = button.GetModifiers();
                        break;
                    }
                    case EventType::CharTyped:
                        record.CodePoint = static_cast<const CharTypedEvent&>(event).GetCodePoint();
                        break;
                    case EventType::CursorEnter:
                        record.Entered = static_cast<const CursorEnterEvent&>(event).Entered();
                        break;
                    case EventType::WindowContentScale:
                        record.ScaleX = static_cast<const WindowContentScaleEvent&>(event).GetXScale();
                        record.ScaleY = static_cast<const WindowContentScaleEvent&>(event).GetYScale();
                        break;
                    default:
                        break;
                    }
                    Events.push_back(std::move(record));
                };
            }
        };

        struct Checker
        {
            const char* Suite = "";
            bool Passed = true;

            void operator()(bool condition, std::string_view message)
            {
                if (condition)
                    return;
                std::cerr << Suite << " failed: " << message << '\n';
                Passed = false;
            }
        };

        // Independent oracle: the numeric values come from the GLFW documentation
        // (GLFW_MOD_SHIFT 0x1 ... GLFW_MOD_NUM_LOCK 0x20), not from the code under test.
        struct ModRow
        {
            int GlfwBit;
            InputModifiers EngineFlag;
        };

        constexpr ModRow kModTable[] = {
            { 0x01, InputModifierShift },
            { 0x02, InputModifierControl },
            { 0x04, InputModifierAlt },
            { 0x08, InputModifierSuper },
            { 0x10, InputModifierCapsLock },
            { 0x20, InputModifierNumLock },
        };

        InputModifiers OracleMods(int glfwMods)
        {
            InputModifiers expected = 0;
            for (const ModRow& row : kModTable)
            {
                if ((static_cast<unsigned>(glfwMods) & static_cast<unsigned>(row.GlfwBit)) != 0)
                    expected |= row.EngineFlag;
            }
            return expected;
        }
    }

    bool TestInputModifierTranslation()
    {
        Checker check { "Input modifier translation" };

        InputModifiers seen = 0;
        for (const ModRow& row : kModTable)
        {
            check((seen & row.EngineFlag) == 0, "engine modifier flags are pairwise disjoint");
            check(row.EngineFlag != 0 && (row.EngineFlag & (row.EngineFlag - 1)) == 0,
                "each engine modifier flag is a single bit");
            seen |= row.EngineFlag;
            check(TranslateGlfwModifiers(row.GlfwBit) == row.EngineFlag, "a single GLFW bit maps to its one flag");
        }
        check(TranslateGlfwModifiers(0) == InputModifierNone, "no modifiers translate to none");

        // Exhaustive over every low-byte pattern, which includes all 64 valid
        // combinations plus every unknown-bit pattern that must be dropped.
        for (int raw = 0; raw < 256; ++raw)
        {
            if (TranslateGlfwModifiers(raw) != OracleMods(raw))
            {
                check(false, "low-byte GLFW modifier pattern translates to the table oracle");
                break;
            }
        }

        check(TranslateGlfwModifiers(-1) == seen, "all bits set translates to exactly the six known flags");
        check(TranslateGlfwModifiers(std::numeric_limits<int>::max()) == seen,
            "INT_MAX carries every known bit and no unknown flag");
        check(TranslateGlfwModifiers(std::numeric_limits<int>::min()) == InputModifierNone,
            "a lone sign bit is unknown and dropped");
        check(TranslateGlfwModifiers(0x40 | 0x80 | 0x100) == InputModifierNone, "unknown bits are dropped");
        check(TranslateGlfwModifiers(0x41) == InputModifierShift, "unknown bits do not disturb known ones");

        // Seeded generated cross-check with replayable trace.
        Spiral::Tests::ChoiceStream stream(0x1badc0de5eedull);
        for (int iteration = 0; iteration < 4096; ++iteration)
        {
            const int raw = static_cast<int>(static_cast<std::uint32_t>(stream.Next()));
            if (TranslateGlfwModifiers(raw) != OracleMods(raw))
            {
                std::cerr << "Input modifier translation failed: seed=0x1badc0de5eed iteration=" << iteration
                    << " raw=" << raw << '\n';
                check(false, "random 32-bit modifier word matches the oracle");
                break;
            }
        }

        return check.Passed;
    }

    bool TestInputEventTranslationAndDispatch()
    {
        Checker check { "Input event translation and dispatch" };

        // The engine-owned values must be the GLFW action values (the real header is
        // checked by static_assert in GLFWWindow.cpp).
        check(GlfwInputValues::ActionRelease == 0 && GlfwInputValues::ActionPress == 1
                && GlfwInputValues::ActionRepeat == 2,
            "action values match the GLFW documentation");

        const Window::EventCallbackFn emptyCallback;

        // ----- keys -----
        {
            Recorder recorder;
            const auto callback = recorder.Callback();
            const int allMods = 0x3F;
            check(DispatchGlfwKey(65, 38, 1, allMods, callback), "key press dispatches");
            check(DispatchGlfwKey(65, 38, 2, 0x01, callback), "key repeat dispatches");
            check(DispatchGlfwKey(65, 38, 0, 0x02 | 0x04, callback), "key release dispatches");
            check(DispatchGlfwKey(-1, 133, 1, 0, callback), "GLFW_KEY_UNKNOWN still dispatches with its scancode");
            check(recorder.Events.size() == 4, "exactly one event per accepted key call");
            if (recorder.Events.size() == 4)
            {
                const Recorded& press = recorder.Events[0];
                check(press.Type == EventType::KeyPressed && !press.Repeat && press.Key == 65 && press.Scancode == 38,
                    "press carries key, scancode and not-repeat");
                check(press.Mods == (InputModifierShift | InputModifierControl | InputModifierAlt
                        | InputModifierSuper | InputModifierCapsLock | InputModifierNumLock),
                    "press carries every modifier");
                const Recorded& repeat = recorder.Events[1];
                check(repeat.Type == EventType::KeyPressed && repeat.Repeat && repeat.Mods == InputModifierShift
                        && repeat.Scancode == 38,
                    "repeat is a KeyPressed with the repeat flag");
                const Recorded& release = recorder.Events[2];
                check(release.Type == EventType::KeyReleased && release.Key == 65 && release.Scancode == 38
                        && release.Mods == (InputModifierControl | InputModifierAlt),
                    "release carries key, scancode and modifiers");
                const Recorded& unknown = recorder.Events[3];
                check(unknown.Key == -1 && unknown.Scancode == 133, "unknown key keeps scancode");
                check((press.Categories & EventCategoryKeyboard) != 0 && (press.Categories & EventCategoryInput) != 0
                        && (press.Categories & EventCategoryMouse) == 0,
                    "key categories are keyboard|input");
            }

            Recorder rejected;
            const auto rejectedCallback = rejected.Callback();
            for (const int action : { -1, 3, 4, 255, std::numeric_limits<int>::max(), std::numeric_limits<int>::min() })
                check(!DispatchGlfwKey(65, 38, action, 0, rejectedCallback), "unknown key action is rejected");
            check(rejected.Events.empty(), "rejected key actions deliver nothing");
            check(!DispatchGlfwKey(65, 38, 1, 0, emptyCallback), "an empty callback rejects a key without calling");
        }

        // ----- mouse buttons -----
        {
            Recorder recorder;
            const auto callback = recorder.Callback();
            check(DispatchGlfwMouseButton(0, 1, 0x01 | 0x10, callback), "button press dispatches");
            check(DispatchGlfwMouseButton(2, 0, 0x08, callback), "button release dispatches");
            check(!DispatchGlfwMouseButton(0, 2, 0, callback), "GLFW never repeats buttons; repeat is rejected");
            check(!DispatchGlfwMouseButton(0, 9, 0, callback), "unknown button action is rejected");
            check(!DispatchGlfwMouseButton(0, 1, 0, emptyCallback), "an empty callback rejects a button");
            check(recorder.Events.size() == 2, "only the two valid button calls deliver");
            if (recorder.Events.size() == 2)
            {
                check(recorder.Events[0].Type == EventType::MouseButtonPressed && recorder.Events[0].Button == 0
                        && recorder.Events[0].Mods == (InputModifierShift | InputModifierCapsLock),
                    "press carries button and modifiers");
                check(recorder.Events[1].Type == EventType::MouseButtonReleased && recorder.Events[1].Button == 2
                        && recorder.Events[1].Mods == InputModifierSuper,
                    "release carries button and modifiers");
                check((recorder.Events[0].Categories & EventCategoryMouseButton) != 0
                        && (recorder.Events[0].Categories & EventCategoryMouse) != 0
                        && (recorder.Events[0].Categories & EventCategoryInput) != 0,
                    "button categories are mouse|input|mouse-button");
            }
        }

        // ----- characters -----
        {
            Recorder recorder;
            const auto callback = recorder.Callback();
            const u32 accepted[] = { 'A', 0x20, 0x7F, 0xE9, 0xD7FF, 0xE000, 0x20AC, 0xFFFF, 0x10000, 0x1F600, 0x10FFFF };
            for (const u32 codePoint : accepted)
                check(DispatchGlfwChar(codePoint, callback), "valid Unicode scalar dispatches");
            check(recorder.Events.size() == std::size(accepted), "one CharTyped per valid code point");
            for (size_t index = 0; index < recorder.Events.size() && index < std::size(accepted); ++index)
            {
                check(recorder.Events[index].Type == EventType::CharTyped
                        && recorder.Events[index].CodePoint == accepted[index],
                    "code point is delivered unchanged and in order");
                check((recorder.Events[index].Categories & EventCategoryKeyboard) != 0
                        && (recorder.Events[index].Categories & EventCategoryInput) != 0,
                    "char categories are keyboard|input");
            }

            const size_t before = recorder.Events.size();
            const u32 rejected[] = { 0, 0xD800, 0xDBFF, 0xDC00, 0xDFFF, 0x110000, 0x7FFFFFFF, 0xFFFFFFFF };
            for (const u32 codePoint : rejected)
                check(!DispatchGlfwChar(codePoint, callback), "surrogate, zero and out-of-range code points are rejected");
            check(recorder.Events.size() == before, "rejected code points deliver nothing");
            check(!DispatchGlfwChar('A', emptyCallback), "an empty callback rejects a character");
            check(IsValidInputCodePoint(0x10FFFF) && !IsValidInputCodePoint(0x110000)
                    && IsValidInputCodePoint(0xD7FF) && !IsValidInputCodePoint(0xD800)
                    && !IsValidInputCodePoint(0xDFFF) && IsValidInputCodePoint(0xE000) && !IsValidInputCodePoint(0),
                "scalar-value predicate matches the Unicode definition at every boundary");
        }

        // ----- cursor enter / leave -----
        {
            Recorder recorder;
            const auto callback = recorder.Callback();
            check(DispatchGlfwCursorEnter(1, callback), "enter dispatches");
            check(DispatchGlfwCursorEnter(0, callback), "leave dispatches");
            check(DispatchGlfwCursorEnter(-3, callback), "any nonzero value is an enter, like GLFW_TRUE");
            check(!DispatchGlfwCursorEnter(1, emptyCallback), "an empty callback rejects cursor enter");
            check(recorder.Events.size() == 3, "three accepted cursor calls");
            if (recorder.Events.size() == 3)
            {
                check(recorder.Events[0].Type == EventType::CursorEnter && recorder.Events[0].Entered
                        && !recorder.Events[1].Entered && recorder.Events[2].Entered,
                    "entered flag follows the raw value");
                check((recorder.Events[0].Categories & EventCategoryMouse) != 0
                        && (recorder.Events[0].Categories & EventCategoryInput) != 0
                        && (recorder.Events[0].Categories & EventCategoryMouseButton) == 0,
                    "cursor-enter categories are mouse|input");
            }
        }

        // ----- content scale -----
        {
            Recorder recorder;
            const auto callback = recorder.Callback();
            check(DispatchGlfwContentScale(1.0f, 1.0f, callback), "unit scale dispatches");
            check(DispatchGlfwContentScale(1.5f, 2.0f, callback), "anisotropic scale dispatches");
            const float nan = std::numeric_limits<float>::quiet_NaN();
            const float inf = std::numeric_limits<float>::infinity();
            for (const auto& [x, y] : { std::pair<float, float> { 0.0f, 1.0f }, { 1.0f, 0.0f }, { -1.0f, 1.0f },
                     { 1.0f, -0.5f }, { nan, 1.0f }, { 1.0f, nan }, { inf, 1.0f }, { 1.0f, -inf } })
            {
                check(!DispatchGlfwContentScale(x, y, callback), "zero, negative and non-finite scale is rejected");
                check(!IsValidContentScale(x, y), "scale predicate agrees with dispatch");
            }
            check(!DispatchGlfwContentScale(1.0f, 1.0f, emptyCallback), "an empty callback rejects a scale");
            check(recorder.Events.size() == 2, "only the two valid scales deliver");
            if (recorder.Events.size() == 2)
            {
                check(recorder.Events[1].Type == EventType::WindowContentScale && recorder.Events[1].ScaleX == 1.5f
                        && recorder.Events[1].ScaleY == 2.0f
                        && recorder.Events[1].Categories == EventCategoryApplication,
                    "scale event carries both axes and the application category");
            }
        }

        // ----- generated mixed sequence against an order-preserving model -----
        {
            Spiral::Tests::ChoiceStream stream(0x5eed1234abcdull);
            Recorder recorder;
            const auto callback = recorder.Callback();
            std::vector<Recorded> expected;
            for (int iteration = 0; iteration < 3000; ++iteration)
            {
                const u64 kind = stream.Next() % 4;
                if (kind == 0)
                {
                    const int key = static_cast<int>(stream.NextI64(-1, 348));
                    const int scancode = static_cast<int>(stream.NextI64(0, 1000));
                    const int action = static_cast<int>(stream.NextI64(-1, 3));
                    const int mods = static_cast<int>(static_cast<std::uint32_t>(stream.Next()));
                    DispatchGlfwKey(key, scancode, action, mods, callback);
                    if (action >= 0 && action <= 2)
                    {
                        Recorded record;
                        record.Type = action == 0 ? EventType::KeyReleased : EventType::KeyPressed;
                        record.Repeat = action == 2;
                        record.Key = key;
                        record.Scancode = scancode;
                        record.Mods = OracleMods(mods);
                        expected.push_back(record);
                    }
                }
                else if (kind == 1)
                {
                    const int button = static_cast<int>(stream.NextI64(0, 7));
                    const int action = static_cast<int>(stream.NextI64(-1, 3));
                    const int mods = static_cast<int>(static_cast<std::uint32_t>(stream.Next()));
                    DispatchGlfwMouseButton(button, action, mods, callback);
                    if (action == 0 || action == 1)
                    {
                        Recorded record;
                        record.Type = action == 1 ? EventType::MouseButtonPressed : EventType::MouseButtonReleased;
                        record.Button = button;
                        record.Mods = OracleMods(mods);
                        expected.push_back(record);
                    }
                }
                else if (kind == 2)
                {
                    const u32 codePoint = static_cast<u32>(stream.Next() % 0x120000u);
                    DispatchGlfwChar(codePoint, callback);
                    const bool surrogate = codePoint >= 0xD800u && codePoint <= 0xDFFFu;
                    if (codePoint != 0 && codePoint <= 0x10FFFFu && !surrogate)
                    {
                        Recorded record;
                        record.Type = EventType::CharTyped;
                        record.CodePoint = codePoint;
                        expected.push_back(record);
                    }
                }
                else
                {
                    const int entered = static_cast<int>(stream.NextI64(-1, 1));
                    DispatchGlfwCursorEnter(entered, callback);
                    Recorded record;
                    record.Type = EventType::CursorEnter;
                    record.Entered = entered != 0;
                    expected.push_back(record);
                }
            }

            bool sequenceMatches = recorder.Events.size() == expected.size();
            for (size_t index = 0; sequenceMatches && index < expected.size(); ++index)
            {
                const Recorded& got = recorder.Events[index];
                const Recorded& want = expected[index];
                sequenceMatches = got.Type == want.Type && got.Repeat == want.Repeat && got.Key == want.Key
                    && got.Scancode == want.Scancode && got.Mods == want.Mods && got.Button == want.Button
                    && got.CodePoint == want.CodePoint && got.Entered == want.Entered;
                if (!sequenceMatches)
                {
                    std::cerr << "Input event translation and dispatch failed: seed=0x5eed1234abcd event=" << index
                        << " (replay with ChoiceStream(seed); the sequence is fully determined by the seed)\n";
                }
            }
            check(sequenceMatches, "generated sequence delivers exactly the expected events in order");
        }

        // ----- Window interface -----
        {
            WindowSpecification specification;
            specification.Headless = true;
            const HeadlessWindow window(specification);
            const WindowContentScale scale = window.GetContentScale();
            check(scale.X == 1.0f && scale.Y == 1.0f, "headless window reports unit content scale");
            check(WindowContentScale {}.X == 1.0f && WindowContentScale {}.Y == 1.0f,
                "default content scale is 1.0 on both axes");
        }

        return check.Passed;
    }

    bool TestInputEventConsumerCompatibility()
    {
        Checker check { "Input event consumer compatibility" };

        // Existing enumerators keep their numeric values; new ones are appended.
        check(static_cast<int>(EventType::None) == 0 && static_cast<int>(EventType::WindowClose) == 1
                && static_cast<int>(EventType::WindowResize) == 2 && static_cast<int>(EventType::FileDrop) == 3
                && static_cast<int>(EventType::WindowFocus) == 4 && static_cast<int>(EventType::WindowLostFocus) == 5
                && static_cast<int>(EventType::AppTick) == 6 && static_cast<int>(EventType::AppUpdate) == 7
                && static_cast<int>(EventType::AppRender) == 8 && static_cast<int>(EventType::KeyPressed) == 9
                && static_cast<int>(EventType::KeyReleased) == 10 && static_cast<int>(EventType::MouseButtonPressed) == 11
                && static_cast<int>(EventType::MouseButtonReleased) == 12 && static_cast<int>(EventType::MouseMoved) == 13
                && static_cast<int>(EventType::MouseScrolled) == 14,
            "pre-existing event type values are unchanged");
        check(static_cast<int>(EventType::CharTyped) == 15 && static_cast<int>(EventType::CursorEnter) == 16
                && static_cast<int>(EventType::WindowContentScale) == 17,
            "new event types are appended after the existing ones");
        check(EventCategoryNone == 0 && EventCategoryApplication == 1 && EventCategoryInput == 2
                && EventCategoryKeyboard == 4 && EventCategoryMouse == 8 && EventCategoryMouseButton == 16,
            "event category bits are unchanged");

        // Legacy constructors keep working with unchanged semantics; new fields default to zero.
        {
            const KeyPressedEvent press(65, false);
            const KeyPressedEvent repeat(65, true);
            const KeyReleasedEvent release(65);
            check(press.GetKeyCode() == 65 && !press.IsRepeat() && press.GetScancode() == 0
                    && press.GetModifiers() == InputModifierNone && repeat.IsRepeat() && release.GetKeyCode() == 65
                    && release.GetScancode() == 0 && release.GetModifiers() == InputModifierNone,
                "legacy key constructors default scancode and modifiers to zero");
            check(press.GetCategoryFlags() == (EventCategoryKeyboard | EventCategoryInput)
                    && release.GetCategoryFlags() == (EventCategoryKeyboard | EventCategoryInput),
                "key categories are unchanged");
            check(press.ToString().rfind("KeyPressedEvent: 65 (repeat = 0", 0) == 0
                    && repeat.ToString().rfind("KeyPressedEvent: 65 (repeat = 1", 0) == 0
                    && release.ToString().rfind("KeyReleasedEvent: 65", 0) == 0,
                "legacy trace text prefixes are preserved");
            check(press.ToString().find("scancode = 0") != std::string::npos
                    && KeyPressedEvent(1, false, 38, InputModifierShift).ToString().find("scancode = 38, mods = 1")
                        != std::string::npos,
                "trace text exposes scancode and modifiers");

            const MouseButtonPressedEvent buttonPress(2);
            const MouseButtonReleasedEvent buttonRelease(2);
            check(buttonPress.GetMouseButton() == 2 && buttonPress.GetModifiers() == InputModifierNone
                    && buttonRelease.GetMouseButton() == 2 && buttonRelease.GetModifiers() == InputModifierNone
                    && buttonPress.GetCategoryFlags()
                        == (EventCategoryMouse | EventCategoryInput | EventCategoryMouseButton),
                "legacy mouse-button constructors default modifiers to none and keep categories");

            const MouseMovedEvent moved(3.0f, 4.0f);
            const MouseScrolledEvent scrolled(1.0f, -2.0f);
            check(moved.GetCategoryFlags() == (EventCategoryMouse | EventCategoryInput)
                    && scrolled.GetCategoryFlags() == (EventCategoryMouse | EventCategoryInput)
                    && moved.ToString() == "MouseMovedEvent: 3, 4",
                "untouched mouse events keep categories and trace text");
        }

        // Dispatch: type-checked, marks Handled, and a new event type never reaches an old handler.
        {
            CharTypedEvent charEvent(0x20AC);
            int keyHandlerCalls = 0;
            int charHandlerCalls = 0;
            EventDispatcher dispatcher(charEvent);
            check(!dispatcher.Dispatch<KeyPressedEvent>([&](KeyPressedEvent&) { ++keyHandlerCalls; return true; }),
                "a CharTyped event does not match a KeyPressed handler");
            check(!charEvent.Handled && keyHandlerCalls == 0, "a mismatched handler neither runs nor marks Handled");
            check(dispatcher.Dispatch<CharTypedEvent>([&](CharTypedEvent& typed)
                {
                    ++charHandlerCalls;
                    return typed.GetCodePoint() == 0x20AC;
                }),
                "a CharTyped handler matches");
            check(charHandlerCalls == 1 && charEvent.Handled, "a handler returning true marks the event Handled");

            CursorEnterEvent cursorEvent(true);
            EventDispatcher cursorDispatcher(cursorEvent);
            check(!cursorDispatcher.Dispatch<MouseMovedEvent>([](MouseMovedEvent&) { return true; }) && !cursorEvent.Handled,
                "a CursorEnter event does not match a MouseMoved handler");
            check(cursorDispatcher.Dispatch<CursorEnterEvent>([](CursorEnterEvent&) { return false; })
                    && !cursorEvent.Handled,
                "a handler returning false leaves the event unhandled");

            WindowContentScaleEvent scaleEvent(2.0f, 2.0f);
            EventDispatcher scaleDispatcher(scaleEvent);
            check(!scaleDispatcher.Dispatch<WindowResizeEvent>([](WindowResizeEvent&) { return true; })
                    && !scaleDispatcher.Dispatch<WindowFocusEvent>([](WindowFocusEvent&) { return true; })
                    && !scaleEvent.Handled,
                "a content-scale event does not reach resize or focus handlers");
        }

        // Consumers that branch on EventType (EditorLayer, Application) see exactly one type per event.
        {
            const KeyPressedEvent key(1, false, 2, 3);
            const CharTypedEvent typed('x');
            const CursorEnterEvent cursor(false);
            const WindowContentScaleEvent scale(1.0f, 1.0f);
            check(key.GetEventType() == EventType::KeyPressed && typed.GetEventType() == EventType::CharTyped
                    && cursor.GetEventType() == EventType::CursorEnter
                    && scale.GetEventType() == EventType::WindowContentScale,
                "each event reports its own type");
            check(typed.GetName() == "CharTyped" && cursor.GetName() == "CursorEnter"
                    && scale.GetName() == "WindowContentScale",
                "event names are stable");
            check(typed.ToString() == "CharTypedEvent: U+78" && cursor.ToString() == "CursorEnterEvent: left",
                "new events have deterministic trace text");
        }

        return check.Passed;
    }
}
