#include "Engine/Renderer/NVRHI/NVRHIVulkanPresentation.h"

#include "Engine/Core/Log.h"
#include "Engine/RHI/NVRHI/VulkanDispatch.h"

#if defined(GE_HAS_NVRHI_VULKAN)
    #include <GLFW/glfw3.h>
    #include <backends/imgui_impl_vulkan.h>

    #include <chrono>
    #include <iterator>
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
            const VkPresentModeKHR requestedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
            m_WindowData.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
                m_PhysicalDevice, m_WindowData.Surface, &requestedPresentMode, 1);

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

        void RenderImGuiDrawData(ImDrawData* drawData, const ClearColor& clearColor, u32 width, u32 height)
        {
#if defined(GE_HAS_NVRHI_VULKAN)
            m_Timing = {};
            if (!m_Initialized || !drawData)
                return;

            int framebufferWidth = static_cast<int>(width);
            int framebufferHeight = static_cast<int>(height);
            glfwGetFramebufferSize(m_Window, &framebufferWidth, &framebufferHeight);
            if (framebufferWidth <= 0 || framebufferHeight <= 0)
                return;
            width = static_cast<u32>(framebufferWidth);
            height = static_cast<u32>(framebufferHeight);

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
            VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkAcquireNextImageKHR(
                m_Device,
                m_WindowData.Swapchain,
                UINT64_MAX,
                semaphores.ImageAcquiredSemaphore,
                VK_NULL_HANDLE,
                &m_WindowData.FrameIndex);
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
            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitForFences(m_Device, 1, &frame.Fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
                return;
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
            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, frame.Fence) != VK_SUCCESS)
                return;

            VkPresentInfoKHR presentInfo {};
            presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            presentInfo.waitSemaphoreCount = 1;
            presentInfo.pWaitSemaphores = &semaphores.RenderCompleteSemaphore;
            presentInfo.swapchainCount = 1;
            presentInfo.pSwapchains = &m_WindowData.Swapchain;
            presentInfo.pImageIndices = &m_WindowData.FrameIndex;

            const Clock::time_point presentStart = Clock::now();
            result = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueuePresentKHR(m_GraphicsQueue, &presentInfo);
            m_Timing.PresentMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - presentStart).count();
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
                m_SwapchainInvalid = true;
            else if (result == VK_SUCCESS)
            {
                m_Timing.PresentSucceeded = true;
                ++m_SuccessfulPresentCount;
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

        bool CreateOrResizeSwapchain(u32 width, u32 height)
        {
            if (width == 0 || height == 0)
                return false;
            VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle(m_Device);
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
            return m_WindowData.Swapchain != VK_NULL_HANDLE && m_WindowData.RenderPass != VK_NULL_HANDLE;
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
        ImGui_ImplVulkanH_Window m_WindowData;
        bool m_ImGuiInitialized = false;
        bool m_SwapchainInvalid = false;
#endif
        RendererPresentationTiming m_Timing;
        u64 m_SuccessfulPresentCount = 0;
        bool m_Initialized = false;
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

    u64 NVRHIVulkanPresentation::GetSuccessfulPresentCount() const
    {
        return m_Impl->m_SuccessfulPresentCount;
    }
}
