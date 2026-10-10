#include "BrowserPanel.h"

#include "BrowserHostLoader.h"
#include "BrowserPanelCore.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/ExternalUrl.h"
#include "Engine/Renderer/Renderer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
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

        // Beside the profile directory, like the sign-in host list, so signing out
        // (which deletes the profile) does not discard the dismissal.
        std::filesystem::path NoticeDismissalPath(const BrowserPanelConfig& config)
        {
            return FabNoticeDismissalPath(config.ProfileDirectory, config.NoticeDismissalFile);
        }

        BrowserPanelEnvironment MakeEnvironment(IBrowserUiTextures& textures, const BrowserExternalOpener& opener)
        {
            BrowserPanelEnvironment environment;
            environment.Textures = &textures;
            environment.LoadNoticeDismissal = [](const BrowserPanelConfig& config, std::string& error)
            {
                const std::filesystem::path path = NoticeDismissalPath(config);
                if (path.empty())
                    return FabNoticeDismissalStatus::Missing;
                return LoadFabNoticeDismissalFile(path, FabDisclosureLimits::kNoticeVersion, error);
            };
            environment.SaveNoticeDismissal = [](const BrowserPanelConfig& config, std::string& error)
            {
                const std::filesystem::path path = NoticeDismissalPath(config);
                if (path.empty())
                {
                    error = "there is no place to save it";
                    return false;
                }
                return SaveFabNoticeDismissalFile(path, FabDisclosureLimits::kNoticeVersion, error);
            };
            environment.OpenExternalUrl = [&opener](std::string_view url, std::string_view host, std::string& error)
            {
                return opener(url, host, error);
            };
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
        constexpr const char* kSignInHostsPopup = "Sign-in hosts";
        constexpr const char* kOpenInBrowserLabel = "Open in browser";

        // The monitor with the largest overlap with the Editor window, as the
        // platform backend reports them. Everything is window (screen) pixels. When
        // the backend does not report the window position (no multi-viewport
        // support) the window counts as sitting at the origin, which picks the
        // monitor that contains it: exact with one monitor, a best guess with several.
        BrowserScreenInfo ReadScreenInfo()
        {
            BrowserScreenInfo info;
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const ImVector<ImGuiPlatformMonitor>& monitors = ImGui::GetPlatformIO().Monitors;
            if (viewport == nullptr || monitors.Size == 0 || viewport->Size.x < 1.0f || viewport->Size.y < 1.0f)
                return info;

            const ImGuiPlatformMonitor* best = nullptr;
            float bestOverlap = -1.0f;
            for (const ImGuiPlatformMonitor& monitor : monitors)
            {
                if (monitor.MainSize.x < 1.0f || monitor.MainSize.y < 1.0f)
                    continue;
                const float width = std::min(viewport->Pos.x + viewport->Size.x, monitor.MainPos.x + monitor.MainSize.x)
                    - std::max(viewport->Pos.x, monitor.MainPos.x);
                const float height = std::min(viewport->Pos.y + viewport->Size.y, monitor.MainPos.y + monitor.MainSize.y)
                    - std::max(viewport->Pos.y, monitor.MainPos.y);
                const float overlap = std::max(width, 0.0f) * std::max(height, 0.0f);
                if (overlap > bestOverlap)
                {
                    bestOverlap = overlap;
                    best = &monitor;
                }
            }
            if (best == nullptr)
                return info;

            const auto round = [](float value) { return static_cast<int>(std::lround(value)); };
            info.Valid = true;
            info.MonitorX = round(best->MainPos.x);
            info.MonitorY = round(best->MainPos.y);
            info.MonitorWidth = round(best->MainSize.x);
            info.MonitorHeight = round(best->MainSize.y);
            const bool hasWorkArea = best->WorkSize.x >= 1.0f && best->WorkSize.y >= 1.0f;
            info.WorkX = round(hasWorkArea ? best->WorkPos.x : best->MainPos.x);
            info.WorkY = round(hasWorkArea ? best->WorkPos.y : best->MainPos.y);
            info.WorkWidth = round(hasWorkArea ? best->WorkSize.x : best->MainSize.x);
            info.WorkHeight = round(hasWorkArea ? best->WorkSize.y : best->MainSize.y);
            return info;
        }

        float ButtonWidth(const char* label)
        {
            return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        }

        // What the last drawn frame showed, for the headless smoke (it cannot read widget state).
        struct DrawnFrame
        {
            bool OpenInBrowserEnabled = false;
            bool NoticeLine = false;
            bool Hint = false;
            bool LockGlyph = false;
            std::string AddressText;
        };

        // Keeps the scheme prefix and the END of the host, which is the part that
        // names the registrable domain; the head is what gets elided.
        std::string ElideHost(const std::string& prefix, const std::string& host, float maximumWidth)
        {
            if (ImGui::CalcTextSize((prefix + host).c_str()).x <= maximumWidth)
                return prefix + host;
            std::string tail = host;
            while (tail.size() > 4 && ImGui::CalcTextSize((prefix + "..." + tail).c_str()).x > maximumWidth)
                tail.erase(0, 1);
            return prefix + "..." + tail;
        }

        // A small padlock drawn with primitives, so it does not depend on a font glyph.
        void DrawLock(float height)
        {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float width = height * 0.62f;
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
            const float bodyTop = origin.y + height * 0.42f;
            const float bodyBottom = origin.y + height * 0.92f;
            drawList->AddRect(ImVec2(origin.x + width * 0.2f, origin.y + height * 0.08f), ImVec2(origin.x + width * 0.8f, bodyTop + 1.0f),
                color, width * 0.3f, ImDrawFlags_RoundCornersTop, 1.5f);
            drawList->AddRectFilled(ImVec2(origin.x, bodyTop), ImVec2(origin.x + width, bodyBottom), color, 1.5f);
            ImGui::Dummy(ImVec2(width, height));
        }

        void DrawToolbar(BrowserPanelCore& core, DrawnFrame& drawn)
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

            const bool ownsKeyboard = core.WantsKeyboard();
            const float releaseWidth = ownsKeyboard ? ButtonWidth("Release keyboard") + style.ItemSpacing.x : 0.0f;
            const float groupWidth = releaseWidth + ButtonWidth(kOpenInBrowserLabel) + style.ItemSpacing.x
                + ButtonWidth(kSignInHostsPopup) + style.ItemSpacing.x + ButtonWidth("Sign out");

            // The real scheme and host (never the path, which can carry an identifier
            // the user did not choose to display), so the user can see whose page it is
            // before typing credentials.
            ImGui::SameLine();
            const std::string host = core.DisplayHost();
            const std::string scheme = core.DisplayScheme();
            const bool secure = scheme == "https" && !host.empty();
            std::string text;
            if (host.empty())
            {
                text = "-";
            }
            else
            {
                const float used = ImGui::GetCursorPosX() - rowStart;
                const float lockWidth = secure ? ImGui::GetTextLineHeight() * 0.62f + style.ItemSpacing.x : 0.0f;
                const float room = std::max(rowWidth - groupWidth - used - style.ItemSpacing.x * 2.0f - lockWidth, 40.0f);
                const std::string prefix = scheme.empty() ? std::string() : scheme + "://";
                text = ElideHost(secure ? prefix : (scheme.empty() ? std::string() : "not secure: " + prefix), host, room);
            }
            if (secure)
            {
                DrawLock(ImGui::GetTextLineHeight());
                ImGui::SameLine();
                ImGui::TextUnformatted(text.c_str());
            }
            else if (!host.empty())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", text.c_str());
            }
            else
            {
                ImGui::TextDisabled("%s", text.c_str());
            }
            if (core.IsLoading())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(loading)");
            }
            drawn.LockGlyph = secure;
            drawn.AddressText = text;

            ImGui::SameLine(std::max(ImGui::GetCursorPosX() - rowStart + style.ItemSpacing.x, rowWidth - groupWidth));
            if (ownsKeyboard)
            {
                if (ImGui::Button("Release keyboard"))
                    core.ReleaseKeyboard();
                ImGui::SameLine();
            }
            // Always available, whatever the browser's state: the page's current https
            // address when the navigation policy allows it, else the Fab home page.
            drawn.OpenInBrowserEnabled = true;
            ImGui::BeginDisabled(!drawn.OpenInBrowserEnabled);
            if (ImGui::Button(kOpenInBrowserLabel))
                core.OpenInBrowser();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("Opens this page, or the Fab home page, in your system browser.");
            ImGui::SameLine();
            ImGui::BeginDisabled(!core.Live());
            if (ImGui::Button(kSignInHostsPopup))
                ImGui::OpenPopup(kSignInHostsPopup);
            ImGui::SameLine();
            if (ImGui::Button("Sign out"))
                ImGui::OpenPopup(kSignOutPopup);
            ImGui::EndDisabled();
        }

        // Shown under the toolbar after a load error, a denied navigation, or a page that
        // ended with HTTP 403 or 503. It only points at the system browser: Spiral never
        // detects, solves, or scripts a security check.
        void DrawPageHint(BrowserPanelCore& core, DrawnFrame& drawn)
        {
            drawn.Hint = core.PageHint() != FabPageHint::None;
            if (!drawn.Hint)
                return;
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%.*s", static_cast<int>(kFabPageHintText.size()), kFabPageHintText.data());
            ImGui::PopTextWrapPos();
        }

        // The one-line notice above the page. It never replaces the page or delays the
        // browser; Dismiss hides it and saves the dismissal.
        void DrawNoticeLine(BrowserPanelCore& core, DrawnFrame& drawn)
        {
            drawn.NoticeLine = true;
            const ImGuiStyle& style = ImGui::GetStyle();
            const float dismissWidth = ButtonWidth("Dismiss");
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - dismissWidth - style.ItemSpacing.x * 2.0f, 80.0f));
            ImGui::TextDisabled("%.*s", static_cast<int>(kFabNoticeText.size()), kFabNoticeText.data());
            ImGui::PopTextWrapPos();
            ImGui::SameLine();
            if (ImGui::Button("Dismiss"))
                core.DismissNotice();
        }

        // Shown above the page while a denied navigation waits for the user's
        // decision. Host only; the target address never reaches the panel.
        void DrawConsentBanner(BrowserPanelCore& core)
        {
            const std::string host = core.PendingConsentHost();
            if (host.empty())
                return;
            ImGui::Separator();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("The page tried to open %s. Allow for sign-in?", host.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Allow once"))
                core.ResolveConsent(SignInConsentChoice::AllowOnce);
            ImGui::SameLine();
            if (ImGui::Button("Allow always"))
                core.ResolveConsent(SignInConsentChoice::AllowAlways);
            ImGui::SameLine();
            if (ImGui::Button("Dismiss"))
                core.ResolveConsent(SignInConsentChoice::Dismiss);
            ImGui::SameLine();
            ImGui::TextDisabled("Once: until the Editor closes. Always: remembered.");
            ImGui::Separator();
        }

        void DrawSignInHostsPopup(BrowserPanelCore& core)
        {
            if (!ImGui::BeginPopup(kSignInHostsPopup))
                return;
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
            ImGui::TextUnformatted("Pages may open these hosts while you sign in. Anything else is blocked until you allow it.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();

            ImGui::SeparatorText("Fab and Epic");
            for (const std::string_view host : { BrowserNavigationPolicy::kFabHost, BrowserNavigationPolicy::kEpicHost })
                ImGui::TextUnformatted(host.data(), host.data() + host.size());

            ImGui::SeparatorText("Sign-in providers");
            for (const BrowserSignInProvider& provider : BrowserPanelCore::DefaultSignInProviders())
            {
                ImGui::Text("%.*s  %.*s", static_cast<int>(provider.Provider.size()), provider.Provider.data(),
                    static_cast<int>(provider.Host.size()), provider.Host.data());
            }

            ImGui::SeparatorText("Allowed by you");
            const std::vector<BrowserGrantedHost> granted(core.GrantedHosts().begin(), core.GrantedHosts().end());
            if (granted.empty())
                ImGui::TextDisabled("None");
            std::string removed;
            for (size_t index = 0; index < granted.size(); ++index)
            {
                ImGui::PushID(static_cast<int>(index));
                ImGui::Text("%s  (%s)", granted[index].Host.c_str(), granted[index].Persistent ? "remembered" : "this session");
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove"))
                    removed = granted[index].Host;
                ImGui::PopID();
            }
            if (!removed.empty())
                core.RevokeGrantedHost(removed);
            if (!granted.empty())
            {
                ImGui::Spacing();
                if (ImGui::Button("Remove all"))
                    core.ClearGrantedHosts();
            }
            if (!core.SignInHostsNotice().empty())
            {
                ImGui::Spacing();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
                ImGui::TextDisabled("%s", core.SignInHostsNotice().c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndPopup();
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
            layout.Screen = ReadScreenInfo();
            core.UpdateLayout(layout);

            if ((hovered || core.Router().HasMouseCapture()) && !layout.ModalOrPopupOpen)
                ImGui::SetMouseCursor(ToImGuiCursor(core.Cursor()));
            if (core.WantsKeyboard())
                ImGui::SetNextFrameWantCaptureKeyboard(true);
        }

        void DrawContents(BrowserPanelCore& core, DrawnFrame& drawn)
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            drawn = DrawnFrame {};
            DrawToolbar(core, drawn);
            if (core.NoticeVisible())
                DrawNoticeLine(core, drawn);
            DrawPageHint(core, drawn);
            if (core.Live())
                DrawConsentBanner(core);

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
            DrawSignInHostsPopup(core);
            DrawSignOutPopup(core);
        }
    }

    struct BrowserPanel::Impl
    {
        // Declared before Core: the core releases its textures while it is destroyed,
        // and its environment refers to the opener.
        RendererUiTextures Textures;
        BrowserExternalOpener Opener = [](std::string_view url, std::string_view host, std::string& error)
        {
            return Engine::OpenExternalHttpsUrl(url, host, error);
        };
        BrowserPanelCore Core;
        DrawnFrame LastDrawn;

        Impl() : Core(MakeEnvironment(Textures, Opener)) {}
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
            DrawContents(core, m_Impl->LastDrawn);
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

    bool BrowserPanel::IsDisabled() const
    {
        return m_Impl->Core.Disabled();
    }

    std::string BrowserPanel::DisabledText() const
    {
        return std::string(m_Impl->Core.DisabledText());
    }

    void BrowserPanel::SetDisabled(FabBrowserDisabledReason reason)
    {
        m_Impl->Core.SetDisabled(reason);
    }

    bool BrowserPanel::NoticeVisible() const
    {
        return m_Impl->Core.NoticeVisible();
    }

    void BrowserPanel::DismissNotice()
    {
        m_Impl->Core.DismissNotice();
    }

    bool BrowserPanel::OpenInBrowser()
    {
        return m_Impl->Core.OpenInBrowser();
    }

    void BrowserPanel::SetExternalOpener(BrowserExternalOpener opener)
    {
        if (opener)
            m_Impl->Opener = std::move(opener);
    }

    BrowserPanelDrawRecord BrowserPanel::LastDrawRecord() const
    {
        const DrawnFrame& drawn = m_Impl->LastDrawn;
        return { drawn.OpenInBrowserEnabled, drawn.NoticeLine, drawn.Hint, drawn.LockGlyph, drawn.AddressText };
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
