#include "Engine/Core/Log.h"
#include "Engine/Jobs/FrameTaskGraph.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/RHI/Device.h"
#include "Engine/Renderer/CapabilityDiagnostics.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    using TestFunction = std::function<bool()>;

    bool Expect(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "  Expected: " << message << '\n';
        return condition;
    }

    std::filesystem::path TestFilePath(std::string_view name)
    {
        return std::filesystem::temp_directory_path() / ("spiral-" + std::string(name));
    }

    bool TestJobSystemContainsWorkerExceptions()
    {
        Engine::JobSystem& jobs = Engine::JobSystem::Get();
        jobs.Initialize(1);

        std::atomic<bool> followingJobRan = false;
        jobs.Submit([]() { throw std::runtime_error("expected test exception"); }, "throwing test job");
        jobs.Submit([&]() { followingJobRan = true; }, "following test job");
        jobs.WaitIdle();
        jobs.Shutdown();

        return Expect(followingJobRan, "a worker remains usable after a job throws");
    }

    bool TestJobSystemInlineFallbackIsReentrant()
    {
        Engine::JobSystem& jobs = Engine::JobSystem::Get();
        jobs.Shutdown();

        bool fallbackRan = false;
        jobs.Submit([&]() { fallbackRan = !jobs.IsRunning(); }, "inline fallback test");
        return Expect(fallbackRan, "inline fallback work can query the job system without deadlocking");
    }

    bool TestJobSystemStealsNestedWorkerJobs()
    {
        Engine::JobSystem& jobs = Engine::JobSystem::Get();
        jobs.Shutdown();
        jobs.Initialize(2);

        constexpr int childCount = 32;
        std::atomic<int> completedChildren = 0;
        std::atomic<bool> timedOut = false;
        jobs.Submit([&]()
        {
            for (int index = 0; index < childCount; ++index)
            {
                jobs.Submit([&]() { ++completedChildren; }, "stealable child");
            }

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (completedChildren.load() != childCount && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            timedOut = completedChildren.load() != childCount;
        }, "nested producer");
        jobs.WaitIdle();

        const Engine::JobSystemStatistics statistics = jobs.GetStatistics();
        jobs.Shutdown();
        return Expect(!timedOut, "another worker steals nested jobs while their producer is occupied")
            && Expect(completedChildren == childCount, "all stolen nested jobs complete")
            && Expect(statistics.StolenJobs > 0, "work stealing is observable in job-system statistics")
            && Expect(statistics.SubmittedJobs == statistics.CompletedJobs, "submitted and completed job counts agree");
    }

    bool TestWorkerWaitAndNestedGraphAreSafe()
    {
        Engine::JobSystem& jobs = Engine::JobSystem::Get();
        jobs.Shutdown();
        jobs.Initialize(1);

        std::atomic<bool> waitRejected = false;
        std::atomic<bool> shutdownRejected = false;
        std::atomic<bool> nestedGraphCompleted = false;
        jobs.Submit([&]()
        {
            try
            {
                jobs.WaitIdle();
            }
            catch (const std::logic_error&)
            {
                waitRejected = true;
            }
            try
            {
                jobs.Shutdown();
            }
            catch (const std::logic_error&)
            {
                shutdownRejected = true;
            }

            Engine::FrameTaskGraph nestedGraph;
            Engine::FrameTaskDescription nestedTask;
            nestedTask.Name = "Nested";
            nestedTask.Execute = [&]() { nestedGraphCompleted = true; };
            nestedGraph.AddTask(std::move(nestedTask));
            const Engine::FrameTaskGraphResult nestedResult = nestedGraph.Execute(jobs);
            if (!nestedResult.Succeeded())
                nestedGraphCompleted = false;
        }, "worker wait and nested graph");
        jobs.WaitIdle();
        jobs.Shutdown();

        return Expect(waitRejected, "worker-side global WaitIdle fails clearly instead of deadlocking")
            && Expect(shutdownRejected, "worker-side shutdown fails clearly instead of joining itself")
            && Expect(nestedGraphCompleted, "a nested graph falls back to execution on its current worker");
    }

    bool TestFrameTaskGraphPublishesDeterministically()
    {
        Engine::JobSystem& jobs = Engine::JobSystem::Get();
        jobs.Shutdown();
        jobs.Initialize(2);

        Engine::FramePublication<std::vector<int>> publication;
        Engine::FrameTaskGraph graph;
        std::vector<int> executionOrder;
        std::vector<Engine::FrameTaskProfileEvent> profileEvents;
        const std::thread::id callerThread = std::this_thread::get_id();

        Engine::FrameTaskDescription producer;
        producer.Name = "Producer";
        producer.Execute = [&]()
        {
            executionOrder.push_back(1);
            publication.Stage({ 4, 8, 15, 16, 23, 42 });
        };
        producer.Publication = publication.GetState();
        const Engine::FrameTaskId producerId = graph.AddTask(std::move(producer));

        Engine::FrameTaskDescription consumer;
        consumer.Name = "Consumer";
        consumer.Dependencies = { producerId };
        consumer.Execute = [&]()
        {
            const std::shared_ptr<const std::vector<int>> values = publication.Read();
            if (!values || values->size() != 6 || values->back() != 42)
                throw std::runtime_error("consumer did not receive the immutable publication");
            executionOrder.push_back(2);
        };
        graph.AddTask(std::move(consumer));

        Engine::FrameTaskExecutionOptions options;
        options.Mode = Engine::FrameTaskExecutionMode::DeterministicSingleThread;
        options.FrameIndex = 77;
        options.ProfileHook = [&](const Engine::FrameTaskProfileEvent& event) { profileEvents.push_back(event); };
        const Engine::FrameTaskGraphResult result = graph.Execute(jobs, options);
        jobs.Shutdown();

        const bool profileEventsValid = profileEvents.size() == 4
            && std::all_of(profileEvents.begin(), profileEvents.end(), [&](const Engine::FrameTaskProfileEvent& event)
            {
                return event.FrameIndex == 77
                    && event.Thread == callerThread
                    && event.WorkerIndex == Engine::kInvalidJobWorkerIndex;
            });
        return Expect(result.Succeeded(), "the deterministic frame task graph succeeds")
            && Expect(executionOrder == std::vector<int>({ 1, 2 }), "deterministic mode follows stable dependency order")
            && Expect(profileEventsValid, "begin/end profile events retain frame and caller-thread identity");
    }

    bool TestFrameTaskGraphPropagatesFailure()
    {
        Engine::JobSystem& jobs = Engine::JobSystem::Get();
        jobs.Shutdown();
        jobs.Initialize(2);

        Engine::FramePublication<int> failedPublication;
        Engine::FrameTaskGraph graph;
        bool dependentRan = false;
        bool independentRan = false;

        Engine::FrameTaskDescription failing;
        failing.Name = "Failing producer";
        failing.Execute = [&]()
        {
            failedPublication.Stage(9);
            throw std::runtime_error("expected graph failure");
        };
        failing.Publication = failedPublication.GetState();
        const Engine::FrameTaskId failingId = graph.AddTask(std::move(failing));

        Engine::FrameTaskDescription dependent;
        dependent.Name = "Dependent";
        dependent.Dependencies = { failingId };
        dependent.Execute = [&]() { dependentRan = true; };
        const Engine::FrameTaskId dependentId = graph.AddTask(std::move(dependent));

        Engine::FrameTaskDescription independent;
        independent.Name = "Independent";
        independent.Execute = [&]() { independentRan = true; };
        const Engine::FrameTaskId independentId = graph.AddTask(std::move(independent));

        const Engine::FrameTaskGraphResult result = graph.Execute(jobs);
        jobs.Shutdown();
        return Expect(!result.Succeeded(), "a task failure fails the graph result")
            && Expect(result.TaskStatuses[failingId] == Engine::FrameTaskStatus::Failed, "the throwing task retains failed status")
            && Expect(result.TaskErrors[failingId] == "expected graph failure", "the throwing task retains its diagnostic")
            && Expect(result.TaskStatuses[dependentId] == Engine::FrameTaskStatus::Skipped && !dependentRan,
                "a failed dependency skips its dependent")
            && Expect(result.TaskStatuses[independentId] == Engine::FrameTaskStatus::Succeeded && independentRan,
                "an independent branch still completes")
            && Expect(!failedPublication.Read(), "failed work never reaches its publication callback");
    }

    bool TestFrameTaskGraphSchedulesFanInAndFanOut()
    {
        Engine::JobSystem& jobs = Engine::JobSystem::Get();
        jobs.Shutdown();
        jobs.Initialize(2);

        Engine::FrameTaskGraph graph;
        std::atomic<bool> firstRootComplete = false;
        std::atomic<bool> secondRootComplete = false;
        std::atomic<bool> branchComplete = false;
        bool joined = false;
        std::vector<Engine::FrameTaskProfileEvent> profileEvents;

        Engine::FrameTaskDescription firstRoot;
        firstRoot.Name = "First root";
        firstRoot.Execute = [&]() { firstRootComplete = true; };
        const Engine::FrameTaskId firstRootId = graph.AddTask(std::move(firstRoot));

        Engine::FrameTaskDescription secondRoot;
        secondRoot.Name = "Second root";
        secondRoot.Execute = [&]() { secondRootComplete = true; };
        const Engine::FrameTaskId secondRootId = graph.AddTask(std::move(secondRoot));

        Engine::FrameTaskDescription branch;
        branch.Name = "Branch";
        branch.Dependencies = { firstRootId };
        branch.Execute = [&]()
        {
            if (!firstRootComplete)
                throw std::runtime_error("branch ran before its root");
            branchComplete = true;
        };
        const Engine::FrameTaskId branchId = graph.AddTask(std::move(branch));

        Engine::FrameTaskDescription join;
        join.Name = "Join";
        join.Dependencies = { branchId, secondRootId };
        join.Execute = [&]()
        {
            if (!branchComplete || !secondRootComplete)
                throw std::runtime_error("join ran before all dependencies");
            joined = true;
        };
        graph.AddTask(std::move(join));

        Engine::FrameTaskExecutionOptions options;
        options.ProfileHook = [&](const Engine::FrameTaskProfileEvent& event) { profileEvents.push_back(event); };
        const Engine::FrameTaskGraphResult result = graph.Execute(jobs, options);
        jobs.Shutdown();

        const bool workerIdentityObserved = std::any_of(profileEvents.begin(), profileEvents.end(), [](const Engine::FrameTaskProfileEvent& event)
        {
            return event.Phase == Engine::FrameTaskProfilePhase::End
                && event.WorkerIndex != Engine::kInvalidJobWorkerIndex;
        });
        return Expect(result.Succeeded() && joined, "fan-out branches join only after all declared dependencies")
            && Expect(workerIdentityObserved, "parallel task profile events identify their worker");
    }

    bool TestFrameTaskGraphRejectsCycles()
    {
        Engine::FrameTaskGraph graph;
        Engine::FrameTaskDescription first;
        first.Name = "First";
        first.Execute = []() {};
        first.Dependencies = { 1 };
        graph.AddTask(std::move(first));

        Engine::FrameTaskDescription second;
        second.Name = "Second";
        second.Execute = []() {};
        second.Dependencies = { 0 };
        graph.AddTask(std::move(second));

        Engine::FrameTaskExecutionOptions options;
        options.Mode = Engine::FrameTaskExecutionMode::DeterministicSingleThread;
        const Engine::FrameTaskGraphResult result = graph.Execute(Engine::JobSystem::Get(), options);
        return Expect(!result.Succeeded(), "a cyclic frame task graph is rejected")
            && Expect(result.GraphError == "frame task graph contains a dependency cycle", "cycle rejection is deterministic");
    }

    bool TestFrameTaskGraphRejectsInvalidDependencies()
    {
        Engine::FrameTaskExecutionOptions options;
        options.Mode = Engine::FrameTaskExecutionMode::DeterministicSingleThread;

        Engine::FrameTaskGraph invalidGraph;
        Engine::FrameTaskDescription invalid;
        invalid.Name = "Invalid";
        invalid.Execute = []() {};
        invalid.Dependencies = { 7 };
        invalidGraph.AddTask(std::move(invalid));
        const Engine::FrameTaskGraphResult invalidResult = invalidGraph.Execute(Engine::JobSystem::Get(), options);

        Engine::FrameTaskGraph selfGraph;
        Engine::FrameTaskDescription self;
        self.Name = "Self";
        self.Execute = []() {};
        self.Dependencies = { 0 };
        selfGraph.AddTask(std::move(self));
        const Engine::FrameTaskGraphResult selfResult = selfGraph.Execute(Engine::JobSystem::Get(), options);

        Engine::FrameTaskGraph duplicateGraph;
        Engine::FrameTaskDescription root;
        root.Name = "Root";
        root.Execute = []() {};
        duplicateGraph.AddTask(std::move(root));
        Engine::FrameTaskDescription duplicate;
        duplicate.Name = "Duplicate";
        duplicate.Execute = []() {};
        duplicate.Dependencies = { 0, 0 };
        duplicateGraph.AddTask(std::move(duplicate));
        const Engine::FrameTaskGraphResult duplicateResult = duplicateGraph.Execute(Engine::JobSystem::Get(), options);

        return Expect(invalidResult.GraphError == "task 'Invalid' has an invalid dependency", "invalid dependency rejection is stable")
            && Expect(selfResult.GraphError == "task 'Self' depends on itself", "self-dependency rejection is stable")
            && Expect(duplicateResult.GraphError == "task 'Duplicate' declares a duplicate dependency",
                "duplicate dependency rejection is stable");
    }

    bool TestSceneRoundTrip()
    {
        const std::filesystem::path path = TestFilePath("scene-round-trip.spiral");
        Engine::Scene source("Round Trip");
        Engine::CameraComponent camera = source.GetMainCamera();
        camera.BackgroundColor = { 0.21f, 0.34f, 0.55f };
        source.SetMainCamera(camera);
        const Engine::Entity cube = source.CreateEntity("Cube");
        Engine::TransformComponent* cubeTransform = source.TryGetTransform(cube);
        cubeTransform->Position = { 1000000000000.25, -999999999999.5, 500000000000.125 };
        Engine::MeshRendererComponent mesh;
        mesh.MeshAsset = 42;
        mesh.MaterialAsset = 84;
        mesh.MeshName = "Test Mesh";
        source.AddMeshRendererComponent(cube, mesh);

        Engine::Scene loaded;
        const bool result = source.SaveToFile(path) && Engine::Scene::LoadFromFile(path, loaded);
        std::error_code error;
        std::filesystem::remove(path, error);

        const Engine::Entity loadedCube = loaded.FindEntityByName("Cube");
        const Engine::TransformComponent* loadedTransform = loaded.TryGetTransform(loadedCube);
        const Engine::MeshRendererComponent* loadedMesh = loaded.TryGetMeshRendererComponent(loadedCube);
        return Expect(result, "a valid scene saves and loads")
            && Expect(loaded.GetName() == "Round Trip", "the scene name round trips")
            && Expect(loadedMesh && loadedMesh->MeshAsset == 42 && loadedMesh->MaterialAsset == 84, "mesh handles round trip")
            && Expect(loadedTransform
                    && loadedTransform->Position.X == cubeTransform->Position.X
                    && loadedTransform->Position.Y == cubeTransform->Position.Y
                    && loadedTransform->Position.Z == cubeTransform->Position.Z,
                "large double-precision positions round trip without loss")
            && Expect(loaded.GetMainCamera().BackgroundColor.X == 0.21f
                && loaded.GetMainCamera().BackgroundColor.Y == 0.34f
                && loaded.GetMainCamera().BackgroundColor.Z == 0.55f, "camera background color round trips");
    }

    bool TestCameraRelativeLargeWorldTransform()
    {
        const Engine::Math::DVec3 translationOrigin {
            1000000000000.25,
            -999999999999.5,
            500000000000.125
        };
        Engine::TransformComponent transform;
        transform.Position = {
            translationOrigin.X + 12.5,
            translationOrigin.Y - 7.25,
            translationOrigin.Z + 0.125
        };

        const Engine::Math::Mat4 translated = transform.GetCameraRelativeTransform(translationOrigin);

        Engine::EditorCamera camera;
        camera.SetPosition(translationOrigin);
        const Engine::CameraView& view = camera.GetCameraView();
        return Expect(std::abs(translated.Values[12] - 12.5f) < 0.001f, "large-world X translation is camera relative")
            && Expect(std::abs(translated.Values[13] + 7.25f) < 0.001f, "large-world Y translation is camera relative")
            && Expect(std::abs(translated.Values[14] - 0.125f) < 0.001f, "large-world Z translation is camera relative")
            && Expect(view.TranslationOrigin.X == translationOrigin.X
                    && view.TranslationOrigin.Y == translationOrigin.Y
                    && view.TranslationOrigin.Z == translationOrigin.Z,
                "the camera publishes its exact double-precision translation origin")
            && Expect(view.View.Values[12] == 0.0f && view.View.Values[13] == 0.0f && view.View.Values[14] == 0.0f,
                "the float view matrix does not contain an absolute-world translation");
    }

    bool TestSceneRenderSnapshotExtractionAndRetainedEpochs()
    {
        Engine::Scene scene("Render Snapshot");
        const Engine::Entity mainCamera = scene.GetMainCameraEntity();
        Engine::TransformComponent* mainCameraTransform = scene.TryGetTransform(mainCamera);
        mainCameraTransform->Position = { 1000.25, -22.5, 9.75 };

        const Engine::Entity secondaryCamera = scene.CreateEntity("Secondary Camera");
        Engine::CameraComponent secondaryCameraComponent;
        secondaryCameraComponent.Primary = true;
        secondaryCameraComponent.Projection.VerticalFovDegrees = 72.0f;
        scene.AddCameraComponent(secondaryCamera, secondaryCameraComponent);

        const Engine::Entity invalidVisibleMesh = scene.CreateEntity("Invalid Visible Mesh");
        Engine::MeshRendererComponent invalidMeshComponent;
        invalidMeshComponent.CastsShadows = false;
        scene.AddMeshRendererComponent(invalidVisibleMesh, invalidMeshComponent);

        const Engine::Entity hiddenMesh = scene.CreateEntity("Hidden Mesh");
        Engine::MeshRendererComponent hiddenMeshComponent;
        hiddenMeshComponent.MeshAsset = 11;
        hiddenMeshComponent.MaterialAsset = 12;
        hiddenMeshComponent.Visible = false;
        scene.AddMeshRendererComponent(hiddenMesh, hiddenMeshComponent);

        const Engine::Entity visibleMesh = scene.CreateEntity("Visible Mesh");
        Engine::TransformComponent* visibleTransform = scene.TryGetTransform(visibleMesh);
        visibleTransform->Position = { 1234567890123.5, 4.0, -8.0 };
        visibleTransform->RotationDegrees = { 10.0f, 20.0f, 30.0f };
        visibleTransform->Scale = { 2.0f, 3.0f, 4.0f };
        Engine::MeshRendererComponent visibleMeshComponent;
        visibleMeshComponent.MeshAsset = 42;
        visibleMeshComponent.MaterialAsset = 84;
        scene.AddMeshRendererComponent(visibleMesh, visibleMeshComponent);

        const Engine::Entity lightEntity = scene.CreateEntity("Light");
        Engine::LightComponent lightComponent;
        lightComponent.Type = Engine::LightType::Spot;
        lightComponent.Color = { 0.2f, 0.4f, 0.8f };
        lightComponent.Intensity = 7.5f;
        lightComponent.Range = 125.0f;
        lightComponent.InnerConeDegrees = 15.0f;
        lightComponent.OuterConeDegrees = 35.0f;
        lightComponent.CastsShadows = false;
        scene.AddLightComponent(lightEntity, lightComponent);

        const std::shared_ptr<const Engine::SceneRenderSnapshot> first =
            std::make_shared<const Engine::SceneRenderSnapshot>(scene.ExtractRenderSnapshot(41));

        scene.TryGetTransform(visibleMesh)->Position.X = -1.0;
        scene.DestroyEntity(lightEntity);
        const std::shared_ptr<const Engine::SceneRenderSnapshot> second =
            std::make_shared<const Engine::SceneRenderSnapshot>(scene.ExtractRenderSnapshot(42));

        const bool cameraAuthorityValid = first
            && first->MainCameraEntity == mainCamera.Id
            && first->Cameras.size() == 2
            && first->Cameras[0].SourceEntity == mainCamera.Id
            && first->Cameras[0].Main
            && first->Cameras[0].Transform.WorldPosition.X == 1000.25
            && first->Cameras[1].SourceEntity == secondaryCamera.Id
            && !first->Cameras[1].Main
            && first->Cameras[1].Projection.VerticalFovDegrees == 72.0f;
        const bool meshExtractionValid = first
            && first->Meshes.size() == 2
            && first->Meshes[0].SourceEntity == invalidVisibleMesh.Id
            && first->Meshes[0].MeshAsset == Engine::kInvalidAssetHandle
            && !first->Meshes[0].CastsShadows
            && first->Meshes[1].SourceEntity == visibleMesh.Id
            && first->Meshes[1].MeshAsset == 42
            && first->Meshes[1].MaterialAsset == 84
            && first->Meshes[1].Transform.WorldPosition.X == 1234567890123.5
            && first->Meshes[1].Transform.RotationDegrees.Y == 20.0f
            && first->Meshes[1].Transform.Scale.Z == 4.0f;
        const bool lightExtractionValid = first
            && first->Lights.size() == 1
            && first->Lights[0].SourceEntity == lightEntity.Id
            && first->Lights[0].Type == Engine::LightType::Spot
            && first->Lights[0].Color.Z == 0.8f
            && first->Lights[0].Intensity == 7.5f
            && first->Lights[0].Range == 125.0f
            && first->Lights[0].InnerConeDegrees == 15.0f
            && first->Lights[0].OuterConeDegrees == 35.0f
            && !first->Lights[0].CastsShadows;
        const bool publicationValid = second
            && second != first
            && first->FrameIndex == 41
            && second->FrameIndex == 42
            && first->Meshes[1].Transform.WorldPosition.X == 1234567890123.5
            && second->Meshes[1].Transform.WorldPosition.X == -1.0
            && first->Lights.size() == 1
            && second->Lights.empty();
        return Expect(cameraAuthorityValid, "the snapshot copies deterministic camera records and authoritative main-camera identity")
            && Expect(meshExtractionValid, "visible meshes retain stable entity and asset handles while hidden meshes are omitted")
            && Expect(lightExtractionValid, "the snapshot copies backend-neutral light state")
            && Expect(publicationValid, "extracting a new frame retains an immutable older snapshot epoch");
    }

    bool TestSceneRejectsTruncatedComponent()
    {
        const std::filesystem::path path = TestFilePath("scene-truncated.spiral");
        {
            std::ofstream file(path);
            file << "SpiralScene 1\n[Entities]\nNextEntityId 2\nMainCameraEntity 1\n"
                 << "Entity 1 \"Camera\"\nTransform 1 0 0\n";
        }

        Engine::Scene loaded;
        const bool rejected = !Engine::Scene::LoadFromFile(path, loaded);
        std::error_code error;
        std::filesystem::remove(path, error);
        return Expect(rejected, "a truncated Transform record is rejected");
    }

    bool TestSceneLoadsVersionOneCamera()
    {
        const std::filesystem::path path = TestFilePath("scene-version-one.spiral");
        {
            std::ofstream file(path);
            file << "SpiralScene 1\nName \"Legacy\"\n\n[MainCamera]\nPrimary true\nVerticalFovDegrees 60\nNearClip 0.1\nFarClip 100\n\n"
                 << "[MainCamera.Transform]\nPosition 0 0 -3.35\nRotationDegrees 0 0 0\nScale 1 1 1\n\n"
                 << "[Entities]\nNextEntityId 2\nMainCameraEntity 1\nEntity 1 \"Main Camera\"\n"
                 << "Transform 1 0 0 -3.35 0 0 0 1 1 1\nCamera 1 true 60 0.1 100\n";
        }

        Engine::Scene loaded;
        const bool result = Engine::Scene::LoadFromFile(path, loaded);
        std::error_code error;
        std::filesystem::remove(path, error);
        const Engine::Math::Vec3& background = loaded.GetMainCamera().BackgroundColor;
        return Expect(result, "a version-one scene still loads")
            && Expect(background.X == 0.08f && background.Y == 0.09f && background.Z == 0.10f,
                "a version-one camera receives the default background color");
    }

    bool TestSceneRejectsDuplicateEntities()
    {
        const std::filesystem::path path = TestFilePath("scene-duplicate.spiral");
        {
            std::ofstream file(path);
            file << "SpiralScene 1\n[Entities]\nNextEntityId 2\nMainCameraEntity 1\n"
                 << "Entity 1 \"First\"\nEntity 1 \"Second\"\n";
        }

        Engine::Scene loaded;
        const bool rejected = !Engine::Scene::LoadFromFile(path, loaded);
        std::error_code error;
        std::filesystem::remove(path, error);
        return Expect(rejected, "duplicate entity IDs are rejected");
    }

    Engine::RHI::AdapterCandidate MakeCapabilityCandidate(std::string name, Engine::RHI::AdapterType type, std::int64_t score)
    {
        using namespace Engine::RHI;

        AdapterCandidate candidate;
        candidate.CandidateBackend = Backend::NVRHIVulkan;
        candidate.Identity.Name = std::move(name);
        candidate.Identity.Type = type;
        candidate.Queues.Graphics = true;
        candidate.Queues.Present = true;
        candidate.ApiMajor = 1;
        candidate.ApiMinor = 3;
        candidate.MaximumTextureDimension2D = 8192;
        candidate.TimelineSynchronization = true;
        candidate.PerformanceScore = score;
        candidate.Formats.push_back({
            Format::R8G8B8A8Unorm,
            FormatUsage::Sampled | FormatUsage::ColorAttachment | FormatUsage::CopySource,
            1
        });
        candidate.Formats.push_back({ Format::D32Float, FormatUsage::DepthStencil, 1 });
        return candidate;
    }

    Engine::RHI::CapabilityProfile MakeBootstrapCapabilityProfile()
    {
        using namespace Engine::RHI;

        CapabilityProfile profile;
        profile.Name = "Test Bootstrap Presentation V1";
        profile.MinimumApiMajor = 1;
        profile.MinimumApiMinor = 3;
        profile.MinimumTextureDimension2D = 4096;
        profile.RequirePresent = true;
        profile.RequireCompute = true;
        profile.RequireCopy = true;
        profile.RequireTimelineSynchronization = true;
        profile.RequiredFormats.push_back({ Format::R8G8B8A8Unorm, FormatUsage::ColorAttachment | FormatUsage::CopySource });
        profile.RequiredFormats.push_back({ Format::D32Float, FormatUsage::DepthStencil });
        return profile;
    }

    bool TestCapabilityStateKeepsLifecycleStagesDistinct()
    {
        using namespace Engine::RHI;

        const CapabilityState advertisedOnly = MakeCapabilityState(true, false, false, false, "reported by adapter");
        const CapabilityState implementedWithoutHardware = MakeCapabilityState(false, true, true, true, "portable fallback exists");
        const CapabilityState exercised = MakeCapabilityState(true, true, true, true, "runtime smoke");
        return Expect(advertisedOnly.IsValid() && !advertisedOnly.IsUsable(), "advertised support is not treated as enabled or implemented")
            && Expect(implementedWithoutHardware.IsValid() && !implementedWithoutHardware.Enabled && !implementedWithoutHardware.Exercised,
                "unadvertised hardware support cannot become enabled or exercised")
            && Expect(exercised.IsValid() && exercised.IsUsable() && exercised.Exercised,
                "an exercised feature records all prerequisite lifecycle stages");
    }

    bool TestCapabilitySelectionRetainsFallbacksAndRejections()
    {
        using namespace Engine::RHI;

        CapabilityProfile profile = MakeBootstrapCapabilityProfile();
        std::vector<AdapterCandidate> candidates;
        candidates.push_back(MakeCapabilityCandidate("Preferred But Incomplete", AdapterType::Discrete, 100));
        candidates.back().Queues.Present = false;
        candidates.push_back(MakeCapabilityCandidate("Integrated Complete", AdapterType::Integrated, 50));

        const AdapterSelectionResult result = EvaluateAdapterCandidates(profile, candidates, "Preferred But Incomplete");
        return Expect(result.HasSelection() && result.SelectedIndex == 1, "an invalid preferred adapter falls back to a qualified candidate")
            && Expect(!result.Evaluations[0].Accepted && !result.Evaluations[0].RejectionReasons.empty(),
                "the preferred adapter rejection reason is retained")
            && Expect(result.Evaluations[1].Fallbacks.size() == 2,
                "missing compute and copy queues select explicit graphics-queue fallbacks");
    }

    bool TestCapabilitySelectionValidatesFormatUsageAndStableRanking()
    {
        using namespace Engine::RHI;

        CapabilityProfile profile = MakeBootstrapCapabilityProfile();
        profile.RequireCompute = false;
        profile.RequireCopy = false;

        std::vector<AdapterCandidate> candidates;
        candidates.push_back(MakeCapabilityCandidate("Zulu", AdapterType::Discrete, 100));
        candidates.push_back(MakeCapabilityCandidate("Alpha", AdapterType::Discrete, 100));
        candidates.push_back(MakeCapabilityCandidate("Missing Color Usage", AdapterType::Discrete, 1000));
        candidates.back().Formats[0].Usages = FormatUsage::Sampled;

        const AdapterSelectionResult result = EvaluateAdapterCandidates(profile, candidates);
        return Expect(result.HasSelection() && result.SelectedIndex == 1, "equal candidates use a stable identity tie-break")
            && Expect(!result.Evaluations[2].Accepted && !result.Evaluations[2].RejectionReasons.empty(),
                "format presence without the required usage is rejected");
    }

    bool TestCapabilitySelectionRejectsApiLimitsAndSynchronization()
    {
        using namespace Engine::RHI;

        CapabilityProfile profile = MakeBootstrapCapabilityProfile();
        profile.RequireCompute = false;
        profile.RequireCopy = false;
        AdapterCandidate candidate = MakeCapabilityCandidate("Old Limited Device", AdapterType::Integrated, 10);
        candidate.ApiMinor = 2;
        candidate.MaximumTextureDimension2D = 2048;
        candidate.TimelineSynchronization = false;

        const AdapterSelectionResult result = EvaluateAdapterCandidates(profile, { candidate });
        return Expect(!result.HasSelection(), "a candidate that misses API, limit, and synchronization requirements is rejected")
            && Expect(result.Evaluations.size() == 1 && result.Evaluations[0].RejectionReasons.size() == 3,
                "each independent bootstrap requirement retains a rejection reason");
    }

    bool TestCapabilitySelectionHonorsStrictPreference()
    {
        using namespace Engine::RHI;

        CapabilityProfile profile = MakeBootstrapCapabilityProfile();
        profile.RequireCompute = false;
        profile.RequireCopy = false;
        std::vector<AdapterCandidate> candidates;
        candidates.push_back(MakeCapabilityCandidate("Requested Adapter", AdapterType::Integrated, 10));
        candidates.back().Identity.StableId = "adapter-id-requested";
        candidates.push_back(MakeCapabilityCandidate("Faster Adapter", AdapterType::Discrete, 1000));
        candidates.back().Identity.StableId = "adapter-id-faster";

        const AdapterSelectionResult selected = EvaluateAdapterCandidates(profile, candidates, "adapter-id-requested", true);
        const AdapterSelectionResult missing = EvaluateAdapterCandidates(profile, candidates, "Missing Adapter", true);
        return Expect(selected.HasSelection() && selected.SelectedIndex == 0,
                   "strict preference selects the requested qualified adapter even when another adapter ranks higher")
            && Expect(!selected.Evaluations[1].Accepted && selected.Evaluations[1].RejectionReasons.size() == 1,
                "strict preference records why otherwise-qualified adapters are not eligible")
            && Expect(!missing.HasSelection(), "strict preference fails when the requested adapter is unavailable");
    }

    bool TestCapabilityDiagnosticsHelpersAreDeterministicAndBoundsSafe()
    {
        using namespace Engine::RHI;

        DeviceCapabilities capabilities;
        capabilities.AdapterCandidates.push_back(MakeCapabilityCandidate("Selected Adapter", AdapterType::Discrete, 100));
        capabilities.AdapterCandidates[0].Identity.StableId = "adapter-selected";
        capabilities.AdapterSelection.SelectedIndex = 0;
        capabilities.AdapterSelection.Evaluations.push_back({ 0, true, 100, {}, {} });

        const std::string usages = FormatUsagesToString(
            FormatUsage::Sampled | FormatUsage::ColorAttachment | FormatUsage::CopySource);
        const bool validSelection = capabilities.GetSelectedAdapter()
            && capabilities.GetSelectedAdapter()->Identity.StableId == "adapter-selected"
            && capabilities.GetSelectedAdapterEvaluation()
            && capabilities.GetSelectedAdapterEvaluation()->Accepted;

        capabilities.AdapterSelection.SelectedIndex = 3;
        capabilities.AdapterSelection.Evaluations.push_back({ 3, true, 100, {}, {} });
        return Expect(validSelection, "diagnostics helpers resolve a valid selected candidate and evaluation")
            && Expect(usages == "sampled, color attachment, copy source",
                "format usage diagnostics use a deterministic readable order")
            && Expect(FormatUsagesToString(FormatUsage::None) == "none", "empty format usages are explicit")
            && Expect(!capabilities.GetSelectedAdapter() && !capabilities.GetSelectedAdapterEvaluation(),
                "malformed selected indices do not escape the capability report bounds");
    }

    bool TestEditorCapabilityReasonDiagnosticsPreserveFallbacksAndRejections()
    {
        using namespace Engine;
        using namespace Engine::RHI;

        DeviceCapabilities capabilities;
        capabilities.Fallbacks.emplace_back("preferred adapter was unavailable; selected fallback adapter");
        capabilities.AdapterCandidates.push_back(MakeCapabilityCandidate("Selected Adapter", AdapterType::Integrated, 10));
        capabilities.AdapterCandidates.push_back(MakeCapabilityCandidate("Rejected Adapter", AdapterType::Discrete, 100));
        capabilities.AdapterSelection.SelectedIndex = 0;
        capabilities.AdapterSelection.Evaluations.push_back({ 0, true, 10, {}, { "compute work aliases the graphics queue" } });
        capabilities.AdapterSelection.Evaluations.push_back({ 1, false, 100, { "presentation support is unavailable" }, {} });

        const RendererCapabilityReasonDiagnostics diagnostics = BuildRendererCapabilityReasonDiagnostics(capabilities);
        return Expect(diagnostics.SelectedFallbacks.size() == 1
                && diagnostics.SelectedFallbacks[0] == "preferred adapter was unavailable; selected fallback adapter",
                "editor diagnostics preserve the selected-device fallback reason")
            && Expect(diagnostics.AdapterCandidates.size() == 2
                && diagnostics.AdapterCandidates[0].Selected
                && diagnostics.AdapterCandidates[0].Fallbacks.size() == 1
                && diagnostics.AdapterCandidates[0].Fallbacks[0] == "compute work aliases the graphics queue",
                "editor diagnostics preserve the selected candidate and its queue fallback")
            && Expect(!diagnostics.AdapterCandidates[1].Accepted
                && diagnostics.AdapterCandidates[1].RejectionReasons.size() == 1
                && diagnostics.AdapterCandidates[1].RejectionReasons[0] == "presentation support is unavailable",
                "editor diagnostics preserve rejected-candidate reasons");
    }

    bool TestFrameTimingCapabilityGroupSelectsUsableGpuTimestamps()
    {
        using namespace Engine;
        using namespace Engine::RHI;

        DeviceCapabilities capabilities;
        capabilities.GetFeature(DeviceFeature::Timestamps) = MakeCapabilityState(
            true, true, true, true, "timestamp query path is exercised");

        CapabilityGroupState group = BuildFrameTimingCapabilityGroup(capabilities);
        return Expect(group.Group == CapabilityGroupId::Phase3FrameTimingV1,
                   "frame timing uses a stable versioned capability group")
            && Expect(group.PreferredPath == CapabilityPath::GpuTimestamps
                    && group.SelectedPath == CapabilityPath::GpuTimestamps,
                "usable GPU timestamps select the preferred timing path")
            && Expect(group.Implemented && !group.Exercised && group.Fallbacks.empty(),
                "consumer exercise remains distinct from feature exercise")
            && Expect(group.IsValid() && group.IsUsable(), "the selected GPU timing group is valid and usable");
    }

    bool TestFrameTimingCapabilityGroupSelectsPortableCpuFallback()
    {
        using namespace Engine;
        using namespace Engine::RHI;

        DeviceCapabilities capabilities;
        capabilities.GetFeature(DeviceFeature::Timestamps) = MakeCapabilityState(
            true, false, false, false, "advertised but the query path is not implemented");

        CapabilityGroupState group = BuildFrameTimingCapabilityGroup(capabilities);
        const bool selectedFallback = group.PreferredPath == CapabilityPath::GpuTimestamps
            && group.SelectedPath == CapabilityPath::CpuSteadyClock
            && group.Implemented && !group.Exercised;
        const bool retainedReasons = group.Fallbacks.size() == 1
            && group.UnsupportedReasons.size() == 1
            && group.UnsupportedReasons[0] == "advertised but the query path is not implemented";

        group.Exercised = true;
        group.Qualification = QualificationLevel::Presentation;
        capabilities.CapabilityGroups.push_back(group);
        const CapabilityGroupState* published = capabilities.GetCapabilityGroup(CapabilityGroupId::Phase3FrameTimingV1);
        return Expect(selectedFallback, "unusable GPU timestamps select the portable CPU timing path")
            && Expect(retainedReasons, "the fallback retains the unavailable GPU path reason")
            && Expect(published && published->IsValid() && published->Exercised,
                "the exercised group is published through bounds-safe device diagnostics")
            && Expect(!capabilities.GetCapabilityGroup(CapabilityGroupId::Count),
                "unknown capability groups do not escape the report bounds");
    }

    bool TestFrameTimingCapabilityGroupRejectsInvalidTimestampLifecycle()
    {
        using namespace Engine;
        using namespace Engine::RHI;

        DeviceCapabilities capabilities;
        CapabilityState& timestamps = capabilities.GetFeature(DeviceFeature::Timestamps);
        timestamps.Advertised = false;
        timestamps.Enabled = true;
        timestamps.Implemented = true;
        timestamps.Exercised = true;
        timestamps.Detail = "invalid synthetic lifecycle";

        const CapabilityGroupState group = BuildFrameTimingCapabilityGroup(capabilities);
        return Expect(!timestamps.IsValid(), "the synthetic timestamp lifecycle is invalid")
            && Expect(group.SelectedPath == CapabilityPath::CpuSteadyClock,
                "an invalid timestamp lifecycle cannot select GPU timing")
            && Expect(group.UnsupportedReasons.size() == 1
                    && group.UnsupportedReasons[0] == "invalid synthetic lifecycle",
                "invalid optional-path diagnostics survive fallback selection");
    }

}

