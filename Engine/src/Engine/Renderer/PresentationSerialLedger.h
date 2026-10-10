#pragma once

#include "Engine/Core/Base.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <unordered_map>

namespace Engine
{
    // Bookkeeping model behind the presentation serials that the UI-texture
    // service uses to decide when a retired texture can no longer be read by the
    // GPU (UiTextureNativeBridge::ReferenceBoundSerial / CompletedSerial).
    //
    // A serial numbers one ImGui frame, not one submission. The first GPU
    // submission of a frame (the main swapchain or, if that was skipped, a
    // detached-window swapchain) allocates the frame's serial; every further
    // submission of the same frame (secondary Dear ImGui viewports, rendered
    // after the main present) joins it. A frame serial N is complete when every
    // fence registered under it has signalled, so "completed serial" keeps its
    // documented meaning: every submission up to and including N has finished.
    //
    // Each submission is tracked by the fence it will signal, identified by a
    // (Owner, Slot) key: Owner 0 is the main swapchain, any other owner is a
    // detached viewport; Slot is the swapchain image index whose fence is used.
    // A key is only reused after its previous submission was waited on (the Dear
    // ImGui backends wait that fence before re-recording the image), so Track()
    // may replace the earlier entry. The ledger never touches a fence itself:
    // polling goes through a probe so the model can be tested without a GPU.
    class PresentationSerialLedger
    {
    public:
        struct Key
        {
            u32 Owner = 0;
            u32 Slot = 0;

            bool operator==(const Key& other) const { return Owner == other.Owner && Slot == other.Slot; }
        };

        // The probe says whether the fence behind a key is still unsignalled.
        // A fence that no longer exists (destroyed viewport, rebuilt swapchain)
        // must be reported Complete: destroying or rebuilding a swapchain waits
        // for the device first.
        enum class FenceState : u8
        {
            Pending,
            Complete
        };

        // Start of an ImGui frame: the next Track() allocates a new serial.
        void BeginFrame() { m_FrameSerial = 0; }

        // Registers a submission that will signal `key`'s fence and returns the
        // serial of the frame it belongs to.
        u64 Track(Key key)
        {
            if (m_FrameSerial == 0)
                m_FrameSerial = ++m_Submitted;
            m_InFlight[key] = m_FrameSerial;
            return m_FrameSerial;
        }

        void Forget(Key key) { m_InFlight.erase(key); }

        // The owner's swapchain was destroyed or rebuilt after a device wait.
        void ForgetOwner(u32 owner)
        {
            for (auto it = m_InFlight.begin(); it != m_InFlight.end();)
                it = it->first.Owner == owner ? m_InFlight.erase(it) : std::next(it);
        }

        // Drops every entry but keeps the serial counter monotonic.
        void Clear() { m_InFlight.clear(); }

        // Last serial issued.
        u64 Submitted() const { return m_Submitted; }
        bool FrameSerialAllocated() const { return m_FrameSerial != 0; }
        size_t InFlightCount() const { return m_InFlight.size(); }

        // Highest serial N such that every submission up to and including N has
        // completed. Entries whose fence completed are dropped.
        template<typename Probe>
        u64 PollCompleted(Probe&& probe)
        {
            u64 oldestPending = 0;
            for (auto it = m_InFlight.begin(); it != m_InFlight.end();)
            {
                if (probe(it->first) == FenceState::Complete)
                {
                    it = m_InFlight.erase(it);
                    continue;
                }
                oldestPending = oldestPending == 0 ? it->second : std::min(oldestPending, it->second);
                ++it;
            }
            return oldestPending == 0 ? m_Submitted : oldestPending - 1;
        }

        // Same answer without mutating, as if only `owner`'s fences were tracked.
        // Diagnostics compare it with PollCompleted() to see whether another owner
        // (a detached viewport) is what held the completed serial back.
        template<typename Probe>
        u64 PeekCompletedForOwnerOnly(u32 owner, Probe&& probe) const
        {
            u64 oldestPending = 0;
            for (const auto& [key, serial] : m_InFlight)
            {
                if (key.Owner != owner || probe(key) == FenceState::Complete)
                    continue;
                oldestPending = oldestPending == 0 ? serial : std::min(oldestPending, serial);
            }
            return oldestPending == 0 ? m_Submitted : oldestPending - 1;
        }

    private:
        struct KeyHash
        {
            size_t operator()(const Key& key) const
            {
                return static_cast<size_t>((static_cast<u64>(key.Owner) << 32) ^ key.Slot) * 0x9e3779b97f4a7c15ull;
            }
        };

        u64 m_Submitted = 0;
        u64 m_FrameSerial = 0;
        std::unordered_map<Key, u64, KeyHash> m_InFlight;
    };
}
