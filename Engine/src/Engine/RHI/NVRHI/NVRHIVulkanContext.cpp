#include "Engine/RHI/NVRHI/NVRHIVulkanContext.h"
#include "Engine/RHI/NVRHI/NVRHIVulkanDevice.h"

#if defined(GE_HAS_NVRHI_VULKAN)
    #define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "Engine/Core/Log.h"
#include "Engine/RHI/NVRHI/VulkanDispatch.h"

#if defined(GE_HAS_NVRHI_VULKAN)
    #include <GLFW/glfw3.h>
    #include <nvrhi/validation.h>
    #include <nvrhi/vulkan.h>
    #include <vulkan/vulkan_beta.h>

    #include <cstring>
    #include <iomanip>
    #include <limits>
    #include <sstream>
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

        AdapterType ConvertAdapterType(VkPhysicalDeviceType type)
        {
            switch (type)
            {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return AdapterType::Discrete;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return AdapterType::Integrated;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return AdapterType::Virtual;
                case VK_PHYSICAL_DEVICE_TYPE_CPU: return AdapterType::Cpu;
                case VK_PHYSICAL_DEVICE_TYPE_OTHER:
                default: return AdapterType::Unknown;
            }
        }

        FormatCapability QueryFormatCapability(VkPhysicalDevice device, Format format, VkFormat nativeFormat)
        {
            VkFormatProperties properties {};
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFormatProperties(device, nativeFormat, &properties);
            const VkFormatFeatureFlags features = properties.optimalTilingFeatures;

            FormatUsage usages = FormatUsage::None;
            if ((features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0)
                usages = usages | FormatUsage::Sampled;
            if ((features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0)
                usages = usages | FormatUsage::Storage;
            if ((features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0)
                usages = usages | FormatUsage::ColorAttachment;
            if ((features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
                usages = usages | FormatUsage::DepthStencil;
            if ((features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0)
                usages = usages | FormatUsage::CopySource;
            if ((features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0)
                usages = usages | FormatUsage::CopyDestination;

            FormatCapability result;
            result.Value = format;
            result.Usages = usages;
            result.SampleCountMask = 1;
            return result;
        }

        std::string FormatDeviceUuid(const u8* bytes)
        {
            std::ostringstream stream;
            stream << "vulkan-uuid-" << std::hex << std::setfill('0');
            for (u32 index = 0; index < VK_UUID_SIZE; ++index)
                stream << std::setw(2) << static_cast<u32>(bytes[index]);
            return stream.str();
        }

        constexpr const char* kPortabilitySubsetExtensionName = "VK_KHR_portability_subset";
    }
#endif

    struct NVRHIVulkanContext::Impl
    {
        bool Initialize(void* nativeWindow, const DeviceDescription& description, NVRHIAdapterInfo& adapterInfo)
        {
#if defined(GE_HAS_NVRHI_VULKAN)
            m_PortabilityEnumerationEnabled = false;
            m_PortabilitySubsetEnabled = false;
            m_Window = static_cast<GLFWwindow*>(nativeWindow);
            if (!m_Window || glfwGetWindowAttrib(m_Window, GLFW_CLIENT_API) != GLFW_NO_API)
            {
                Log::Warn("Vulkan requires a GLFW window created with GLFW_NO_API");
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

            // GLFW normally opens the platform Vulkan loader independently. Sharing the
            // entry point also supports MoltenVK's directly loaded dylib on macOS.
            glfwInitVulkanLoader(m_GetInstanceProcAddr);
            if (!glfwVulkanSupported())
            {
                Log::Warn("GLFW reports that the Vulkan loader or a Vulkan ICD is unavailable");
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

            u32 availableExtensionCount = 0;
            VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateInstanceExtensionProperties(
                nullptr, &availableExtensionCount, nullptr);
            std::vector<VkExtensionProperties> availableExtensions(availableExtensionCount);
            VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateInstanceExtensionProperties(
                nullptr, &availableExtensionCount, availableExtensions.data());

#if defined(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
            if (HasExtension(availableExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
            {
                bool alreadyRequired = false;
                for (const char* extension : m_InstanceExtensions)
                {
                    if (std::strcmp(extension, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0)
                    {
                        alreadyRequired = true;
                        break;
                    }
                }
                if (!alreadyRequired)
                    m_InstanceExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
                m_PortabilityEnumerationEnabled = true;
            }
#endif

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
            if (m_PortabilityEnumerationEnabled)
                instanceInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            instanceInfo.enabledExtensionCount = static_cast<u32>(m_InstanceExtensions.size());
            instanceInfo.ppEnabledExtensionNames = m_InstanceExtensions.data();

            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateInstance(&instanceInfo, nullptr, &m_Instance) != VK_SUCCESS)
            {
                Log::Error("Could not create Vulkan 1.3 instance");
                return false;
            }
            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Instance, m_GetInstanceProcAddr);

            if (m_PortabilityEnumerationEnabled)
                Log::Info("Vulkan portability enumeration enabled");

            if (glfwCreateWindowSurface(m_Instance, m_Window, nullptr, &m_Surface) != VK_SUCCESS)
            {
                Log::Error("Could not create the GLFW Vulkan window surface");
                return false;
            }

            if (!SelectPhysicalDevice(description))
                return false;
            LogPortabilitySubsetFeatures();
            if (!CreateLogicalDevice())
                return false;

            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Instance, m_GetInstanceProcAddr, m_Device);
            if (!CreateNVRHIDevice(description.EnableValidation))
                return false;

            adapterInfo.Available = true;
            adapterInfo.HasNativeDevice = true;
            adapterInfo.AdapterName = m_SelectedProperties.deviceName;
            adapterInfo.NativeBackendName = "Vulkan";

            m_Capabilities.ActiveBackend = Backend::NVRHIVulkan;
            m_Capabilities.ProfileName = "Phase 3 Vulkan Bootstrap Presentation V1";
            m_Capabilities.Identity = m_SelectedIdentity;
            m_Capabilities.Queues.Graphics = (m_SelectedQueueProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            m_Capabilities.Queues.Compute = (m_SelectedQueueProperties.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            m_Capabilities.Queues.Copy = (m_SelectedQueueProperties.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
            m_Capabilities.Queues.Present = true;
            m_Capabilities.Qualification = QualificationLevel::Bootstrap;
            m_Capabilities.Formats = m_SelectedFormats;
            m_Capabilities.AdapterCandidates = m_AdapterCandidates;
            m_Capabilities.AdapterSelection = m_AdapterSelection;

            const bool nvrhiRayTracingAdvertised =
                m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct)
                && m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline);
            const bool nvrhiMeshShadersAdvertised = m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::Meshlets);
            const bool nvrhiNeuralShadersAdvertised =
                m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::CooperativeVectorInferencing);
            m_Capabilities.GetFeature(DeviceFeature::RayTracing) = MakeCapabilityState(
                nvrhiRayTracingAdvertised, false, false, false,
                "NVRHI advertisement only; the bootstrap profile does not enable or implement ray tracing");
            m_Capabilities.GetFeature(DeviceFeature::MeshShaders) = MakeCapabilityState(
                nvrhiMeshShadersAdvertised, false, false, false,
                "NVRHI advertisement only; the bootstrap profile does not enable or implement mesh shaders");
            m_Capabilities.GetFeature(DeviceFeature::WorkGraphs) = MakeCapabilityState(
                false, false, false, false, "No Vulkan work-graph path is selected by the bootstrap profile");
            m_Capabilities.GetFeature(DeviceFeature::NeuralShaders) = MakeCapabilityState(
                nvrhiNeuralShadersAdvertised, false, false, false,
                "NVRHI advertisement only; the bootstrap profile does not enable or implement neural shaders");
            m_Capabilities.GetFeature(DeviceFeature::Timestamps) = MakeCapabilityState(
                m_SelectedQueueProperties.timestampValidBits > 0,
                m_SelectedQueueProperties.timestampValidBits > 0,
                false,
                false,
                "Selected queue timestampValidBits=" + std::to_string(m_SelectedQueueProperties.timestampValidBits)
                    + "; Vulkan timestamp recording is not implemented or exercised");
            m_Capabilities.GetFeature(DeviceFeature::TimelineSynchronization) = MakeCapabilityState(
                m_TimelineSemaphoreAdvertised, true, true, false,
                "Required and enabled for the NVRHI Vulkan bootstrap; no focused timeline exercise is recorded yet");
            m_Capabilities.GetFeature(DeviceFeature::DynamicRendering) = MakeCapabilityState(
                m_DynamicRenderingAdvertised, m_DynamicRenderingEnabled, true, false,
                "Required by NVRHI graphics pipelines and beginRendering; enabled in the Vulkan 1.3 feature chain");
            m_Capabilities.GetFeature(DeviceFeature::Synchronization2) = MakeCapabilityState(
                m_Synchronization2Advertised, m_Synchronization2Enabled, true, false,
                "Required by NVRHI state tracking pipelineBarrier2; enabled in the Vulkan 1.3 feature chain");
            m_Capabilities.GetFeature(DeviceFeature::BufferDeviceAddress) = MakeCapabilityState(
                m_BufferDeviceAddressAdvertised,
                m_BufferDeviceAddressEnabled,
                false,
                false,
                m_BufferDeviceAddressAdvertised
                    ? "Advertised but deliberately not enabled: the bootstrap profile has no implemented consumer"
                    : "Not advertised by the selected Vulkan device");

            m_Capabilities.Fallbacks = m_SelectionFallbacks;

            m_NativeHandles.Instance = m_Instance;
            m_NativeHandles.PhysicalDevice = m_PhysicalDevice;
            m_NativeHandles.Device = m_Device;
            m_NativeHandles.Surface = m_Surface;
            m_NativeHandles.GraphicsQueue = m_GraphicsQueue;
            m_NativeHandles.NVRHIDevice = m_NVRHIDevice.Get();
            m_NativeHandles.GraphicsQueueFamily = m_GraphicsQueueFamily;
            m_RHIDevice = CreateNVRHIVulkanDevice(
                description,
                m_Capabilities,
                m_NVRHIDevice.Get(),
                m_NativeNVRHIDevice.Get());
            if (!m_RHIDevice)
            {
                Log::Error("Could not create the Engine::RHI wrapper around the NVRHI Vulkan device");
                return false;
            }
            m_Initialized = true;

            Log::Info("NVRHI Vulkan device created on adapter: ", adapterInfo.AdapterName);
            Log::Info("Vulkan capability profile: ", m_Capabilities.ProfileName,
                ", qualification=", ToString(m_Capabilities.Qualification),
                ", type=", ToString(m_Capabilities.Identity.Type),
                ", vendor=", m_Capabilities.Identity.VendorId,
                ", device=", m_Capabilities.Identity.DeviceId,
                ", API=", VK_API_VERSION_MAJOR(m_SelectedProperties.apiVersion), ".",
                VK_API_VERSION_MINOR(m_SelectedProperties.apiVersion), ".",
                VK_API_VERSION_PATCH(m_SelectedProperties.apiVersion),
                ", driver=", m_Capabilities.Identity.DriverVersion);
            for (u32 featureIndex = 0; featureIndex < static_cast<u32>(DeviceFeature::Count); ++featureIndex)
            {
                const DeviceFeature feature = static_cast<DeviceFeature>(featureIndex);
                const CapabilityState& state = m_Capabilities.GetFeature(feature);
                Log::Info("Vulkan capability state: ", ToString(feature),
                    " advertised=", state.Advertised ? "yes" : "no",
                    ", enabled=", state.Enabled ? "yes" : "no",
                    ", implemented=", state.Implemented ? "yes" : "no",
                    ", exercised=", state.Exercised ? "yes" : "no",
                    state.Detail.empty() ? "" : ", detail=", state.Detail);
            }
            for (const std::string& fallback : m_Capabilities.Fallbacks)
                Log::Info("Vulkan capability fallback: ", fallback);
            return true;
#else
            (void)nativeWindow;
            (void)description;
            (void)adapterInfo;
            return false;
#endif
        }

        void Shutdown()
        {
#if defined(GE_HAS_NVRHI_VULKAN)
            WaitIdle();
            m_RHIDevice.reset();
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
        bool SelectPhysicalDevice(const DeviceDescription& description)
        {
            m_PhysicalDevice = VK_NULL_HANDLE;
            m_SelectedFormats.clear();
            m_SelectionFallbacks.clear();
            m_AdapterCandidates.clear();
            m_AdapterSelection = {};
            u32 deviceCount = 0;
            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0)
            {
                Log::Error("No Vulkan physical device is available");
                return false;
            }

            std::vector<VkPhysicalDevice> devices(deviceCount);
            if (VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data()) != VK_SUCCESS)
            {
                Log::Error("Could not enumerate Vulkan physical devices");
                return false;
            }

            struct NativeCandidate
            {
                VkPhysicalDevice Device = VK_NULL_HANDLE;
                VkPhysicalDeviceProperties Properties {};
                VkQueueFamilyProperties GraphicsQueueProperties {};
                u32 GraphicsQueueFamily = std::numeric_limits<u32>::max();
                bool BufferDeviceAddress = false;
                bool DynamicRendering = false;
                bool Synchronization2 = false;
                bool PortabilitySubset = false;
                std::vector<std::string> QueryNotes;
            };

            CapabilityProfile profile;
            profile.Name = "Phase 3 Vulkan Bootstrap Presentation V1";
            profile.MinimumApiMajor = 1;
            profile.MinimumApiMinor = 3;
            profile.MinimumTextureDimension2D = 4096;
            profile.RequirePresent = true;
            profile.RequireCompute = true;
            profile.RequireCopy = true;
            profile.AllowGraphicsQueueFallback = true;
            profile.RequireTimelineSynchronization = true;
            profile.RequireDynamicRendering = true;
            profile.RequireSynchronization2 = true;
            profile.AllowSoftwareAdapter = true;
            profile.RequiredFormats.push_back({
                Format::R8G8B8A8Unorm,
                FormatUsage::ColorAttachment | FormatUsage::CopySource
            });
            profile.RequiredFormats.push_back({ Format::D32Float, FormatUsage::DepthStencil });

            std::vector<AdapterCandidate> candidates;
            std::vector<NativeCandidate> nativeCandidates;
            candidates.reserve(devices.size());
            nativeCandidates.reserve(devices.size());
            for (VkPhysicalDevice device : devices)
            {
                AdapterCandidate candidate;
                NativeCandidate native;
                native.Device = device;
                VkPhysicalDeviceIDProperties idProperties {};
                idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
                VkPhysicalDeviceProperties2 properties2 {};
                properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                properties2.pNext = &idProperties;
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties2(device, &properties2);
                const VkPhysicalDeviceProperties& properties = properties2.properties;
                native.Properties = properties;

                candidate.CandidateBackend = Backend::NVRHIVulkan;
                candidate.Identity.Name = properties.deviceName;
                candidate.Identity.StableId = FormatDeviceUuid(idProperties.deviceUUID);
                candidate.Identity.DriverVersion =
                    "raw Vulkan driverVersion " + std::to_string(properties.driverVersion);
                candidate.Identity.VendorId = properties.vendorID;
                candidate.Identity.DeviceId = properties.deviceID;
                candidate.Identity.Type = ConvertAdapterType(properties.deviceType);
                candidate.ApiMajor = VK_API_VERSION_MAJOR(properties.apiVersion);
                candidate.ApiMinor = VK_API_VERSION_MINOR(properties.apiVersion);
                candidate.MaximumTextureDimension2D = properties.limits.maxImageDimension2D;
                candidate.PerformanceScore = static_cast<std::int64_t>(properties.limits.maxImageDimension2D);
                if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                    candidate.PerformanceScore += 1000000;
                else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
                    candidate.PerformanceScore += 500000;

                VkPhysicalDeviceMemoryProperties memoryProperties {};
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceMemoryProperties(device, &memoryProperties);
                for (u32 index = 0; index < memoryProperties.memoryHeapCount; ++index)
                {
                    if ((memoryProperties.memoryHeaps[index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
                        candidate.Identity.DedicatedVideoMemoryBytes += memoryProperties.memoryHeaps[index].size;
                }

                candidate.Formats.push_back(QueryFormatCapability(
                    device, Format::R8G8B8A8Unorm, VK_FORMAT_R8G8B8A8_UNORM));
                candidate.Formats.push_back(QueryFormatCapability(
                    device, Format::D32Float, VK_FORMAT_D32_SFLOAT));

                u32 extensionCount = 0;
                VkResult extensionResult = VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateDeviceExtensionProperties(
                    device, nullptr, &extensionCount, nullptr);
                std::vector<VkExtensionProperties> extensions(extensionCount);
                if (extensionResult == VK_SUCCESS)
                    extensionResult = VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateDeviceExtensionProperties(
                        device, nullptr, &extensionCount, extensions.data());
                const bool hasSwapchain = extensionResult == VK_SUCCESS
                    && HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                if (extensionResult != VK_SUCCESS)
                    native.QueryNotes.emplace_back("device-extension enumeration failed");
                else if (!hasSwapchain)
                    native.QueryNotes.emplace_back("VK_KHR_swapchain is unavailable");
                native.PortabilitySubset = HasExtension(extensions, kPortabilitySubsetExtensionName);

                u32 surfaceFormatCount = 0;
                u32 presentModeCount = 0;
                const VkResult surfaceFormatResult = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfaceFormatsKHR(
                    device, m_Surface, &surfaceFormatCount, nullptr);
                const VkResult presentModeResult = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfacePresentModesKHR(
                    device, m_Surface, &presentModeCount, nullptr);
                if (surfaceFormatResult != VK_SUCCESS || surfaceFormatCount == 0)
                    native.QueryNotes.emplace_back("no usable surface format was reported");
                if (presentModeResult != VK_SUCCESS || presentModeCount == 0)
                    native.QueryNotes.emplace_back("no usable presentation mode was reported");

                u32 queueFamilyCount = 0;
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
                std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

                for (u32 index = 0; index < queueFamilyCount; ++index)
                {
                    VkBool32 supportsPresent = VK_FALSE;
                    const VkResult supportResult = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfaceSupportKHR(
                        device, index, m_Surface, &supportsPresent);
                    const VkQueueFlags flags = queueFamilies[index].queueFlags;
                    if ((flags & VK_QUEUE_COMPUTE_BIT) != 0 && (flags & VK_QUEUE_GRAPHICS_BIT) == 0)
                        candidate.Queues.DedicatedCompute = true;
                    if ((flags & VK_QUEUE_TRANSFER_BIT) != 0
                        && (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0)
                        candidate.Queues.DedicatedCopy = true;
                    if (native.GraphicsQueueFamily == std::numeric_limits<u32>::max()
                        && supportResult == VK_SUCCESS
                        && (flags & VK_QUEUE_GRAPHICS_BIT) != 0
                        && supportsPresent)
                    {
                        native.GraphicsQueueFamily = index;
                        native.GraphicsQueueProperties = queueFamilies[index];
                    }
                }
                const bool hasCombinedGraphicsPresent =
                    native.GraphicsQueueFamily != std::numeric_limits<u32>::max();
                candidate.Queues.Graphics = hasCombinedGraphicsPresent;
                candidate.Queues.Present = hasCombinedGraphicsPresent
                    && hasSwapchain
                    && surfaceFormatResult == VK_SUCCESS && surfaceFormatCount > 0
                    && presentModeResult == VK_SUCCESS && presentModeCount > 0;
                // This bootstrap admits one unified queue. The evaluator records the
                // required compute/copy classes as explicit graphics-queue fallbacks.
                candidate.Queues.Compute = false;
                candidate.Queues.Copy = false;
                if (!hasCombinedGraphicsPresent)
                    native.QueryNotes.emplace_back("no queue family supports both graphics and presentation");

                VkPhysicalDeviceVulkan12Features vulkan12Features {};
                vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
                VkPhysicalDeviceVulkan13Features vulkan13Features {};
                vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
                vulkan12Features.pNext = &vulkan13Features;
                VkPhysicalDeviceFeatures2 features {};
                features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                features.pNext = &vulkan12Features;
                VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(device, &features);
                candidate.TimelineSynchronization = vulkan12Features.timelineSemaphore == VK_TRUE;
                native.BufferDeviceAddress = vulkan12Features.bufferDeviceAddress == VK_TRUE;
                candidate.DynamicRendering = vulkan13Features.dynamicRendering == VK_TRUE;
                candidate.Synchronization2 = vulkan13Features.synchronization2 == VK_TRUE;
                native.DynamicRendering = candidate.DynamicRendering;
                native.Synchronization2 = candidate.Synchronization2;
                native.QueryNotes.emplace_back(
                    "Vulkan 1.3 graphics features: dynamicRendering="
                    + std::string(native.DynamicRendering ? "yes" : "no")
                    + ", synchronization2=" + std::string(native.Synchronization2 ? "yes" : "no"));

                candidates.push_back(std::move(candidate));
                nativeCandidates.push_back(std::move(native));
            }

            const AdapterSelectionResult selection = EvaluateAdapterCandidates(
                profile,
                candidates,
                description.PreferredAdapterName,
                description.RequirePreferredAdapter);
            m_AdapterCandidates = candidates;
            m_AdapterSelection = selection;
            for (const AdapterEvaluation& evaluation : selection.Evaluations)
            {
                const AdapterCandidate& candidate = candidates[evaluation.CandidateIndex];
                const NativeCandidate& native = nativeCandidates[evaluation.CandidateIndex];
                for (const std::string& note : native.QueryNotes)
                    Log::Warn("Vulkan adapter '", candidate.Identity.Name, "': ", note);
                if (evaluation.Accepted)
                    Log::Info("Vulkan adapter accepted: ", candidate.Identity.Name,
                        " [", candidate.Identity.StableId, "] (score ", evaluation.Score, ")");
                else
                {
                    for (const std::string& reason : evaluation.RejectionReasons)
                        Log::Warn("Vulkan adapter rejected: ", candidate.Identity.Name,
                            " [", candidate.Identity.StableId, "] - ", reason);
                }
                for (const std::string& fallback : evaluation.Fallbacks)
                    Log::Info("Vulkan adapter fallback: ", candidate.Identity.Name, " - ", fallback);
            }

            if (!selection.HasSelection())
            {
                Log::Error("No Vulkan adapter satisfies capability profile '", profile.Name, "'");
                return false;
            }

            const AdapterCandidate& selectedCandidate = candidates[selection.SelectedIndex];
            const NativeCandidate& selectedNative = nativeCandidates[selection.SelectedIndex];
            if (description.RequirePreferredAdapter
                && (description.PreferredAdapterName.empty()
                    || (selectedCandidate.Identity.Name != description.PreferredAdapterName
                        && selectedCandidate.Identity.StableId != description.PreferredAdapterName)))
            {
                Log::Error("Required Vulkan adapter was not selected: '", description.PreferredAdapterName, "'");
                return false;
            }

            m_PhysicalDevice = selectedNative.Device;
            m_GraphicsQueueFamily = selectedNative.GraphicsQueueFamily;
            m_SelectedProperties = selectedNative.Properties;
            m_SelectedQueueProperties = selectedNative.GraphicsQueueProperties;
            m_SelectedIdentity = selectedCandidate.Identity;
            m_TimelineSemaphoreAdvertised = selectedCandidate.TimelineSynchronization;
            m_DynamicRenderingAdvertised = selectedNative.DynamicRendering;
            m_Synchronization2Advertised = selectedNative.Synchronization2;
            m_DynamicRenderingEnabled = false;
            m_Synchronization2Enabled = false;
            m_BufferDeviceAddressAdvertised = selectedNative.BufferDeviceAddress;
            m_BufferDeviceAddressEnabled = false;
            m_SelectedFormats = selectedCandidate.Formats;
            m_SelectionFallbacks = selection.Evaluations[selection.SelectedIndex].Fallbacks;
            if (selectedCandidate.Queues.DedicatedCompute)
                m_SelectionFallbacks.emplace_back("A dedicated compute queue is advertised but not enabled by the bootstrap profile");
            if (selectedCandidate.Queues.DedicatedCopy)
                m_SelectionFallbacks.emplace_back("A dedicated copy queue is advertised but not enabled by the bootstrap profile");
            if (!description.PreferredAdapterName.empty()
                && selectedCandidate.Identity.Name != description.PreferredAdapterName
                && selectedCandidate.Identity.StableId != description.PreferredAdapterName)
            {
                m_SelectionFallbacks.emplace_back(
                    "Preferred adapter '" + description.PreferredAdapterName
                    + "' was unavailable; selected '" + selectedCandidate.Identity.Name + "'");
            }

            m_DeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
            m_PortabilitySubsetEnabled = selectedNative.PortabilitySubset;
            if (m_PortabilitySubsetEnabled)
                m_DeviceExtensions.push_back(kPortabilitySubsetExtensionName);

            Log::Info("Selected Vulkan adapter: ", selectedCandidate.Identity.Name,
                " [", selectedCandidate.Identity.StableId, "] for profile '", profile.Name, "'");
            if (m_PortabilitySubsetEnabled)
                Log::Info("Vulkan portability subset device extension enabled");
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
            vulkan12Features.bufferDeviceAddress = m_BufferDeviceAddressEnabled ? VK_TRUE : VK_FALSE;
            VkPhysicalDeviceVulkan13Features vulkan13Features {};
            vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            vulkan13Features.dynamicRendering = m_DynamicRenderingAdvertised ? VK_TRUE : VK_FALSE;
            vulkan13Features.synchronization2 = m_Synchronization2Advertised ? VK_TRUE : VK_FALSE;
            vulkan12Features.pNext = &vulkan13Features;

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
            m_DynamicRenderingEnabled = m_DynamicRenderingAdvertised;
            m_Synchronization2Enabled = m_Synchronization2Advertised;
            return m_GraphicsQueue != VK_NULL_HANDLE;
        }

        void LogPortabilitySubsetFeatures() const
        {
            if (!m_PortabilitySubsetEnabled)
                return;

            VkPhysicalDevicePortabilitySubsetFeaturesKHR portabilityFeatures {};
            portabilityFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
            VkPhysicalDeviceFeatures2 features {};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &portabilityFeatures;
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &features);

            struct NamedFeature
            {
                const char* Name;
                VkBool32 Supported;
            };
            const NamedFeature namedFeatures[] = {
                { "constantAlphaColorBlendFactors", portabilityFeatures.constantAlphaColorBlendFactors },
                { "events", portabilityFeatures.events },
                { "imageViewFormatReinterpretation", portabilityFeatures.imageViewFormatReinterpretation },
                { "imageViewFormatSwizzle", portabilityFeatures.imageViewFormatSwizzle },
                { "imageView2DOn3DImage", portabilityFeatures.imageView2DOn3DImage },
                { "multisampleArrayImage", portabilityFeatures.multisampleArrayImage },
                { "mutableComparisonSamplers", portabilityFeatures.mutableComparisonSamplers },
                { "pointPolygons", portabilityFeatures.pointPolygons },
                { "samplerMipLodBias", portabilityFeatures.samplerMipLodBias },
                { "separateStencilMaskRef", portabilityFeatures.separateStencilMaskRef },
                { "shaderSampleRateInterpolationFunctions", portabilityFeatures.shaderSampleRateInterpolationFunctions },
                { "tessellationIsolines", portabilityFeatures.tessellationIsolines },
                { "tessellationPointMode", portabilityFeatures.tessellationPointMode },
                { "triangleFans", portabilityFeatures.triangleFans },
                { "vertexAttributeAccessBeyondStride", portabilityFeatures.vertexAttributeAccessBeyondStride }
            };

            std::ostringstream stream;
            bool hasUnsupportedFeature = false;
            for (const NamedFeature& feature : namedFeatures)
            {
                if (feature.Supported)
                    continue;
                stream << (hasUnsupportedFeature ? ", " : "") << feature.Name;
                hasUnsupportedFeature = true;
            }
            Log::Info("Vulkan portability subset unsupported features: ",
                hasUnsupportedFeature ? stream.str() : std::string("none"));
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
            description.bufferDeviceAddressSupported = m_BufferDeviceAddressEnabled;

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
        VkPhysicalDeviceProperties m_SelectedProperties {};
        VkQueueFamilyProperties m_SelectedQueueProperties {};
        AdapterIdentity m_SelectedIdentity;
        bool m_TimelineSemaphoreAdvertised = false;
        bool m_DynamicRenderingAdvertised = false;
        bool m_DynamicRenderingEnabled = false;
        bool m_Synchronization2Advertised = false;
        bool m_Synchronization2Enabled = false;
        bool m_BufferDeviceAddressAdvertised = false;
        bool m_BufferDeviceAddressEnabled = false;
        bool m_PortabilityEnumerationEnabled = false;
        bool m_PortabilitySubsetEnabled = false;
        std::vector<FormatCapability> m_SelectedFormats;
        std::vector<std::string> m_SelectionFallbacks;
        std::vector<AdapterCandidate> m_AdapterCandidates;
        AdapterSelectionResult m_AdapterSelection;
        std::vector<const char*> m_InstanceExtensions;
        std::vector<const char*> m_DeviceExtensions;
        NVRHIVulkanMessageCallback m_MessageCallback;
        nvrhi::vulkan::DeviceHandle m_NativeNVRHIDevice;
        nvrhi::DeviceHandle m_NVRHIDevice;
        Scope<Device> m_RHIDevice;
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

    bool NVRHIVulkanContext::Initialize(
        void* nativeWindow,
        const DeviceDescription& description,
        NVRHIAdapterInfo& adapterInfo)
    {
        return m_Impl->Initialize(nativeWindow, description, adapterInfo);
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

    Device* NVRHIVulkanContext::GetRHIDevice() const
    {
        return m_Impl->m_RHIDevice.get();
    }

    void* NVRHIVulkanContext::GetInstanceProcAddress(const char* name) const
    {
        return m_Impl->GetInstanceProcAddress(name);
    }
}