int main()
{
    Engine::Log::Init();

    const std::vector<std::pair<std::string_view, TestFunction>> tests = {
        { "JobSystem contains worker exceptions", TestJobSystemContainsWorkerExceptions },
        { "JobSystem inline fallback is reentrant", TestJobSystemInlineFallbackIsReentrant },
        { "JobSystem steals nested worker jobs", TestJobSystemStealsNestedWorkerJobs },
        { "Worker wait and nested graph are safe", TestWorkerWaitAndNestedGraphAreSafe },
        { "Frame task graph publishes deterministically", TestFrameTaskGraphPublishesDeterministically },
        { "Frame task graph propagates failure", TestFrameTaskGraphPropagatesFailure },
        { "Frame task graph schedules fan-in and fan-out", TestFrameTaskGraphSchedulesFanInAndFanOut },
        { "Frame task graph rejects cycles", TestFrameTaskGraphRejectsCycles },
        { "Frame task graph rejects invalid dependencies", TestFrameTaskGraphRejectsInvalidDependencies },
        { "Scene round trip", TestSceneRoundTrip },
        { "Camera-relative large-world transform", TestCameraRelativeLargeWorldTransform },
        { "Scene render snapshot extraction and retained epochs", TestSceneRenderSnapshotExtractionAndRetainedEpochs },
        { "Scene rejects truncated components", TestSceneRejectsTruncatedComponent },
        { "Scene loads version-one camera", TestSceneLoadsVersionOneCamera },
        { "Scene rejects duplicate entities", TestSceneRejectsDuplicateEntities },
        { "Capability state keeps lifecycle stages distinct", TestCapabilityStateKeepsLifecycleStagesDistinct },
        { "Capability selection retains fallbacks and rejections", TestCapabilitySelectionRetainsFallbacksAndRejections },
        { "Capability selection validates format usage and stable ranking", TestCapabilitySelectionValidatesFormatUsageAndStableRanking },
        { "Capability selection rejects API limits and synchronization", TestCapabilitySelectionRejectsApiLimitsAndSynchronization },
        { "Capability selection honors strict preference", TestCapabilitySelectionHonorsStrictPreference },
        { "Capability diagnostics helpers are deterministic and bounds safe", TestCapabilityDiagnosticsHelpersAreDeterministicAndBoundsSafe },
        { "Editor capability reason diagnostics preserve fallbacks and rejections", TestEditorCapabilityReasonDiagnosticsPreserveFallbacksAndRejections },
        { "Frame timing capability group selects usable GPU timestamps", TestFrameTimingCapabilityGroupSelectsUsableGpuTimestamps },
        { "Frame timing capability group selects portable CPU fallback", TestFrameTimingCapabilityGroupSelectsPortableCpuFallback },
        { "Frame timing capability group rejects invalid timestamp lifecycle", TestFrameTimingCapabilityGroupRejectsInvalidTimestampLifecycle }
    };

    size_t failures = 0;
    for (const auto& [name, test] : tests)
    {
        std::cout << "[ RUN      ] " << name << '\n';
        if (test())
            std::cout << "[       OK ] " << name << '\n';
        else
        {
            std::cout << "[  FAILED  ] " << name << '\n';
            ++failures;
        }
    }

    Engine::JobSystem::Get().Shutdown();
    Engine::Log::Shutdown();
    std::cout << tests.size() - failures << "/" << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
