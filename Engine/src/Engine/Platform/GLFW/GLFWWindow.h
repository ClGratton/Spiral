#pragma once

#include "Engine/Core/Window.h"

struct GLFWwindow;

namespace Engine
{
    class GLFWWindow final : public Window
    {
    public:
        explicit GLFWWindow(WindowSpecification specification);
        ~GLFWWindow() override;

        void PollEvents() override;
        u32 GetWidth() const override { return m_Data.Width; }
        u32 GetHeight() const override { return m_Data.Height; }
        const std::string& GetTitle() const override { return m_Data.Title; }
        bool ShouldClose() const override;
        void RequestClose() override;
        void SetSize(u32 width, u32 height) override;
        void SwapBuffers() override;
        void SetCursorMode(CursorMode mode) override;
        void GetCursorPosition(double& outX, double& outY) const override;
        void SetCursorPosition(double x, double y) override;
        void* GetNativeWindow() const override { return m_Window; }
        void SetEventCallback(EventCallbackFn callback) override { m_Data.EventCallback = std::move(callback); }

    private:
        void Initialize();
        void Shutdown();

    private:
        struct WindowData
        {
            std::string Title;
            u32 Width = 0;
            u32 Height = 0;
            EventCallbackFn EventCallback;
        };

        WindowSpecification m_Specification;
        WindowData m_Data;
        GLFWwindow* m_Window = nullptr;
        bool m_UsesOpenGLContext = false;
    };
}
