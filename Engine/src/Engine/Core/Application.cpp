#include "Engine/Core/Application.h"

#include "Engine/Core/Log.h"
#include "Engine/Jobs/FrameTaskGraph.h"
#include "Engine/RHI/Device.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/FramePacingBenchmark.h"
#include "Engine/UI/ImGuiLayer.h"

#include <chrono>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <thread>

#if defined(GE_PLATFORM_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOGDI
        #define NOGDI
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

namespace Engine
{
    namespace
    {
        struct ApplicationFrameInput
        {
            u64 FrameIndex = 0;
            Timestep Delta;
        };

        std::string DescribeFrameTaskFailure(const FrameTaskGraphResult& result)
        {
            if (!result.GraphError.empty())
                return result.GraphError;

            for (size_t index = 0; index < result.TaskStatuses.size(); ++index)
            {
                if (result.TaskStatuses[index] == FrameTaskStatus::Failed)
                    return result.TaskErrors[index];
            }
            return "a dependency did not succeed";
        }

        void ApplySmoothFrametimeSmokePolicy(const ApplicationCommandLineArgs& args)
        {
            if (args.HasFlag("--frame-pacing-benchmark-responsive"))
            {
                ResolvedFramePacingPolicy responsive = Renderer::GetFramePacingPolicy();
                responsive.ProjectMode = FramePacingMode::Responsive;
                responsive.RuntimeOverride = FramePacingOverride::Responsive;
                responsive.EffectiveMode = FramePacingMode::Responsive;
                responsive.SmoothTargetFramesPerSecond.reset();
                responsive.Behavior = "no-intentional-wait";
                Renderer::SetFramePacingPolicy(responsive);
                return;
            }
            if ((!args.HasFlag("--smooth-frametime-candidate-smoke") && !args.HasFlag("--frame-pacing-benchmark"))
                || args.HasFlag("--frame-pacing-benchmark-responsive"))
                return;

            ResolvedFramePacingPolicy policy = Renderer::GetFramePacingPolicy();
            policy.EffectiveMode = FramePacingMode::SmoothFrametime;
            policy.ProjectMode = FramePacingMode::SmoothFrametime;
            policy.RuntimeOverride = FramePacingOverride::SmoothFrametime;
            const std::string_view targetValue = args.GetOptionValue("--smooth-frametime-target-fps");
            const double target = targetValue.empty() ? 5.0 : std::strtod(std::string(targetValue).c_str(), nullptr);
            if (!IsValidSmoothTargetFramesPerSecond(target))
                throw std::runtime_error("Smooth Frametime candidate smoke target is invalid");
            policy.SmoothTargetFramesPerSecond = target;
            const std::string_view candidate = args.GetOptionValue("--smooth-frametime-candidate");
            if (candidate == "submission-gate")
                policy.Candidate = SmoothFrametimeCandidate::SubmissionGate;
            else if (candidate.empty() || candidate == "inter-frame")
                policy.Candidate = SmoothFrametimeCandidate::InterFrame;
            else
                throw std::runtime_error("Smooth Frametime candidate smoke selector is invalid");
            policy.Behavior = policy.Candidate == SmoothFrametimeCandidate::InterFrame
                ? "experimental-inter-frame" : "experimental-submission-gate";
            Renderer::SetFramePacingPolicy(policy);
        }

        std::filesystem::path AbsoluteNormalizedPath(std::filesystem::path path)
        {
            std::error_code error;
            const std::filesystem::path absolute = std::filesystem::absolute(std::move(path), error);
            return error ? std::filesystem::path() : absolute.lexically_normal();
        }

        std::string ReadTextFile(const std::filesystem::path& path)
        {
            std::ifstream input(path, std::ios::binary);
            return input ? std::string(std::istreambuf_iterator<char>(input), {}) : std::string();
        }

        u32 AttachmentTimeoutMilliseconds(const ApplicationCommandLineArgs& args)
        {
            const std::string_view value = args.GetOptionValue("--frame-pacing-benchmark-attachment-timeout-ms");
            if (value.empty())
                return 30000;
            const std::string timeoutText(value);
            char* end = nullptr;
            const long parsed = std::strtol(timeoutText.c_str(), &end, 10);
            if (end == timeoutText.c_str() || *end != '\0' || parsed < 100 || parsed > 60000)
                throw std::runtime_error("frame pacing attachment timeout must be in the inclusive 100-60000 ms range");
            return static_cast<u32>(parsed);
        }

