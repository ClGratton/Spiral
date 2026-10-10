#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Layer.h"

namespace Engine
{
    class UiViewportSmoke;

    class ImGuiLayer final : public Layer
    {
    public:
        ImGuiLayer();
        ~ImGuiLayer() override;

        void OnAttach() override;
        void OnDetach() override;

        void Begin();
        void End();

        void SetDarkThemeColors();

        // Detachable panel windows (Dear ImGui multi-viewport: a panel dragged out
        // of the main window becomes its own OS window). Off by default and only
        // switched on when requested here or with --ui-viewports AND the renderer
        // and platform report support; Renderer::GetUiViewportDiagnostics() says
        // whether it is on and why not. Read once in OnAttach, so call
        // SetViewportsRequested before the application attaches the layer; a
        // change afterwards takes effect at the next start.
        static void SetViewportsRequested(bool requested);
        static bool GetViewportsRequested();
        bool AreViewportsActive() const { return m_ViewportsActive; }

        // --ui-viewport-smoke bookkeeping for Application: the smoke keeps the
        // application alive until it has printed its verdict.
        bool IsViewportSmokePending() const;
        bool HasViewportSmokeFinished() const;

    private:
        void ConfigureViewports();

        bool m_UseNativeRenderer = false;
        bool m_ViewportsActive = false;
        Scope<UiViewportSmoke> m_ViewportSmoke;
    };
}
