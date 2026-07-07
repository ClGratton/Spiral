#include "Engine/UI/ImGuiLayer.h"

#include "Engine/Core/Application.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Window.h"
#include "Engine/Renderer/Renderer.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl2.h>

namespace Engine
{
    ImGuiLayer::ImGuiLayer()
        : Layer("ImGuiLayer")
    {
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

        Window& window = Application::Get().GetWindow();
        GLFWwindow* nativeWindow = static_cast<GLFWwindow*>(window.GetNativeWindow());
        GE_CORE_ASSERT(nativeWindow, "ImGuiLayer requires a native GLFW window");

        m_UseNativeRenderer = Renderer::InitializeImGui(nativeWindow);
        if (m_UseNativeRenderer)
        {
            ImGui_ImplGlfw_InitForOther(nativeWindow, true);
        }
        else
        {
            ImGui_ImplGlfw_InitForOpenGL(nativeWindow, true);
            ImGui_ImplOpenGL2_Init();
        }
    }

    void ImGuiLayer::OnDetach()
    {
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
        }
        else
        {
            ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
            window.SwapBuffers();
        }
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
