#include "Engine/UI/ImGuiLayer.h"

#include "Engine/Core/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Window.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/UiViewportPolicy.h"
#include "Engine/UI/UiViewportSmoke.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl2.h>

#include <atomic>
#include <stdexcept>
#include <string>

namespace Engine
{
    namespace
    {
        std::atomic<bool> s_ViewportsRequested { false };

        UiViewportPlatformKind ToPlatformKind(int glfwPlatform)
        {
            switch (glfwPlatform)
            {
                case GLFW_PLATFORM_WIN32: return UiViewportPlatformKind::Win32;
                case GLFW_PLATFORM_X11: return UiViewportPlatformKind::X11;
                case GLFW_PLATFORM_WAYLAND: return UiViewportPlatformKind::Wayland;
                case GLFW_PLATFORM_COCOA: return UiViewportPlatformKind::Cocoa;
                case GLFW_PLATFORM_NULL: return UiViewportPlatformKind::Null;
                default: return UiViewportPlatformKind::Unknown;
            }
        }

        std::string JoinModes(const std::vector<int>& modes)
        {
            std::string text;
            for (int mode : modes)
                text += (text.empty() ? "" : ",") + std::to_string(mode);
            return text.empty() ? "none" : text;
        }
    }

    ImGuiLayer::ImGuiLayer()
        : Layer("ImGuiLayer")
    {
    }

    ImGuiLayer::~ImGuiLayer() = default;

    void ImGuiLayer::SetViewportsRequested(bool requested)
    {
        s_ViewportsRequested.store(requested, std::memory_order_relaxed);
    }

    bool ImGuiLayer::GetViewportsRequested()
    {
        return s_ViewportsRequested.load(std::memory_order_relaxed);
    }

    bool ImGuiLayer::IsViewportSmokePending() const
    {
        return m_ViewportSmoke && m_ViewportSmoke->IsPending();
    }

    bool ImGuiLayer::HasViewportSmokeFinished() const
    {
        return m_ViewportSmoke && m_ViewportSmoke->IsFinished();
    }

    // Decides, after both ImGui backends exist, whether detachable OS windows can
    // be enabled safely, publishes that decision (with its reason) to the renderer
    // and only then sets ImGuiConfigFlags_ViewportsEnable. The flag is never set
    // without a positive decision, so an unrequested or unsupported run behaves
    // exactly as before.
    void ImGuiLayer::ConfigureViewports()
    {
        ImGuiIO& io = ImGui::GetIO();
        const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;

        UiViewportCapabilityInput input;
        input.Requested = GetViewportsRequested() || args.HasFlag("--ui-viewports");
        input.Renderer = m_UseNativeRenderer ? Renderer::GetUiViewportRendererKind() : UiViewportRendererKind::None;
        input.Platform = ToPlatformKind(glfwGetPlatform());
        input.PlatformBackendHasViewports = (io.BackendFlags & ImGuiBackendFlags_PlatformHasViewports) != 0;
        input.RendererBackendHasViewports = (io.BackendFlags & ImGuiBackendFlags_RendererHasViewports) != 0;
        input.SurfacePresentModes = Renderer::GetUiViewportSurfacePresentModes();
        input.SurfaceModesKnown = !input.SurfacePresentModes.empty();

        UiViewportDecision decision = DecideUiViewports(input);
        Renderer::PublishUiViewportDecision(input.Requested, decision, input.Renderer, input.Platform);
        if (decision.Enabled)
        {
            if (Renderer::ActivateUiViewports())
            {
                io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
                m_ViewportsActive = true;
            }
            else
            {
                decision = { false, UiViewportReason::RendererHandlersUnavailable };
                Renderer::PublishUiViewportDecision(input.Requested, decision, input.Renderer, input.Platform);
            }
        }
        Log::Info("UiViewportsDecisionV1 requested=", input.Requested ? "yes" : "no", " enabled=", decision.Enabled ? "yes" : "no",
            " reason=", ToString(decision.Reason), " renderer=", ToString(input.Renderer), " platform=", ToString(input.Platform),
            " surfacePresentModes=", JoinModes(input.SurfacePresentModes));
    }

    void ImGuiLayer::OnAttach()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        io.Fonts->AddFontDefault();
        SetDarkThemeColors();

