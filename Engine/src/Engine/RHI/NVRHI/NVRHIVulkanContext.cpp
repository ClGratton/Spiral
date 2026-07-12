#include "Engine/RHI/NVRHI/NVRHIVulkanContext.h"

#include "Engine/Core/Log.h"
#include "Engine/RHI/NVRHI/VulkanDispatch.h"

#if defined(GE_HAS_NVRHI_VULKAN)
    #include <GLFW/glfw3.h>
    #include <nvrhi/validation.h>
    #include <nvrhi/vulkan.h>

    #include <cstring>
    #include <limits>
    #include <string>
    #include <vector>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#endif

namespace Engine::RHI
{
#if defined(GE_HAS_NVRHI_VULKAN)
    namespace
    {
#if VK_HEADER_VERSION >= 301
        using VulkanDynamicLoader = vk::detail::DynamicLoader;
#else
        using VulkanDynamicLoader = vk::DynamicLoader;
#endif

        class NVRHIVulkanMessageCallback final : public nvrhi::IMessageCallback
        {
        public:
            void message(nvrhi::MessageSeverity severity, const char* messageText) override
            {
                switch (severity)
                {
                    case nvrhi::MessageSeverity::Info: Log::Info("[NVRHI Vulkan] ", messageText); break;
                    case nvrhi::MessageSeverity::Warning: Log::Warn("[NVRHI Vulkan] ", messageText); break;
                    case nvrhi::MessageSeverity::Error:
                    case nvrhi::MessageSeverity::Fatal: Log::Error("[NVRHI Vulkan] ", messageText); break;
                }
            }
        };

        bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
        {
            for (const VkExtensionProperties& extension : extensions)
            {
                if (std::strcmp(extension.extensionName, name) == 0)
                    return true;
            }
            return false;
        }
    }
#endif

    struct NVRHIVulkanContext::Impl
    {
        bool Initialize(void* nativeWindow, bool enableValidation, NVRHIAdapterInfo& adapterInfo)
        {
#if defined(GE_HAS_NVRHI_VULKAN)
            m_Window = static_cast<GLFWwindow*>(nativeWindow);
            if (!m_Window || glfwGetWindowAttrib(m_Window, GLFW_CLIENT_API) != GLFW_NO_API)
            {
                Log::Warn("Vulkan requires a GLFW window created with GLFW_NO_API");
                return false;
            }

            if (!glfwVulkanSupported())
            {
                Log::Warn("GLFW reports that the Vulkan loader or a Vulkan ICD is unavailable");
                return false;
            }

            try
            {
                m_DynamicLoader = CreateScope<VulkanDynamicLoader>();
                m_GetInstanceProcAddr = m_DynamicLoader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
            }
            catch (const std::exception& exception)
            {
                Log::Error("Could not load Vulkan: ", exception.what());
                return false;
            }

            if (!m_GetInstanceProcAddr)
            {
                Log::Error("Vulkan loader does not export vkGetInstanceProcAddr");
                return false;
            }
            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_GetInstanceProcAddr);

            u32 requiredExtensionCount = 0;
            const char** requiredExtensions = glfwGetRequiredInstanceExtensions(&requiredExtensionCount);
            if (!requiredExtensions || requiredExtensionCount == 0)
            {
                Log::Error("GLFW did not provide Vulkan surface extensions");
                return false;
            }
            m_InstanceExtensions.assign(requiredExtensions, requiredExtensions + requiredExtensionCount);

            VkApplicationInfo applicationInfo {};
            applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            applicationInfo.pApplicationName = "Spiral";
            applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
            applicationInfo.pEngineName = "Spiral";
            applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
            applicationInfo.apiVersion = VK_API_VERSION_1_3;

            VkInstanceCreateInfo instanceInfo {};
            instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            instanceInfo.pApplicationInfo = &applicationInfo;
            instanceInfo.enabledExtensionCount = static_cast<u32>(m_InstanceExtensions.size());
            instanceInfo.ppEnabledExtensionNames = m_InstanceExtensions.data();

            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateInstance(&instanceInfo, nullptr, &m_Instance) != VK_SUCCESS)
            {
                Log::Error("Could not create Vulkan 1.3 instance");
                return false;
            }
            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Instance, m_GetInstanceProcAddr);

            if (glfwCreateWindowSurface(m_Instance, m_Window, nullptr, &m_Surface) != VK_SUCCESS)
            {
                Log::Error("Could not create the GLFW Vulkan window surface");
                return false;
            }

            if (!SelectPhysicalDevice())
                return false;
            if (!CreateLogicalDevice())
                return false;

            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Instance, m_GetInstanceProcAddr, m_Device);
            if (!CreateNVRHIDevice(enableValidation))
                return false;

            VkPhysicalDeviceProperties properties {};
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties(m_PhysicalDevice, &properties);
            adapterInfo.Available = true;
            adapterInfo.HasNativeDevice = true;
            adapterInfo.AdapterName = properties.deviceName;
            adapterInfo.NativeBackendName = "Vulkan";

            m_Capabilities.ActiveBackend = Backend::NVRHIVulkan;
            m_Capabilities.SupportsRayTracing = m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct)
                && m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline);
            m_Capabilities.SupportsMeshShaders = m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::Meshlets);
            m_Capabilities.SupportsNeuralShaders = m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::CooperativeVectorInferencing);

            m_NativeHandles.Instance = m_Instance;
            m_NativeHandles.PhysicalDevice = m_PhysicalDevice;
            m_NativeHandles.Device = m_Device;
            m_NativeHandles.Surface = m_Surface;
            m_NativeHandles.GraphicsQueue = m_GraphicsQueue;
            m_NativeHandles.NVRHIDevice = m_NVRHIDevice.Get();
            m_NativeHandles.GraphicsQueueFamily = m_GraphicsQueueFamily;
            m_Initialized = true;

            Log::Info("NVRHI Vulkan device created on adapter: ", adapterInfo.AdapterName);
            return true;
