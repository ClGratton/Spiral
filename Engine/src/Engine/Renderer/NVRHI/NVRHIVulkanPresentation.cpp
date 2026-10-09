#include "Engine/Renderer/NVRHI/NVRHIVulkanPresentation.h"

#include "Engine/Core/Log.h"
#include "Engine/RHI/NVRHI/VulkanDispatch.h"

#if defined(GE_HAS_NVRHI_VULKAN)
    #include <GLFW/glfw3.h>
    #include <backends/imgui_impl_vulkan.h>

    #include <algorithm>
    #include <chrono>
    #include <cstring>
    #include <functional>
    #include <iterator>
    #include <stdexcept>
    #include <unordered_map>
#endif

namespace Engine
{
#if defined(GE_HAS_NVRHI_VULKAN)
    namespace
    {
        using Clock = std::chrono::steady_clock;

        void CheckVulkanResult(VkResult result)
        {
            if (result < 0)
                Log::Error("ImGui Vulkan backend reported VkResult ", static_cast<int>(result));
        }

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
            initInfo.CheckVkResultFn = CheckVulkanResult;
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
            m_InFlightSerialByImage.clear();
            ReleaseViewportOutput();
            if (m_ImGuiInitialized)
                ImGui_ImplVulkan_Shutdown();
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
            m_InFlightSerialByImage.erase(m_WindowData.FrameIndex);
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
            m_InFlightSerialByImage[m_WindowData.FrameIndex] = ++m_SubmittedSerial;
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

        u64 PollCompletedPresentationSerial()
        {
            if (!m_Device)
                return m_SubmittedSerial;
            u64 oldestInFlight = 0;
            for (auto it = m_InFlightSerialByImage.begin(); it != m_InFlightSerialByImage.end();)
            {
                bool finished = it->first >= m_WindowData.ImageCount;
                if (!finished)
                {
                    // Anything but not-ready is terminal (signalled or device lost).
                    finished = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetFenceStatus(
                        m_Device, m_WindowData.Frames[it->first].Fence) != VK_NOT_READY;
                }
                if (finished)
                {
                    it = m_InFlightSerialByImage.erase(it);
                    continue;
                }
                oldestInFlight = oldestInFlight == 0 ? it->second : std::min(oldestInFlight, it->second);
                ++it;
            }
            return oldestInFlight == 0 ? m_SubmittedSerial : oldestInFlight - 1;
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
            m_InFlightSerialByImage.clear();
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
        u64 m_SubmittedSerial = 0;
        // Presentation serial still in flight per swapchain image.
        std::unordered_map<u32, u64> m_InFlightSerialByImage;
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
        return m_Impl->m_SubmittedSerial;
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
