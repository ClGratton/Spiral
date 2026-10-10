#pragma once

#include "Engine/Core/Window.h"

#include <utility>
#include <vector>

namespace Engine
{
    class HeadlessWindow final : public Window
    {
    public:
        explicit HeadlessWindow(WindowSpecification specification);
        ~HeadlessWindow() override;

        void PollEvents() override;
        void WaitEvents(double timeoutSeconds) override;
        // Queues a size change that the next PollEvents/WaitEvents delivers through
        // the event callback, the way GLFW's size callback fires inside
        // glfwPollEvents. Lets a deterministic test place a resize (including the
        // 0x0 of a minimize) exactly at the poll inside an application iteration.
        void QueueResize(u32 width, u32 height) { m_PendingResizes.emplace_back(width, height); }
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
        WindowContentScale GetContentScale() const override { return {}; }
        void* GetNativeWindow() const override { return nullptr; }
        void SetEventCallback(EventCallbackFn callback) override { m_EventCallback = std::move(callback); }

    private:
        WindowSpecification m_Specification;
        EventCallbackFn m_EventCallback;
        std::vector<std::pair<u32, u32>> m_PendingResizes;
        bool m_ShouldClose = false;
    };
}
