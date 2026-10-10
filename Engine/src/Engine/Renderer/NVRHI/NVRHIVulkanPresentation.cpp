#include "Engine/Renderer/NVRHI/NVRHIVulkanPresentation.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/RHI/NVRHI/VulkanDispatch.h"
#include "Engine/Renderer/PresentationSerialLedger.h"

#if defined(GE_HAS_NVRHI_VULKAN)
    #include <GLFW/glfw3.h>
    #include <backends/imgui_impl_vulkan.h>
    #include <imgui.h>

    #include <algorithm>
    #include <atomic>
    #include <chrono>
    #include <cstring>
    #include <functional>
    #include <iterator>
    #include <stdexcept>
    #include <thread>
    #include <unordered_map>
#endif

namespace Engine
{
#if defined(GE_HAS_NVRHI_VULKAN)
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // Negative VkResult values reported through the backend's check hook.
        // Published as UiViewportDiagnostics::BackendErrors so a smoke can prove a
        // run was error-free (no validation layer is required for this counter).
        std::atomic<u64> s_BackendErrorCount { 0 };

        void CheckVulkanResult(VkResult result)
        {
            if (result < 0)
            {
                s_BackendErrorCount.fetch_add(1, std::memory_order_relaxed);
                Log::Error("ImGui Vulkan backend reported VkResult ", static_cast<int>(result));
            }
        }

        static_assert(UiViewportPresentModeValue::Immediate == static_cast<int>(VK_PRESENT_MODE_IMMEDIATE_KHR));
        static_assert(UiViewportPresentModeValue::Mailbox == static_cast<int>(VK_PRESENT_MODE_MAILBOX_KHR));
        static_assert(UiViewportPresentModeValue::Fifo == static_cast<int>(VK_PRESENT_MODE_FIFO_KHR));
        static_assert(UiViewportPresentModeValue::FifoRelaxed == static_cast<int>(VK_PRESENT_MODE_FIFO_RELAXED_KHR));

        PFN_vkVoidFunction LoadImGuiVulkanFunction(const char* name, void* userData)
        {
            auto* context = static_cast<RHI::NVRHIVulkanContext*>(userData);
            return reinterpret_cast<PFN_vkVoidFunction>(context ? context->GetInstanceProcAddress(name) : nullptr);
        }
    }
#endif

    struct NVRHIVulkanPresentation::Impl
    {
        bool Initialize(RHI::NVRHIVulkanContext* context, void* nativeWindow, u32 width, u32 height)
        {
#if defined(GE_HAS_NVRHI_VULKAN)
            m_Context = context;
            m_Window = static_cast<GLFWwindow*>(nativeWindow);
            m_OwnerThread = std::this_thread::get_id();
            if (!m_Context || !m_Context->IsInitialized())
                return false;

            const RHI::NVRHIVulkanNativeHandles& handles = m_Context->GetNativeHandles();
            m_Instance = static_cast<VkInstance>(handles.Instance);
            m_PhysicalDevice = static_cast<VkPhysicalDevice>(handles.PhysicalDevice);
            m_Device = static_cast<VkDevice>(handles.Device);
            m_GraphicsQueue = static_cast<VkQueue>(handles.GraphicsQueue);
            m_QueueFamily = handles.GraphicsQueueFamily;
            m_WindowData.Surface = static_cast<VkSurfaceKHR>(handles.Surface);
            if (!m_Instance || !m_PhysicalDevice || !m_Device || !m_GraphicsQueue || !m_WindowData.Surface)
                return false;

            if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, LoadImGuiVulkanFunction, m_Context))
            {
                Log::Error("Dear ImGui could not load Vulkan functions");
                return false;
            }

            VkBool32 supportsPresentation = VK_FALSE;
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfaceSupportKHR(
                m_PhysicalDevice, m_QueueFamily, m_WindowData.Surface, &supportsPresentation);
            if (!supportsPresentation)
            {
                Log::Error("Vulkan graphics queue cannot present to the editor surface");
                return false;
            }

            const VkFormat requestedFormats[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
            m_WindowData.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
                m_PhysicalDevice,
                m_WindowData.Surface,
                requestedFormats,
                static_cast<int>(std::size(requestedFormats)),
                VK_COLORSPACE_SRGB_NONLINEAR_KHR);
            if (!ResolveRequestedPresentMode())
                return false;

            if (!CreateDescriptorPool())
                return false;
            int framebufferWidth = static_cast<int>(width);
            int framebufferHeight = static_cast<int>(height);
            glfwGetFramebufferSize(m_Window, &framebufferWidth, &framebufferHeight);
            if (!CreateOrResizeSwapchain(static_cast<u32>(framebufferWidth), static_cast<u32>(framebufferHeight)))
                return false;

            ImGui_ImplVulkan_InitInfo initInfo {};
            initInfo.ApiVersion = VK_API_VERSION_1_3;
            initInfo.Instance = m_Instance;
            initInfo.PhysicalDevice = m_PhysicalDevice;
            initInfo.Device = m_Device;
            initInfo.QueueFamily = m_QueueFamily;
            initInfo.Queue = m_GraphicsQueue;
            initInfo.DescriptorPool = m_DescriptorPool;
            initInfo.MinImageCount = kMinimumImageCount;
            initInfo.ImageCount = m_WindowData.ImageCount;
            initInfo.PipelineInfoMain.RenderPass = m_WindowData.RenderPass;
            initInfo.PipelineInfoMain.Subpass = 0;
            initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            // Secondary (detached-window) pipeline: same sample count and subpass
            // as the main one. The backend fills in the render pass from the first
            // secondary swapchain and shares one pipeline between all of them, so
            // every secondary must use the same surface format (checked when a
            // window is created). No extra swapchain usage is requested.
            initInfo.PipelineInfoForViewports.Subpass = 0;
            initInfo.PipelineInfoForViewports.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            initInfo.PipelineInfoForViewports.SwapChainImageUsage = 0;
            initInfo.CheckVkResultFn = CheckVulkanResult;
            m_InitImageCount = initInfo.ImageCount;
            if (!ImGui_ImplVulkan_Init(&initInfo))
            {
                Log::Error("Could not initialize the Dear ImGui Vulkan renderer");
                return false;
            }

            m_ImGuiInitialized = true;
            m_Initialized = true;
            Log::Info("Vulkan swapchain and ImGui presentation initialized (", framebufferWidth, "x", framebufferHeight, ")");
            return true;
#else
            (void)context;
            (void)nativeWindow;
            (void)width;
            (void)height;
            return false;
#endif
        }

        void Shutdown()
        {
#if defined(GE_HAS_NVRHI_VULKAN)
            if (m_Device)
                VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle(m_Device);
            m_SubmittedFrameIds.clear();
            m_Ledger.Clear();
            ReleaseViewportOutput();
            // Shutdown order for detached windows: the device is idle, then
            // ImGui_ImplVulkan_Shutdown destroys every secondary swapchain and
            // surface (ImGui::DestroyPlatformWindows -> our DestroyWindow wrapper
            // -> the backend's, then the GLFW window) while the instance and
            // device are alive. The wrappers are uninstalled only afterwards.
            if (m_ImGuiInitialized)
                ImGui_ImplVulkan_Shutdown();
            m_UiViewportsActive = false;
            if (s_ViewportImpl == this)
                s_ViewportImpl = nullptr;
            m_Secondaries.clear();
            if (m_Instance && m_Device && m_WindowData.Swapchain)
                ImGui_ImplVulkanH_DestroyWindow(m_Instance, m_Device, &m_WindowData, nullptr);
            if (m_Device && m_DescriptorPool)
                VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);

            m_DescriptorPool = VK_NULL_HANDLE;
            m_GraphicsQueue = VK_NULL_HANDLE;
            m_Device = VK_NULL_HANDLE;
            m_PhysicalDevice = VK_NULL_HANDLE;
            m_Instance = VK_NULL_HANDLE;
            m_WindowData = {};
            m_Context = nullptr;
            m_Window = nullptr;
            m_ImGuiInitialized = false;
