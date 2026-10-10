// `--editor-history-smoke` and `--editor-history-benchmark`: both run the real Inspector
// code through a private headless ImGui context (no window, no GPU, nothing written to
// disk) and read the history the way a user would. The pointer and key events are
// injected into that private context only; the live Editor is never driven this way.
//
// Failure hypotheses the smoke targets, each with its own oracle below:
// - A widget block that mutates live state before the history captured Before would
//   make undo a no-op. Oracle: an independent snapshot taken before the gesture must
//   equal the project after Undo and the post-gesture snapshot must equal it after Redo.
// - A drag or a typed value that records one entry per frame. Oracle: the exact entry
//   count and the exact number of project snapshots captured (two per gesture).
// - Per-frame whole-project copies. Oracle: the capture counter over idle frames.
// - Undo moving the navigation camera, or failing to move it for a camera edit.
// - Shortcuts firing while typing or dragging, and Ctrl+Shift+Z undoing.
// - Special entities lost on rename, duplicate names, undo and redo.
#include "EditorLayer.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace
{
    using EditorHistory::EditProperty;

    struct Pointer
    {
        float X = -300.0f;
        float Y = -300.0f;
        bool Down = false;
    };

    struct WidgetRect
    {
        EditProperty Property = EditProperty::None;
        ImVec2 Min;
        ImVec2 Max;
    };

    class Failures
    {
    public:
        void Expect(bool condition, const std::string& message)
        {
            if (!condition && m_Messages.size() < 24)
                m_Messages.push_back(message);
            if (!condition)
                ++m_Count;
        }
        bool Empty() const { return m_Count == 0; }
        std::string Join() const
        {
            std::ostringstream stream;
            for (const std::string& message : m_Messages)
                stream << "\n  - " << message;
            if (m_Count > m_Messages.size())
                stream << "\n  (" << m_Count - m_Messages.size() << " more)";
            return stream.str();
        }

    private:
        std::vector<std::string> m_Messages;
        size_t m_Count = 0;
    };

    // The Inspector draws its widgets in a window of this fixed size on a display large
    // enough that nothing is clipped, so every widget is interactable.
    constexpr float kDisplayWidth = 1700.0f;
    constexpr float kDisplayHeight = 3400.0f;
}

