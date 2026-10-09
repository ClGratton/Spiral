#include "BrowserPanel.h"

#include <imgui.h>

#include <utility>

namespace Fab
{
    struct BrowserPanel::Impl
    {
        BrowserPanelConfig Config;
        bool Visible = false;
        std::string Status = "Fab browser panel is not built yet.";
    };

    BrowserPanel::BrowserPanel() : m_Impl(std::make_unique<Impl>()) {}
    BrowserPanel::~BrowserPanel() = default;

    void BrowserPanel::Configure(BrowserPanelConfig config)
    {
        m_Impl->Config = std::move(config);
    }

    void BrowserPanel::SetVisible(bool visible)
    {
        m_Impl->Visible = visible;
    }

    bool BrowserPanel::IsVisible() const
    {
        return m_Impl->Visible;
    }

    void BrowserPanel::Pump() {}

    void BrowserPanel::Draw()
    {
        if (!m_Impl->Visible)
            return;
        if (ImGui::Begin("Fab", &m_Impl->Visible))
            ImGui::TextUnformatted(m_Impl->Status.c_str());
        ImGui::End();
    }

    bool BrowserPanel::OnEvent(Engine::Event&)
    {
        return false;
    }

    bool BrowserPanel::WantsKeyboard() const
    {
        return false;
    }

    void BrowserPanel::ClearBrowsingData() {}

    bool BrowserPanel::TryTakeCompletedDownload(BrowserPanelDownload&)
    {
        return false;
    }

    void BrowserPanel::Shutdown() {}

    const std::string& BrowserPanel::StatusLine() const
    {
        return m_Impl->Status;
    }

    BrowserPanelDiagnostics BrowserPanel::GetDiagnostics() const
    {
        BrowserPanelDiagnostics diagnostics;
        diagnostics.State = "NotStarted";
        diagnostics.Visible = m_Impl->Visible;
        return diagnostics;
    }
}
