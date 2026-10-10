#pragma once

#include "Engine/Core/Base.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace Engine
{
    // This request permits a backend's documented immediate/tearing path. It
    // never asserts driver, display, or panel VRR state.
    enum class PresentationPolicy { Synchronized, TearingAllowed };
    enum class PresentationActualMode { Unavailable, D3D12Synchronized, D3D12TearingAllowed, VulkanFifo, VulkanImmediate };

    inline const char* ToString(PresentationPolicy value) { return value == PresentationPolicy::TearingAllowed ? "TearingAllowed" : "Synchronized"; }
    inline const char* ToString(PresentationActualMode value)
    {
        switch (value)
        {
            case PresentationActualMode::D3D12Synchronized: return "D3D12FlipSynchronized";
            case PresentationActualMode::D3D12TearingAllowed: return "D3D12FlipTearingAllowed";
            case PresentationActualMode::VulkanFifo: return "VulkanFifo";
            case PresentationActualMode::VulkanImmediate: return "VulkanImmediate";
            default: return "Unavailable";
        }
    }

    inline const char* PresentationSyncEncoding(PresentationActualMode value)
    {
        switch (value)
        {
            case PresentationActualMode::D3D12Synchronized: return "interval-1";
            case PresentationActualMode::D3D12TearingAllowed: return "interval-0";
            case PresentationActualMode::VulkanFifo: return "fifo";
            case PresentationActualMode::VulkanImmediate: return "immediate";
            default: return "unavailable";
        }
    }

    struct RendererPresentationPolicyDiagnostics
    {
        std::string Backend = "unavailable";
        PresentationPolicy Requested = PresentationPolicy::Synchronized;
        std::string Capability = "unavailable";
        PresentationActualMode Actual = PresentationActualMode::Unavailable;
        std::string FallbackReason = "unavailable";
        u32 SyncInterval = 0;
        bool PresentAllowsTearing = false;
        u64 SwapchainGeneration = 0;
        u64 EffectiveApplicationFrame = 0;
        u64 LastSuccessfulPresentGeneration = 0;
        u64 LastSuccessfulPresentApplicationFrame = 0;
    };

    // A request is complete when it has been resolved, not merely when Actual
    // happens to equal it.  In particular an unsupported TearingAllowed
    // request resolves to FIFO/synchronized once and must not recreate every
    // frame.  Backends keep this tiny state separate from native objects so it
    // is also deterministic to exercise without a window.
    struct PresentationPolicyTransitionState
    {
        PresentationPolicy Requested = PresentationPolicy::Synchronized;
        std::optional<PresentationPolicy> LastAppliedRequest;
        u64 Generation = 0;

        void Request(PresentationPolicy policy) { Requested = policy; }
        bool IsPending() const { return !LastAppliedRequest || *LastAppliedRequest != Requested; }
        // Committing a request that is already applied changes nothing, so a
        // recovery path that commits again after swapchain creation already
        // committed cannot advance the generation twice for one transition.
        void Commit()
        {
            if (!IsPending())
                return;
            LastAppliedRequest = Requested;
            ++Generation;
        }
        void Reset() { LastAppliedRequest.reset(); Generation = 0; }
    };

    // What a backend must do when asked to apply a pending presentation-policy
    // transition. A transition is deferred, never failed, while the window has
    // no drawable framebuffer (a minimized window reports 0x0): the request
    // stays pending and is applied on the first frame with a real size. Only a
    // genuinely failed recreation may abort.
    enum class PendingPolicyDecision { Failed, NothingPending, DeferUntilDrawable, Apply };
    inline PendingPolicyDecision DecidePendingPolicyApplication(
        bool initialized, bool transitionPending, int framebufferWidth, int framebufferHeight)
    {
        if (!initialized)
            return PendingPolicyDecision::Failed;
        if (!transitionPending)
            return PendingPolicyDecision::NothingPending;
        if (framebufferWidth <= 0 || framebufferHeight <= 0)
            return PendingPolicyDecision::DeferUntilDrawable;
        return PendingPolicyDecision::Apply;
    }

    // VK_SUBOPTIMAL_KHR is a success code: the image was acquired or presented.
    // It only hints that the swapchain could be recreated to match the surface
    // better. A driver or compositor can keep answering SUBOPTIMAL after the
    // recreation, so one recreation is allowed per swapchain extent; asking
    // again for the same extent would serialize every frame on a device-idle
    // rebuild without ever changing the answer.
    class SuboptimalRecreationGate
    {
    public:
        bool ShouldRecreate(u32 width, u32 height)
        {
            if (m_HasExtent && m_Width == width && m_Height == height)
                return false;
            m_HasExtent = true;
            m_Width = width;
            m_Height = height;
            return true;
        }

        // A deliberate transition (policy change) may justify one new recreation.
        void Reset() { m_HasExtent = false; }

    private:
        bool m_HasExtent = false;
        u32 m_Width = 0;
        u32 m_Height = 0;
    };

    struct VulkanPresentationResolution { PresentationActualMode Actual = PresentationActualMode::VulkanFifo; std::string Capability; std::string FallbackReason; };
    inline VulkanPresentationResolution ResolveVulkanPresentationPolicy(PresentationPolicy requested, const std::vector<int>& modes, int fifoMode, int immediateMode)
    {
        const bool hasFifo = std::find(modes.begin(), modes.end(), fifoMode) != modes.end();
        const bool hasImmediate = std::find(modes.begin(), modes.end(), immediateMode) != modes.end();
        if (requested == PresentationPolicy::TearingAllowed && hasImmediate)
            return { PresentationActualMode::VulkanImmediate, "immediate-supported", "none" };
        if (requested == PresentationPolicy::TearingAllowed)
            return { PresentationActualMode::VulkanFifo, hasFifo ? "immediate-unsupported" : "fifo-required-by-vulkan", "TearingAllowed requested; IMMEDIATE unavailable, selected FIFO without replacement" };
        return { PresentationActualMode::VulkanFifo, hasFifo ? "fifo-supported" : "fifo-required-by-vulkan", "none" };
    }
}
