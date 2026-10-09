#include "BrowserPanel.h"

#include "BrowserHostLoader.h"
#include "BrowserPanelCore.h"
#include "Engine/Core/Log.h"
#include "Engine/Renderer/Renderer.h"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <utility>

// Input precedence between the page and the Editor (the router in
// BrowserInputRouter.cpp is the single decision point; this file only latches
// ImGui state into it once per frame and forwards engine events through it):
//
//  1. ImGui always receives every raw GLFW event through its chained callbacks;
//     nothing here can or does stop that. "Taking" an event means that
//     OnEvent() returns true and sets Event::Handled, so the Editor's own
//     handlers (viewport navigation, shortcuts, file drop) never see it.
//  2. Mouse: while the pointer is over the page surface (ImGui item hover, with no
//     popup, modal, or drag-and-drop payload active), and from a press inside the
//     surface until every button is released, move/button/wheel go to the page
//     and are Handled. A gesture that starts elsewhere stays with the Editor.
//  3. Keyboard: the page owns the keyboard only after a click on its surface while
//     the OS window, this ImGui window, and no other text widget have focus.
//     While it owns it, keys and typed characters go to the page and are Handled,
//     WantsKeyboard() is true (the Editor must skip Ctrl+Z, Ctrl+Y, F, ...), and
//     ImGui is told to capture the keyboard. Escape is an ordinary page key.
//     Clicking any other ImGui window, typing in another widget, the toolbar
//     button, closing/collapsing/hiding the panel, or losing OS focus ends
//     ownership; every key that went down to the page is released first.
//  4. Otherwise nothing is Handled and the Editor behaves as without the panel.
//
// The window is NoNavInputs so ImGui keyboard navigation cannot grab Tab, the
// arrows, Enter, or Escape while the page is focused. Page-requested cursors are
// applied with ImGui::SetMouseCursor while the pointer is over (or captured by)
// the surface. ImGui units are treated as window pixels, which is true while the
// Editor does not scale its UI; the page device scale comes from the window
// content-scale event.
namespace Fab
{
    namespace
    {
        // Production seam: the Renderer's UI-texture statics.
        class RendererUiTextures final : public IBrowserUiTextures
        {
        public:
            Engine::UiTextureHandle Create(Engine::u32 width, Engine::u32 height, std::string_view debugName) override
            {
                return Engine::Renderer::CreateUiTexture(width, height, debugName);
            }

            bool Update(Engine::UiTextureHandle handle, const Engine::UiTextureUpdate& update) override
            {
                return Engine::Renderer::UpdateUiTexture(handle, update);
            }

            bool Destroy(Engine::UiTextureHandle handle) override { return Engine::Renderer::DestroyUiTexture(handle); }

            Engine::u64 GetImGuiId(Engine::UiTextureHandle handle) override
            {
                return Engine::Renderer::GetUiTextureImGuiId(handle);
            }

            Engine::UiTextureError LastError() override { return Engine::Renderer::GetLastUiTextureError(); }
        };

        BrowserPanelEnvironment MakeEnvironment(IBrowserUiTextures& textures)
        {
            BrowserPanelEnvironment environment;
            environment.Textures = &textures;
            environment.LoadSurface = [](const BrowserPanelConfig& config)
            {
                return LoadBrowserHostSurface(config.EditorDirectory);
            };
            environment.LogInfo = [](std::string_view message) { Engine::Log::Info("[Fab] ", message); };
            environment.LogWarn = [](std::string_view message) { Engine::Log::Warn("[Fab] ", message); };
            return environment;
        }

        ImGuiMouseCursor ToImGuiCursor(BrowserCursor cursor)
        {
            switch (cursor)
            {
            case BrowserCursor::Arrow: return ImGuiMouseCursor_Arrow;
            case BrowserCursor::IBeam: return ImGuiMouseCursor_TextInput;
            case BrowserCursor::Hand: return ImGuiMouseCursor_Hand;
            case BrowserCursor::ResizeEW: return ImGuiMouseCursor_ResizeEW;
            case BrowserCursor::ResizeNS: return ImGuiMouseCursor_ResizeNS;
            case BrowserCursor::ResizeAll: return ImGuiMouseCursor_ResizeAll;
            case BrowserCursor::NotAllowed: return ImGuiMouseCursor_NotAllowed;
            case BrowserCursor::Wait: return ImGuiMouseCursor_Wait;
            case BrowserCursor::Hidden: return ImGuiMouseCursor_None;
            }
            return ImGuiMouseCursor_Arrow;
        }