#else
            (void)nativeWindow;
            (void)enableValidation;
            (void)adapterInfo;
            return false;
#endif
        }

        void Shutdown()
        {
#if defined(GE_HAS_NVRHI_VULKAN)
            WaitIdle();
            m_NVRHIDevice.Reset();
            m_NativeNVRHIDevice.Reset();

            if (m_Device)
                VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyDevice(m_Device, nullptr);
            if (m_Surface)
                VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
            if (m_Instance)
                VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyInstance(m_Instance, nullptr);

            m_Device = VK_NULL_HANDLE;
            m_Surface = VK_NULL_HANDLE;
            m_Instance = VK_NULL_HANDLE;
            m_PhysicalDevice = VK_NULL_HANDLE;
            m_GraphicsQueue = VK_NULL_HANDLE;
            m_DynamicLoader.reset();
#endif
            m_NativeHandles = {};
            m_Capabilities = {};
            m_Initialized = false;
        }

        void WaitIdle()
        {
#if defined(GE_HAS_NVRHI_VULKAN)
            if (m_NVRHIDevice)
                m_NVRHIDevice->waitForIdle();
            else if (m_Device)
                VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle(m_Device);
#endif
        }

        void* GetInstanceProcAddress(const char* name) const
        {
#if defined(GE_HAS_NVRHI_VULKAN)
            return m_GetInstanceProcAddr && name
                ? reinterpret_cast<void*>(m_GetInstanceProcAddr(m_Instance, name))
                : nullptr;
#else
            (void)name;
            return nullptr;
#endif
        }