#endif
            m_Initialized = false;
        }

        void BeginImGuiFrame()
        {
#if defined(GE_HAS_NVRHI_VULKAN)
            // The next submission, main or detached, opens this frame's serial.
            m_Ledger.BeginFrame();
            if (m_ImGuiInitialized)
                ImGui_ImplVulkan_NewFrame();
#endif
        }

        void SetPresentationPolicy(PresentationPolicy policy)
        {
            m_Transition.Request(policy);
            m_RequestedPolicy = policy;
            m_PolicyDiagnostics.Requested = policy;
        }

        const RendererPresentationPolicyDiagnostics& GetPresentationPolicyDiagnostics() const { return m_PolicyDiagnostics; }

        void RenderImGuiDrawData(ImDrawData* drawData, const ClearColor& clearColor, u32 width, u32 height)
        {
#if defined(GE_HAS_NVRHI_VULKAN)
            m_Timing = {};
            m_Timing.SwapchainGeneration = m_SwapchainGeneration;
            m_Timing.LastSuccessfulPresentGeneration = m_LastSuccessfulPresentGeneration;
            if (!m_Initialized || !drawData)
                return;
            // Only enforced once detached windows exist, so a run without the
            // feature keeps exactly its previous behaviour.
            if (m_UiViewportsActive)
                AssertOwnerThread();

            int framebufferWidth = static_cast<int>(width);
            int framebufferHeight = static_cast<int>(height);
            glfwGetFramebufferSize(m_Window, &framebufferWidth, &framebufferHeight);
            if (framebufferWidth <= 0 || framebufferHeight <= 0)
                return;
            width = static_cast<u32>(framebufferWidth);
            height = static_cast<u32>(framebufferHeight);

            if (m_Transition.IsPending())
            {
                if (!RecreateForRequestedPolicy(width, height))
                    throw std::runtime_error("Vulkan presentation-policy transition could not restore a valid swapchain");
            }
            if (m_SwapchainInvalid || width != static_cast<u32>(m_WindowData.Width) || height != static_cast<u32>(m_WindowData.Height))
            {
                if (!CreateOrResizeSwapchain(width, height))
                    return;
                ImGui_ImplVulkan_SetMinImageCount(kMinimumImageCount);
                m_SwapchainInvalid = false;
            }

            m_WindowData.ClearValue.color.float32[0] = clearColor.R;
            m_WindowData.ClearValue.color.float32[1] = clearColor.G;
            m_WindowData.ClearValue.color.float32[2] = clearColor.B;
            m_WindowData.ClearValue.color.float32[3] = clearColor.A;

            ImGui_ImplVulkanH_FrameSemaphores& semaphores = m_WindowData.FrameSemaphores[m_WindowData.SemaphoreIndex];
            const Clock::time_point acquireStart = Clock::now();
            VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkAcquireNextImageKHR(
                m_Device,
                m_WindowData.Swapchain,
                UINT64_MAX,
                semaphores.ImageAcquiredSemaphore,
                VK_NULL_HANDLE,
                &m_WindowData.FrameIndex);
            Renderer::RecordFrameWait(Renderer::GetLastFrameTiming().FrameIndex,
                RendererFrameWaitKind::MandatoryVulkanAcquire,
                result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR,
                std::chrono::duration<double, std::milli>(Clock::now() - acquireStart).count());
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            {
                m_SwapchainInvalid = true;
                if (result == VK_ERROR_OUT_OF_DATE_KHR)
                    return;
            }
            else if (result != VK_SUCCESS)
            {
                CheckVulkanResult(result);
                return;
            }

            ImGui_ImplVulkanH_Frame& frame = m_WindowData.Frames[m_WindowData.FrameIndex];
            const Clock::time_point fenceWaitStart = Clock::now();
            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitForFences(m_Device, 1, &frame.Fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
                return;
            Renderer::RecordFrameWait(Renderer::GetLastFrameTiming().FrameIndex,
                RendererFrameWaitKind::MandatoryVulkanFence,
                true,
                std::chrono::duration<double, std::milli>(Clock::now() - fenceWaitStart).count());
            // The wait proved this image's previous presentation submission finished.
            m_Ledger.Forget({ 0, m_WindowData.FrameIndex });
            if (const auto completed = m_SubmittedFrameIds.find(m_WindowData.FrameIndex); completed != m_SubmittedFrameIds.end()
                && completed->second.SwapchainGeneration == m_SwapchainGeneration)
                Renderer::RecordGpuCompletionObservation(completed->second.ApplicationFrameIndex);
            VULKAN_HPP_DEFAULT_DISPATCHER.vkResetFences(m_Device, 1, &frame.Fence);
            VULKAN_HPP_DEFAULT_DISPATCHER.vkResetCommandPool(m_Device, frame.CommandPool, 0);

            VkCommandBufferBeginInfo beginInfo {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkBeginCommandBuffer(frame.CommandBuffer, &beginInfo) != VK_SUCCESS)
                return;

            VkRenderPassBeginInfo renderPassInfo {};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassInfo.renderPass = m_WindowData.RenderPass;
            renderPassInfo.framebuffer = frame.Framebuffer;
            renderPassInfo.renderArea.extent = { static_cast<u32>(m_WindowData.Width), static_cast<u32>(m_WindowData.Height) };
            renderPassInfo.clearValueCount = 1;
            renderPassInfo.pClearValues = &m_WindowData.ClearValue;
            VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBeginRenderPass(frame.CommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(drawData, frame.CommandBuffer);
            VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdEndRenderPass(frame.CommandBuffer);
            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkEndCommandBuffer(frame.CommandBuffer) != VK_SUCCESS)
                return;

            const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo submitInfo {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = &semaphores.ImageAcquiredSemaphore;
            submitInfo.pWaitDstStageMask = &waitStage;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &frame.CommandBuffer;
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &semaphores.RenderCompleteSemaphore;
            const FramePacingWaitResult pacing = Renderer::ApplySmoothFrametimeCandidate(SmoothFrametimeCandidate::SubmissionGate);
            if (Renderer::GetLastFrameTiming().FramePacingPolicy.EffectiveMode == FramePacingMode::SmoothFrametime
                && Renderer::GetLastFrameTiming().FramePacingPolicy.Candidate == SmoothFrametimeCandidate::SubmissionGate)
            {
                Log::Info("SmoothFrametimeNativeV1 backend=Vulkan candidate=SubmissionGate control=pre-vkQueueSubmit ",
                    "waitMs=", pacing.WaitMilliseconds, " missed=", pacing.DeadlineMissed ? "yes" : "no",
                    " frame=", Renderer::GetLastFrameTiming().FrameIndex);
            }
            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, frame.Fence) != VK_SUCCESS)
                return;
            const u64 applicationFrameIndex = Renderer::GetLastFrameTiming().FrameIndex;
            m_SubmittedFrameIds[m_WindowData.FrameIndex] = { applicationFrameIndex, m_SwapchainGeneration };
            m_Ledger.Track({ 0, m_WindowData.FrameIndex });
            Renderer::RecordFrameLifecyclePhase(applicationFrameIndex, RendererFrameLifecyclePhase::RenderSubmission);

            VkPresentInfoKHR presentInfo {};
            presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            presentInfo.waitSemaphoreCount = 1;
            presentInfo.pWaitSemaphores = &semaphores.RenderCompleteSemaphore;
            presentInfo.swapchainCount = 1;
            presentInfo.pSwapchains = &m_WindowData.Swapchain;
            presentInfo.pImageIndices = &m_WindowData.FrameIndex;

            Renderer::RecordFrameLifecyclePhase(applicationFrameIndex, RendererFrameLifecyclePhase::PresentBegin);
            const Clock::time_point presentStart = Clock::now();
            m_Timing.PresentSucceeded = false;
            result = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueuePresentKHR(m_GraphicsQueue, &presentInfo);
            m_Timing.PresentMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - presentStart).count();
            m_Timing.ApplicationFrameIndex = applicationFrameIndex;
            Renderer::RecordFrameLifecyclePhase(applicationFrameIndex, RendererFrameLifecyclePhase::PresentEnd);
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
                m_SwapchainInvalid = true;
            else if (result == VK_SUCCESS)
            {
                m_Timing.PresentSucceeded = true;
                m_LastSuccessfulPresentGeneration = m_SwapchainGeneration;
                m_Timing.LastSuccessfulPresentGeneration = m_LastSuccessfulPresentGeneration;
                m_PolicyDiagnostics.LastSuccessfulPresentGeneration = m_LastSuccessfulPresentGeneration;
                m_PolicyDiagnostics.LastSuccessfulPresentApplicationFrame = applicationFrameIndex;
                ++m_SuccessfulPresentCount;
                if (m_ViewportTextureQueued)
                {
                    if (m_ViewportOutputGeneration != m_LastLoggedViewportOutputGeneration
                        || m_ViewportDescriptorGeneration != m_LastLoggedViewportDescriptorGeneration
                        || m_SwapchainGeneration != m_LastLoggedViewportSwapchainGeneration)
                    {
                        Log::Info("VulkanSceneOutputHandoffV1 producer=pass outputGeneration=", m_ViewportOutputGeneration,
                            " descriptor=registered descriptorGeneration=", m_ViewportDescriptorGeneration,
                            " imgui=queued present=pass swapchainGeneration=", m_SwapchainGeneration);
                        m_LastLoggedViewportOutputGeneration = m_ViewportOutputGeneration;
                        m_LastLoggedViewportDescriptorGeneration = m_ViewportDescriptorGeneration;
                        m_LastLoggedViewportSwapchainGeneration = m_SwapchainGeneration;
                    }
                    m_ViewportTextureQueued = false;
                }
            }
            else
                CheckVulkanResult(result);

            m_WindowData.SemaphoreIndex = (m_WindowData.SemaphoreIndex + 1) % m_WindowData.SemaphoreCount;