        constexpr const char* kSignOutPopup = "Sign out of Fab";
        constexpr size_t kMaximumToolbarHostCharacters = 48;

        float ButtonWidth(const char* label)
        {
            return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        }

        void DrawToolbar(BrowserPanelCore& core)
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            const float rowStart = ImGui::GetCursorStartPos().x;
            const float rowWidth = ImGui::GetContentRegionAvail().x;

            ImGui::BeginDisabled(!core.CanGoBack());
            if (ImGui::Button("Back"))
                core.GoBack();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!core.CanGoForward());
            if (ImGui::Button("Forward"))
                core.GoForward();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!core.Live());
            if (ImGui::Button("Reload"))
                core.Reload();
            ImGui::SameLine();
            if (ImGui::Button("Home"))
                core.GoHome();
            ImGui::EndDisabled();

            // The host only: the path of a Fab page can carry an identifier the user did not choose to display.
            ImGui::SameLine();
            std::string host = core.DisplayHost();
            if (host.size() > kMaximumToolbarHostCharacters)
                host = host.substr(0, kMaximumToolbarHostCharacters - 3) + "...";
            if (core.IsLoading())
                host += "  (loading)";
            ImGui::TextDisabled("%s", host.empty() ? "-" : host.c_str());

            const bool ownsKeyboard = core.WantsKeyboard();
            const float releaseWidth = ownsKeyboard ? ButtonWidth("Release keyboard") + style.ItemSpacing.x : 0.0f;
            const float groupWidth = releaseWidth + ButtonWidth("Sign out");
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() - rowStart + style.ItemSpacing.x, rowWidth - groupWidth));
            if (ownsKeyboard)
            {
                if (ImGui::Button("Release keyboard"))
                    core.ReleaseKeyboard();
                ImGui::SameLine();
            }
            ImGui::BeginDisabled(!core.Live());
            if (ImGui::Button("Sign out"))
                ImGui::OpenPopup(kSignOutPopup);
            ImGui::EndDisabled();
        }

        void DrawSignOutPopup(BrowserPanelCore& core)
        {
            if (!ImGui::BeginPopupModal(kSignOutPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                return;
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
            ImGui::TextUnformatted("This deletes the saved Fab and Epic sign-in from this computer and closes the browser. "
                                   "Restart the Editor to sign in again.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button("Sign out and close the browser"))
            {
                core.ClearBrowsingData();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        void DrawSurface(BrowserPanelCore& core, const ImVec2& origin, const ImVec2& size)
        {
            const BrowserTextureUploader& uploader = core.Uploader();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 limit(origin.x + size.x, origin.y + size.y);
            const bool hasPicture = uploader.HasDisplayTexture();
            // The page background is opaque white before its first paint and around a frame
            // that is smaller than the region while a resize is settling.
            drawList->AddRectFilled(origin, limit, hasPicture ? IM_COL32(255, 255, 255, 255) : IM_COL32(24, 26, 30, 255));
            if (hasPicture)
            {
                // Pixel-exact, top-left anchored: no stretching while the view size settles.
                const float frameWidth = static_cast<float>(uploader.DisplayWidth());
                const float frameHeight = static_cast<float>(uploader.DisplayHeight());
                const float shownWidth = std::min(frameWidth, size.x);
                const float shownHeight = std::min(frameHeight, size.y);
                ImGui::SetCursorScreenPos(origin);
                ImGui::Image(static_cast<ImTextureID>(uploader.DisplayId()), ImVec2(shownWidth, shownHeight), ImVec2(0.0f, 0.0f),
                    ImVec2(shownWidth / frameWidth, shownHeight / frameHeight));
            }
            else
            {
                ImGui::SetCursorScreenPos(ImVec2(origin.x + 12.0f, origin.y + 12.0f));
                ImGui::PushTextWrapPos(limit.x - 12.0f);
                ImGui::TextUnformatted(core.StatusLine().c_str());
                ImGui::PopTextWrapPos();
            }

            ImGui::SetCursorScreenPos(origin);
            ImGui::InvisibleButton("##FabSurface", size,
                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)
                || ImGui::IsItemClicked(ImGuiMouseButton_Middle))
            {
                ImGui::SetWindowFocus();
            }

            const ImGuiIO& io = ImGui::GetIO();
            BrowserPanelLayout layout;
            layout.Surface = { origin.x, origin.y, size.x, size.y };
            layout.PanelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            layout.SurfaceHovered = hovered;
            layout.OtherTextInputActive = io.WantTextInput;
            layout.DragDropPayloadActive = ImGui::GetDragDropPayload() != nullptr;
            layout.ModalOrPopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            layout.Modifiers = (io.KeyShift ? BrowserModifier::Shift : 0u) | (io.KeyCtrl ? BrowserModifier::Control : 0u)
                | (io.KeyAlt ? BrowserModifier::Alt : 0u) | (io.KeySuper ? BrowserModifier::Super : 0u);
            core.UpdateLayout(layout);

            if ((hovered || core.Router().HasMouseCapture()) && !layout.ModalOrPopupOpen)
                ImGui::SetMouseCursor(ToImGuiCursor(core.Cursor()));
            if (core.WantsKeyboard())
                ImGui::SetNextFrameWantCaptureKeyboard(true);
        }

        void DrawContents(BrowserPanelCore& core)
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            DrawToolbar(core);

            const float statusHeight = ImGui::GetTextLineHeightWithSpacing();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const ImVec2 size(std::max(available.x, 0.0f), std::max(available.y - statusHeight - style.ItemSpacing.y, 0.0f));
            const ImVec2 origin = ImGui::GetCursorScreenPos();

            if (core.Live())
            {
                DrawSurface(core, origin, size);
            }
            else
            {
                // Failed, closing, or closed: the status text is the whole panel.
                ImGui::PushTextWrapPos(origin.x + size.x);
                ImGui::TextUnformatted(core.StatusLine().c_str());
                ImGui::PopTextWrapPos();
            }

            if (core.Live())
            {
                ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + size.y + style.ItemSpacing.y));
                ImGui::TextDisabled("%s", core.StatusLine().c_str());
            }
            DrawSignOutPopup(core);
        }
    }

    struct BrowserPanel::Impl
    {
        // Declared before Core: the core releases its textures while it is destroyed.
        RendererUiTextures Textures;
        BrowserPanelCore Core;

        Impl() : Core(MakeEnvironment(Textures)) {}
    };

    BrowserPanel::BrowserPanel() : m_Impl(std::make_unique<Impl>()) {}
    BrowserPanel::~BrowserPanel() = default;

    void BrowserPanel::Configure(BrowserPanelConfig config)
    {
        m_Impl->Core.Configure(std::move(config));
    }

    void BrowserPanel::SetVisible(bool visible)
    {
        m_Impl->Core.SetVisible(visible);
    }

    bool BrowserPanel::IsVisible() const
    {
        return m_Impl->Core.IsVisible();
    }

    void BrowserPanel::Pump()
    {
        m_Impl->Core.Pump();
    }

    void BrowserPanel::Draw()
    {
        BrowserPanelCore& core = m_Impl->Core;
        if (!core.IsVisible() || ImGui::GetCurrentContext() == nullptr)
            return;

        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(960.0f, 640.0f), ImGuiCond_FirstUseEver);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (ImGui::Begin("Fab", &open, flags))
            DrawContents(core);
        else
            core.NotifyContentHidden();
        ImGui::End();
        if (!open)
            core.SetVisible(false);
    }

    bool BrowserPanel::OnEvent(Engine::Event& event)
    {
        return m_Impl->Core.OnEvent(event);
    }

    bool BrowserPanel::WantsKeyboard() const
    {
        return m_Impl->Core.WantsKeyboard();
    }

    void BrowserPanel::ClearBrowsingData()
    {
        m_Impl->Core.ClearBrowsingData();
    }

    bool BrowserPanel::TryTakeCompletedDownload(BrowserPanelDownload& outDownload)
    {
        return m_Impl->Core.TryTakeCompletedDownload(outDownload);
    }

    void BrowserPanel::Shutdown()
    {
        m_Impl->Core.Shutdown();
    }

    const std::string& BrowserPanel::StatusLine() const
    {
        return m_Impl->Core.StatusLine();
    }

    BrowserPanelDiagnostics BrowserPanel::GetDiagnostics() const
    {
        return m_Impl->Core.GetDiagnostics();
    }
}
