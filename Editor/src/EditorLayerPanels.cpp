// EditorLayer members for panel close buttons, the Window menu entries, and the
// persisted panel visibility. The file is a workspace-global sibling of the
// navigation-preset file (never part of a project); its codec is the pure
// Layout/PanelVisibility.
#include "EditorLayer.h"

#include "Layout/PanelVisibility.h"

#include "Engine/Core/AtomicFile.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
    namespace Layout = SpiralEditor::Layout;

    // Stable ASCII ids, in EditorLayer::PanelIndex order. The ImGui window titles stay
    // the ini keys, so existing dock layouts keep working.
    const char* const kPanelIds[] = { "scene.hierarchy", "scene.inspector", "scene.viewport",
        "content.browser", "diagnostics.console", "diagnostics.profiler" };

    std::vector<std::string> KnownPanelIds()
    {
        return { std::begin(kPanelIds), std::end(kPanelIds) };
    }
}

void EditorLayer::LoadPanelVisibility()
{
    m_PanelVisibilityPath = (std::filesystem::path(m_EditorSettingsPath).parent_path()
        / "panel-visibility.spiralsettings").string();
    m_PanelVisible.fill(true);

    std::error_code error;
    if (std::filesystem::exists(m_PanelVisibilityPath, error) && !error)
    {
        std::ifstream input(m_PanelVisibilityPath, std::ios::binary);
        std::string text;
        if (input)
        {
            text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            std::vector<Layout::PanelVisibility> panels;
            std::string parseError;
            if (Layout::ParsePanelVisibility(text, KnownPanelIds(), panels, parseError))
            {
                for (size_t index = 0; index < kPanelCount && index < panels.size(); ++index)
                    m_PanelVisible[index] = panels[index].Visible;
            }
            else
            {
                Engine::Log::Warn("Panel visibility file rejected; showing every panel: ",
                    m_PanelVisibilityPath, " (", parseError, ")");
            }
        }
    }

    // The capability smoke inspects the Profiler, so a hidden Profiler must not hide it.
    if (m_RendererCapabilitySmokeRequested)
        m_PanelVisible[PanelProfiler] = true;
    m_PanelVisiblePersisted = m_PanelVisible;
}

void EditorLayer::PersistPanelVisibilityIfChanged()
{
    if (m_PanelVisible == m_PanelVisiblePersisted)
        return;

    std::vector<Layout::PanelVisibility> panels;
    for (size_t index = 0; index < kPanelCount; ++index)
        panels.push_back({ kPanelIds[index], m_PanelVisible[index] });
    std::string error;
    if (!Engine::WriteFileAtomically(m_PanelVisibilityPath, Layout::FormatPanelVisibility(panels), error))
    {
        Engine::Log::Warn("Panel visibility could not be saved: ", m_PanelVisibilityPath, " (", error, ")");
        m_ConsoleLines.emplace_back("Panel layout could not be saved: " + m_PanelVisibilityPath);
    }
    // One attempt per change: a failing disk must not retry every frame.
    m_PanelVisiblePersisted = m_PanelVisible;
}

void EditorLayer::ResetPanelVisibility()
{
    m_PanelVisible.fill(true);
}

bool EditorLayer::BeginClosablePanel(size_t panel, const char* title, int windowFlags)
{
    if (!m_PanelVisible[panel])
        return false;

    bool open = true;
    ImGui::Begin(title, &open, windowFlags);
    // The close button hides the panel from the next frame on; the Window menu brings
    // it back, and ImGui restores the dock node it was in.
    if (!open)
        m_PanelVisible[panel] = false;
    return true;
}

