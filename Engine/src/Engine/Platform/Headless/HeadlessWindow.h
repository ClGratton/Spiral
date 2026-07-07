#pragma once

#include "Engine/Core/Window.h"

namespace Engine
{
    class HeadlessWindow final : public Window
    {
    public:
        explicit HeadlessWindow(WindowSpecification specification);
        ~HeadlessWindow() override;

        void OnUpdate() override;
        u32 GetWidth() const override { return m_Specification.Width; }
        u32 GetHeight() const override { return m_Specification.Height; }
        const std::string& GetTitle() const override { return m_Specification.Title; }
        bool ShouldClose() const override { return m_ShouldClose; }
        void RequestClose() override { m_ShouldClose = true; }
        void SwapBuffers() override {}
        void* GetNativeWindow() const override { return nullptr; }
        void SetEventCallback(EventCallbackFn callback) override { m_EventCallback = std::move(callback); }

    private:
        WindowSpecification m_Specification;
        EventCallbackFn m_EventCallback;
        bool m_ShouldClose = false;
    };
}