        std::optional<FramePacingBenchmarkIdentity> WaitForFramePacingAttachment(const Application& application)
        {
            const ApplicationCommandLineArgs& args = application.GetSpecification().CommandLineArgs;
            const std::string_view readinessValue = args.GetOptionValue("--frame-pacing-benchmark-attachment-readiness");
            if (readinessValue.empty())
                return std::nullopt;
            if (!args.HasFlag("--frame-pacing-benchmark"))
                throw std::runtime_error("frame pacing attachment requires --frame-pacing-benchmark");
            const std::string_view releaseValue = args.GetOptionValue("--frame-pacing-benchmark-attachment-release");
            const std::string_view artifactValue = args.GetOptionValue("--frame-pacing-benchmark-output");
            if (releaseValue.empty() || artifactValue.empty())
                throw std::runtime_error("frame pacing attachment requires release and benchmark-output paths");
#if !defined(GE_PLATFORM_WINDOWS)
            throw std::runtime_error("frame pacing attachment requires Windows QueryPerformanceCounter identity");
#else
            LARGE_INTEGER frequency {}, tick {};
            if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&tick) || frequency.QuadPart <= 0 || tick.QuadPart <= 0)
                throw std::runtime_error("frame pacing attachment could not query Windows performance counter identity");
            std::array<char, 32768> executable {};
            const DWORD length = GetModuleFileNameA(nullptr, executable.data(), static_cast<DWORD>(std::size(executable)));
            if (length == 0 || length >= std::size(executable))
                throw std::runtime_error("frame pacing attachment could not resolve canonical executable path");
            const std::filesystem::path readinessPath = AbsoluteNormalizedPath(std::filesystem::path(readinessValue));
            const std::filesystem::path releasePath = AbsoluteNormalizedPath(std::filesystem::path(releaseValue));
            const std::filesystem::path artifactPath = AbsoluteNormalizedPath(std::filesystem::path(artifactValue));
            if (readinessPath.empty() || releasePath.empty() || artifactPath.empty())
                throw std::runtime_error("frame pacing attachment paths must be resolvable");
            const u32 processId = static_cast<u32>(GetCurrentProcessId());
            const std::string runId = "frame-pacing-" + std::to_string(processId) + "-" + std::to_string(static_cast<u64>(tick.QuadPart));
            FramePacingBenchmarkCondition condition;
            condition.RunId = runId;
            condition.ProcessId = processId;
            condition.ExecutablePath = AbsoluteNormalizedPath(std::filesystem::path(executable.data())).string();
            condition.QpcFrequency = static_cast<u64>(frequency.QuadPart);
            condition.Backend = Renderer::GetActiveBackendName();
            condition.TargetFramesPerSecond = std::strtod(std::string(args.GetOptionValue("--smooth-frametime-target-fps")).c_str(), nullptr);
            condition.Policy = Renderer::GetFramePacingPolicy();
            const auto conditionValue = [&](std::string_view option) { const std::string_view value = args.GetOptionValue(option); return value.empty() ? std::string("unknown") : std::string(value); };
            condition.PresentationMode = conditionValue("--frame-pacing-benchmark-presentation");
            condition.SyncMode = conditionValue("--frame-pacing-benchmark-sync");
            condition.VrrMode = conditionValue("--frame-pacing-benchmark-vrr");
            condition.TearingMode = conditionValue("--frame-pacing-benchmark-tearing");
            if (const RHI::DeviceCapabilities* capabilities = Renderer::GetDeviceCapabilities())
            {
                condition.Adapter = capabilities->Identity.Name;
                condition.AdapterStableId = capabilities->Identity.StableId;
            }
            FramePacingBenchmarkAttachmentReadiness readiness;
            readiness.RunId = runId;
            readiness.ProcessId = processId;
            readiness.ExecutablePath = condition.ExecutablePath;
            readiness.QpcFrequency = condition.QpcFrequency;
            readiness.QpcTick = static_cast<u64>(tick.QuadPart);
            readiness.BenchmarkArtifactPath = artifactPath.string();
            readiness.Condition = std::move(condition);
            std::string error;
            if (!FramePacingBenchmarkCapture::WriteAttachmentReadiness(readiness, readinessPath, error))
                throw std::runtime_error("frame pacing attachment readiness publish failed: " + error);
            Log::Info("FramePacingAttachmentV1 state=ready runId=", readiness.RunId, " pid=", readiness.ProcessId,
                " qpcFrequency=", readiness.QpcFrequency, " qpcTick=", readiness.QpcTick);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(AttachmentTimeoutMilliseconds(args));
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (std::filesystem::exists(releasePath))
                {
                    if (!FramePacingBenchmarkCapture::IsValidAttachmentRelease(readiness, ReadTextFile(releasePath), error))
                    {
                        Log::Error("FramePacingAttachmentV1 state=rejected reason=", error);
                        throw std::runtime_error("frame pacing attachment release rejected: " + error);
                    }
                    Log::Info("FramePacingAttachmentV1 state=released runId=", readiness.RunId, " pid=", readiness.ProcessId);
                    return FramePacingBenchmarkIdentity { readiness.RunId, readiness.ProcessId, readiness.ExecutablePath, readiness.QpcFrequency };
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            Log::Error("FramePacingAttachmentV1 state=timeout runId=", readiness.RunId, " pid=", readiness.ProcessId);
            throw std::runtime_error("frame pacing attachment timed out waiting for supervisor release");
#endif
        }
    }

    Application* Application::s_Instance = nullptr;

    const char* ApplicationCommandLineArgs::operator[](int index) const
    {
        if (index < 0 || index >= Count)
            throw std::out_of_range("Application command-line argument index is out of range");
        return Args[index];
    }

    bool ApplicationCommandLineArgs::HasFlag(std::string_view flag) const
    {
        for (int i = 0; i < Count; ++i)
        {
            if (Args[i] == flag)
                return true;
        }

        return false;
    }

    std::string_view ApplicationCommandLineArgs::GetOptionValue(std::string_view option) const
    {
        const std::string prefix = std::string(option) + "=";
        for (int i = 0; i < Count; ++i)
        {
            const std::string_view argument = Args[i];
            if (argument.starts_with(prefix))
                return argument.substr(prefix.size());
            if (argument == option && i + 1 < Count)
                return Args[i + 1];
        }

        return {};
    }

    Application::Application(ApplicationSpecification specification)
        : m_Specification(std::move(specification))
    {
        if (s_Instance)
            throw std::logic_error("Application already exists");
        s_Instance = this;

        try
        {
            if (!m_Specification.WorkingDirectory.empty())
                std::filesystem::current_path(m_Specification.WorkingDirectory);

            m_Specification.Window.Title = m_Specification.Name;
            m_Window = Window::Create(m_Specification.Window);
            m_Window->SetEventCallback(GE_BIND_EVENT_FN(Application::OnEvent));

            Renderer::Initialize();
        }
        catch (...)
        {
            s_Instance = nullptr;
            throw;
        }
    }

    Application::~Application()
    {
        Renderer::Shutdown();
        s_Instance = nullptr;
    }

    Application& Application::Get()
    {
        if (!s_Instance)
            throw std::logic_error("Application has not been created");
        return *s_Instance;
    }

    void Application::Run()
    {
        Log::Info("Running application: ", m_Specification.Name);

        auto lastFrameTime = std::chrono::steady_clock::now();

        while (m_Running && !m_Window->ShouldClose())
        {
            ApplySmoothFrametimeSmokePolicy(m_Specification.CommandLineArgs);
            if (!m_FramePacingAttachmentCheckResolved)
            {
                m_FramePacingAttachmentCheckResolved = true;
                if (const std::optional<FramePacingBenchmarkIdentity> identity = WaitForFramePacingAttachment(*this))
                {
                    m_FramePacingAttachmentReleased = true;
                    m_FramePacingAttachmentRunId = identity->RunId;
                    m_FramePacingAttachmentProcessId = identity->ProcessId;
                    m_FramePacingAttachmentExecutablePath = identity->ExecutablePath;
                    m_FramePacingAttachmentQpcFrequency = identity->QpcFrequency;
                }
            }
            constexpr u32 benchmarkWarmupFrames = 30;
            if (m_Specification.CommandLineArgs.HasFlag("--frame-pacing-benchmark") && !m_FramePacingBenchmarkStarted && m_FrameIndex >= benchmarkWarmupFrames)
            {
                const std::string_view targetValue = m_Specification.CommandLineArgs.GetOptionValue("--smooth-frametime-target-fps");
                const auto conditionValue = [&](std::string_view option)
                {
                    const std::string_view value = m_Specification.CommandLineArgs.GetOptionValue(option);
                    return value.empty() ? std::string("unknown") : std::string(value);
                };
                Renderer::BeginFramePacingBenchmark(512, targetValue.empty() ? 0.0 : std::strtod(std::string(targetValue).c_str(), nullptr), benchmarkWarmupFrames,
                    conditionValue("--frame-pacing-benchmark-presentation"), conditionValue("--frame-pacing-benchmark-sync"),
                    conditionValue("--frame-pacing-benchmark-vrr"), conditionValue("--frame-pacing-benchmark-tearing"),
                    { m_FramePacingAttachmentRunId, m_FramePacingAttachmentProcessId,
                        m_FramePacingAttachmentExecutablePath, m_FramePacingAttachmentQpcFrequency });
                m_FramePacingBenchmarkStarted = true;
            }
            const FramePacingWaitResult preFramePacing = Renderer::WaitForInterFrameBeforeFrame();
            const auto now = std::chrono::steady_clock::now();
            const std::chrono::duration<float> delta = now - lastFrameTime;
            lastFrameTime = now;
            Timestep timestep(delta.count());

            if (!m_Minimized)
            {
                Renderer::BeginFrame(m_FrameIndex, preFramePacing);

                FramePublication<ApplicationFrameInput> frameInput;
                FrameTaskGraph frameTasks;

                FrameTaskDescription publishInput;
                publishInput.Name = "Frame.PublishInput";
                publishInput.Lane = FrameTaskLane::CallingThread;
                publishInput.Execute = [&]() { frameInput.Stage({ m_FrameIndex, timestep }); };
                publishInput.Publication = frameInput.GetState();
                const FrameTaskId publishInputTask = frameTasks.AddTask(std::move(publishInput));

                FrameTaskDescription updateLayers;
                updateLayers.Name = "Frame.UpdateLayers";
                updateLayers.Lane = FrameTaskLane::CallingThread;
                updateLayers.Dependencies = { publishInputTask };
                updateLayers.Execute = [&]()
                {
                    Renderer::RecordFrameLifecyclePhase(m_FrameIndex, RendererFrameLifecyclePhase::InputSimulation);
                    const std::shared_ptr<const ApplicationFrameInput> input = frameInput.Read();
                    if (!input)
                        throw std::logic_error("frame input was not published");
                    for (auto& layer : m_LayerStack)
                        layer->OnUpdate(input->Delta);
                };
                const FrameTaskId updateTask = frameTasks.AddTask(std::move(updateLayers));

                FrameTaskDescription prepareSceneRaster;
                prepareSceneRaster.Name = "Frame.PrepareSceneRaster";
                prepareSceneRaster.Dependencies = { updateTask };
                prepareSceneRaster.Lane = FrameTaskLane::Worker;
                prepareSceneRaster.Execute = []()
                {
                    if (!Renderer::PrepareCurrentSceneRasterFrame())
                        throw std::logic_error("scene raster preparation did not receive a scene snapshot");
                };
                const FrameTaskId prepareSceneRasterTask = frameTasks.AddTask(std::move(prepareSceneRaster));

                FrameTaskDescription renderLayers;
                renderLayers.Name = "Frame.RenderLayers";
                renderLayers.Lane = FrameTaskLane::CallingThread;
                renderLayers.Dependencies = { prepareSceneRasterTask };
                renderLayers.Execute = [&]()
                {
                    // Editor settings publish during InputSimulation. Reapply the
                    // explicit benchmark condition before render/submission so the
                    // recorded condition cannot silently drift to project UI state.
                    ApplySmoothFrametimeSmokePolicy(m_Specification.CommandLineArgs);
                    for (auto& layer : m_LayerStack)
                        layer->OnRender();
                };
                frameTasks.AddTask(std::move(renderLayers));

                size_t completedProfileEvents = 0;
                FrameTaskExecutionOptions taskOptions;
                taskOptions.FrameIndex = m_FrameIndex;
                taskOptions.Mode = m_Specification.CommandLineArgs.HasFlag("--frame-task-single-thread")
                    ? FrameTaskExecutionMode::DeterministicSingleThread
                    : FrameTaskExecutionMode::Parallel;
                const bool sceneRasterPreparationSmoke = m_Specification.CommandLineArgs.HasFlag("--scene-raster-preparation-smoke");
                std::optional<FrameTaskProfileEvent> sceneRasterPreparationEvent;
                if (m_Specification.CommandLineArgs.HasFlag("--frame-task-graph-smoke") || sceneRasterPreparationSmoke)
                {
                    taskOptions.ProfileHook = [&](const FrameTaskProfileEvent& event)
                    {
                        if (event.Phase == FrameTaskProfilePhase::End)
                            ++completedProfileEvents;
                        if (event.Phase == FrameTaskProfilePhase::End && event.Task == prepareSceneRasterTask)
                            sceneRasterPreparationEvent = event;
                    };
                }

                const FrameTaskGraphResult taskResult = frameTasks.Execute(JobSystem::Get(), taskOptions);
                if (!taskResult.Succeeded())
                    throw std::runtime_error("CPU frame task graph failed: " + DescribeFrameTaskFailure(taskResult));
                if (m_Specification.CommandLineArgs.HasFlag("--frame-task-graph-smoke") && m_FrameIndex == 0)
                {
                    const std::shared_ptr<const ApplicationFrameInput> publishedInput = frameInput.Read();
                    if (!publishedInput || publishedInput->FrameIndex != m_FrameIndex || completedProfileEvents != frameTasks.GetTaskCount())
                        throw std::runtime_error("CPU frame task graph smoke did not publish or profile every task");
                    Log::Info("CPU frame task graph smoke passed: frame=", m_FrameIndex,
                        ", tasks=", frameTasks.GetTaskCount(),
                        ", mode=", taskOptions.Mode == FrameTaskExecutionMode::DeterministicSingleThread ? "single-thread" : "parallel");
                }
                if (sceneRasterPreparationSmoke && m_FrameIndex == 0)
                {
                    const std::shared_ptr<const SceneRenderSnapshot> snapshot = Renderer::GetSceneRenderSnapshot();
                    const std::shared_ptr<const SceneRasterFrame> prepared = Renderer::GetPreparedSceneRasterFrame();
                    const bool expectedWorker = taskOptions.Mode == FrameTaskExecutionMode::Parallel;
                    if (!snapshot || !prepared || !sceneRasterPreparationEvent
                        || prepared->SnapshotFrameIndex != snapshot->FrameIndex
                        || (expectedWorker && sceneRasterPreparationEvent->WorkerIndex == kInvalidJobWorkerIndex)
                        || (!expectedWorker && sceneRasterPreparationEvent->WorkerIndex != kInvalidJobWorkerIndex))
                    {
                        throw std::runtime_error("scene raster preparation smoke did not use the declared task lane or publish its immutable frame");
                    }
                    Log::Info("SceneRasterPreparationV1 mode=",
                        expectedWorker ? "parallel" : "single-thread",
                        " task=Frame.PrepareSceneRaster worker=",
                        sceneRasterPreparationEvent->WorkerIndex == kInvalidJobWorkerIndex ? "caller" : std::to_string(sceneRasterPreparationEvent->WorkerIndex),
                        " snapshot=", prepared->SnapshotFrameIndex,
                        " instances=", prepared->Instances.size(),
                        " result=pass");
                }

                Renderer::EndFrame();
            }

            if (m_ImGuiLayer && !m_Minimized)
                m_ImGuiLayer->Begin();

            for (auto& layer : m_LayerStack)
                layer->OnUiRender();

            if (m_ImGuiLayer && !m_Minimized)
                m_ImGuiLayer->End();

            m_Window->OnUpdate();

            if ((m_Specification.CommandLineArgs.HasFlag("--frame-lifecycle-telemetry-smoke")
                    || m_Specification.CommandLineArgs.HasFlag("--smooth-frametime-candidate-smoke"))
                && !m_FrameLifecycleTelemetrySmokeComplete
                && Renderer::GetLastFrameTiming().HasGpuCompletionObservation)
            {
                const RendererFrameTiming& timing = Renderer::GetLastFrameTiming();
                const bool d3d12 = Renderer::GetActiveBackend() == RendererBackend::NVRHID3D12;
                const bool vulkan = Renderer::GetActiveBackend() == RendererBackend::NVRHIVulkan;
                if (!HasValidFrameLifecycleTelemetry(timing, d3d12, vulkan))
                {
                    throw std::runtime_error("frame lifecycle telemetry smoke did not preserve the required trace/fallback contract");
                }
                const bool pacingSmoke = m_Specification.CommandLineArgs.HasFlag("--smooth-frametime-candidate-smoke");
                bool intentionalWaitApplied = !pacingSmoke;
                for (const RendererFrameWaitTiming& wait : timing.Waits)
                    intentionalWaitApplied |= wait.Kind == RendererFrameWaitKind::IntentionalPacing && wait.Applied && wait.Milliseconds > 0.0;
                if (pacingSmoke && !intentionalWaitApplied)
                    throw std::runtime_error("Smooth Frametime candidate smoke did not observe a nonzero intentional wait");
                Log::Info(pacingSmoke ? "SmoothFrametimeCandidateSmokeV1 backend=" : "FrameLifecycleTelemetryV1 backend=", Renderer::GetActiveBackendName(),
                    " frame=", timing.FrameIndex,
                    " phases=frame-start,input-simulation,render-submission,present-begin,present-end",
                    " candidate=", ToString(timing.FramePacingPolicy.Candidate),
                    " intentionalWait=", pacingSmoke ? std::to_string(timing.IntentionalPacingMilliseconds) : "not-applied:0",
                    " startToStartMs=", timing.StartToStartMilliseconds,
                    " cpuActiveMs=", timing.CpuActiveMilliseconds,
                    " gpuCompletion=observed completedFrame=", timing.LastGpuCompletionObservedFrameIndex,
                    " completionSwapchainGeneration=", timing.Presentation.SwapchainGeneration,
                    " mandatoryWaits=", d3d12 ? "dxgi-latency" : "vulkan-acquire+fence",
                    " inputLatency=unavailable display=unavailable replacementDrop=unavailable result=pass");
                m_FrameLifecycleTelemetrySmokeComplete = true;
            }

            if (m_Specification.CommandLineArgs.HasFlag("--vulkan-render-smoke") && m_FrameIndex == 0)
            {
                const u32 resizedWidth = m_Window->GetWidth() > 128 ? m_Window->GetWidth() - 64 : m_Window->GetWidth();
                const u32 resizedHeight = m_Window->GetHeight() > 128 ? m_Window->GetHeight() - 64 : m_Window->GetHeight();
                m_Window->SetSize(resizedWidth, resizedHeight);
                Log::Info("Vulkan render smoke requested window resize to ", resizedWidth, "x", resizedHeight);
            }

            if (m_Specification.CommandLineArgs.HasFlag("--vulkan-render-smoke"))
            {
                const RendererPresentationTiming& presentation = Renderer::GetLastFrameTiming().Presentation;
                const bool requiresLifecycleObservation = m_Specification.CommandLineArgs.HasFlag("--frame-lifecycle-telemetry-smoke")
                    || m_Specification.CommandLineArgs.HasFlag("--smooth-frametime-candidate-smoke");
                const bool requiresGraphGpuTiming = m_Specification.CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke");
                const bool graphGpuTimingComplete = !requiresGraphGpuTiming
                    || Renderer::GetLastCompletedFrameTiming().GpuStatus == RendererTimingStatus::Ready;
                if (Renderer::GetActiveBackend() == RendererBackend::NVRHIVulkan
                    && (!requiresLifecycleObservation || m_FrameLifecycleTelemetrySmokeComplete)
                    && graphGpuTimingComplete
                    && presentation.SwapchainGeneration >= 2
                    && presentation.LastSuccessfulPresentGeneration == presentation.SwapchainGeneration)
                {
                    Close();
                }
            }

            ++m_FrameIndex;
            if (m_Specification.MaxFrames != 0 && m_FrameIndex >= m_Specification.MaxFrames)
                Close();
        }

        if (m_Specification.CommandLineArgs.HasFlag("--vulkan-render-smoke"))
        {
            const RendererFrameTiming& timing = Renderer::GetLastFrameTiming();
            if (Renderer::GetActiveBackend() != RendererBackend::NVRHIVulkan
                || timing.Presentation.SwapchainGeneration < 2
                || timing.Presentation.LastSuccessfulPresentGeneration != timing.Presentation.SwapchainGeneration)
            {
                Log::Error("Vulkan render smoke stopped at swapchain generation ",
                    timing.Presentation.SwapchainGeneration,
                    " with last successful present generation ",
                    timing.Presentation.LastSuccessfulPresentGeneration);
                throw std::runtime_error("Vulkan render smoke did not complete a successful presentation after resize");
            }
            Log::Info("Vulkan render smoke verified native ImGui presentation after resize");
        }
        if ((m_Specification.CommandLineArgs.HasFlag("--frame-lifecycle-telemetry-smoke")
                || m_Specification.CommandLineArgs.HasFlag("--smooth-frametime-candidate-smoke")) && !m_FrameLifecycleTelemetrySmokeComplete)
            throw std::runtime_error("frame lifecycle telemetry smoke did not observe GPU completion within its frame budget");
        if (m_Specification.CommandLineArgs.HasFlag("--frame-pacing-benchmark"))
        {
            const std::shared_ptr<const FramePacingBenchmarkSnapshot> snapshot = Renderer::GetFramePacingBenchmarkSnapshot();
            const std::string_view output = m_Specification.CommandLineArgs.GetOptionValue("--frame-pacing-benchmark-output");
            std::string error;
            if (!snapshot || snapshot->Frames.size() < 2 || output.empty() || !FramePacingBenchmarkCapture::WriteArtifacts(*snapshot, std::filesystem::path(output), error))
                throw std::runtime_error("frame pacing benchmark capture did not produce a complete artifact: " + error);
            Log::Info("FramePacingBenchmarkV1 frames=", snapshot->Frames.size(), " p99Ms=", snapshot->Summary.StartToStartP99Milliseconds,
                " display=unavailable inputLatency=unavailable gpuHeadroom=unavailable result=pass");
        }

        Log::Info("Application stopped after ", m_FrameIndex, " frame(s)");
    }

    void Application::OnEvent(Event& event)
    {
        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<WindowCloseEvent>(GE_BIND_EVENT_FN(Application::OnWindowClose));
        dispatcher.Dispatch<WindowResizeEvent>(GE_BIND_EVENT_FN(Application::OnWindowResize));

        for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it)
        {
            if (event.Handled)
                break;

            (*it)->OnEvent(event);
        }
    }

    Layer* Application::PushLayer(Scope<Layer> layer)
    {
        return m_LayerStack.PushLayer(std::move(layer));
    }

    Layer* Application::PushOverlay(Scope<Layer> layer)
    {
        return m_LayerStack.PushOverlay(std::move(layer));
    }

    void Application::SetImGuiLayer(ImGuiLayer* layer)
    {
        m_ImGuiLayer = layer;
    }

    void Application::Close()
    {
        m_Running = false;
        m_Window->RequestClose();
    }

    bool Application::OnWindowClose(WindowCloseEvent& event)
    {
        (void)event;
        Close();
        return true;
    }

    bool Application::OnWindowResize(WindowResizeEvent& event)
    {
        m_Minimized = event.GetWidth() == 0 || event.GetHeight() == 0;
        return false;
    }
}
