#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Events/Event.h"

#include <functional>
#include <string>

namespace Engine
{
    enum class WindowGraphicsAPI
    {
        Default,
        OpenGL,
        None
    };

    struct WindowSpecification
    {
        std::string Title = "Spiral";
        u32 Width = 1280;
        u32 Height = 720;
        bool Headless = false;
        WindowGraphicsAPI GraphicsAPI = WindowGraphicsAPI::Default;
    };

    class Window
    {
    public:
        using EventCallbackFn = std::function<void(Event&)>;

        virtual ~Window() = default;

        virtual void OnUpdate() = 0;
        virtual u32 GetWidth() const = 0;
        virtual u32 GetHeight() const = 0;
        virtual const std::string& GetTitle() const = 0;
        virtual bool ShouldClose() const = 0;
        virtual void RequestClose() = 0;
        virtual void SetSize(u32 width, u32 height) = 0;
        virtual void SwapBuffers() = 0;
        virtual void* GetNativeWindow() const = 0;
        virtual void SetEventCallback(EventCallbackFn callback) = 0;

        static Scope<Window> Create(const WindowSpecification& specification);
    };
}