bool EditorLayer::RunPanelUiSmokeFrame()
{
    constexpr unsigned int kFrames = 14;
    if (m_PanelUiSmokeFrames >= kFrames)
        return false;

    const auto fail = [this](const std::string& what)
    {
        if (m_PanelUiSmokeContext)
            ImGui::DestroyContext(m_PanelUiSmokeContext);
        m_PanelUiSmokeContext = nullptr;
        throw std::runtime_error("Panel UI smoke failed: " + what);
    };
    if (!m_PanelUiSmokeContext)
    {
        m_PanelUiSmokeContext = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        m_PanelVisible.fill(true);
        m_SelectedEntity = m_PrototypeMeshEntity;

        // Persistence wiring against a unique temporary fixture (never output/editor): save
        // writes the canonical text, load restores it, a corrupt file shows everything and
        // is left untouched.
        const std::filesystem::path fixture = std::filesystem::temp_directory_path()
            / ("spiral-panel-visibility-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ignored;
        std::filesystem::create_directories(fixture, ignored);
        const std::string savedSettingsPath = m_EditorSettingsPath;
        const std::string savedVisibilityPath = m_PanelVisibilityPath;
        m_EditorSettingsPath = (fixture / "engine-settings.spiralsettings").string();
        const auto readFile = [](const std::string& path)
        {
            std::ifstream input(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        };
        const auto allVisible = [this]()
        {
            return std::all_of(m_PanelVisible.begin(), m_PanelVisible.end(), [](bool visible) { return visible; });
        };
        LoadPanelVisibility();
        bool persistenceOk = allVisible() && !std::filesystem::exists(m_PanelVisibilityPath);
        m_PanelVisible[PanelConsole] = false;
        m_PanelVisible[PanelProfiler] = false;
        PersistPanelVisibilityIfChanged();
        const std::string expectedText =
            "SpiralEditorPanels 1\nPanel scene.hierarchy 1\nPanel scene.inspector 1\nPanel scene.viewport 1\n"
            "Panel content.browser 1\nPanel diagnostics.console 0\nPanel diagnostics.profiler 0\n";
        persistenceOk = persistenceOk && readFile(m_PanelVisibilityPath) == expectedText;
        m_PanelVisible.fill(true);
        LoadPanelVisibility();
        persistenceOk = persistenceOk && !m_PanelVisible[PanelConsole] && !m_PanelVisible[PanelProfiler]
            && m_PanelVisible[PanelSceneHierarchy] && m_PanelVisible[PanelInspector]
            && m_PanelVisible[PanelViewport] && m_PanelVisible[PanelContentBrowser];
        {
            std::ofstream corrupt(m_PanelVisibilityPath, std::ios::binary | std::ios::trunc);
            corrupt << "SpiralEditorPanels 1\nPanel scene.minimap 0\n";
        }
        const std::string corruptText = readFile(m_PanelVisibilityPath);
        m_PanelVisible[PanelConsole] = false;
        LoadPanelVisibility();
        persistenceOk = persistenceOk && allVisible() && readFile(m_PanelVisibilityPath) == corruptText;
        std::filesystem::remove_all(fixture, ignored);
        m_EditorSettingsPath = savedSettingsPath;
        m_PanelVisibilityPath = savedVisibilityPath;
        m_PanelVisible.fill(true);
        m_PanelVisiblePersisted = m_PanelVisible;
        if (!persistenceOk)
            fail("panel visibility save/load wiring");
    }

    // Frames 0-3 draw everything visible with the prototype selected; frames 4-9 hide one panel
    // each; frame 10 clears the selection (the Inspector must keep it empty); frame 11 filters the
    // selected row out of the hierarchy while a scroll request is pending; the last frames show all.
    const unsigned int frame = m_PanelUiSmokeFrames;
    m_PanelVisible.fill(true);
    if (frame >= 4 && frame < 4 + kPanelCount)
        m_PanelVisible[frame - 4] = false;
    if (frame == 10)
        m_SelectedEntity = Engine::Entity {};
    if (frame == 3)
        m_HierarchyScrollRequest = true;
    if (frame == 11)
    {
        m_SelectedEntity = m_PrototypeMeshEntity;
        m_HierarchyScrollRequest = true;
        std::snprintf(m_HierarchyFilter.data(), m_HierarchyFilter.size(), "no-entity-has-this-name");
    }
    if (frame == 12)
        m_HierarchyFilter.fill('\0');

    ImGui::NewFrame();
    DrawDockspace();
    DrawSceneHierarchyPanel();
    DrawInspectorPanel();
    DrawViewportPanel();
    DrawConsolePanel();
    DrawProfilerPanel();
    DrawProjectPanel();
    ImGui::Render();
    ++m_PanelUiSmokeFrames;

    // ImGui asserted on unbalanced stacks and duplicate IDs inside Render. Now the visible
    // state: every panel that was shown is an active window, the hidden one is not.
    static const char* const titles[kPanelCount] = { "Scene Hierarchy", "Inspector", "Viewport",
        "Content Browser", "Console", "Profiler" };
    for (size_t panel = 0; panel < kPanelCount; ++panel)
    {
        const ImGuiWindow* window = ImGui::FindWindowByName(titles[panel]);
        const bool active = window && window->Active;
        const bool shouldBeActive = m_PanelVisible[panel];
        if (active != shouldBeActive)
            fail(std::string(titles[panel]) + (shouldBeActive ? " did not draw" : " drew while hidden"));
    }
    if (frame == 3 && m_HierarchyScrollRequest)
        fail("the hierarchy did not consume its scroll request");
    if (frame == 10 && m_SelectedEntity)
        fail("the Inspector re-selected an entity after the selection was cleared");
    if (frame == 11 && m_HierarchyScrollRequest)
        fail("a filtered-out hierarchy row kept a scroll request alive");
    if (frame == 4 + PanelViewport && (m_ViewportImageValid || m_ViewportPickAvailable || m_ViewportHovered))
        fail("a hidden Viewport left picking state behind");
    if (frame == kFrames - 1 && !m_ViewportImageValid)
        fail("the visible Viewport did not publish its image rectangle");

    if (m_PanelUiSmokeFrames == kFrames)
    {
        ImGui::DestroyContext(m_PanelUiSmokeContext);
        m_PanelUiSmokeContext = nullptr;
        m_PanelVisible.fill(true);
        m_PanelVisiblePersisted = m_PanelVisible;
        Engine::Log::Info("PanelUiSmokeV1 frames=", kFrames,
            " panels=hierarchy,inspector,viewport,content,console,profiler hidden=each-once"
            " selection=cleared-stays-empty scrollRequest=consumed-or-dropped"
            " persistence=save-load-corrupt-fails-closed result=pass");
        Engine::Application::Get().Close();
    }
    return true;
}
