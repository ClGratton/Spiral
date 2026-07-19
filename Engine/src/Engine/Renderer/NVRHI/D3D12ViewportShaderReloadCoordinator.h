#pragma once

#include "Engine/Core/Base.h"

namespace Engine
{
    // D3D12 viewport-only publication gate. It deliberately owns no compiler,
    // filesystem watcher, or backend abstraction: its single consumer is the
    // viewport PSO replaced after portable shader package completion.
    class D3D12ViewportShaderReloadCoordinator final
    {
    public:
        struct Ticket { u64 Epoch = 0; u64 Revision = 0; u64 Request = 0; bool IsValid() const { return Request != 0; } };

        Ticket Request(u64 revision)
        {
            if (revision == m_LastRequestedRevision) return {};
            m_LastRequestedRevision = revision;
            m_CurrentPublished = false;
            return m_Current = { m_Epoch, revision, ++m_NextRequest };
        }
        bool IsCurrent(const Ticket& ticket) const { return ticket.IsValid() && ticket.Epoch == m_Epoch && ticket.Request == m_Current.Request; }
        bool Publish(const Ticket& ticket, bool succeeded)
        {
            if (!IsCurrent(ticket) || m_CurrentPublished) return false;
            m_CurrentPublished = true;
            if (succeeded) ++m_ActiveGeneration;
            return succeeded;
        }
        void Invalidate() { ++m_Epoch; m_Current = {}; m_CurrentPublished = false; m_LastRequestedRevision = 0; }
        u64 ActiveGeneration() const { return m_ActiveGeneration; }

    private:
        u64 m_Epoch = 1, m_NextRequest = 0, m_LastRequestedRevision = 0, m_ActiveGeneration = 0;
        bool m_CurrentPublished = false;
        Ticket m_Current;
    };
}
