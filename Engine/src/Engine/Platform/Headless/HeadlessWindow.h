#pragma once

#include "Engine/Core/Window.h"

namespace Engine
{
    class HeadlessWindow final : public Window
    {
    public:
        explicit HeadlessWindow(WindowSpecification specification);
        ~HeadlessWindow() override;

        void PollEvents() override;
        u32 GetWidth() const override { return m_Specification.Width; }
        u32 GetHeight() const override { return m_Specification.Height; }
        const std::string& GetTitle() const override { return m_Specification.Title; }
        bool ShouldClose() const override { return m_ShouldClose; }
        void RequestClose() override { m_ShouldClose = true; }
        void SetSize(u32 width, u32 height) override { m_Specification.Width = width; m_Specification.Height = height; }
        void SwapBuffers() override {}
        void SetCursorMode(CursorMode mode) override { (void)mode; }
        void GetCursorPosition(double& outX, double& outY) const override { outX = 0.0; outY = 0.0; }
        void SetCursorPosition(double x, double y) override { (void)x; (void)y; }
        void* GetNativeWindow() const override { return nullptr; }
        void SetEventCallback(EventCallbackFn callback) override { m_EventCallback = std::move(callback); }

    private:
        WindowSpecification m_Specification;
        EventCallbackFn m_EventCallback;
        bool m_ShouldClose = false;
    };
}