#if defined(GE_HAS_NVRHI_VULKAN)
        bool SelectPhysicalDevice()
        {
            u32 deviceCount = 0;
            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0)
            {
                Log::Error("No Vulkan physical device is available");
                return false;
            }

            std::vector<VkPhysicalDevice> devices(deviceCount);
            VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

            int bestScore = std::numeric_limits<int>::min();
            for (VkPhysicalDevice device : devices)
            {
                VkPhysicalDeviceProperties properties {};
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties(device, &properties);
                if (VK_API_VERSION_MAJOR(properties.apiVersion) < 1
                    || (VK_API_VERSION_MAJOR(properties.apiVersion) == 1 && VK_API_VERSION_MINOR(properties.apiVersion) < 3))
                    continue;

                u32 extensionCount = 0;
                VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
                std::vector<VkExtensionProperties> extensions(extensionCount);
                VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
                if (!HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                    continue;

                u32 surfaceFormatCount = 0;
                u32 presentModeCount = 0;
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfaceFormatsKHR(
                    device, m_Surface, &surfaceFormatCount, nullptr);
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfacePresentModesKHR(
                    device, m_Surface, &presentModeCount, nullptr);
                if (surfaceFormatCount == 0 || presentModeCount == 0)
                    continue;

                u32 queueFamilyCount = 0;
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
                std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

                u32 queueFamily = std::numeric_limits<u32>::max();
                for (u32 index = 0; index < queueFamilyCount; ++index)
                {
                    VkBool32 supportsPresent = VK_FALSE;
                    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfaceSupportKHR(device, index, m_Surface, &supportsPresent);
                    if ((queueFamilies[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && supportsPresent)
                    {
                        queueFamily = index;
                        break;
                    }
                }
                if (queueFamily == std::numeric_limits<u32>::max())
                    continue;

                VkPhysicalDeviceVulkan12Features vulkan12Features {};
                vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
                VkPhysicalDeviceFeatures2 features {};
                features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                features.pNext = &vulkan12Features;
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(device, &features);
                if (!vulkan12Features.timelineSemaphore)
                    continue;

                int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000 : 0;
                score += static_cast<int>(properties.limits.maxImageDimension2D);
                if (score > bestScore)
                {
                    bestScore = score;
                    m_PhysicalDevice = device;
                    m_GraphicsQueueFamily = queueFamily;
                    m_BufferDeviceAddressSupported = vulkan12Features.bufferDeviceAddress == VK_TRUE;
                    m_DeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
#if defined(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)
                    if (HasExtension(extensions, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
                        m_DeviceExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
                }
            }

            if (!m_PhysicalDevice)
            {
                Log::Error("No Vulkan 1.3 device supports graphics presentation and timeline semaphores");
                return false;
            }
            return true;
        }

        bool CreateLogicalDevice()
        {
            constexpr float queuePriority = 1.0f;
            VkDeviceQueueCreateInfo queueInfo {};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = m_GraphicsQueueFamily;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &queuePriority;

            VkPhysicalDeviceVulkan12Features vulkan12Features {};
            vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            vulkan12Features.timelineSemaphore = VK_TRUE;
            vulkan12Features.bufferDeviceAddress = m_BufferDeviceAddressSupported ? VK_TRUE : VK_FALSE;

            VkDeviceCreateInfo deviceInfo {};
            deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            deviceInfo.pNext = &vulkan12Features;
            deviceInfo.queueCreateInfoCount = 1;
            deviceInfo.pQueueCreateInfos = &queueInfo;
            deviceInfo.enabledExtensionCount = static_cast<u32>(m_DeviceExtensions.size());
            deviceInfo.ppEnabledExtensionNames = m_DeviceExtensions.data();

            const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDevice(m_PhysicalDevice, &deviceInfo, nullptr, &m_Device);
            if (result != VK_SUCCESS)
            {
                Log::Error("Could not create Vulkan logical device: ", static_cast<int>(result));
                return false;
            }
            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Instance, m_GetInstanceProcAddr, m_Device);
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceQueue(m_Device, m_GraphicsQueueFamily, 0, &m_GraphicsQueue);
            return m_GraphicsQueue != VK_NULL_HANDLE;
        }

        bool CreateNVRHIDevice(bool enableValidation)
        {
            nvrhi::vulkan::DeviceDesc description {};
            description.errorCB = &m_MessageCallback;
            description.instance = m_Instance;
            description.physicalDevice = m_PhysicalDevice;
            description.device = m_Device;
            description.graphicsQueue = m_GraphicsQueue;
            description.graphicsQueueIndex = static_cast<int>(m_GraphicsQueueFamily);
            description.instanceExtensions = m_InstanceExtensions.data();
            description.numInstanceExtensions = m_InstanceExtensions.size();
            description.deviceExtensions = m_DeviceExtensions.data();
            description.numDeviceExtensions = m_DeviceExtensions.size();
            description.bufferDeviceAddressSupported = m_BufferDeviceAddressSupported;

            m_NativeNVRHIDevice = nvrhi::vulkan::createDevice(description);
            if (!m_NativeNVRHIDevice)
            {
                Log::Error("Could not wrap the Vulkan device with NVRHI");
                return false;
            }

            m_NVRHIDevice = m_NativeNVRHIDevice;
            if (enableValidation)
            {
                nvrhi::DeviceHandle validationDevice = nvrhi::validation::createValidationLayer(m_NVRHIDevice.Get());
                if (validationDevice)
                    m_NVRHIDevice = validationDevice;
            }
            return true;
        }

        GLFWwindow* m_Window = nullptr;
        Scope<VulkanDynamicLoader> m_DynamicLoader;
        PFN_vkGetInstanceProcAddr m_GetInstanceProcAddr = nullptr;
        VkInstance m_Instance = VK_NULL_HANDLE;
        VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
        VkDevice m_Device = VK_NULL_HANDLE;
        VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
        VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
        u32 m_GraphicsQueueFamily = 0;
        bool m_BufferDeviceAddressSupported = false;
        std::vector<const char*> m_InstanceExtensions;
        std::vector<const char*> m_DeviceExtensions;
        NVRHIVulkanMessageCallback m_MessageCallback;
        nvrhi::vulkan::DeviceHandle m_NativeNVRHIDevice;
        nvrhi::DeviceHandle m_NVRHIDevice;
#endif
        NVRHIVulkanNativeHandles m_NativeHandles;
        DeviceCapabilities m_Capabilities;
        bool m_Initialized = false;
    };

    NVRHIVulkanContext::NVRHIVulkanContext()
        : m_Impl(CreateScope<Impl>())
    {
    }

    NVRHIVulkanContext::~NVRHIVulkanContext()
    {
        Shutdown();
    }

    bool NVRHIVulkanContext::Initialize(void* nativeWindow, bool enableValidation, NVRHIAdapterInfo& adapterInfo)
    {
        return m_Impl->Initialize(nativeWindow, enableValidation, adapterInfo);
    }

    void NVRHIVulkanContext::Shutdown()
    {
        m_Impl->Shutdown();
    }

    void NVRHIVulkanContext::WaitIdle()
    {
        m_Impl->WaitIdle();
    }

    bool NVRHIVulkanContext::IsInitialized() const
    {
        return m_Impl->m_Initialized;
    }

    const NVRHIVulkanNativeHandles& NVRHIVulkanContext::GetNativeHandles() const
    {
        return m_Impl->m_NativeHandles;
    }

    const DeviceCapabilities& NVRHIVulkanContext::GetCapabilities() const
    {
        return m_Impl->m_Capabilities;
    }

    void* NVRHIVulkanContext::GetInstanceProcAddress(const char* name) const
    {
        return m_Impl->GetInstanceProcAddress(name);
    }
}