bool EditorLayer::RunEditorHistorySmoke()
{
    if (m_EditorHistorySmokeCompleted)
        return false;
    if (!Engine::Application::Get().GetSpecification().Window.Headless)
        throw std::runtime_error("--editor-history-smoke requires --headless");

    Failures failures;
    const auto contextOwner = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigInputTrickleEventQueue = false;
    io.DisplaySize = ImVec2(kDisplayWidth, kDisplayHeight);
    unsigned char* pixels = nullptr;
    int fontWidth = 0;
    int fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fontWidth, &fontHeight);

    std::vector<WidgetRect> widgets;
    std::vector<std::pair<size_t, WidgetRect>> rows;
    m_WidgetProbe = [&widgets](EditProperty property, const ImVec2& minimum, const ImVec2& maximum)
    {
        widgets.push_back({ property, minimum, maximum });
    };
    m_HistoryRowProbe = [&rows](size_t index, const ImVec2& minimum, const ImVec2& maximum)
    {
        rows.push_back({ index, { EditProperty::None, minimum, maximum } });
    };
    bool contextDestroyed = false;
    const auto finish = [&](bool destroy)
    {
        m_WidgetProbe = nullptr;
        m_HistoryRowProbe = nullptr;
        if (destroy && !contextDestroyed)
            ImGui::DestroyContext(contextOwner);
        contextDestroyed |= destroy;
    };

    try
    {
        m_PanelVisible.fill(true);
        const auto frame = [&](const Pointer& pointer, const std::function<void(ImGuiIO&)>& inject = {},
                               bool pollShortcuts = false, bool drawHistoryPanel = false)
        {
            io.DeltaTime = 1.0f / 60.0f;
            io.AddMousePosEvent(pointer.X, pointer.Y);
            io.AddMouseButtonEvent(0, pointer.Down);
            if (inject)
                inject(io);
            ImGui::NewFrame();
            widgets.clear();
            rows.clear();
            if (pollShortcuts)
                PollHistoryShortcuts();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(760.0f, kDisplayHeight - 100.0f));
            DrawInspectorPanel();
            if (drawHistoryPanel)
            {
                ImGui::SetNextWindowPos(ImVec2(800.0f, 0.0f));
                ImGui::SetNextWindowSize(ImVec2(600.0f, 700.0f));
                DrawHistoryPanel();
            }
            EndOfFrameEditGestureFlush();
            ImGui::Render();
        };
        const auto settle = [&](int frames, bool drawHistoryPanel = false)
        {
            for (int index = 0; index < frames; ++index)
                frame(Pointer {}, {}, false, drawHistoryPanel);
        };
        const auto find = [&](EditProperty property) -> std::optional<WidgetRect>
        {
            for (const WidgetRect& widget : widgets)
            {
                if (widget.Property == property)
                    return widget;
            }
            return std::nullopt;
        };
        const auto aim = [](const WidgetRect& widget)
        {
            return Pointer { widget.Min.x + 14.0f, (widget.Min.y + widget.Max.y) * 0.5f, false };
        };
        const auto openMaterialHeader = [&]()
        {
            ImGuiWindow* window = ImGui::FindWindowByName("Inspector");
            if (!window)
                return;
            const ImGuiID component = ImHashStr("MeshRendererComponent", 0, window->ID);
            window->StateStorage.SetInt(ImHashStr("Material Properties", 0, component), 1);
        };

        // ---- gesture drivers: a drag, a typed value, a click ----
        // A second press on the same spot inside the double-click time turns a drag widget
        // into a text field, so every gesture starts after the interval has passed.
        const auto waitOutDoubleClick = [&]() { settle(24); };
        const auto dragWidget = [&](EditProperty property, float perFrame, int frames)
        {
            waitOutDoubleClick();
            const std::optional<WidgetRect> widget = find(property);
            failures.Expect(widget.has_value(), "widget probe missing for property " + std::to_string(static_cast<int>(property)));
            if (!widget)
                return;
            Pointer pointer = aim(*widget);
            frame(pointer);
            pointer.Down = true;
            frame(pointer);
            for (int index = 0; index < frames; ++index)
            {
                pointer.X += perFrame;
                frame(pointer);
            }
            pointer.Down = false;
            frame(pointer);
            settle(2);
        };
        const auto clickWidget = [&](EditProperty property)
        {
            waitOutDoubleClick();
            const std::optional<WidgetRect> widget = find(property);
            failures.Expect(widget.has_value(), "widget probe missing for property " + std::to_string(static_cast<int>(property)));
            if (!widget)
                return;
            Pointer pointer = aim(*widget);
            pointer.X = widget->Min.x + 8.0f;
            frame(pointer);
            pointer.Down = true;
            frame(pointer);
            pointer.Down = false;
            frame(pointer);
            settle(2);
        };
        const auto keyFrame = [&](ImGuiKey key, bool down, Pointer pointer)
        {
            frame(pointer, [key, down](ImGuiIO& input) { input.AddKeyEvent(key, down); });
        };
        // Opens a combo with a click and picks the next entry with the keyboard, which is how
        // the popup is driven without knowing where its rows are drawn.
        const auto chooseNext = [&](EditProperty property)
        {
            waitOutDoubleClick();
            const std::optional<WidgetRect> widget = find(property);
            failures.Expect(widget.has_value(), "combo probe missing for property " + std::to_string(static_cast<int>(property)));
            if (!widget)
                return;
            Pointer pointer = aim(*widget);
            frame(pointer);
            pointer.Down = true;
            frame(pointer);
            pointer.Down = false;
            frame(pointer);
            frame(pointer);
            keyFrame(ImGuiKey_DownArrow, true, pointer);
            keyFrame(ImGuiKey_DownArrow, false, pointer);
            frame(pointer);
            keyFrame(ImGuiKey_Enter, true, pointer);
            keyFrame(ImGuiKey_Enter, false, pointer);
            settle(3);
        };
        const auto typeInto = [&](EditProperty property, const std::string& text)
        {
            waitOutDoubleClick();
            const std::optional<WidgetRect> widget = find(property);
            failures.Expect(widget.has_value(), "widget probe missing for property " + std::to_string(static_cast<int>(property)));
            if (!widget)
                return;
            Pointer pointer = aim(*widget);
            frame(pointer);
            pointer.Down = true;
            frame(pointer);
            pointer.Down = false;
            frame(pointer);
            frame(pointer);
            for (const char character : text)
                frame(pointer, [character](ImGuiIO& input) { input.AddInputCharacter(static_cast<unsigned int>(character)); });
            keyFrame(ImGuiKey_Enter, true, pointer);
            keyFrame(ImGuiKey_Enter, false, pointer);
            settle(3);
        };

        struct Report
        {
            size_t Widgets = 0;
            size_t Gestures = 0;
        } report;

        // One widget exercised end to end. `gesture` performs the interaction; `retry`
        // performs the opposite direction when the first one hit a clamp.
        const auto exercise = [&]([[maybe_unused]] EditProperty property, const std::string& expectedDisplay,
                                  const std::function<void()>& gesture, const std::function<void()>& retry,
                                  size_t expectedEntries = 1)
        {
            settle(2);
            const std::string name = expectedDisplay;
            const size_t depth = History().UndoDepth();
            const Engine::u64 head = History().HeadRevision();
            const HistorySnapshot before = CaptureHistorySnapshot();
            Engine::u64 capturesBefore = m_HistorySnapshotCaptures;
            gesture();
            Engine::u64 captures = m_HistorySnapshotCaptures - capturesBefore;
            if (retry && History().UndoDepth() == depth)
            {
                capturesBefore = m_HistorySnapshotCaptures;
                retry();
                captures = m_HistorySnapshotCaptures - capturesBefore;
            }
            ++report.Widgets;
            failures.Expect(History().UndoDepth() == depth + expectedEntries,
                name + ": the gesture must record exactly " + std::to_string(expectedEntries) + " entry (depth "
                    + std::to_string(depth) + " -> " + std::to_string(History().UndoDepth()) + ")");
            if (History().UndoDepth() != depth + expectedEntries)
                return;
            ++report.Gestures;
            failures.Expect(History().HeadRevision() > head, name + ": the head revision must advance");
            failures.Expect(!History().GestureOpen() && !m_EditGestureTracker.Open(), name + ": no gesture may stay open");
            failures.Expect(History().RedoDepth() == 0, name + ": a new entry clears redo");
            failures.Expect(History().TopUndo() && History().TopUndo()->Label.Display() == expectedDisplay,
                name + ": entry named \"" + (History().TopUndo() ? History().TopUndo()->Label.Display() : "<none>")
                    + "\" instead of \"" + expectedDisplay + "\"");
            failures.Expect(captures == 2, name + ": a gesture captures exactly Before and After, captured " + std::to_string(captures));

            const HistorySnapshot after = CaptureHistorySnapshot();
            failures.Expect(Undo(), name + ": undo succeeded");
            const HistorySnapshot undone = CaptureHistorySnapshot();
            failures.Expect(EditorHistoryStatesEqual(*before, *undone), name + ": undo must restore the project exactly");
            failures.Expect(undone->SelectedEntity == before->SelectedEntity, name + ": undo restores the entry's selection");
            failures.Expect(Redo(), name + ": redo succeeded");
            failures.Expect(EditorHistoryStatesEqual(*after, *CaptureHistorySnapshot()), name + ": redo must restore the edited project exactly");
            settle(2);
        };

        const auto nameOf = [&](Engine::Entity entity) { return m_ActiveScene.TryGetEntity(entity)->Name; };
        const auto label = [&](EditProperty property, const std::string& target)
        {
            return EditorHistory::MakeEditLabel(property, target).Display();
        };
        const auto drag = [&](EditProperty property, const std::string& target)
        {
            exercise(property, label(property, target), [&] { dragWidget(property, 8.0f, 5); },
                [&] { dragWidget(property, -8.0f, 5); });
        };
        const auto typed = [&](EditProperty property, const std::string& target, const std::string& text)
        {
            exercise(property, label(property, target), [&] { typeInto(property, text); }, nullptr);
        };
        const auto clicked = [&](EditProperty property, const std::string& target)
        {
            exercise(property, label(property, target), [&] { clickWidget(property); }, nullptr);
        };
        const auto chosen = [&](EditProperty property, const std::string& target)
        {
            exercise(property, label(property, target), [&] { chooseNext(property); }, nullptr);
        };

        // ---- idle frames copy nothing ----
        m_SelectedEntity = m_PrototypeMeshEntity;
        m_ViewportDiscontinuousRelocationPending = false;
        settle(3);
        {
            const Engine::u64 captures = m_HistorySnapshotCaptures;
            const size_t depth = History().UndoDepth();
            settle(120);
            failures.Expect(m_HistorySnapshotCaptures == captures, "120 idle Inspector frames must capture no project snapshot, captured "
                    + std::to_string(m_HistorySnapshotCaptures - captures));
            failures.Expect(History().UndoDepth() == depth, "idle frames record nothing");
        }

        // ---- prototype mesh: transform, mesh renderer, material ----
        const std::string prototype = nameOf(m_PrototypeMeshEntity);
        openMaterialHeader();
        settle(3);
        drag(EditProperty::TransformPosition, prototype);
        drag(EditProperty::TransformRotation, prototype);
        drag(EditProperty::TransformScale, prototype);
        const std::string materialName = m_MaterialLibrary.Get(
            m_ActiveScene.TryGetMeshRendererComponent(m_PrototypeMeshEntity)->MaterialAsset)->Name;
        const EditProperty materialDrags[] = {
            EditProperty::MaterialBaseColor, EditProperty::MaterialMetallic, EditProperty::MaterialRoughness,
            EditProperty::MaterialNormalScale, EditProperty::MaterialOcclusionStrength,
            EditProperty::MaterialEmissiveColor, EditProperty::MaterialEmissiveStrength,
            EditProperty::MaterialDiffuseFresnel, EditProperty::MaterialRetroreflection,
            EditProperty::MaterialDiffuseFalloff, EditProperty::MaterialRetroreflectionFalloff,
            EditProperty::MaterialSmoothTerminator
        };
        for (const EditProperty property : materialDrags)
            drag(property, materialName);
        clicked(EditProperty::MeshRendererVisible, prototype);
        clicked(EditProperty::MeshRendererCastsShadows, prototype);
        clicked(EditProperty::MaterialTwoSided, materialName);
        chosen(EditProperty::MaterialShadingModel, materialName);
        chosen(EditProperty::MaterialAlphaMode, materialName);
        chosen(EditProperty::MaterialSamplerBaseColor, materialName);

        // Text values: a rename, typed in several keystrokes, is one entry.
        typed(EditProperty::MeshRendererMeshName, prototype, "abc");
        typed(EditProperty::MaterialName, materialName, "xyz");

        // Handles last: a typed handle may leave the material or mesh unresolved until undo.
        typed(EditProperty::MaterialTextureBaseColor, m_MaterialLibrary.Get(
            m_ActiveScene.TryGetMeshRendererComponent(m_PrototypeMeshEntity)->MaterialAsset)->Name, "7");
        typed(EditProperty::MeshRendererMaterialAsset, prototype, "5");
        typed(EditProperty::MeshRendererMeshAsset, prototype, "3");

        // ---- directional light ----
        m_SelectedEntity = m_DirectionalLightEntity;
        settle(3);
        const std::string light = nameOf(m_DirectionalLightEntity);
        drag(EditProperty::LightColor, light);
        drag(EditProperty::LightRange, light);
        drag(EditProperty::LightInnerCone, light);
        drag(EditProperty::LightOuterCone, light);
        typed(EditProperty::LightPhotometricValue, light, "1234");
        clicked(EditProperty::LightCastsShadows, light);
        chosen(EditProperty::LightType, light);

        // ---- main camera: every edit here is a camera edit ----
        const Engine::Entity mainCamera = m_ActiveScene.GetMainCameraEntity();
        m_SelectedEntity = mainCamera;
        settle(3);
        const std::string cameraName = nameOf(mainCamera);
        drag(EditProperty::CameraVerticalFov, cameraName);
        drag(EditProperty::CameraNearClip, cameraName);
        drag(EditProperty::CameraFarClip, cameraName);
        drag(EditProperty::CameraBackgroundColor, cameraName);
        clicked(EditProperty::CameraPrimary, cameraName);
        drag(EditProperty::TransformPosition, cameraName);
        drag(EditProperty::TransformRotation, cameraName);
        failures.Expect(m_CameraEpoch > 0, "camera edits advance the camera epoch");
        // The app's per-frame camera update, which the Primary checkbox test left undone.
        ApplyEditorCameraStateToScene();

        // ---- scenario: a drag out and back records nothing ----
        m_SelectedEntity = m_PrototypeMeshEntity;
        settle(3);
        {
            const size_t depth = History().UndoDepth();
            const std::optional<WidgetRect> widget = find(EditProperty::TransformRotation);
            if (widget)
            {
                Pointer pointer = aim(*widget);
                frame(pointer);
                pointer.Down = true;
                frame(pointer);
                for (int index = 0; index < 4; ++index)
                {
                    pointer.X += 8.0f;
                    frame(pointer);
                }
                for (int index = 0; index < 4; ++index)
                {
                    pointer.X -= 8.0f;
                    frame(pointer);
                }
                pointer.Down = false;
                frame(pointer);
                settle(3);
            }
            failures.Expect(widget.has_value() && History().UndoDepth() == depth,
                "a rotation dragged out and back to the exact start records no entry");
        }

        // ---- scenario: two separate drags are two entries ----
        {
            const size_t depth = History().UndoDepth();
            dragWidget(EditProperty::TransformScale, 8.0f, 4);
            dragWidget(EditProperty::TransformScale, 8.0f, 4);
            failures.Expect(History().UndoDepth() == depth + 2, "two separate drags are two entries");
        }

        // ---- scenario: a widget activated while another gesture is still open ----
        // The Mesh Name field is submitted after the Rotation control, so pressing on
        // Rotation while Mesh Name is being edited rejects the activation (the open gesture
        // has not been seen to end yet). The drag that follows must still be one entry whose
        // Before state is the project before the first frame of the drag.
        {
            m_SelectedEntity = m_PrototypeMeshEntity;
            waitOutDoubleClick();
            const std::optional<WidgetRect> meshName = find(EditProperty::MeshRendererMeshName);
            const std::optional<WidgetRect> rotationWidget = find(EditProperty::TransformRotation);
            failures.Expect(meshName.has_value() && rotationWidget.has_value(), "overlap scenario probes");
            if (meshName && rotationWidget)
            {
                const size_t depth = History().UndoDepth();
                const HistorySnapshot before = CaptureHistorySnapshot();
                Pointer pointer = aim(*meshName);
                frame(pointer);
                pointer.Down = true;
                frame(pointer);
                pointer.Down = false;
                frame(pointer);
                frame(pointer);
                failures.Expect(m_EditGestureTracker.Open(), "focusing Mesh Name opens a gesture");
                const Engine::u64 capturesBefore = m_HistorySnapshotCaptures;
                pointer = aim(*rotationWidget);
                frame(pointer);
                pointer.Down = true;
                frame(pointer);
                for (int index = 0; index < 5; ++index)
                {
                    pointer.X += 8.0f;
                    frame(pointer);
                }
                pointer.Down = false;
                frame(pointer);
                settle(3);
                failures.Expect(History().UndoDepth() == depth + 1 && !History().GestureOpen() && !m_EditGestureTracker.Open(),
                    "pressing on another widget while a text field is open records only the drag (depth "
                        + std::to_string(depth) + " -> " + std::to_string(History().UndoDepth()) + ")");
                failures.Expect(History().TopUndo() && History().TopUndo()->Label.Display() == "Rotate " + nameOf(m_PrototypeMeshEntity),
                    "the overlapped drag is named Rotate");
                failures.Expect(m_HistorySnapshotCaptures - capturesBefore == 2,
                    "the unchanged text field captures nothing more and the late-opened drag captures Before and After, captured "
                        + std::to_string(m_HistorySnapshotCaptures - capturesBefore));
                const HistorySnapshot after = CaptureHistorySnapshot();
                failures.Expect(Undo() && EditorHistoryStatesEqual(*before, *CaptureHistorySnapshot()),
                    "undo restores the project from before the first frame of the overlapped drag");
                failures.Expect(Redo() && EditorHistoryStatesEqual(*after, *CaptureHistorySnapshot()),
                    "redo restores the overlapped drag exactly");
            }
        }

        // ---- scenario: press and first movement in the same frame ----
        {
            waitOutDoubleClick();
            const std::optional<WidgetRect> widget = find(EditProperty::TransformScale);
            failures.Expect(widget.has_value(), "same-frame scenario probe");
            if (widget)
            {
                const size_t depth = History().UndoDepth();
                const HistorySnapshot before = CaptureHistorySnapshot();
                Pointer pointer = aim(*widget);
                frame(pointer);
                pointer.Down = true;
                pointer.X += 10.0f;
                frame(pointer);
                for (int index = 0; index < 4; ++index)
                {
                    pointer.X += 8.0f;
                    frame(pointer);
                }
                pointer.Down = false;
                frame(pointer);
                settle(3);
                failures.Expect(History().UndoDepth() == depth + 1 && !History().GestureOpen(),
                    "a press that moves in its first frame is still one entry");
                const HistorySnapshot after = CaptureHistorySnapshot();
                failures.Expect(Undo() && EditorHistoryStatesEqual(*before, *CaptureHistorySnapshot()),
                    "undo restores the project from before the press");
                failures.Expect(Redo() && EditorHistoryStatesEqual(*after, *CaptureHistorySnapshot()),
                    "redo restores the drag exactly");
            }
        }

        // ---- scenario: shortcuts ----
        const auto chord = [&](ImGuiKey key, bool shift, Pointer pointer)
        {
            frame(pointer, [key, shift](ImGuiIO& input)
            {
                input.AddKeyEvent(ImGuiMod_Ctrl, true);
                input.AddKeyEvent(ImGuiMod_Shift, shift);
                input.AddKeyEvent(key, true);
            }, true);
            frame(pointer, [key](ImGuiIO& input)
            {
                input.AddKeyEvent(key, false);
                input.AddKeyEvent(ImGuiMod_Shift, false);
                input.AddKeyEvent(ImGuiMod_Ctrl, false);
            }, true);
        };
        {
            settle(3);
            const size_t depth = History().UndoDepth();
            const size_t redo = History().RedoDepth();

            // Ctrl+Z undoes with the Edit menu's wording; Ctrl+Shift+Z redoes (it used to undo).
            const std::string topUndo = History().TopUndo()->Label.Display();
            chord(ImGuiKey_Z, false, Pointer {});
            failures.Expect(History().UndoDepth() == depth - 1 && History().RedoDepth() == redo + 1
                    && m_LastHistoryAnnouncement == "Undo: " + topUndo,
                "Ctrl+Z undoes and announces \"Undo: " + topUndo + "\", announced \"" + m_LastHistoryAnnouncement + "\"");
            failures.Expect(HistoryStatusText() == "Undo: " + topUndo, "the status text shows the announcement");
            chord(ImGuiKey_Z, true, Pointer {});
            failures.Expect(History().UndoDepth() == depth && History().RedoDepth() == redo
                    && m_LastHistoryAnnouncement == "Redo: " + topUndo,
                "Ctrl+Shift+Z redoes");
            chord(ImGuiKey_Z, false, Pointer {});
            chord(ImGuiKey_Y, false, Pointer {});
            failures.Expect(History().UndoDepth() == depth && History().RedoDepth() == redo, "Ctrl+Y redoes");
            // Plain Z and Ctrl+Alt+Z are not history chords.
            frame(Pointer {}, [](ImGuiIO& input) { input.AddKeyEvent(ImGuiKey_Z, true); }, true);
            frame(Pointer {}, [](ImGuiIO& input) { input.AddKeyEvent(ImGuiKey_Z, false); }, true);
            failures.Expect(History().UndoDepth() == depth, "an unmodified Z does nothing");

            // No window focus: nothing fires.
            m_WindowFocused = false;
            chord(ImGuiKey_Z, false, Pointer {});
            failures.Expect(History().UndoDepth() == depth, "an unfocused window ignores Ctrl+Z");
            m_WindowFocused = true;

            // A text field being edited keeps Ctrl+Z for itself.
            {
                const std::optional<WidgetRect> widget = find(EditProperty::EntityName);
                failures.Expect(widget.has_value(), "name widget probe");
                if (widget)
                {
                    Pointer pointer = aim(*widget);
                    frame(pointer);
                    pointer.Down = true;
                    frame(pointer);
                    pointer.Down = false;
                    frame(pointer);
                    frame(pointer);
                    chord(ImGuiKey_Z, false, pointer);
                    failures.Expect(History().UndoDepth() == depth, "Ctrl+Z is ignored while a text field is active");
                    keyFrame(ImGuiKey_Escape, true, pointer);
                    keyFrame(ImGuiKey_Escape, false, pointer);
                    settle(3);
                    failures.Expect(History().UndoDepth() == depth && !History().GestureOpen(),
                        "leaving a text field without a change records nothing and closes its gesture");
                }
            }

            // A drag in progress keeps the history still; after release it records and Ctrl+Z works.
            {
                const std::optional<WidgetRect> widget = find(EditProperty::TransformPosition);
                failures.Expect(widget.has_value(), "position widget probe");
                if (widget)
                {
                    Pointer pointer = aim(*widget);
                    frame(pointer);
                    pointer.Down = true;
                    frame(pointer);
                    pointer.X += 8.0f;
                    frame(pointer);
                    pointer.X += 8.0f;
                    frame(pointer);
                    failures.Expect(History().GestureOpen(), "a drag keeps a gesture open");
                    const EditorHistory::HistoryCommandText midDrag = UndoCommandText();
                    failures.Expect(!midDrag.Enabled && midDrag.Reason == "Undo unavailable: finish the current edit",
                        "the Edit menu disables Undo mid-gesture and says why: " + midDrag.Reason);
                    chord(ImGuiKey_Z, false, pointer);
                    failures.Expect(History().UndoDepth() == depth && History().GestureOpen()
                            && m_LastHistoryAnnouncement == "Finish the current edit first",
                        "Ctrl+Z is refused with a reason during a drag: \"" + m_LastHistoryAnnouncement + "\"");
                    pointer.Down = false;
                    frame(pointer);
                    settle(3);
                    failures.Expect(History().UndoDepth() == depth + 1 && !History().GestureOpen(),
                        "the drag records one entry on release");
                    const std::string moved = History().TopUndo() ? History().TopUndo()->Label.Display() : "";
                    chord(ImGuiKey_Z, false, Pointer {});
                    failures.Expect(History().UndoDepth() == depth && m_LastHistoryAnnouncement == "Undo: " + moved,
                        "after release Ctrl+Z undoes the drag: \"" + m_LastHistoryAnnouncement + "\"");
                    chord(ImGuiKey_Y, false, Pointer {});
                    failures.Expect(History().UndoDepth() == depth + 1, "Ctrl+Y brings it back");
                }
            }
        }

        // ---- scenario: the selection changes while a drag is open ----
        {
            m_SelectedEntity = m_PrototypeMeshEntity;
            settle(3);
            const size_t depth = History().UndoDepth();
            const HistorySnapshot lightBefore = CaptureHistorySnapshot();
            const std::string owner = nameOf(m_PrototypeMeshEntity);
            const std::optional<WidgetRect> widget = find(EditProperty::TransformRotation);
            failures.Expect(widget.has_value(), "rotation widget probe");
            if (widget)
            {
                Pointer pointer = aim(*widget);
                frame(pointer);
                pointer.Down = true;
                frame(pointer);
                pointer.X += 8.0f;
                frame(pointer);
                pointer.X += 8.0f;
                frame(pointer);
                m_SelectedEntity = m_DirectionalLightEntity;
                pointer.X += 8.0f;
                frame(pointer);
                pointer.X += 8.0f;
                frame(pointer);
                pointer.Down = false;
                frame(pointer);
                settle(3);
                failures.Expect(History().UndoDepth() == depth + 1 && !History().GestureOpen(),
                    "a drag whose entity changed still ends as one entry");
                failures.Expect(History().TopUndo() && History().TopUndo()->Label.Display() == "Rotate " + owner,
                    "the entry keeps the entity the drag started on");
                const HistorySnapshot now = CaptureHistorySnapshot();
                const Engine::SceneEntity* lightNow = now->Scene.TryGetEntity(m_DirectionalLightEntity);
                const Engine::SceneEntity* lightThen = lightBefore->Scene.TryGetEntity(m_DirectionalLightEntity);
                failures.Expect(lightNow && lightThen && lightNow->Transform.RotationDegrees.X == lightThen->Transform.RotationDegrees.X
                        && lightNow->Transform.RotationDegrees.Y == lightThen->Transform.RotationDegrees.Y,
                    "an edit refused after the selection changed does not leak onto the new entity");
                Undo();
                Redo();
            }
        }

        // ---- scenario: undo keeps the navigation camera; a camera edit moves it ----
        {
            m_SelectedEntity = m_PrototypeMeshEntity;
            settle(3);
            dragWidget(EditProperty::TransformRotation, 8.0f, 3);
            m_CameraPosition = { 11.0, 12.0, -13.0 };
            m_CameraRotation = { 4.0f, 5.0f, 0.0f };
            ApplyEditorCameraStateToScene();
            m_ViewportDiscontinuousRelocationPending = false;
            const std::array<double, 3> navigated = m_CameraPosition;
            const std::array<float, 3> navigatedRotation = m_CameraRotation;
            failures.Expect(Undo(), "camera scenario: undo of a non-camera entry");
            failures.Expect(m_CameraPosition == navigated && m_CameraRotation == navigatedRotation,
                "undoing a non-camera entry must not move the navigation camera");
            Engine::Math::DVec3 sceneCamera;
            failures.Expect(m_ActiveScene.TryGetEntityApproximateWorldPosition(m_ActiveScene.GetMainCameraEntity(), sceneCamera)
                    && sceneCamera.X == navigated[0] && sceneCamera.Y == navigated[1] && sceneCamera.Z == navigated[2],
                "the restored scene carries the live camera pose");
            failures.Expect(!m_ViewportDiscontinuousRelocationPending, "undoing a non-camera entry does not flag a camera relocation");
            failures.Expect(Redo() && m_CameraPosition == navigated && !m_ViewportDiscontinuousRelocationPending,
                "redoing a non-camera entry keeps the navigation camera too");

            // A camera edit: undo returns to the pose the entry started from, redo to the pose it made.
            m_SelectedEntity = m_ActiveScene.GetMainCameraEntity();
            settle(3);
            const std::array<double, 3> poseBefore = m_CameraPosition;
            dragWidget(EditProperty::TransformPosition, 8.0f, 4);
            const std::array<double, 3> poseAfter = m_CameraPosition;
            failures.Expect(poseAfter != poseBefore, "camera scenario: the Position drag moved the main camera");
            m_CameraPosition = { -21.0, 22.0, 23.0 };
            ApplyEditorCameraStateToScene();
            failures.Expect(Undo() && m_CameraPosition == poseBefore && m_ViewportDiscontinuousRelocationPending,
                "undoing a camera edit puts the camera back where the edit started");
            failures.Expect(Redo() && m_CameraPosition == poseAfter,
                "redoing a camera edit puts the camera where the edit left it");
            m_ViewportDiscontinuousRelocationPending = false;
        }

        // ---- scenario: special entities are tracked by id, never by name ----
        {
            m_SelectedEntity = m_PrototypeMeshEntity;
            settle(3);
            const Engine::Entity original = m_PrototypeMeshEntity;
            const std::string oldName = nameOf(original);
            typed(EditProperty::EntityName, oldName, "Q");
            const std::string newName = nameOf(original);
            failures.Expect(newName != oldName, "the rename changed the entity name");
            failures.Expect(m_PrototypeMeshEntity == original && !m_ActiveScene.FindEntityByName(oldName),
                "a renamed Prototype Mesh is still the tracked prototype and no entity carries the old name");
            // A duplicate carrying the original name must not capture the role.
            const Engine::Entity duplicate = CreateSceneEntity(oldName);
            failures.Expect(duplicate && duplicate != original && m_ActiveScene.FindEntityByName(oldName) == duplicate,
                "the duplicate-named entity exists");
            failures.Expect(m_PrototypeMeshEntity == original, "a duplicate name does not steal the Prototype Mesh role");
            failures.Expect(Undo() && m_PrototypeMeshEntity == original && !m_ActiveScene.IsEntityValid(duplicate),
                "undoing the duplicate keeps the role");
            failures.Expect(Undo() && m_PrototypeMeshEntity == original && nameOf(original) == oldName,
                "undoing the rename keeps the role and restores the name");
            failures.Expect(Redo() && m_PrototypeMeshEntity == original && nameOf(original) == newName,
                "redoing the rename keeps the role");
            failures.Expect(Redo() && m_PrototypeMeshEntity == original && m_ActiveScene.IsEntityValid(duplicate)
                    && m_ActiveScene.FindEntityByName(oldName) == duplicate,
                "redoing the duplicate keeps the role on the original entity");
            failures.Expect(m_ActiveScene.TryGetMeshRendererComponent(m_PrototypeMeshEntity) != nullptr,
                "the tracked prototype still has its mesh renderer");
            // Deleting the tracked entity clears the role; undo brings it back with the same id.
            m_SelectedEntity = original;
            failures.Expect(DeleteSelectedEntity() && !m_PrototypeMeshEntity, "deleting the prototype clears the role");
            failures.Expect(History().TopUndo() && History().TopUndo()->Label.Display() == "Delete " + newName,
                "the delete entry is named for the entity");
            failures.Expect(Undo() && m_PrototypeMeshEntity == original, "undoing the delete restores the role by id");
            // Put the name back so later checks see the fixture.
            m_ActiveScene.TryGetEntity(original)->Name = oldName;
            m_SelectedEntity = original;
        }

        // ---- scenario: redo is discarded by a new entry, with a notice ----
        {
            settle(2);
            failures.Expect(Undo() && Undo(), "two undos for the redo scenario");
            const size_t redoDepth = History().RedoDepth();
            failures.Expect(redoDepth >= 2, "redo entries exist");
            const Engine::Entity created = CreateSceneEntity();
            failures.Expect(created && History().RedoDepth() == 0, "a new entry empties redo");
            failures.Expect(History().TopUndo() && History().TopUndo()->Label.Display() == "Create " + nameOf(created),
                "the create entry is named for the new entity");
            failures.Expect(m_LastHistoryAnnouncement == "Redo history discarded (" + std::to_string(redoDepth) + ")",
                "the redo discard is announced: \"" + m_LastHistoryAnnouncement + "\"");
            failures.Expect(!Redo(), "redo after a discard reports nothing to redo");
            failures.Expect(m_LastHistoryAnnouncement == "Redo unavailable: nothing to redo",
                "the empty redo is announced: \"" + m_LastHistoryAnnouncement + "\"");
        }

        // ---- scenario: the History panel lists newest first and jumps on click ----
        {
            settle(3, true);
            const std::vector<EditorHistory::HistoryRow> rowsNow = History().Rows();
            failures.Expect(rowsNow.size() > 4 && rowsNow.back().Current, "the newest entry is current");
            std::optional<WidgetRect> twoBack;
            std::optional<WidgetRect> newest;
            for (const auto& entry : rows)
            {
                if (entry.first + 3 == rowsNow.size())
                    twoBack = entry.second;
                if (entry.first + 1 == rowsNow.size())
                    newest = entry.second;
            }
            failures.Expect(twoBack.has_value() && newest.has_value() && newest->Min.y < twoBack->Min.y,
                "rows are drawn newest first");
            if (twoBack && newest)
            {
                const auto clickRow = [&](const WidgetRect& row)
                {
                    Pointer pointer { (row.Min.x + row.Max.x) * 0.5f, (row.Min.y + row.Max.y) * 0.5f, false };
                    frame(pointer, {}, false, true);
                    pointer.Down = true;
                    frame(pointer, {}, false, true);
                    pointer.Down = false;
                    frame(pointer, {}, false, true);
                    settle(2, true);
                };
                clickRow(*twoBack);
                failures.Expect(History().Cursor() == rowsNow.size() - 3 && History().RedoDepth() == 2
                        && m_LastHistoryAnnouncement.rfind("Jumped to: ", 0) == 0,
                    "clicking a row jumps there: cursor " + std::to_string(History().Cursor()) + ", \"" + m_LastHistoryAnnouncement + "\"");
                std::optional<WidgetRect> head;
                for (const auto& entry : rows)
                {
                    if (entry.first + 1 == rowsNow.size())
                        head = entry.second;
                }
                failures.Expect(head.has_value(), "the newest row is still listed after the jump");
                if (head)
                    clickRow(*head);
                failures.Expect(History().RedoDepth() == 0 && History().Rows().back().Current,
                    "clicking the newest row jumps forward again");
            }
        }

        // ---- the project was edited in memory only ----
        failures.Expect(!History().GestureOpen() && !m_EditGestureTracker.Open(), "no gesture leaks out of the smoke");

        finish(true);
        if (!failures.Empty())
            throw std::runtime_error("Editor history smoke failed:" + failures.Join());
        Engine::Log::Info("EditorHistorySmokeV1 widgets=", report.Widgets, " gestures=", report.Gestures,
            " entries=", History().EntryCount(), " idleFrameSnapshots=0 snapshotsPerGesture=2",
            " undoRedo=byte-exact shortcuts=ctrl-z,ctrl-y,ctrl-shift-z,blocked-while-typing-and-dragging",
            " camera=navigation-kept-unless-edited specialEntities=id-tracked panelJump=click result=pass");
    }
    catch (...)
    {
        finish(true);
        throw;
    }
    m_EditorHistorySmokeCompleted = true;
    Engine::Application::Get().Close();
    return true;
}