        try
        {
            Window& window = Application::Get().GetWindow();
            GLFWwindow* nativeWindow = static_cast<GLFWwindow*>(window.GetNativeWindow());
            if (!nativeWindow)
                throw std::runtime_error("ImGuiLayer requires a native GLFW window");

            m_UseNativeRenderer = Renderer::InitializeImGui(nativeWindow);
            if (m_UseNativeRenderer)
            {
                if (Renderer::GetActiveBackend() == RendererBackend::NVRHIVulkan)
                    ImGui_ImplGlfw_InitForVulkan(nativeWindow, true);
                else
                    ImGui_ImplGlfw_InitForOther(nativeWindow, true);
            }
            else
            {
                if (glfwGetWindowAttrib(nativeWindow, GLFW_CLIENT_API) == GLFW_NO_API)
                    throw std::runtime_error("Native renderer initialization failed for a GLFW_NO_API window");
                ImGui_ImplGlfw_InitForOpenGL(nativeWindow, true);
                ImGui_ImplOpenGL2_Init();
            }
        }
        catch (...)
        {
            // The layer stack discards a layer whose OnAttach threw without calling
            // OnDetach, so a failed attach releases its own context here.
            ImGui::DestroyContext();
            throw;
        }

        ConfigureViewports();
        if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--ui-viewport-smoke"))
            m_ViewportSmoke = CreateScope<UiViewportSmoke>();
    }

    void ImGuiLayer::OnDetach()
    {
        // The smoke's watchdog must be gone before the renderer and windows are.
        m_ViewportSmoke.reset();
        m_ViewportsActive = false;
        if (m_UseNativeRenderer)
            Renderer::ShutdownImGui();
        else
            ImGui_ImplOpenGL2_Shutdown();

        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        m_UseNativeRenderer = false;
    }

    void ImGuiLayer::Begin()
    {
        if (m_UseNativeRenderer)
            Renderer::BeginImGuiFrame();
        else
            ImGui_ImplOpenGL2_NewFrame();

        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (m_ViewportSmoke)
            m_ViewportSmoke->OnFrameBegin();
    }

    void ImGuiLayer::End()
    {
        ImGuiIO& io = ImGui::GetIO();
        Window& window = Application::Get().GetWindow();
        io.DisplaySize = ImVec2(static_cast<float>(window.GetWidth()), static_cast<float>(window.GetHeight()));

        ImGui::Render();
        if (m_UseNativeRenderer)
        {
            Renderer::RenderImGuiDrawData(ImGui::GetDrawData());
            // The main window has presented; detached windows follow. This is a
            // no-op unless ConfigureViewports enabled the feature.
            if (m_ViewportsActive)
                Renderer::RenderUiPlatformWindows();
        }
        else
        {
            ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
            window.SwapBuffers();
        }

        if (m_ViewportSmoke)
            m_ViewportSmoke->OnFrameEnd();
    }

    void ImGuiLayer::SetDarkThemeColors()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 3.0f;
        style.FrameRounding = 3.0f;
        style.PopupRounding = 3.0f;
        style.ScrollbarRounding = 3.0f;
        style.TabRounding = 3.0f;
        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.ItemSpacing = ImVec2(8.0f, 6.0f);
        style.WindowPadding = ImVec2(10.0f, 10.0f);

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.12f, 1.00f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.09f, 0.10f, 1.00f);
        colors[ImGuiCol_Border] = ImVec4(0.24f, 0.27f, 0.30f, 1.00f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.20f, 1.00f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.24f, 0.27f, 1.00f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.31f, 0.38f, 1.00f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.09f, 0.10f, 1.00f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.14f, 0.16f, 1.00f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.09f, 0.10f, 0.11f, 1.00f);
        colors[ImGuiCol_Header] = ImVec4(0.20f, 0.31f, 0.42f, 1.00f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.38f, 0.50f, 1.00f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.46f, 0.60f, 1.00f);
        colors[ImGuiCol_Button] = ImVec4(0.18f, 0.25f, 0.31f, 1.00f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.36f, 0.44f, 1.00f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.29f, 0.43f, 0.54f, 1.00f);
        colors[ImGuiCol_Tab] = ImVec4(0.13f, 0.16f, 0.19f, 1.00f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.25f, 0.38f, 0.50f, 1.00f);
        colors[ImGuiCol_TabActive] = ImVec4(0.20f, 0.29f, 0.37f, 1.00f);
        colors[ImGuiCol_DockingPreview] = ImVec4(0.27f, 0.52f, 0.70f, 0.70f);
    }
}
