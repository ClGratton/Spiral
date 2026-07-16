#include "Engine/Core/Application.h"

#include "Engine/Core/Log.h"
#include "Engine/Jobs/FrameTaskGraph.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/UI/ImGuiLayer.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>

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
            const auto now = std::chrono::steady_clock::now();
            const std::chrono::duration<float> delta = now - lastFrameTime;
            lastFrameTime = now;
            Timestep timestep(delta.count());

            if (!m_Minimized)
            {
                Renderer::BeginFrame(m_FrameIndex);

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

            if (m_Specification.CommandLineArgs.HasFlag("--frame-lifecycle-telemetry-smoke")
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
                Log::Info("FrameLifecycleTelemetryV1 backend=", Renderer::GetActiveBackendName(),
                    " frame=", timing.FrameIndex,
                    " phases=frame-start,input-simulation,render-submission,present-begin,present-end",
                    " intentionalWait=not-applied:0",
                    " gpuCompletion=observed completedFrame=", timing.LastGpuCompletionObservedFrameIndex,
                    " completionSwapchainGeneration=", timing.Presentation.SwapchainGeneration,
                    " mandatoryWaits=", d3d12 ? "dxgi-latency" : "vulkan-acquire+fence",
                    " display=unavailable replacementDrop=unavailable result=pass");
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
                if (Renderer::GetActiveBackend() == RendererBackend::NVRHIVulkan
                    && (!m_Specification.CommandLineArgs.HasFlag("--frame-lifecycle-telemetry-smoke") || m_FrameLifecycleTelemetrySmokeComplete)
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
        if (m_Specification.CommandLineArgs.HasFlag("--frame-lifecycle-telemetry-smoke") && !m_FrameLifecycleTelemetrySmokeComplete)
            throw std::runtime_error("frame lifecycle telemetry smoke did not observe GPU completion within its frame budget");

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