bool EditorLayer::RunEditorHistoryBenchmark()
{
    if (!Engine::Application::Get().GetSpecification().Window.Headless)
        throw std::runtime_error("--editor-history-benchmark requires --headless");

    const auto contextOwner = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigInputTrickleEventQueue = false;
    io.DisplaySize = ImVec2(kDisplayWidth, kDisplayHeight);
    unsigned char* pixels = nullptr;
    int fontWidth = 0;
    int fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fontWidth, &fontHeight);
    std::optional<WidgetRect> rotation;
    size_t widgetsThisFrame = 0;
    m_WidgetProbe = [&rotation, &widgetsThisFrame](EditProperty property, const ImVec2& minimum, const ImVec2& maximum)
    {
        ++widgetsThisFrame;
        if (property == EditProperty::TransformRotation)
            rotation = WidgetRect { property, minimum, maximum };
    };
    m_PanelVisible.fill(true);

    using Clock = std::chrono::steady_clock;
    const auto milliseconds = [](Clock::duration duration)
    {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    size_t widgetsPerFrame = 0;
    const auto frame = [&](const Pointer& pointer)
    {
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(pointer.X, pointer.Y);
        io.AddMouseButtonEvent(0, pointer.Down);
        ImGui::NewFrame();
        widgetsThisFrame = 0;
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(760.0f, kDisplayHeight - 100.0f));
        DrawInspectorPanel();
        widgetsPerFrame = widgetsThisFrame;
        EndOfFrameEditGestureFlush();
        ImGui::Render();
    };

    // The scene the finding describes: about a thousand entities with mesh renderers and
    // the project's registry and materials. A second pass at ten thousand shows the trend.
    const Engine::MeshRendererComponent* prototypeRenderer = m_ActiveScene.TryGetMeshRendererComponent(m_PrototypeMeshEntity);
    if (!prototypeRenderer)
        throw std::runtime_error("history benchmark requires the prototype mesh renderer");
    const Engine::MeshRendererComponent renderer = *prototypeRenderer;
    int created = 0;
    const auto growTo = [&](int total)
    {
        for (; created < total; ++created)
        {
            const Engine::Entity entity = m_ActiveScene.CreateEntity("Benchmark Entity " + std::to_string(created));
            m_ActiveScene.AddMeshRendererComponent(entity, renderer);
            m_ActiveScene.SetEntityWorldPosition(entity, { static_cast<double>(created % 40), static_cast<double>(created / 40), 0.0 });
        }
    };

    struct Measurement
    {
        size_t Entities = 0;
        Engine::u64 SnapshotBytes = 0;
        double IdleFrameMs = 0.0;
        double RemovedSnapshotMs = 0.0;
        double DragFrameMs = 0.0;
        double GestureMs = 0.0;
        Engine::u64 IdleCaptures = 0;
        Engine::u64 DragCaptures = 0;
        size_t DragEntries = 0;
        size_t Widgets = 0;
    };
    const auto measure = [&]() -> Measurement
    {
        Measurement result;
        result.Entities = m_ActiveScene.GetEntities().size();
        m_SelectedEntity = m_PrototypeMeshEntity;
        for (int index = 0; index < 5; ++index)
            frame(Pointer {});

        // What the Inspector used to pay on every frame an entity was selected.
        constexpr int kCaptureSamples = 40;
        const Clock::time_point captureStart = Clock::now();
        HistorySnapshot keep;
        for (int index = 0; index < kCaptureSamples; ++index)
            keep = CaptureHistorySnapshot();
        result.RemovedSnapshotMs = milliseconds(Clock::now() - captureStart) / kCaptureSamples;
        result.SnapshotBytes = keep ? EstimateEditorHistoryStateBytes(*keep) : 0;

        // The Inspector now: idle frames.
        constexpr int kIdleSamples = 200;
        const Engine::u64 capturesBeforeIdle = m_HistorySnapshotCaptures;
        const Clock::time_point idleStart = Clock::now();
        for (int index = 0; index < kIdleSamples; ++index)
            frame(Pointer {});
        result.IdleFrameMs = milliseconds(Clock::now() - idleStart) / kIdleSamples;
        result.IdleCaptures = m_HistorySnapshotCaptures - capturesBeforeIdle;
        result.Widgets = widgetsPerFrame;

        // A two-second drag: 120 frames at 60 Hz.
        if (rotation)
        {
            Pointer pointer { rotation->Min.x + 14.0f, (rotation->Min.y + rotation->Max.y) * 0.5f, false };
            frame(pointer);
            const size_t depth = History().UndoDepth();
            const Engine::u64 capturesBeforeDrag = m_HistorySnapshotCaptures;
            const Clock::time_point gestureStart = Clock::now();
            pointer.Down = true;
            frame(pointer);
            for (int index = 0; index < 120; ++index)
            {
                pointer.X += 0.5f;
                frame(pointer);
            }
            pointer.Down = false;
            frame(pointer);
            frame(pointer);
            result.GestureMs = milliseconds(Clock::now() - gestureStart);
            result.DragFrameMs = result.GestureMs / 124.0;
            result.DragCaptures = m_HistorySnapshotCaptures - capturesBeforeDrag;
            result.DragEntries = History().UndoDepth() - depth;
        }
        return result;
    };

    growTo(1000);
    const Measurement thousand = measure();
    growTo(10000);
    const Measurement tenThousand = measure();
    m_WidgetProbe = nullptr;
    ImGui::DestroyContext(contextOwner);

    const auto observed = [](const Measurement& value)
    {
        return value.IdleCaptures == 0 && value.DragCaptures == 2 && value.DragEntries == 1 && value.Widgets >= 8;
    };
    if (!rotation || !observed(thousand) || !observed(tenThousand))
        throw std::runtime_error("Editor history benchmark did not observe copy-then-apply (idle captures "
            + std::to_string(thousand.IdleCaptures) + ", drag captures " + std::to_string(thousand.DragCaptures)
            + ", drag entries " + std::to_string(thousand.DragEntries) + ", widgets "
            + std::to_string(thousand.Widgets) + ")");
    const auto report = [](const char* label, const Measurement& value)
    {
        Engine::Log::Info("EditorHistoryBenchmarkV1 scene=", label, " entities=", value.Entities,
            " inspectorWidgetsPerFrame=", value.Widgets, " snapshotBytes=", value.SnapshotBytes,
            " newInspectorIdleFrameMs=", value.IdleFrameMs, " removedPerFrameSnapshotMs=", value.RemovedSnapshotMs,
            " oldInspectorIdleFrameMs=", value.IdleFrameMs + value.RemovedSnapshotMs,
            " newDragFrameMs=", value.DragFrameMs, " dragGestureTotalMs=", value.GestureMs,
            " idleFrameSnapshots=", value.IdleCaptures, " dragSnapshots=", value.DragCaptures,
            " dragEntries=", value.DragEntries, " result=pass");
    };
    report("1000", thousand);
    report("10000", tenThousand);
    m_EditorHistorySmokeCompleted = true;
    Engine::Application::Get().Close();
    return true;
}