#else
            (void)drawData;
            (void)clearColor;
            (void)width;
            (void)height;
#endif
        }

#if defined(GE_HAS_NVRHI_VULKAN)
        bool CreateDescriptorPool()
        {
            const VkDescriptorPoolSize poolSizes[] = {
                { VK_DESCRIPTOR_TYPE_SAMPLER, 128 },
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128 },
                { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 128 }
            };
            VkDescriptorPoolCreateInfo poolInfo {};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            poolInfo.maxSets = 384;
            poolInfo.poolSizeCount = static_cast<u32>(std::size(poolSizes));
            poolInfo.pPoolSizes = poolSizes;
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool) == VK_SUCCESS;
        }

        bool RegisterViewportOutput(const RHI::NVRHIVulkanTextureNativeHandles& handles, u64 outputGeneration)
        {
            if (!m_Initialized || !m_ImGuiInitialized || !handles.Image || !handles.ImageView || outputGeneration == 0)
                return false;
            if (m_ViewportTextureId != 0 && m_ViewportOutputGeneration == outputGeneration)
                return true;

            ReleaseViewportOutput();
            VkSamplerCreateInfo samplerInfo {};
            samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxLod = 0.0f;
            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSampler(m_Device, &samplerInfo, nullptr, &m_ViewportSampler) != VK_SUCCESS)
                return false;
            m_ViewportImageView = static_cast<VkImageView>(handles.ImageView);
            m_ViewportTextureId = reinterpret_cast<u64>(ImGui_ImplVulkan_AddTexture(m_ViewportSampler, m_ViewportImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            if (m_ViewportTextureId == 0)
            {
                VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySampler(m_Device, m_ViewportSampler, nullptr);
                m_ViewportSampler = VK_NULL_HANDLE;
                m_ViewportImageView = VK_NULL_HANDLE;
                return false;
            }
            m_ViewportOutputGeneration = outputGeneration;
            m_ViewportDescriptorGeneration = outputGeneration;
            Log::Info("Vulkan Scene output registered for ImGui: outputGeneration=", outputGeneration, ", layout=shader-read-only");
            return true;
        }

        void ReleaseViewportOutput()
        {
            if (m_ViewportTextureId != 0 && m_ImGuiInitialized)
                ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(m_ViewportTextureId));
            if (m_ViewportSampler != VK_NULL_HANDLE && m_Device)
                VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySampler(m_Device, m_ViewportSampler, nullptr);
            m_ViewportTextureId = 0;
            m_ViewportOutputGeneration = 0;
            m_ViewportDescriptorGeneration = 0;
            m_ViewportImageView = VK_NULL_HANDLE;
            m_ViewportSampler = VK_NULL_HANDLE;
            m_ViewportTextureQueued = false;
            m_LastLoggedViewportOutputGeneration = 0;
            m_LastLoggedViewportDescriptorGeneration = 0;
            m_LastLoggedViewportSwapchainGeneration = 0;
        }

        void MarkViewportTextureQueued(u64 textureId)
        {
            m_ViewportTextureQueued = textureId != 0 && textureId == m_ViewportTextureId;
        }

        u64 RegisterUiTexture(RHI::Texture& texture)
        {
            if (!m_Initialized || !m_ImGuiInitialized)
                return 0;
            const RHI::NVRHIVulkanTextureNativeHandles handles = RHI::GetNVRHIVulkanTextureNativeHandles(texture);
            if (!handles.ImageView)
                return 0;
            // The ImGui backend binds its one shared linear/clamp sampler for
            // every texture, so registration needs only the view and layout.
            return reinterpret_cast<u64>(ImGui_ImplVulkan_AddTexture(
                static_cast<VkImageView>(handles.ImageView), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        }

        void UnregisterUiTexture(u64 imGuiId)
        {
            if (imGuiId != 0 && m_ImGuiInitialized)
                ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(imGuiId));
        }

        // Fence state behind a ledger key: owner 0 is the main swapchain, any other
        // owner is a detached viewport found by its ImGui id. A swapchain that was
        // rebuilt or destroyed waited for the device first, so a vanished fence is
        // complete.
        PresentationSerialLedger::FenceState ProbeFence(const PresentationSerialLedger::Key& key) const
        {
            using State = PresentationSerialLedger::FenceState;
            VkFence fence = VK_NULL_HANDLE;
            if (key.Owner == 0)
            {
                if (key.Slot < m_WindowData.ImageCount && key.Slot < static_cast<u32>(m_WindowData.Frames.Size))
                    fence = m_WindowData.Frames[key.Slot].Fence;
            }
            else if (ImGuiViewport* viewport = ImGui::FindViewportByID(key.Owner))
            {
                const ImGui_ImplVulkanH_Window* window = ImGui_ImplVulkanH_GetWindowDataFromViewport(viewport);
                if (window && window->Swapchain != VK_NULL_HANDLE && key.Slot < window->ImageCount
                    && key.Slot < static_cast<u32>(window->Frames.Size))
                    fence = window->Frames[key.Slot].Fence;
            }
            if (fence == VK_NULL_HANDLE)
                return State::Complete;
            // Anything but not-ready is terminal (signalled or device lost).
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetFenceStatus(m_Device, fence) == VK_NOT_READY ? State::Pending : State::Complete;
        }

        u64 PollCompletedPresentationSerial()
        {
            if (!m_Device)
                return m_Ledger.Submitted();
            const auto probe = [this](const PresentationSerialLedger::Key& key) { return ProbeFence(key); };
            const u64 completed = m_Ledger.PollCompleted(probe);
            if (m_UiViewportsActive && completed < m_Ledger.PeekCompletedForOwnerOnly(0, probe))
                ++m_SerialPollsHeldBySecondary;
            return completed;
        }

        // ---- Detachable OS-window panels (Dear ImGui multi-viewport) ----------
        //
        // Threading and queue contract: ImGui records secondary windows into its own
        // per-image command pools and submits/presents them with raw vkQueueSubmit /
        // vkQueuePresentKHR on the NVRHI graphics queue, exactly like the main
        // window path above. NVRHI's own queue submit takes an internal mutex that
        // these raw calls do not, and RHI::Device::Submit mutates unsynchronised
        // completion bookkeeping, so the only safe arrangement is one submitting
        // thread. Every entry point here asserts it is the thread that initialised
        // the presentation (the application's main thread).
        void AssertOwnerThread() const
        {
            GE_ASSERT(std::this_thread::get_id() == m_OwnerThread,
                "Vulkan presentation and detached-window submission must stay on the thread that initialised them");
        }

        struct SecondaryRecord
        {
            int PresentMode = static_cast<int>(VK_PRESENT_MODE_FIFO_KHR);
            u32 Width = 0;
            u32 Height = 0;
            u32 ImageCount = 0;
            u64 FramesRendered = 0;
            bool Skipped = false;
            std::string SkipReason;
            // The backend sizes each window's vertex/index ring from the main
            // swapchain's image count. A secondary swapchain with more images could
            // reuse a ring slot the GPU still reads; such a window is rendered only
            // after all of its own frames finished.
            bool RingHazard = false;
            bool RenderedThisFrame = false;
        };

        static double ElapsedMilliseconds(Clock::time_point start)
        {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }

        // create/resize/destroy each hide a vkDeviceWaitIdle in the backend; keep the
        // slow ones visible.
        void NoteWaitIdleCost(const char* operation, u32 viewportId, double milliseconds)
        {
            if (milliseconds <= kUiViewportSlowOperationMilliseconds)
                return;
            ++m_ViewportDiagnostics.SlowOperations;
            Log::Warn("UiViewportsV1 backend=Vulkan viewport=", viewportId, " slowOperation=", operation, " ms=", milliseconds,
                " reason=backend-vkDeviceWaitIdle-drains-the-queue");
        }

        std::vector<int> QuerySurfaceModes(VkSurfaceKHR surface) const
        {
            std::vector<int> result;
            u32 count = 0;
            auto& vk = VULKAN_HPP_DEFAULT_DISPATCHER;
            if (vk.vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, surface, &count, nullptr) != VK_SUCCESS || count == 0)
                return result;
            std::vector<VkPresentModeKHR> modes(count);
            if (vk.vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, surface, &count, modes.data()) != VK_SUCCESS)
                return result;
            for (VkPresentModeKHR mode : modes)
                result.push_back(static_cast<int>(mode));
            return result;
        }

        bool EnableUiViewports()
        {
            if (m_UiViewportsActive)
                return true;
            if (!m_Initialized || !m_ImGuiInitialized)
                return false;
            AssertOwnerThread();
            ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
            if (!platformIo.Platform_CreateVkSurface || !platformIo.Renderer_CreateWindow || !platformIo.Renderer_DestroyWindow
                || !platformIo.Renderer_SetWindowSize || !platformIo.Renderer_RenderWindow || !platformIo.Renderer_SwapBuffers)
                return false;
            m_OriginalCreateWindow = platformIo.Renderer_CreateWindow;
            m_OriginalDestroyWindow = platformIo.Renderer_DestroyWindow;
            m_OriginalSetWindowSize = platformIo.Renderer_SetWindowSize;
            m_OriginalRenderWindow = platformIo.Renderer_RenderWindow;
            m_OriginalSwapBuffers = platformIo.Renderer_SwapBuffers;
            s_ViewportImpl = this;
            platformIo.Renderer_CreateWindow = [](ImGuiViewport* viewport) { s_ViewportImpl->OnCreateWindow(viewport); };
            platformIo.Renderer_DestroyWindow = [](ImGuiViewport* viewport) { s_ViewportImpl->OnDestroyWindow(viewport); };
            platformIo.Renderer_SetWindowSize = [](ImGuiViewport* viewport, ImVec2 size) { s_ViewportImpl->OnSetWindowSize(viewport, size); };
            platformIo.Renderer_RenderWindow = [](ImGuiViewport* viewport, void* argument) { s_ViewportImpl->OnRenderWindow(viewport, argument); };
            platformIo.Renderer_SwapBuffers = [](ImGuiViewport* viewport, void* argument) { s_ViewportImpl->OnSwapBuffers(viewport, argument); };
            m_UiViewportsActive = true;
            Log::Info("UiViewportsV1 backend=Vulkan wrappers=installed secondaryPresent=MAILBOX-or-IMMEDIATE-never-FIFO ",
                "order=main-present-then-secondaries threading=main-thread-only");
            return true;
        }

        void OnCreateWindow(ImGuiViewport* viewport)
        {
            AssertOwnerThread();
            const Clock::time_point start = Clock::now();
            m_OriginalCreateWindow(viewport);
            const double milliseconds = ElapsedMilliseconds(start);
            m_ViewportDiagnostics.LastCreateMilliseconds = milliseconds;
            m_ViewportDiagnostics.MaxCreateMilliseconds = std::max(m_ViewportDiagnostics.MaxCreateMilliseconds, milliseconds);
            NoteWaitIdleCost("create", viewport->ID, milliseconds);

            ImGui_ImplVulkanH_Window* window = ImGui_ImplVulkanH_GetWindowDataFromViewport(viewport);
            if (!window || window->Swapchain == VK_NULL_HANDLE)
            {
                // The backend leaves no renderer data behind on failure and its
                // render/swap handlers ignore such a viewport.
                ++m_ViewportDiagnostics.SecondaryCreateFailures;
                Log::Error("UiViewportsV1 backend=Vulkan viewport=", viewport->ID, " create=failed");
                return;
            }
            ++m_ViewportDiagnostics.SecondaryCreates;

            SecondaryRecord record;
            const UiViewportSecondaryResolution resolution =
                ResolveUiViewportSecondaryMode(static_cast<int>(window->PresentMode), QuerySurfaceModes(window->Surface));
            if (resolution.Action == UiViewportSecondaryAction::Override)
            {
                // The backend picked a blocking mode although MAILBOX/IMMEDIATE is
                // available on this surface. Rebuild with the allowed one (one more
                // device wait, counted).
                window->PresentMode = static_cast<VkPresentModeKHR>(resolution.Mode);
                ImGui_ImplVulkanH_CreateOrResizeWindow(m_Instance, m_PhysicalDevice, m_Device, window, m_QueueFamily, nullptr,
                    window->Width, window->Height, kMinimumImageCount, 0);
                ++m_ViewportDiagnostics.PresentModeOverrides;
            }
            else if (resolution.Action == UiViewportSecondaryAction::Skip)
            {
                record.Skipped = true;
                record.SkipReason = "no-non-blocking-present-mode";
            }
            if (!record.Skipped && window->SurfaceFormat.format != m_WindowData.SurfaceFormat.format)
            {
                // All secondaries share one pipeline built for the first window's
                // render pass; a different format would be incompatible.
                record.Skipped = true;
                record.SkipReason = "surface-format-differs-from-main";
            }
            record.PresentMode = static_cast<int>(window->PresentMode);
            record.Width = static_cast<u32>(window->Width);
            record.Height = static_cast<u32>(window->Height);
            record.ImageCount = window->ImageCount;
            record.RingHazard = window->ImageCount > m_InitImageCount;
            if (record.Skipped)
            {
                ++m_ViewportDiagnostics.SkippedWindows;
                Log::Warn("UiViewportsV1 backend=Vulkan viewport=", viewport->ID, " skipped=", record.SkipReason,
                    " presentMode=", record.PresentMode);
            }
            Log::Info("UiViewportsV1 backend=Vulkan viewport=", viewport->ID, " created presentMode=", record.PresentMode,
                " images=", record.ImageCount, " size=", record.Width, "x", record.Height, " createMs=", milliseconds,
                " override=", resolution.Action == UiViewportSecondaryAction::Override ? "yes" : "no",
                " ringHazard=", record.RingHazard ? "yes" : "no");
            m_Secondaries[viewport->ID] = std::move(record);
            m_ViewportDiagnostics.PeakSecondaryCount = std::max<u32>(m_ViewportDiagnostics.PeakSecondaryCount, static_cast<u32>(m_Secondaries.size()));
        }

        void OnDestroyWindow(ImGuiViewport* viewport)
        {
            AssertOwnerThread();
            const Clock::time_point start = Clock::now();
            const bool tracked = m_Secondaries.erase(viewport->ID) != 0;
            // The backend waits for the device before freeing the swapchain, so
            // every fence it owned is complete when this returns.
            m_OriginalDestroyWindow(viewport);
            m_Ledger.ForgetOwner(viewport->ID);
            if (!tracked)
                return;
            const double milliseconds = ElapsedMilliseconds(start);
            ++m_ViewportDiagnostics.SecondaryDestroys;
            m_ViewportDiagnostics.LastDestroyMilliseconds = milliseconds;
            m_ViewportDiagnostics.MaxDestroyMilliseconds = std::max(m_ViewportDiagnostics.MaxDestroyMilliseconds, milliseconds);
            NoteWaitIdleCost("destroy", viewport->ID, milliseconds);
            Log::Info("UiViewportsV1 backend=Vulkan viewport=", viewport->ID, " destroyed destroyMs=", milliseconds);
        }

        void OnSetWindowSize(ImGuiViewport* viewport, ImVec2 size)
        {
            AssertOwnerThread();
            const Clock::time_point start = Clock::now();
            m_OriginalSetWindowSize(viewport, size);
            const double milliseconds = ElapsedMilliseconds(start);
            auto found = m_Secondaries.find(viewport->ID);
            if (found == m_Secondaries.end())
                return;
            ++m_ViewportDiagnostics.SecondaryResizes;
            m_ViewportDiagnostics.LastResizeMilliseconds = milliseconds;
            m_ViewportDiagnostics.MaxResizeMilliseconds = std::max(m_ViewportDiagnostics.MaxResizeMilliseconds, milliseconds);
            NoteWaitIdleCost("resize", viewport->ID, milliseconds);
            if (const ImGui_ImplVulkanH_Window* window = ImGui_ImplVulkanH_GetWindowDataFromViewport(viewport))
            {
                found->second.Width = static_cast<u32>(window->Width);
                found->second.Height = static_cast<u32>(window->Height);
                found->second.ImageCount = window->ImageCount;
                found->second.RingHazard = window->ImageCount > m_InitImageCount;
            }
        }

        // Waits (bounded) until every frame of a detached window has finished.
        bool WaitForSecondaryFrames(const ImGui_ImplVulkanH_Window* window)
        {
            constexpr u64 kTimeoutNanoseconds = 1000ull * 1000ull * 1000ull;
            for (int index = 0; index < window->Frames.Size; ++index)
                if (VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitForFences(m_Device, 1, &window->Frames[index].Fence, VK_TRUE, kTimeoutNanoseconds) != VK_SUCCESS)
                    return false;
            return true;
        }

        void OnRenderWindow(ImGuiViewport* viewport, void* argument)
        {
            AssertOwnerThread();
            auto found = m_Secondaries.find(viewport->ID);
            if (found == m_Secondaries.end())
                return;
            SecondaryRecord& record = found->second;
            record.RenderedThisFrame = false;
            ImGui_ImplVulkanH_Window* window = ImGui_ImplVulkanH_GetWindowDataFromViewport(viewport);
            if (record.Skipped || !window || window->Swapchain == VK_NULL_HANDLE)
            {
                ++m_ViewportDiagnostics.SkippedRenders;
                return;
            }
            if (record.RingHazard && !WaitForSecondaryFrames(window))
            {
                ++m_ViewportDiagnostics.SkippedRenders;
                return;
            }
            const Clock::time_point start = Clock::now();
            m_OriginalRenderWindow(viewport, argument);
            m_FrameRenderMilliseconds += ElapsedMilliseconds(start);
            record.RenderedThisFrame = true;
            ++record.FramesRendered;
            ++m_ViewportDiagnostics.SecondaryFramesRendered;

            // The submission, if it happened, signals this image's fence. A fence
            // that is already signalled finished before we looked; one that is
            // not-ready belongs to this frame's serial.
            if (window->FrameIndex < window->ImageCount && window->FrameIndex < static_cast<u32>(window->Frames.Size)
                && VULKAN_HPP_DEFAULT_DISPATCHER.vkGetFenceStatus(m_Device, window->Frames[window->FrameIndex].Fence) == VK_NOT_READY)
            {
                m_Ledger.Track({ viewport->ID, window->FrameIndex });
                ++m_ViewportDiagnostics.SecondarySubmissionsTracked;
            }
        }

        void OnSwapBuffers(ImGuiViewport* viewport, void* argument)
        {
            AssertOwnerThread();
            auto found = m_Secondaries.find(viewport->ID);
            if (found == m_Secondaries.end() || !found->second.RenderedThisFrame)
                return;
            found->second.RenderedThisFrame = false;
            const Clock::time_point start = Clock::now();
            m_OriginalSwapBuffers(viewport, argument);
            m_FrameSwapMilliseconds += ElapsedMilliseconds(start);
        }

        void RenderUiPlatformWindows()
        {
            if (!m_UiViewportsActive || !m_Initialized)
                return;
            AssertOwnerThread();
            m_FrameRenderMilliseconds = 0.0;
            m_FrameSwapMilliseconds = 0.0;
            const Clock::time_point updateStart = Clock::now();
            ImGui::UpdatePlatformWindows();
            m_ViewportDiagnostics.LastUpdateMilliseconds = ElapsedMilliseconds(updateStart);
            ImGui::RenderPlatformWindowsDefault();
            m_ViewportDiagnostics.LastRenderMilliseconds = m_FrameRenderMilliseconds;
            m_ViewportDiagnostics.LastSwapMilliseconds = m_FrameSwapMilliseconds;
        }

        UiViewportDiagnostics GetUiViewportDiagnostics() const
        {
            UiViewportDiagnostics result = m_ViewportDiagnostics;
            result.SerialPollsHeldBySecondary = m_SerialPollsHeldBySecondary;
            result.BackendErrors = s_BackendErrorCount.load(std::memory_order_relaxed);
            result.SecondaryCount = static_cast<u32>(m_Secondaries.size());
            for (const auto& [id, record] : m_Secondaries)
            {
                UiViewportSecondaryInfo info;
                info.ViewportId = id;
                info.PresentMode = record.PresentMode;
                info.Width = record.Width;
                info.Height = record.Height;
                info.ImageCount = record.ImageCount;
                info.FramesRendered = record.FramesRendered;
                info.Skipped = record.Skipped;
                info.SkipReason = record.SkipReason;
                result.Secondaries.push_back(std::move(info));
            }
            std::sort(result.Secondaries.begin(), result.Secondaries.end(),
                [](const UiViewportSecondaryInfo& left, const UiViewportSecondaryInfo& right) { return left.ViewportId < right.ViewportId; });
            return result;
        }

        bool CaptureDrawDataOffscreen(ImDrawData* drawData, u32 width, u32 height, std::vector<u8>& outRgba)
        {
            outRgba.clear();
            const VkFormat format = m_WindowData.SurfaceFormat.format;
            const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM;
            if (!m_Initialized || !m_ImGuiInitialized || !drawData || width == 0 || height == 0
                || (!bgra && format != VK_FORMAT_R8G8B8A8_UNORM))
                return false;

            auto& vk = VULKAN_HPP_DEFAULT_DISPATCHER;
            // The ImGui vertex/index ring this draw reuses must not be read by the GPU.
            vk.vkDeviceWaitIdle(m_Device);

            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory imageMemory = VK_NULL_HANDLE;
            VkImageView imageView = VK_NULL_HANDLE;
            VkRenderPass renderPass = VK_NULL_HANDLE;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            VkBuffer readback = VK_NULL_HANDLE;
            VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
            VkCommandPool commandPool = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            struct ScopeExit
            {
                std::function<void()> Function;
                ~ScopeExit() { if (Function) Function(); }
            } cleanup;
            cleanup.Function = [&]
            {
                if (fence) vk.vkDestroyFence(m_Device, fence, nullptr);
                if (commandPool) vk.vkDestroyCommandPool(m_Device, commandPool, nullptr);
                if (readback) vk.vkDestroyBuffer(m_Device, readback, nullptr);
                if (readbackMemory) vk.vkFreeMemory(m_Device, readbackMemory, nullptr);
                if (framebuffer) vk.vkDestroyFramebuffer(m_Device, framebuffer, nullptr);
                if (renderPass) vk.vkDestroyRenderPass(m_Device, renderPass, nullptr);
                if (imageView) vk.vkDestroyImageView(m_Device, imageView, nullptr);
                if (image) vk.vkDestroyImage(m_Device, image, nullptr);
                if (imageMemory) vk.vkFreeMemory(m_Device, imageMemory, nullptr);
            };

            const auto allocate = [&](const VkMemoryRequirements& requirements, VkMemoryPropertyFlags required, VkDeviceMemory& outMemory)
            {
                VkPhysicalDeviceMemoryProperties properties {};
                vk.vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &properties);
                for (u32 type = 0; type < properties.memoryTypeCount; ++type)
                {
                    if (!(requirements.memoryTypeBits & (1u << type))
                        || (properties.memoryTypes[type].propertyFlags & required) != required)
                        continue;
                    VkMemoryAllocateInfo allocateInfo {};
                    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                    allocateInfo.allocationSize = requirements.size;
                    allocateInfo.memoryTypeIndex = type;
                    return vk.vkAllocateMemory(m_Device, &allocateInfo, nullptr, &outMemory) == VK_SUCCESS;
                }
                return false;
            };

            VkImageCreateInfo imageInfo {};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = { width, height, 1 };
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vk.vkCreateImage(m_Device, &imageInfo, nullptr, &image) != VK_SUCCESS)
                return false;
            VkMemoryRequirements imageRequirements {};
            vk.vkGetImageMemoryRequirements(m_Device, image, &imageRequirements);
            if (!allocate(imageRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, imageMemory)
                || vk.vkBindImageMemory(m_Device, image, imageMemory, 0) != VK_SUCCESS)
                return false;

            VkImageViewCreateInfo viewInfo {};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vk.vkCreateImageView(m_Device, &viewInfo, nullptr, &imageView) != VK_SUCCESS)
                return false;

            // Same attachment format, sample count and single-subpass shape as the
            // presentation pass the ImGui pipeline was built for, so the pipeline is
            // compatible; only the layouts differ.
            VkAttachmentDescription attachment {};
            attachment.format = format;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkAttachmentReference colorReference { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkSubpassDescription subpass {};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorReference;
            // The single dependency is identical to the presentation pass so the
            // ImGui pipeline built for that pass remains compatible.
            VkSubpassDependency dependency {};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo renderPassInfo {};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            renderPassInfo.attachmentCount = 1;
            renderPassInfo.pAttachments = &attachment;
            renderPassInfo.subpassCount = 1;
            renderPassInfo.pSubpasses = &subpass;
            renderPassInfo.dependencyCount = 1;
            renderPassInfo.pDependencies = &dependency;
            if (vk.vkCreateRenderPass(m_Device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS)
                return false;

            VkFramebufferCreateInfo framebufferInfo {};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &imageView;
            framebufferInfo.width = width;
            framebufferInfo.height = height;
            framebufferInfo.layers = 1;
            if (vk.vkCreateFramebuffer(m_Device, &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS)
                return false;

            const VkDeviceSize readbackBytes = static_cast<VkDeviceSize>(width) * height * 4u;
            VkBufferCreateInfo bufferInfo {};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = readbackBytes;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vk.vkCreateBuffer(m_Device, &bufferInfo, nullptr, &readback) != VK_SUCCESS)
                return false;
            VkMemoryRequirements bufferRequirements {};
            vk.vkGetBufferMemoryRequirements(m_Device, readback, &bufferRequirements);
            if (!allocate(bufferRequirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, readbackMemory)
                || vk.vkBindBufferMemory(m_Device, readback, readbackMemory, 0) != VK_SUCCESS)
                return false;

            VkCommandPoolCreateInfo poolInfo {};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = m_QueueFamily;
            if (vk.vkCreateCommandPool(m_Device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS)
                return false;
            VkCommandBufferAllocateInfo commandInfo {};
            commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            commandInfo.commandPool = commandPool;
            commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            commandInfo.commandBufferCount = 1;
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            if (vk.vkAllocateCommandBuffers(m_Device, &commandInfo, &commandBuffer) != VK_SUCCESS)
                return false;

            VkCommandBufferBeginInfo beginInfo {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vk.vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
                return false;
            VkClearValue clearValue {};
            clearValue.color.float32[3] = 1.0f;
            VkRenderPassBeginInfo passBegin {};
            passBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            passBegin.renderPass = renderPass;
            passBegin.framebuffer = framebuffer;
            passBegin.renderArea.extent = { width, height };
            passBegin.clearValueCount = 1;
            passBegin.pClearValues = &clearValue;
            vk.vkCmdBeginRenderPass(commandBuffer, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
            vk.vkCmdEndRenderPass(commandBuffer);
            VkImageMemoryBarrier toTransfer {};
            toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = image;
            toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vk.vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &toTransfer);
            VkBufferImageCopy copy {};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { width, height, 1 };
            vk.vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
            if (vk.vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
                return false;

            VkFenceCreateInfo fenceInfo {};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (vk.vkCreateFence(m_Device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
                return false;
            VkSubmitInfo submitInfo {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer;
            if (vk.vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, fence) != VK_SUCCESS)
            {
                vk.vkDeviceWaitIdle(m_Device);
                return false;
            }
            constexpr u64 kCaptureTimeoutNanoseconds = 10ull * 1000ull * 1000ull * 1000ull;
            if (vk.vkWaitForFences(m_Device, 1, &fence, VK_TRUE, kCaptureTimeoutNanoseconds) != VK_SUCCESS)
            {
                // The GPU may still own the resources; drain before they are destroyed.
                vk.vkDeviceWaitIdle(m_Device);
                return false;
            }

            void* mapped = nullptr;
            if (vk.vkMapMemory(m_Device, readbackMemory, 0, readbackBytes, 0, &mapped) != VK_SUCCESS || !mapped)
                return false;
            outRgba.resize(static_cast<size_t>(readbackBytes));
            std::memcpy(outRgba.data(), mapped, outRgba.size());
            vk.vkUnmapMemory(m_Device, readbackMemory);
            if (bgra)
            {
                for (size_t offset = 0; offset < outRgba.size(); offset += 4)
                    std::swap(outRgba[offset], outRgba[offset + 2]);
            }
            return true;
        }

        bool CreateOrResizeSwapchain(u32 width, u32 height)
        {
            if (width == 0 || height == 0)
                return false;
            if (!ResolveRequestedPresentMode())
                return false;
            VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle(m_Device);
            m_SubmittedFrameIds.clear();
            m_Ledger.ForgetOwner(0);
            ImGui_ImplVulkanH_CreateOrResizeWindow(
                m_Instance,
                m_PhysicalDevice,
                m_Device,
                &m_WindowData,
                m_QueueFamily,
                nullptr,
                static_cast<int>(width),
                static_cast<int>(height),
                kMinimumImageCount,
                0);
            if (m_WindowData.Swapchain == VK_NULL_HANDLE || m_WindowData.RenderPass == VK_NULL_HANDLE)
                return false;

            ++m_SwapchainGeneration;
            m_ActualPolicy = m_WindowData.PresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR
                ? PresentationPolicy::TearingAllowed : PresentationPolicy::Synchronized;
            m_PolicyDiagnostics.Backend = "NVRHI Vulkan";
            m_PolicyDiagnostics.Requested = m_RequestedPolicy;
            m_PolicyDiagnostics.Actual = m_ActualPolicy == PresentationPolicy::TearingAllowed
                ? PresentationActualMode::VulkanImmediate : PresentationActualMode::VulkanFifo;
            m_PolicyDiagnostics.SyncInterval = 0;
            m_PolicyDiagnostics.PresentAllowsTearing = m_ActualPolicy == PresentationPolicy::TearingAllowed;
            m_PolicyDiagnostics.SwapchainGeneration = m_SwapchainGeneration;
            m_PolicyDiagnostics.EffectiveApplicationFrame = Renderer::GetLastFrameTiming().FrameIndex;
            m_Transition.Commit();
            if (!m_SuppressPolicyMarker)
                Log::Info("PresentationPolicyV1 backend=Vulkan requested=", ToString(m_PolicyDiagnostics.Requested),
                    " capability=", m_PolicyDiagnostics.Capability, " actual=", ToString(m_PolicyDiagnostics.Actual),
                    " fallback=", m_PolicyDiagnostics.FallbackReason, " generation=", m_PolicyDiagnostics.SwapchainGeneration,
                    " effectiveFrame=", m_PolicyDiagnostics.EffectiveApplicationFrame);
            m_Timing.SwapchainGeneration = m_SwapchainGeneration;
            if (m_SwapchainGeneration > 1)
                Log::Info("Vulkan swapchain recreated after window resize (generation ", m_SwapchainGeneration, ")");
            return true;
        }

        bool ResolveRequestedPresentMode()
        {
            u32 count = 0;
            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_WindowData.Surface, &count, nullptr) != VK_SUCCESS || count == 0)
                return false;
            std::vector<VkPresentModeKHR> modes(count);
            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_WindowData.Surface, &count, modes.data()) != VK_SUCCESS)
                return false;
            std::vector<int> values;
            values.reserve(modes.size());
            for (VkPresentModeKHR mode : modes) values.push_back(static_cast<int>(mode));
            m_SurfacePresentModes = values;
            const VulkanPresentationResolution resolution = ResolveVulkanPresentationPolicy(m_RequestedPolicy, values,
                static_cast<int>(VK_PRESENT_MODE_FIFO_KHR), static_cast<int>(VK_PRESENT_MODE_IMMEDIATE_KHR));
            m_WindowData.PresentMode = resolution.Actual == PresentationActualMode::VulkanImmediate
                ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
            m_PolicyDiagnostics.Capability = resolution.Capability;
            m_PolicyDiagnostics.FallbackReason = resolution.FallbackReason;
            return true;
        }

        bool RecreateForRequestedPolicy(u32 width, u32 height)
        {
            const PresentationPolicy desired = m_RequestedPolicy;
            const PresentationPolicy prior = m_ActualPolicy;
            if (CreateOrResizeSwapchain(width, height))
                return true;
            m_RequestedPolicy = prior;
            m_SuppressPolicyMarker = true;
            const bool restored = CreateOrResizeSwapchain(width, height);
            m_SuppressPolicyMarker = false;
            m_RequestedPolicy = desired;
            m_PolicyDiagnostics.Requested = desired;
            if (restored)
            {
                m_PolicyDiagnostics.FallbackReason = "requested-recreation-failed-restored-prior-mode";
                m_PolicyDiagnostics.Requested = desired;
                m_PolicyDiagnostics.EffectiveApplicationFrame = Renderer::GetLastFrameTiming().FrameIndex;
                m_Transition.Request(desired);
                m_Transition.Commit();
                Log::Info("PresentationPolicyV1 backend=Vulkan requested=", ToString(m_PolicyDiagnostics.Requested),
                    " capability=", m_PolicyDiagnostics.Capability, " actual=", ToString(m_PolicyDiagnostics.Actual),
                    " fallback=", m_PolicyDiagnostics.FallbackReason, " generation=", m_PolicyDiagnostics.SwapchainGeneration,
                    " effectiveFrame=", m_PolicyDiagnostics.EffectiveApplicationFrame);
                return true;
            }
            m_PolicyDiagnostics.Actual = PresentationActualMode::Unavailable;
            m_PolicyDiagnostics.FallbackReason = "requested-and-restore-recreation-failed";
            m_PolicyDiagnostics.EffectiveApplicationFrame = Renderer::GetLastFrameTiming().FrameIndex;
            Log::Error("PresentationPolicyV1 backend=Vulkan requested=", ToString(m_PolicyDiagnostics.Requested),
                " capability=", m_PolicyDiagnostics.Capability, " actual=Unavailable fallback=", m_PolicyDiagnostics.FallbackReason,
                " generation=", m_PolicyDiagnostics.SwapchainGeneration, " effectiveFrame=", m_PolicyDiagnostics.EffectiveApplicationFrame);
            return false;
        }

        static constexpr u32 kMinimumImageCount = 2;
        RHI::NVRHIVulkanContext* m_Context = nullptr;
        GLFWwindow* m_Window = nullptr;
        VkInstance m_Instance = VK_NULL_HANDLE;
        VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
        VkDevice m_Device = VK_NULL_HANDLE;
        VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
        u32 m_QueueFamily = 0;
        VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
        VkSampler m_ViewportSampler = VK_NULL_HANDLE;
        VkImageView m_ViewportImageView = VK_NULL_HANDLE;
        ImGui_ImplVulkanH_Window m_WindowData;
        bool m_ImGuiInitialized = false;
        bool m_SwapchainInvalid = false;
        u64 m_ViewportTextureId = 0;
        u64 m_ViewportOutputGeneration = 0;
        u64 m_ViewportDescriptorGeneration = 0;
        u64 m_LastLoggedViewportOutputGeneration = 0;
        u64 m_LastLoggedViewportDescriptorGeneration = 0;
        u64 m_LastLoggedViewportSwapchainGeneration = 0;
        bool m_ViewportTextureQueued = false;
        struct SubmittedFrameAssociation { u64 ApplicationFrameIndex = 0; u64 SwapchainGeneration = 0; };
        std::unordered_map<u32, SubmittedFrameAssociation> m_SubmittedFrameIds;
        // Presentation serials (one per ImGui frame) and the fences that still
        // hold them: the main swapchain images and every detached viewport.
        PresentationSerialLedger m_Ledger;
        std::vector<int> m_SurfacePresentModes;
        std::thread::id m_OwnerThread;
        u32 m_InitImageCount = 0;
        bool m_UiViewportsActive = false;
        std::unordered_map<u32, SecondaryRecord> m_Secondaries;
        UiViewportDiagnostics m_ViewportDiagnostics;
        u64 m_SerialPollsHeldBySecondary = 0;
        double m_FrameRenderMilliseconds = 0.0;
        double m_FrameSwapMilliseconds = 0.0;
        void (*m_OriginalCreateWindow)(ImGuiViewport*) = nullptr;
        void (*m_OriginalDestroyWindow)(ImGuiViewport*) = nullptr;
        void (*m_OriginalSetWindowSize)(ImGuiViewport*, ImVec2) = nullptr;
        void (*m_OriginalRenderWindow)(ImGuiViewport*, void*) = nullptr;
        void (*m_OriginalSwapBuffers)(ImGuiViewport*, void*) = nullptr;
        // Dear ImGui handler pointers carry no user data, so the one active
        // presentation is reachable through this pointer (single renderer, main
        // thread).
        static Impl* s_ViewportImpl;
#endif
        RendererPresentationTiming m_Timing;
        u64 m_SuccessfulPresentCount = 0;
        u64 m_SwapchainGeneration = 0;
        u64 m_LastSuccessfulPresentGeneration = 0;
        bool m_Initialized = false;
        PresentationPolicy m_RequestedPolicy = PresentationPolicy::Synchronized;
        PresentationPolicy m_ActualPolicy = PresentationPolicy::Synchronized;
        PresentationPolicyTransitionState m_Transition;
        RendererPresentationPolicyDiagnostics m_PolicyDiagnostics;
        bool m_SuppressPolicyMarker = false;
    };

#if defined(GE_HAS_NVRHI_VULKAN)
    NVRHIVulkanPresentation::Impl* NVRHIVulkanPresentation::Impl::s_ViewportImpl = nullptr;
#endif

    NVRHIVulkanPresentation::NVRHIVulkanPresentation()
        : m_Impl(CreateScope<Impl>())
    {
    }

    NVRHIVulkanPresentation::~NVRHIVulkanPresentation()
    {
        Shutdown();
    }

    bool NVRHIVulkanPresentation::Initialize(RHI::NVRHIVulkanContext* context, void* nativeWindow, u32 width, u32 height)
    {
        return m_Impl->Initialize(context, nativeWindow, width, height);
    }

    void NVRHIVulkanPresentation::Shutdown()
    {
        m_Impl->Shutdown();
    }

    bool NVRHIVulkanPresentation::IsInitialized() const
    {
        return m_Impl->m_Initialized;
    }

    void NVRHIVulkanPresentation::BeginImGuiFrame()
    {
        m_Impl->BeginImGuiFrame();
    }

    void NVRHIVulkanPresentation::RenderImGuiDrawData(ImDrawData* drawData, const ClearColor& clearColor, u32 width, u32 height)
    {
        m_Impl->RenderImGuiDrawData(drawData, clearColor, width, height);
    }

    const RendererPresentationTiming& NVRHIVulkanPresentation::GetTiming() const
    {
        return m_Impl->m_Timing;
    }

    void NVRHIVulkanPresentation::SetPresentationPolicy(PresentationPolicy policy) { m_Impl->SetPresentationPolicy(policy); }
    const RendererPresentationPolicyDiagnostics& NVRHIVulkanPresentation::GetPresentationPolicyDiagnostics() const { return m_Impl->GetPresentationPolicyDiagnostics(); }

    u64 NVRHIVulkanPresentation::GetSuccessfulPresentCount() const
    {
        return m_Impl->m_SuccessfulPresentCount;
    }

    bool NVRHIVulkanPresentation::RegisterViewportOutput(const RHI::NVRHIVulkanTextureNativeHandles& handles, u64 outputGeneration) { return m_Impl->RegisterViewportOutput(handles, outputGeneration); }
    void NVRHIVulkanPresentation::ReleaseViewportOutput() { m_Impl->ReleaseViewportOutput(); }
    u64 NVRHIVulkanPresentation::GetViewportTextureId() const { return m_Impl->m_ViewportTextureId; }
    void NVRHIVulkanPresentation::MarkViewportTextureQueued(u64 textureId) { m_Impl->MarkViewportTextureQueued(textureId); }

    u64 NVRHIVulkanPresentation::RegisterUiTexture(RHI::Texture& texture)
    {
#if defined(GE_HAS_NVRHI_VULKAN)
        return m_Impl->RegisterUiTexture(texture);
#else
        (void)texture;
        return 0;
#endif
    }

    void NVRHIVulkanPresentation::UnregisterUiTexture(u64 imGuiId)
    {
#if defined(GE_HAS_NVRHI_VULKAN)
        m_Impl->UnregisterUiTexture(imGuiId);
#else
        (void)imGuiId;
#endif
    }

    u64 NVRHIVulkanPresentation::GetSubmittedPresentationSerial() const
    {
#if defined(GE_HAS_NVRHI_VULKAN)
        return m_Impl->m_Ledger.Submitted();
#else
        return 0;
#endif
    }

    u64 NVRHIVulkanPresentation::PollCompletedPresentationSerial()
    {
#if defined(GE_HAS_NVRHI_VULKAN)
        return m_Impl->PollCompletedPresentationSerial();
#else
        return 0;
#endif
    }

    const std::vector<int>& NVRHIVulkanPresentation::GetSurfacePresentModes() const
    {
#if defined(GE_HAS_NVRHI_VULKAN)
        return m_Impl->m_SurfacePresentModes;
#else
        static const std::vector<int> empty;
        return empty;
#endif
    }

    bool NVRHIVulkanPresentation::EnableUiViewports()
    {
#if defined(GE_HAS_NVRHI_VULKAN)
        return m_Impl->EnableUiViewports();
#else
        return false;
#endif
    }

    bool NVRHIVulkanPresentation::AreUiViewportsActive() const
    {
#if defined(GE_HAS_NVRHI_VULKAN)
        return m_Impl->m_UiViewportsActive;
#else
        return false;
#endif
    }

    void NVRHIVulkanPresentation::RenderUiPlatformWindows()
    {
#if defined(GE_HAS_NVRHI_VULKAN)
        m_Impl->RenderUiPlatformWindows();
#endif
    }

    UiViewportDiagnostics NVRHIVulkanPresentation::GetUiViewportDiagnostics() const
    {
#if defined(GE_HAS_NVRHI_VULKAN)
        return m_Impl->GetUiViewportDiagnostics();
#else
        return {};
#endif
    }

    bool NVRHIVulkanPresentation::CaptureDrawDataOffscreen(ImDrawData* drawData, u32 width, u32 height, std::vector<u8>& outRgba)
    {
#if defined(GE_HAS_NVRHI_VULKAN)
        return m_Impl->CaptureDrawDataOffscreen(drawData, width, height, outRgba);
#else
        (void)drawData;
        (void)width;
        (void)height;
        outRgba.clear();
        return false;
#endif
    }
}
