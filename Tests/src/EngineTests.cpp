#include "Engine/Core/Log.h"
#include "Engine/Jobs/FrameTaskGraph.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/Math/WorldGrid.h"
#include "Engine/RenderGraph/RenderGraph.h"
#include "Engine/RHI/Device.h"
#include "Engine/RHI/SubmissionDependency.h"
#include "Engine/RHI/BufferOwnership.h"
#include "Engine/RHI/TextureOwnership.h"
#include "Engine/RHI/NVRHI/NVRHID3D12Device.h"
#include "Engine/RHI/NVRHI/VulkanQueueAdmission.h"
#include "Engine/Renderer/CapabilityDiagnostics.h"
#include "Engine/Renderer/AsyncShaderPackageService.h"
#include "Engine/Renderer/PortableShaderContract.h"
#include "Engine/Renderer/SlangShaderCompiler.h"
#include "Engine/Renderer/SceneRasterPreparation.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
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

    bool MatricesNear(const Engine::Math::Mat4& left, const Engine::Math::Mat4& right, float tolerance = 0.001f)
    {
        for (size_t index = 0; index < std::size(left.Values); ++index)
        {
            if (std::abs(left.Values[index] - right.Values[index]) > tolerance)
                return false;
        }
        return true;
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
        const Engine::Math::DVec3 cubePosition {
            1000000000000.25,
            -999999999999.5,
            500000000000.125
        };
        source.SetEntityWorldPosition(cube, cubePosition);
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
        Engine::Math::DVec3 loadedPosition;
        return Expect(result, "a valid scene saves and loads")
            && Expect(loaded.GetName() == "Round Trip", "the scene name round trips")
            && Expect(loadedMesh && loadedMesh->MeshAsset == 42 && loadedMesh->MaterialAsset == 84, "mesh handles round trip")
            && Expect(loadedTransform
                    && loaded.TryGetEntityApproximateWorldPosition(loadedCube, loadedPosition)
                    && loadedPosition.X == cubePosition.X
                    && loadedPosition.Y == cubePosition.Y
                    && loadedPosition.Z == cubePosition.Z,
                "large double-precision positions round trip without loss")
            && Expect(loaded.GetMainCamera().BackgroundColor.X == 0.21f
                && loaded.GetMainCamera().BackgroundColor.Y == 0.34f
                && loaded.GetMainCamera().BackgroundColor.Z == 0.55f, "camera background color round trips");
    }

    bool TestSceneVersionFourCanonicalPersistence()
    {
        using namespace Engine;
        using namespace Engine::Math;

        WorldGridPolicy policy;
        policy.SectorExtent = 8192.0;
        policy.OriginHysteresis = 128.0;
        policy.OriginMode = WorldOriginMode::SectorSnapped;
        Scene source("Canonical V4", policy);
        const Entity entity = source.CreateEntity("Far Entity");
        SectorLocalPosition expected;
        expected.Sector = { std::numeric_limits<i64>::max() - 17, -9007199254740993LL, 42 };
        expected.Local = { -4096.0, 17.25, 4095.5 };
        const bool assigned = source.SetEntitySectorLocalPosition(entity, expected);
        const Entity inspectorAxisEntity = source.CreateEntity("Inspector Axis");
        SectorLocalPosition inspectorAxisPosition;
        inspectorAxisPosition.Sector = { 7, std::numeric_limits<i64>::min() + 29, -9007199254740993LL };
        inspectorAxisPosition.Local = { 1.0, -4096.0, 4095.5 };
        const bool inspectorAssigned = source.SetEntitySectorLocalPosition(inspectorAxisEntity, inspectorAxisPosition);
        const bool inspectorAxisEdited = source.SetEntityWorldPositionAxis(inspectorAxisEntity, 0, 12.5);
        const TransformComponent* inspectorTransform = source.TryGetTransform(inspectorAxisEntity);

        const std::filesystem::path path = TestFilePath("scene-version-four.spiral");
        const bool saved = source.SaveToFile(path);
        std::ifstream savedFile(path);
        const std::string contents(
            (std::istreambuf_iterator<char>(savedFile)),
            std::istreambuf_iterator<char>());
        savedFile.close();

        Scene loaded;
        const bool loadedSuccessfully = Scene::LoadFromFile(path, loaded);
        const Entity loadedEntity = loaded.FindEntityByName("Far Entity");
        const TransformComponent* loadedTransform = loaded.TryGetTransform(loadedEntity);
        std::error_code error;
        std::filesystem::remove(path, error);

        const WorldGridPolicy& loadedPolicy = loaded.GetWorldGridPolicy();
        return Expect(assigned && saved && loadedSuccessfully, "canonical sector/local version 4 state saves and loads")
            && Expect(inspectorAssigned
                    && inspectorAxisEdited
                    && inspectorTransform
                    && inspectorTransform->GetPosition().Local.X == 12.5
                    && inspectorTransform->GetPosition().Sector.Y == inspectorAxisPosition.Sector.Y
                    && inspectorTransform->GetPosition().Local.Y == inspectorAxisPosition.Local.Y
                    && inspectorTransform->GetPosition().Sector.Z == inspectorAxisPosition.Sector.Z
                    && inspectorTransform->GetPosition().Local.Z == inspectorAxisPosition.Local.Z,
                "absolute inspector axis edits preserve untouched canonical sector/local axes")
            && Expect(contents.find("SpiralScene 4") != std::string::npos
                    && contents.find("[WorldGrid]") != std::string::npos
                    && contents.find("Version 1") != std::string::npos
                    && contents.find("SectorExtent 8192") != std::string::npos
                    && contents.find("OriginHysteresis 128") != std::string::npos
                    && contents.find("OriginMode SectorSnapped") != std::string::npos,
                "version 4 writes the explicit immutable world-grid policy")
            && Expect(contents.find("[MainCamera.Transform]") == std::string::npos,
                "version 4 does not write a duplicated main-camera transform authority")
            && Expect(loadedPolicy.Version == policy.Version
                    && loadedPolicy.SectorExtent == policy.SectorExtent
                    && loadedPolicy.OriginHysteresis == policy.OriginHysteresis
                    && loadedPolicy.OriginMode == policy.OriginMode,
                "the loaded scene retains the serialized world-grid policy")
            && Expect(loadedTransform
                    && loadedTransform->GetPosition().Sector == expected.Sector
                    && loadedTransform->GetPosition().Local.X == expected.Local.X
                    && loadedTransform->GetPosition().Local.Y == expected.Local.Y
                    && loadedTransform->GetPosition().Local.Z == expected.Local.Z,
                "sector identity and canonical locals round trip without absolute-double conversion");
    }

    bool TestSceneLoadsLegacyAbsoluteTransforms()
    {
        using namespace Engine;
        using namespace Engine::Math;

        bool loadedAll = true;
        for (int version : { 1, 2, 3 })
        {
            const std::filesystem::path path = TestFilePath("scene-version-" + std::to_string(version) + ".spiral");
            {
                std::ofstream file(path);
                file << "SpiralScene " << version << "\nName \"Legacy\"\n\n"
                     << "[MainCamera]\nPrimary true\nVerticalFovDegrees 60\nNearClip 0.1\nFarClip 100\n";
                if (version >= 2)
                    file << "BackgroundColor 0.1 0.2 0.3\n";
                file << "\n"
                     << "[MainCamera.Transform]\nPosition 10 20 30\nRotationDegrees 1 2 3\nScale 4 5 6\n\n"
                     << "[Entities]\nNextEntityId 3\nMainCameraEntity 1\n"
                     << "Entity 1 \"Main Camera\"\nTransform 1 0 0 -3.35 7 8 9 1 1 1\n"
                     << "Camera 1 true 60 0.1 100";
                if (version >= 2)
                    file << " 0.1 0.2 0.3";
                file << "\n"
                     << "Entity 2 \"Legacy Far\"\n"
                     << "Transform 2 1000000000000.25 -999999999999.5 500000000000.125 0 0 0 1 1 1\n";
            }

            Scene loaded;
            const bool result = Scene::LoadFromFile(path, loaded);
            const Entity entity = loaded.FindEntityByName("Legacy Far");
            const TransformComponent* transform = loaded.TryGetTransform(entity);
            Math::DVec3 mainCameraPosition;
            SectorLocalPosition expected;
            const bool expectedValid = TryDecomposeWorldPosition(
                { 1000000000000.25, -999999999999.5, 500000000000.125 },
                loaded.GetWorldGridPolicy(),
                expected);
            loadedAll = loadedAll
                && result
                && expectedValid
                && transform
                && transform->GetPosition().Sector == expected.Sector
                && transform->GetPosition().Local.X == expected.Local.X
                && transform->GetPosition().Local.Y == expected.Local.Y
                && transform->GetPosition().Local.Z == expected.Local.Z
                && loaded.TryGetEntityApproximateWorldPosition(loaded.GetMainCameraEntity(), mainCameraPosition)
                && mainCameraPosition.X == 0.0
                && mainCameraPosition.Y == 0.0
                && mainCameraPosition.Z == -3.35
                && loaded.GetMainCameraTransform().RotationDegrees.X == 7.0f
                && loaded.GetWorldGridPolicy().SectorExtent == 4096.0;

            std::error_code error;
            std::filesystem::remove(path, error);
        }

        return Expect(loadedAll,
            "scene formats 1-3 migrate absolute doubles using the default grid and retain entity main-camera precedence");
    }

    bool TestSceneRejectsInvalidVersionFourWorldState()
    {
        const std::filesystem::path noncanonicalPath = TestFilePath("scene-v4-noncanonical.spiral");
        {
            std::ofstream file(noncanonicalPath);
            file << "SpiralScene 4\nName \"Invalid\"\n\n"
                 << "[WorldGrid]\nVersion 1\nSectorExtent 4096\nOriginHysteresis 256\nOriginMode ExactCamera\n\n"
                 << "[Entities]\nNextEntityId 2\nMainCameraEntity 1\n"
                 << "Entity 1 \"Main Camera\"\nTransform 1 0 0 0 2048 0 0 0 0 0 1 1 1\n";
        }

        const std::filesystem::path invalidPolicyPath = TestFilePath("scene-v4-invalid-policy.spiral");
        {
            std::ofstream file(invalidPolicyPath);
            file << "SpiralScene 4\nName \"Invalid\"\n\n"
                 << "[WorldGrid]\nVersion 1\nSectorExtent 0\nOriginHysteresis 256\nOriginMode ExactCamera\n\n"
                 << "[Entities]\nNextEntityId 1\nMainCameraEntity 0\n";
        }

        Engine::Scene loaded("Unchanged Destination");
        const Engine::Entity preserved = loaded.CreateEntity("Preserved");
        const bool positioned = loaded.SetEntityWorldPosition(preserved, { 17.0, -23.0, 5.0 });
        const bool rejectedNoncanonical = !Engine::Scene::LoadFromFile(noncanonicalPath, loaded);
        const bool rejectedInvalidPolicy = !Engine::Scene::LoadFromFile(invalidPolicyPath, loaded);
        Engine::Math::DVec3 preservedPosition;
        const bool destinationPreserved = loaded.GetName() == "Unchanged Destination"
            && loaded.FindEntityByName("Preserved") == preserved
            && loaded.TryGetEntityApproximateWorldPosition(preserved, preservedPosition)
            && preservedPosition.X == 17.0
            && preservedPosition.Y == -23.0
            && preservedPosition.Z == 5.0;
        std::error_code error;
        std::filesystem::remove(noncanonicalPath, error);
        std::filesystem::remove(invalidPolicyPath, error);

        return Expect(rejectedNoncanonical, "version 4 rejects noncanonical centered-half-open locals")
            && Expect(rejectedInvalidPolicy, "version 4 rejects invalid immutable world-grid policies")
            && Expect(positioned && destinationPreserved,
                "rejected version 4 records leave the destination scene unchanged");
    }

    bool TestCameraRelativeLargeWorldTransform()
    {
        const Engine::Math::DVec3 translationOrigin {
            1000000000000.25,
            -999999999999.5,
            500000000000.125
        };
        Engine::Scene scene("Camera Relative");
        const Engine::Entity entity = scene.CreateEntity("Transform");
        scene.SetEntityWorldPosition(entity, {
            translationOrigin.X + 12.5,
            translationOrigin.Y - 7.25,
            translationOrigin.Z + 0.125
        });
        const Engine::TransformComponent* transform = scene.TryGetTransform(entity);

        const Engine::Math::Mat4 translated = transform
            ? transform->GetCameraRelativeTransform(translationOrigin, scene.GetWorldGridPolicy())
            : Engine::Math::Mat4::Identity();

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
            && Expect(view.WorldPosition.X == translationOrigin.X
                    && view.WorldPosition.Y == translationOrigin.Y
                    && view.WorldPosition.Z == translationOrigin.Z,
                "the camera view retains its double-precision world position")
            && Expect(view.View.Values[12] == 0.0f && view.View.Values[13] == 0.0f && view.View.Values[14] == 0.0f,
                "the float view matrix does not contain an absolute-world translation");
    }

    bool TestWorldGridCanonicalizationAndBounds()
    {
        using namespace Engine::Math;

        const WorldGridPolicy policy;
        const double extent = policy.SectorExtent;
        const double halfExtent = extent * 0.5;
        const double belowZero = std::nextafter(0.0, -1.0);
        const double belowNegativeBoundary = std::nextafter(-halfExtent, -extent);
        const double belowPositiveBoundary = std::nextafter(halfExtent, 0.0);
        const struct
        {
            double World;
            Engine::i64 Sector;
            double Local;
        } cases[] = {
            { -extent, -1, 0.0 },
            { belowNegativeBoundary, -1, halfExtent - std::abs(belowNegativeBoundary + halfExtent) },
            { -halfExtent, 0, -halfExtent },
            { -0.25, 0, -0.25 },
            { belowZero, 0, belowZero },
            { 0.0, 0, 0.0 },
            { 0.25, 0, 0.25 },
            { belowPositiveBoundary, 0, belowPositiveBoundary },
            { halfExtent, 1, -halfExtent },
            { extent, 1, 0.0 },
            { 1000000000000.25, 244140625, 0.25 }
        };

        bool canonicalCases = true;
        for (const auto& testCase : cases)
        {
            SectorLocalPosition decomposed;
            DVec3 recomposed;
            const bool decomposedValid = TryDecomposeWorldPosition({ testCase.World, 0.0, 0.0 }, policy, decomposed);
            const bool caseValid = decomposedValid
                && decomposed.Sector.X == testCase.Sector
                && decomposed.Local.X == testCase.Local
                && IsCanonical(decomposed, policy)
                && TryComposeApproximateWorldPosition(decomposed, policy, recomposed)
                && recomposed.X == testCase.World;
            if (!caseValid)
            {
                std::cerr << "  World grid case failed: world=" << testCase.World
                          << ", expected sector=" << testCase.Sector
                          << ", actual sector=" << decomposed.Sector.X
                          << ", expected local=" << testCase.Local
                          << ", actual local=" << decomposed.Local.X << '\n';
            }
            canonicalCases = canonicalCases && caseValid;
        }

        SectorLocalPosition noncanonical;
        noncanonical.Sector = { -2, 3, 0 };
        noncanonical.Local = { extent * 2.0 + 0.5, -0.25, extent };
        SectorLocalPosition normalized;
        const bool normalizedValid = TryNormalizeSectorLocal(noncanonical, policy, normalized)
            && normalized.Sector == SectorIndex { 0, 3, 1 }
            && normalized.Local.X == 0.5
            && normalized.Local.Y == -0.25
            && normalized.Local.Z == 0.0
            && IsCanonical(normalized, policy);

        WorldGridPolicy customPolicy;
        customPolicy.SectorExtent = 3.0;
        customPolicy.OriginHysteresis = 0.25;
        SectorLocalPosition customDecomposed;
        DVec3 customRecomposed;
        const bool customExtentValid = TryDecomposeWorldPosition({ 10.0, -10.0, 0.0 }, customPolicy, customDecomposed)
            && customDecomposed.Sector == SectorIndex { 3, -3, 0 }
            && customDecomposed.Local.X == 1.0
            && customDecomposed.Local.Y == -1.0
            && TryComposeApproximateWorldPosition(customDecomposed, customPolicy, customRecomposed)
            && customRecomposed.X == 10.0
            && customRecomposed.Y == -10.0;

        const SectorRange crossing = GetOverlappingSectorRange(
            { -halfExtent - 1.0, -halfExtent - 1.0, -halfExtent },
            { halfExtent, halfExtent, halfExtent },
            policy,
            32);
        const bool crossingValid = crossing.Status == SectorRangeStatus::Finite
            && crossing.Min == SectorIndex { -1, -1, 0 }
            && crossing.Max == SectorIndex { 0, 0, 0 }
            && crossing.SectorCount == 4;
        const SectorRange exactBoundary = GetOverlappingSectorRange(
            { -halfExtent, -halfExtent, -halfExtent },
            { halfExtent, halfExtent, halfExtent },
            policy,
            8);
        const SectorRange oversized = GetOverlappingSectorRange(
            { -extent * 4.0, -extent * 4.0, -extent * 4.0 },
            { extent * 4.0, extent * 4.0, extent * 4.0 },
            policy,
            64);
        const SectorRange empty = GetOverlappingSectorRange(
            { 1.0, 0.0, 0.0 },
            { 1.0, 2.0, 3.0 },
            policy,
            8);

        WorldGridPolicy invalidPolicy = policy;
        invalidPolicy.SectorExtent = 0.0;
        WorldGridPolicy invalidOriginMode = policy;
        invalidOriginMode.OriginMode = static_cast<WorldOriginMode>(99);
        SectorLocalPosition rejected;
        const bool invalidInputsRejected = !TryDecomposeWorldPosition({ 0.0, 0.0, 0.0 }, invalidPolicy, rejected)
            && !TryDecomposeWorldPosition({ 0.0, 0.0, 0.0 }, invalidOriginMode, rejected)
            && !TryDecomposeWorldPosition({ std::numeric_limits<double>::infinity(), 0.0, 0.0 }, policy, rejected);
        SectorLocalPosition overflowing;
        overflowing.Sector.X = std::numeric_limits<Engine::i64>::max();
        overflowing.Local.X = extent;
        const bool overflowRejected = !TryNormalizeSectorLocal(overflowing, policy, rejected)
            && !TryDecomposeWorldPosition({ std::numeric_limits<double>::max(), 0.0, 0.0 }, policy, rejected);

        SectorLocalPosition positiveExtreme;
        positiveExtreme.Sector.X = std::numeric_limits<Engine::i64>::max();
        SectorLocalPosition negativeExtreme;
        negativeExtreme.Sector.X = std::numeric_limits<Engine::i64>::min();
        DVec3 rejectedRelative { 7.0, 8.0, 9.0 };
        const bool relativeOverflowRejected = !TryGetSectorLocalRelativePosition(
                positiveExtreme, negativeExtreme, policy, rejectedRelative)
            && !TryGetSectorLocalRelativePosition(
                negativeExtreme, positiveExtreme, policy, rejectedRelative)
            && rejectedRelative.X == 7.0
            && rejectedRelative.Y == 8.0
            && rejectedRelative.Z == 9.0;

        WorldGridPolicy floatRangePolicy;
        floatRangePolicy.SectorExtent = 1.0e39;
        SectorLocalPosition oneSectorAway;
        oneSectorAway.Sector.X = 1;
        SectorLocalPosition zeroOrigin;
        const bool floatRangeRejected = IsWorldGridPolicyValid(floatRangePolicy)
            && !TryGetSectorLocalRelativePosition(
                oneSectorAway, zeroOrigin, floatRangePolicy, rejectedRelative);

        WorldGridPolicy nonFiniteRelativePolicy;
        nonFiniteRelativePolicy.SectorExtent = 1.0e308;
        SectorLocalPosition twoSectorsAway;
        twoSectorsAway.Sector.X = 2;
        const bool nonFiniteRelativeRejected = IsWorldGridPolicyValid(nonFiniteRelativePolicy)
            && !TryGetSectorLocalRelativePosition(
                twoSectorsAway, zeroOrigin, nonFiniteRelativePolicy, rejectedRelative);

        return Expect(canonicalCases, "signed world coordinates decompose into one canonical centered half-open sector/local form")
            && Expect(normalizedValid, "noncanonical local coordinates carry across signed sectors deterministically")
            && Expect(customExtentValid, "valid project-specific non-power-of-two sector extents remain canonical")
            && Expect(crossingValid, "cross-sector bounds cover negative and positive sectors")
            && Expect(exactBoundary.Status == SectorRangeStatus::Finite
                    && exactBoundary.Min == SectorIndex {}
                    && exactBoundary.Max == SectorIndex {}
                    && exactBoundary.SectorCount == 1,
                "max-exclusive bounds do not include a sector touched only at the exact boundary")
            && Expect(oversized.Status == SectorRangeStatus::Oversized && oversized.SectorCount == 0,
                "oversized bounds are classified without unbounded sector enumeration")
            && Expect(empty.Status == SectorRangeStatus::Empty,
                "degenerate max-exclusive bounds are reported as empty")
            && Expect(invalidInputsRejected, "invalid grid policies and non-finite positions are rejected")
            && Expect(overflowRejected, "sector carries and absolute decomposition reject signed-sector overflow")
            && Expect(relativeOverflowRejected, "relative conversion rejects signed-sector subtraction overflow transactionally")
            && Expect(floatRangeRejected, "relative conversion rejects finite results outside the float translated-space range")
            && Expect(nonFiniteRelativeRejected, "relative conversion rejects non-finite sector-delta products");
    }

    bool TestPerViewSectorSnappedOriginTracking()
    {
        using namespace Engine;
        using namespace Engine::Math;

        WorldGridPolicy snappedPolicy;
        snappedPolicy.OriginMode = WorldOriginMode::SectorSnapped;
        CameraViewOriginTracker tracker;
        TrackedCameraViewRequest request;
        request.StableViewId = 101;
        request.Projection = {};
        request.AspectRatio = 16.0f / 9.0f;

        request.WorldPosition = { 0.0, 0.0, 0.0 };
        const CameraView initial = tracker.BuildView(request, snappedPolicy);
        request.WorldPosition = { 2200.0, 0.0, 0.0 };
        const CameraView insideForwardBand = tracker.BuildView(request, snappedPolicy);
        request.WorldPosition = { 2310.0, 0.0, 0.0 };
        const CameraView crossedForwardBand = tracker.BuildView(request, snappedPolicy);
        request.WorldPosition = { 2200.0, 0.0, 0.0 };
        const CameraView insideReturnBand = tracker.BuildView(request, snappedPolicy);

        request.WorldPosition = { 10.0 * 4096.0 + 17.0, 0.0, 0.0 };
        request.DiscontinuousRelocation = true;
        const CameraView teleported = tracker.BuildView(request, snappedPolicy);
        request.DiscontinuousRelocation = false;

        TrackedCameraViewRequest secondView = request;
        secondView.StableViewId = 202;
        secondView.WorldPosition = { -10.0 * 4096.0 - 17.0, 0.0, 0.0 };
        const CameraView secondInitial = tracker.BuildView(secondView, snappedPolicy);
        Scene scene("Per-view origin epochs", snappedPolicy);
        const std::shared_ptr<const SceneRenderSnapshot> firstViewEpoch =
            std::make_shared<const SceneRenderSnapshot>(scene.ExtractRenderSnapshot(1, teleported));
        const std::shared_ptr<const SceneRenderSnapshot> secondViewEpoch =
            std::make_shared<const SceneRenderSnapshot>(scene.ExtractRenderSnapshot(2, secondInitial));
        request.WorldPosition = { 10.0 * 4096.0 + 32.0, 0.0, 0.0 };
        const CameraView firstAfterSecond = tracker.BuildView(request, snappedPolicy);

        WorldGridPolicy exactPolicy = snappedPolicy;
        exactPolicy.OriginMode = WorldOriginMode::ExactCamera;
        request.WorldPosition = { 1234567890123.25, -8.0, 4.0 };
        const CameraView exact = tracker.BuildView(request, exactPolicy);

        const double threshold = snappedPolicy.SectorExtent * 0.5 + snappedPolicy.OriginHysteresis;
        CameraViewOriginTracker positiveBoundaryTracker;
        TrackedCameraViewRequest positiveBoundaryRequest = request;
        positiveBoundaryRequest.StableViewId = 303;
        positiveBoundaryRequest.WorldPosition = { 0.0, 0.0, 0.0 };
        positiveBoundaryTracker.BuildView(positiveBoundaryRequest, snappedPolicy);
        positiveBoundaryRequest.WorldPosition = { threshold, 0.0, 0.0 };
        const CameraView positiveBoundary = positiveBoundaryTracker.BuildView(positiveBoundaryRequest, snappedPolicy);
        positiveBoundaryRequest.WorldPosition = { std::nextafter(threshold, std::numeric_limits<double>::infinity()), 0.0, 0.0 };
        const CameraView positiveBeyondBoundary = positiveBoundaryTracker.BuildView(positiveBoundaryRequest, snappedPolicy);

        CameraViewOriginTracker negativeBoundaryTracker;
        TrackedCameraViewRequest negativeBoundaryRequest = request;
        negativeBoundaryRequest.StableViewId = 404;
        negativeBoundaryRequest.WorldPosition = { 0.0, 0.0, 0.0 };
        negativeBoundaryTracker.BuildView(negativeBoundaryRequest, snappedPolicy);
        negativeBoundaryRequest.WorldPosition = { -threshold, 0.0, 0.0 };
        const CameraView negativeBoundary = negativeBoundaryTracker.BuildView(negativeBoundaryRequest, snappedPolicy);
        negativeBoundaryRequest.WorldPosition = { std::nextafter(-threshold, -std::numeric_limits<double>::infinity()), 0.0, 0.0 };
        const CameraView negativeBeyondBoundary = negativeBoundaryTracker.BuildView(negativeBoundaryRequest, snappedPolicy);

        CameraViewOriginTracker transactionalTracker;
        TrackedCameraViewRequest transactionalRequest = request;
        transactionalRequest.StableViewId = 505;
        transactionalRequest.WorldPosition = {};
        transactionalTracker.BuildView(transactionalRequest, snappedPolicy);
        transactionalRequest.AspectRatio = std::numeric_limits<float>::quiet_NaN();
        const CameraView invalidModeChange = transactionalTracker.BuildView(transactionalRequest, exactPolicy);
        transactionalRequest.AspectRatio = 16.0f / 9.0f;
        const CameraView validModeChange = transactionalTracker.BuildView(transactionalRequest, exactPolicy);
        WorldGridPolicy changedPolicy = snappedPolicy;
        changedPolicy.SectorExtent = 8192.0;
        transactionalRequest.AspectRatio = std::numeric_limits<float>::quiet_NaN();
        const CameraView invalidPolicyChange = transactionalTracker.BuildView(transactionalRequest, changedPolicy);
        transactionalRequest.AspectRatio = 16.0f / 9.0f;
        const CameraView validPolicyChange = transactionalTracker.BuildView(transactionalRequest, changedPolicy);

        const bool noFlap = initial.Valid
            && initial.TranslationOriginSector.X == 0
            && insideForwardBand.TranslationOriginSector.X == 0
            && crossedForwardBand.TranslationOriginSector.X == 1
            && insideReturnBand.TranslationOriginSector.X == 1
            && !insideForwardBand.TemporalHistoryInvalidated
            && crossedForwardBand.TemporalHistoryInvalidated
            && !insideReturnBand.TemporalHistoryInvalidated;
        const bool teleportAndViewsIndependent = teleported.Valid
            && teleported.TranslationOriginSector.X == 10
            && teleported.TemporalHistoryInvalidated
            && secondInitial.Valid
            && secondInitial.TranslationOriginSector.X == -10
            && firstAfterSecond.Valid
            && firstAfterSecond.TranslationOriginSector.X == 10
            && teleported.TranslationOriginSector.X == 10
            && secondInitial.TranslationOriginSector.X == -10
            && firstViewEpoch->Views[0].Camera.StableViewId == 101
            && firstViewEpoch->Views[0].Camera.TranslationOriginSector.X == 10
            && secondViewEpoch->Views[0].Camera.StableViewId == 202
            && secondViewEpoch->Views[0].Camera.TranslationOriginSector.X == -10;
        const bool exactCameraRemainsDefaultBehavior = exact.Valid
            && exact.TranslationOrigin.X == request.WorldPosition.X
            && exact.TranslationOrigin.Y == request.WorldPosition.Y
            && exact.TranslationOrigin.Z == request.WorldPosition.Z
            && exact.StableViewId == request.StableViewId;
        const bool exactBoundaries = positiveBoundary.Valid
            && positiveBoundary.TranslationOriginSector.X == 0
            && !positiveBoundary.TemporalHistoryInvalidated
            && positiveBeyondBoundary.TranslationOriginSector.X == 1
            && positiveBeyondBoundary.TemporalHistoryInvalidated
            && negativeBoundary.Valid
            && negativeBoundary.TranslationOriginSector.X == 0
            && !negativeBoundary.TemporalHistoryInvalidated
            && negativeBeyondBoundary.TranslationOriginSector.X == -1
            && negativeBeyondBoundary.TemporalHistoryInvalidated;
        const bool failedRequestsDoNotCommitState = !invalidModeChange.Valid
            && validModeChange.Valid
            && validModeChange.TemporalHistoryInvalidated
            && !invalidPolicyChange.Valid
            && validPolicyChange.Valid
            && validPolicyChange.TemporalHistoryInvalidated;
        const i64 maxSector = std::numeric_limits<i64>::max();
        const i64 minSector = std::numeric_limits<i64>::min();
        const double positiveExactLocal = -snappedPolicy.SectorExtent * 0.5 + snappedPolicy.OriginHysteresis;
        const double negativeExactLocal = snappedPolicy.SectorExtent * 0.5 - snappedPolicy.OriginHysteresis;
        const bool extremeSectorPrecision = !ShouldRebaseCameraOriginAxis(
                maxSector, positiveExactLocal, maxSector - 1, snappedPolicy)
            && ShouldRebaseCameraOriginAxis(
                maxSector,
                std::nextafter(positiveExactLocal, std::numeric_limits<double>::infinity()),
                maxSector - 1,
                snappedPolicy)
            && !ShouldRebaseCameraOriginAxis(
                minSector, negativeExactLocal, minSector + 1, snappedPolicy)
            && ShouldRebaseCameraOriginAxis(
                minSector,
                std::nextafter(negativeExactLocal, -std::numeric_limits<double>::infinity()),
                minSector + 1,
                snappedPolicy)
            && ShouldRebaseCameraOriginAxis(maxSector, 0.0, minSector, snappedPolicy);

        return Expect(noFlap, "sector-snapped origins retain an axis inside the hysteresis band without flapping")
            && Expect(teleportAndViewsIndependent, "stable view IDs retain independent origins and teleports jump directly to the destination sector")
            && Expect(exactCameraRemainsDefaultBehavior, "exact-camera mode publishes the exact camera origin without sector hysteresis")
            && Expect(exactBoundaries, "sector-snapped hysteresis retains exact positive and negative boundaries, then changes just beyond them")
            && Expect(failedRequestsDoNotCommitState, "invalid mode or policy-change requests leave prior view state intact for the next valid epoch")
            && Expect(extremeSectorPrecision, "adjacent extreme signed sectors retain local hysteresis detail without double-precision sector collapse");
    }

    bool TestSceneRasterOriginEpochInvariance()
    {
        constexpr double base = 1000000000000.0;
        Engine::Scene scene("Raster Origin Epochs");
        const Engine::Entity mesh = scene.CreateEntity("Origin Mesh");
        Engine::MeshRendererComponent meshRenderer;
        meshRenderer.MeshAsset = 42;
        meshRenderer.MaterialAsset = 84;
        scene.AddMeshRendererComponent(mesh, meshRenderer);

        Engine::CameraProjection projection;
        const Engine::Math::DVec3 cameraA { base, -base, base - 3.35 };
        const Engine::Math::DVec3 meshA { base, -base, base };
        scene.SetEntityWorldPosition(mesh, meshA);
        const Engine::CameraView viewA = Engine::BuildCameraView(
            cameraA, {}, projection, 16.0f / 9.0f, cameraA);
        const std::shared_ptr<const Engine::SceneRenderSnapshot> snapshotA =
            std::make_shared<const Engine::SceneRenderSnapshot>(scene.ExtractRenderSnapshot(100, viewA));
        const Engine::SceneRasterFrame rasterA = Engine::PrepareSceneRasterFrame(*snapshotA);

        scene.SetEntityWorldPosition(mesh, { meshA.X + 1.0, meshA.Y, meshA.Z });
        const Engine::SceneRenderSnapshot snapshotB = scene.ExtractRenderSnapshot(101, viewA);
        const Engine::SceneRasterFrame rasterB = Engine::PrepareSceneRasterFrame(snapshotB);

        const Engine::Math::DVec3 cameraC { cameraA.X + 1.0, cameraA.Y, cameraA.Z };
        const Engine::CameraView viewC = Engine::BuildCameraView(
            cameraC, {}, projection, 16.0f / 9.0f, cameraC);
        const Engine::SceneRenderSnapshot snapshotC = scene.ExtractRenderSnapshot(102, viewC);
        const Engine::SceneRasterFrame rasterC = Engine::PrepareSceneRasterFrame(snapshotC);

        const Engine::Math::DVec3 alternateOrigin {
            cameraA.X + 4096.0,
            cameraA.Y - 2048.0,
            cameraA.Z + 1024.0
        };
        const Engine::CameraView alternateView = Engine::BuildCameraView(
            cameraA, {}, projection, 16.0f / 9.0f, alternateOrigin);
        scene.SetEntityWorldPosition(mesh, meshA);
        const Engine::SceneRasterFrame alternateRaster = Engine::PrepareSceneRasterFrame(
            scene.ExtractRenderSnapshot(103, alternateView));

        const Engine::SceneRasterFrame invalidRaster = Engine::PrepareSceneRasterFrame(
            scene.ExtractRenderSnapshot(104, {}));

        Engine::Scene positiveExtremeScene("Positive Canonical Raster Extreme");
        const Engine::Entity positiveExtremeMesh = positiveExtremeScene.CreateEntity("Positive Extreme Mesh");
        positiveExtremeScene.AddMeshRendererComponent(positiveExtremeMesh, meshRenderer);
        Engine::Math::SectorLocalPosition positiveCameraPosition;
        positiveCameraPosition.Sector.X = std::numeric_limits<Engine::i64>::max() - 1;
        positiveCameraPosition.Local.X = 0.25;
        Engine::Math::SectorLocalPosition positiveMeshPosition = positiveCameraPosition;
        positiveMeshPosition.Local.X = 0.5;
        const bool positiveExtremeAssigned = positiveExtremeScene.SetEntitySectorLocalPosition(
            positiveExtremeMesh, positiveMeshPosition);
        Engine::Math::DVec3 positiveApproximateCamera;
        Engine::Math::DVec3 positiveApproximateMesh;
        const bool positiveApproximateAliased = Engine::Math::TryComposeApproximateWorldPosition(
                positiveCameraPosition, positiveExtremeScene.GetWorldGridPolicy(), positiveApproximateCamera)
            && Engine::Math::TryComposeApproximateWorldPosition(
                positiveMeshPosition, positiveExtremeScene.GetWorldGridPolicy(), positiveApproximateMesh)
            && positiveApproximateCamera.X == positiveApproximateMesh.X;
        Engine::TrackedCameraViewRequest positiveViewRequest;
        positiveViewRequest.StableViewId = 901;
        positiveViewRequest.WorldPosition = positiveApproximateCamera;
        positiveViewRequest.CanonicalWorldPosition = positiveCameraPosition;
        positiveViewRequest.HasCanonicalWorldPosition = true;
        positiveViewRequest.Projection = projection;
        positiveViewRequest.AspectRatio = 16.0f / 9.0f;
        Engine::CameraViewOriginTracker positiveTracker;
        const Engine::CameraView positiveView = positiveTracker.BuildView(
            positiveViewRequest, positiveExtremeScene.GetWorldGridPolicy());
        const Engine::SceneRasterFrame positiveExtremeRaster = Engine::PrepareSceneRasterFrame(
            positiveExtremeScene.ExtractRenderSnapshot(105, positiveView));

        Engine::Scene negativeExtremeScene("Negative Canonical Raster Extreme");
        const Engine::Entity negativeExtremeMesh = negativeExtremeScene.CreateEntity("Negative Extreme Mesh");
        negativeExtremeScene.AddMeshRendererComponent(negativeExtremeMesh, meshRenderer);
        Engine::Math::SectorLocalPosition negativeCameraPosition;
        negativeCameraPosition.Sector.X = std::numeric_limits<Engine::i64>::min() + 1;
        negativeCameraPosition.Local.X = -0.25;
        Engine::Math::SectorLocalPosition negativeMeshPosition = negativeCameraPosition;
        negativeMeshPosition.Local.X = 0.25;
        const bool negativeExtremeAssigned = negativeExtremeScene.SetEntitySectorLocalPosition(
            negativeExtremeMesh, negativeMeshPosition);
        Engine::Math::DVec3 negativeApproximateCamera;
        Engine::Math::DVec3 negativeApproximateMesh;
        const bool negativeApproximateAliased = Engine::Math::TryComposeApproximateWorldPosition(
                negativeCameraPosition, negativeExtremeScene.GetWorldGridPolicy(), negativeApproximateCamera)
            && Engine::Math::TryComposeApproximateWorldPosition(
                negativeMeshPosition, negativeExtremeScene.GetWorldGridPolicy(), negativeApproximateMesh)
            && negativeApproximateCamera.X == negativeApproximateMesh.X;
        Engine::TrackedCameraViewRequest negativeViewRequest;
        negativeViewRequest.StableViewId = 902;
        negativeViewRequest.WorldPosition = negativeApproximateCamera;
        negativeViewRequest.CanonicalWorldPosition = negativeCameraPosition;
        negativeViewRequest.HasCanonicalWorldPosition = true;
        negativeViewRequest.Projection = projection;
        negativeViewRequest.AspectRatio = 16.0f / 9.0f;
        Engine::CameraViewOriginTracker negativeTracker;
        const Engine::CameraView negativeView = negativeTracker.BuildView(
            negativeViewRequest, negativeExtremeScene.GetWorldGridPolicy());
        const Engine::SceneRasterFrame negativeExtremeRaster = Engine::PrepareSceneRasterFrame(
            negativeExtremeScene.ExtractRenderSnapshot(106, negativeView));

        const bool epochDataValid = rasterA.HasValidView
            && rasterA.SnapshotFrameIndex == 100
            && rasterA.Instances.size() == 1
            && rasterA.Instances[0].SourceEntity == mesh.Id
            && rasterA.Instances[0].CameraRelativePosition.X == 0.0f
            && rasterB.Instances.size() == 1
            && rasterB.Instances[0].CameraRelativePosition.X == 1.0f
            && rasterC.Instances.size() == 1
            && rasterC.Instances[0].CameraRelativePosition.X == 0.0f;
        const bool canonicalExtremeValid = positiveExtremeAssigned
            && positiveApproximateAliased
            && positiveView.Valid
            && positiveView.HasCanonicalTranslationOrigin
            && positiveView.TranslationOriginPosition.Sector == positiveCameraPosition.Sector
            && positiveView.TranslationOriginPosition.Local.X == positiveCameraPosition.Local.X
            && positiveExtremeRaster.HasValidView
            && positiveExtremeRaster.Instances.size() == 1
            && positiveExtremeRaster.Instances[0].CameraRelativePosition.X == 0.25f
            && negativeExtremeAssigned
            && negativeApproximateAliased
            && negativeView.Valid
            && negativeView.HasCanonicalTranslationOrigin
            && negativeView.TranslationOriginPosition.Sector == negativeCameraPosition.Sector
            && negativeView.TranslationOriginPosition.Local.X == negativeCameraPosition.Local.X
            && negativeExtremeRaster.HasValidView
            && negativeExtremeRaster.Instances.size() == 1
            && negativeExtremeRaster.Instances[0].CameraRelativePosition.X == 0.5f;
        const bool retainedEpochValid = snapshotA->FrameIndex == 100
            && snapshotA->Views.size() == 1
            && snapshotA->Views[0].Camera.TranslationOrigin.X == cameraA.X
            && snapshotA->Meshes[0].Transform.Position.Sector.X == 244140625
            && snapshotA->Meshes[0].Transform.Position.Local.X == 0.0;
        const bool equivalentViewsValid = alternateRaster.Instances.size() == 1
            && MatricesNear(rasterA.Instances[0].ModelViewProjection,
                alternateRaster.Instances[0].ModelViewProjection);

        return Expect(epochDataValid, "each raster frame uses one snapshot's view, origin, and mesh transforms")
            && Expect(!MatricesNear(rasterA.Instances[0].ModelViewProjection, rasterB.Instances[0].ModelViewProjection),
                "moving only the mesh changes its raster transform")
            && Expect(MatricesNear(rasterA.Instances[0].ModelViewProjection, rasterC.Instances[0].ModelViewProjection),
                "moving camera and mesh together across an origin transition preserves the raster transform")
            && Expect(retainedEpochValid, "a newer origin epoch does not mutate a retained snapshot")
            && Expect(equivalentViewsValid, "an arbitrary translated origin preserves the same camera-relative raster result")
            && Expect(canonicalExtremeValid, "tracker-derived canonical snapshot/raster conversion preserves nonzero local deltas when extreme absolute doubles alias")
            && Expect(!invalidRaster.HasValidView && invalidRaster.Instances.empty(),
                "a snapshot without a valid view cannot produce raster instances");
    }

    bool TestSceneRenderSnapshotExtractionAndRetainedEpochs()
    {
        Engine::Scene scene("Render Snapshot");
        const Engine::Entity mainCamera = scene.GetMainCameraEntity();
        scene.SetEntityWorldPosition(mainCamera, { 1000.25, -22.5, 9.75 });

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
        scene.SetEntityWorldPosition(visibleMesh, { 1234567890123.5, 4.0, -8.0 });
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

        Engine::EditorCamera renderCamera;
        renderCamera.SetPosition({ 1000.25, -22.5, 9.75 });
        const std::shared_ptr<const Engine::SceneRenderSnapshot> first =
            std::make_shared<const Engine::SceneRenderSnapshot>(
                scene.ExtractRenderSnapshot(41, renderCamera.GetCameraView()));

        scene.SetEntityWorldPosition(visibleMesh, { -1.0, 4.0, -8.0 });
        scene.DestroyEntity(lightEntity);
        const std::shared_ptr<const Engine::SceneRenderSnapshot> second =
            std::make_shared<const Engine::SceneRenderSnapshot>(
                scene.ExtractRenderSnapshot(42, renderCamera.GetCameraView()));

        const bool cameraAuthorityValid = first
            && first->MainCameraEntity == mainCamera.Id
            && first->Views.size() == 1
            && first->Views[0].Camera.WorldPosition.X == 1000.25
            && first->Cameras.size() == 2
            && first->Cameras[0].SourceEntity == mainCamera.Id
            && first->Cameras[0].Main
            && first->Cameras[0].Transform.Position.Sector.X == 0
            && first->Cameras[0].Transform.Position.Local.X == 1000.25
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
            && first->Meshes[1].Transform.Position.Sector.X == 301408176
            && first->Meshes[1].Transform.Position.Local.X == 1227.5
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
            && first->Meshes[1].Transform.Position.Sector.X == 301408176
            && first->Meshes[1].Transform.Position.Local.X == 1227.5
            && second->Meshes[1].Transform.Position.Sector.X == 0
            && second->Meshes[1].Transform.Position.Local.X == -1.0
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

    bool TestTransientCapabilityGroupSelectsPlacedAliasingOnlyWhenBothRhiFeaturesAreUsable()
    {
        using namespace Engine;
        using namespace Engine::RHI;

        DeviceCapabilities capabilities;
        capabilities.GetFeature(DeviceFeature::PlacedResources) = MakeCapabilityState(
            true, true, true, true, "placed resources are translated and exercised");
        capabilities.GetFeature(DeviceFeature::AliasingBarriers) = MakeCapabilityState(
            true, true, true, true, "aliasing barriers are translated and exercised");

        const CapabilityGroupState group = RenderGraph::BuildTransientResourceCapabilityGroup(capabilities);
        return Expect(group.Group == CapabilityGroupId::Phase3TransientResourcesV1,
                   "transient resources use a stable versioned capability group")
            && Expect(group.PreferredPath == CapabilityPath::PlacedAliasedTransient
                    && group.SelectedPath == CapabilityPath::PlacedAliasedTransient,
                "only usable placement plus alias barriers select the aliased path")
            && Expect(group.Implemented && !group.Exercised && group.Fallbacks.empty()
                    && group.IsValid() && group.IsUsable(),
                "the selected aliased policy remains separately exercised from feature evidence");
    }

    bool TestTransientCapabilityGroupSelectsGpuRetiredNonAliasedFallback()
    {
        using namespace Engine;
        using namespace Engine::RHI;

        DeviceCapabilities capabilities;
        capabilities.GetFeature(DeviceFeature::PlacedResources) = MakeCapabilityState(
            true, true, true, true, "placed resources are translated and exercised");
        capabilities.GetFeature(DeviceFeature::AliasingBarriers) = MakeCapabilityState(
            false, false, false, false, "alias barriers are not translated by the active RHI device");

        CapabilityGroupState group = RenderGraph::BuildTransientResourceCapabilityGroup(capabilities);
        const bool fallback = group.PreferredPath == CapabilityPath::PlacedAliasedTransient
            && group.SelectedPath == CapabilityPath::NonAliasedGpuRetiredPool
            && group.Implemented && !group.Exercised
            && group.Fallbacks.size() == 1 && group.UnsupportedReasons.size() == 2
            && group.Fallbacks[0].find("GPU-retired reuse") != std::string::npos;

        group.Exercised = true;
        group.Qualification = QualificationLevel::Presentation;
        capabilities.CapabilityGroups.push_back(group);
        const CapabilityGroupState* published = capabilities.GetCapabilityGroup(
            CapabilityGroupId::Phase3TransientResourcesV1);
        return Expect(fallback, "missing alias barriers select the committed or pooled non-aliased GPU-retired fallback")
            && Expect(published && published->IsValid() && published->Exercised,
                "the selected fallback policy is publishable with its independent runtime exercise");
    }

    bool TestPortableShaderContract()
    {
        using namespace Engine;
        PortableShaderRequest request;
        request.SourceName = "fixture.slang"; request.Source = "float4 main() : SV_Target { return 1; }"; request.EntryPoint = "main";
        request.Stage = RHI::ShaderStage::Pixel; request.Targets = { PortableShaderTarget::Dxil, PortableShaderTarget::Spirv };
        request.CompilerIdentity = "Slang"; request.CompilerVersion = "2026.13.1";
        request.CompilerPackageHash = "slang-archive-sha256";
        request.DownstreamCompilerPackageHash = "dxc-archive-sha256";
        request.Defines = { "B=2", "A=1" }; request.Options = { "-O3" };
        request.Dependencies = { { "b.slang", "2", "b" }, { "a.slang", "1", "a" } };
        request.ExpectedLayout = {
            { "View", 'b', 0, 0, RHI::ShaderStage::Pixel, "ConstantBuffer", "constant-buffer", 1, 64, 0, 0 }
        };
        const std::string key = PortableShaderContract::CacheKey(request);
        PortableShaderRequest reordered = request; std::reverse(reordered.Defines.begin(), reordered.Defines.end()); std::reverse(reordered.Dependencies.begin(), reordered.Dependencies.end());
        std::string error;
        const std::vector<PortableShaderBinding> reflection = request.ExpectedLayout;
        const bool accepted = PortableShaderContract::Validate(request, reflection, {}, error);
        reordered.Source += " "; const bool sourceInvalidates = key != PortableShaderContract::CacheKey(reordered);
        reordered = request; reordered.Dependencies[0].ContentHash = "changed"; const bool dependencyInvalidates = key != PortableShaderContract::CacheKey(reordered);
        reordered = request; reordered.LayoutVersion++; const bool versionInvalidates = key != PortableShaderContract::CacheKey(reordered);
        const auto invalidates = [&](const std::function<void(PortableShaderRequest&)>& mutate)
        {
            PortableShaderRequest changed = request;
            mutate(changed);
            return key != PortableShaderContract::CacheKey(changed);
        };
        const bool independentInputs = invalidates([](auto& r) { r.Stage = RHI::ShaderStage::Vertex; })
            && invalidates([](auto& r) { r.Targets = { PortableShaderTarget::Dxil }; })
            && invalidates([](auto& r) { r.Defines[0] = "B=3"; })
            && invalidates([](auto& r) { r.CompilerIdentity = "DXC"; })
            && invalidates([](auto& r) { r.CompilerVersion = "2026.13.2"; })
            && invalidates([](auto& r) { r.CompilerPackageHash = "changed-package-hash"; })
            && invalidates([](auto& r) { r.DownstreamCompilerPackageHash = "changed-downstream-package-hash"; })
            && invalidates([](auto& r) { r.Options.push_back("-Od"); })
            && invalidates([](auto& r) { r.ReflectionVersion++; })
            && invalidates([](auto& r) { r.Conventions.Version++; })
            && invalidates([](auto& r) { r.ExpectedLayout[0].Register++; })
            && invalidates([](auto& r) { r.ExpectedLayout[0].ByteSize++; });
        auto mismatch = reflection; mismatch[0].Register = 1;
        const bool layoutRejected = !PortableShaderContract::Validate(request, mismatch, {}, error);
        const auto rejectsBindingMutation = [&](const std::function<void(PortableShaderBinding&)>& mutate)
        {
            auto changed = reflection;
            mutate(changed[0]);
            return !PortableShaderContract::Validate(request, changed, {}, error);
        };
        const bool richLayoutRejected = rejectsBindingMutation([](auto& binding) { binding.Name = "Other"; })
            && rejectsBindingMutation([](auto& binding) { binding.ResourceKind = "Texture2D"; })
            && rejectsBindingMutation([](auto& binding) { binding.TypeShape = "float32x4"; })
            && rejectsBindingMutation([](auto& binding) { binding.Count = 2; })
            && rejectsBindingMutation([](auto& binding) { binding.ByteSize = 80; });
        const std::filesystem::path path = TestFilePath("portable-shader.shaderpkg");
        PortableShaderPackage written; written.Key = key; written.Dxil = { 1, 2 }; written.Spirv = { 3, 4 }; written.Reflection = reflection;
        PortableShaderPackage loaded;
        const bool roundTrip = PortableShaderContract::StoreAtomic(path, written)
            && PortableShaderContract::Load(path, request, loaded) && loaded == written;

        PortableShaderPackage missingRequestedArtifact = written;
        missingRequestedArtifact.Dxil.clear();
        PortableShaderRequest spirvOnlyRequest = request;
        spirvOnlyRequest.Targets = { PortableShaderTarget::Spirv };
        PortableShaderPackage spirvOnlyPackage = missingRequestedArtifact;
        spirvOnlyPackage.Key = PortableShaderContract::CacheKey(spirvOnlyRequest);
        std::string packageError;
        const bool artifactCompleteness = !PortableShaderContract::ValidatePackage(request, missingRequestedArtifact, packageError)
            && packageError.find("missing requested DXIL artifact") != std::string::npos
            && PortableShaderContract::ValidatePackage(spirvOnlyRequest, spirvOnlyPackage, packageError);
        const std::filesystem::path incompletePath = TestFilePath("portable-shader-incomplete.shaderpkg");
        const bool incompleteCacheRejected = PortableShaderContract::StoreAtomic(incompletePath, missingRequestedArtifact)
            && !PortableShaderContract::Load(incompletePath, request, loaded);

        PortableShaderPackage sentinel;
        sentinel.Version = 77;
        sentinel.Key = "sentinel-key";
        sentinel.Dxil = { 91 };
        sentinel.Spirv = { 92 };
        sentinel.Reflection = { { "Sentinel", 'u', 9, 8, RHI::ShaderStage::Compute, "RWTexture3D", "uint32", 7, 6, 5, 4 } };
        sentinel.VertexInputs = { { "SentinelInput", "SENTINEL", 3, 2, "float64x2", 16, 1, 2 } };
        sentinel.Conventions.Coordinates = "SentinelCoordinates";
        sentinel.Diagnostics = { { "sentinel", "entry", "target", "backend", "message" } };
        PortableShaderPackage output = sentinel;
        PortableShaderRequest wrongKeyRequest = request;
        wrongKeyRequest.Source += " changed";
        const bool badKeyRejected = !PortableShaderContract::Load(path, wrongKeyRequest, output) && output == sentinel;

        std::ifstream validStream(path, std::ios::binary);
        const std::vector<char> validBytes(
            (std::istreambuf_iterator<char>(validStream)),
            std::istreambuf_iterator<char>());
        std::vector<char> wrongVersionBytes = validBytes;
        wrongVersionBytes[5] = 99;
        { std::ofstream corrupt(path, std::ios::binary | std::ios::trunc); corrupt.write(wrongVersionBytes.data(), static_cast<std::streamsize>(wrongVersionBytes.size())); }
        const bool versionRejected = !PortableShaderContract::Load(path, request, output) && output == sentinel;
        { std::ofstream corrupt(path, std::ios::binary | std::ios::trunc); corrupt.write(validBytes.data(), static_cast<std::streamsize>(validBytes.size() - 3)); }
        const bool lateTruncationRejected = !PortableShaderContract::Load(path, request, output) && output == sentinel;

        const bool stableSha = PortableShaderContract::Sha256("abc")
            == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        const bool deterministicKey = key.size() == 64
            && key == PortableShaderContract::CacheKey(request)
            && key == PortableShaderContract::CacheKey(PortableShaderRequest(request));
        return Expect(stableSha && deterministicKey, "portable shader cache key is deterministic SHA-256")
            && Expect(accepted && sourceInvalidates && dependencyInvalidates && versionInvalidates && independentInputs, "portable shader key covers independently mutable compiler inputs")
            && Expect(layoutRejected && richLayoutRejected, "portable shader semantic layout collisions are rejected")
            && Expect(artifactCompleteness && incompleteCacheRejected,
                "portable shader cache accepts only every requested artifact while allowing unrequested artifacts to be empty")
            && Expect(roundTrip && badKeyRejected && versionRejected && lateTruncationRejected,
                "portable shader cache parse failures preserve every caller field");
    }

    bool TestSlangShaderCompilerProducesPortableValidatedPackages()
    {
        using namespace Engine;
        const std::filesystem::path cacheDirectory = TestFilePath("slang-shader-cache");
        std::error_code error;
        std::filesystem::remove_all(cacheDirectory, error);

        PortableShaderRequest request;
        request.SourceName = "portable-fixture.slang";
        request.Source = R"(
#include "portable-shared.inc"
#ifndef COLOR_SCALE
#define COLOR_SCALE 1
#endif
cbuffer View : register(b0, space0)
{
    float4x4 ViewProjection;
};
Texture2D Albedo : register(t1, space0);
SamplerState LinearSampler : register(s2, space0);
Texture2D Layers[2] : register(t3, space0);

float4 main(float4 position : SV_Position) : SV_Target
{
    return Albedo.Sample(LinearSampler, position.xy) + Layers[1].Sample(LinearSampler, position.xy)
        + ViewProjection[0] + IncludedColor * COLOR_SCALE;
}
)";
        request.EntryPoint = "main";
        request.Stage = RHI::ShaderStage::Pixel;
        #ifdef _WIN32
        request.Targets = { PortableShaderTarget::Dxil, PortableShaderTarget::Spirv };
        #else
        request.Targets = { PortableShaderTarget::Spirv };
        #endif
        request.CompilerIdentity = "Slang";
        request.CompilerVersion = "2026.13.1";
        request.CompilerPackageHash = "fa1c9bcab2cdcd3626f7a1e250dd35d606c1b84745b64627f1dd63fca3746a70";
        #ifdef _WIN32
        request.DownstreamCompilerPackageHash = "a1e89031421cf3c1fca6627766ab3020ca4f962ac7e2caa7fab2b33a8436151e";
        #endif
        request.Defines = { "COLOR_SCALE=1" };
        request.Options = { "-O0" };
        PortableShaderDependency dependency;
        dependency.Path = "portable-shared.inc";
        dependency.Content = "static const float4 IncludedColor = float4(0.1, 0.2, 0.3, 0.4);\n";
        dependency.ContentHash = PortableShaderContract::Sha256(dependency.Content);
        request.Dependencies = { dependency };
        request.ExpectedLayout = {
            { "View", 'b', 0, 0, RHI::ShaderStage::Pixel, "ConstantBuffer", "struct{ViewProjection:float32x4x4:row-major@0}", 1, 64, 0, 0 },
            { "Albedo", 't', 1, 0, RHI::ShaderStage::Pixel, "Texture2D", "float32x4", 1, 0, 1, 4 },
            { "LinearSampler", 's', 2, 0, RHI::ShaderStage::Pixel, "SamplerState", "sampler", 1, 0, 0, 0 },
            { "Layers", 't', 3, 0, RHI::ShaderStage::Pixel, "Texture2D", "float32x4", 2, 0, 1, 4 }
        };

        SlangShaderCompiler compiler(cacheDirectory);
        const PortableShaderPackage first = compiler.Compile(request);
        const PortableShaderPackage second = compiler.Compile(request);
        const auto printShaderDiagnostics = [](const PortableShaderPackage& package)
        {
            for (const PortableShaderDiagnostic& diagnostic : package.Diagnostics)
                std::cerr << "  Shader diagnostic: " << diagnostic.Target << ": " << diagnostic.Message << '\n';
        };
        if (!first.Succeeded()) printShaderDiagnostics(first);
        const bool compiled = first.Succeeded()
            #ifdef _WIN32
            && !first.Dxil.empty()
            #else
            && first.Dxil.empty()
            #endif
            && !first.Spirv.empty()
            && first.Reflection == request.ExpectedLayout && first.VertexInputs.empty()
            && first.Conventions == request.Conventions;
        const bool deterministic = second.Succeeded() && first.Dxil == second.Dxil
            && first.Spirv == second.Spirv && first.Reflection == second.Reflection
            && std::filesystem::exists(cacheDirectory / (first.Key + ".shaderpkg"));

        PortableShaderRequest changedDefine = request;
        changedDefine.Defines = { "COLOR_SCALE=2" };
        const PortableShaderPackage defineOutput = compiler.Compile(changedDefine);
        PortableShaderRequest changedInclude = request;
        changedInclude.Dependencies[0].Content = "static const float4 IncludedColor = float4(0.8, 0.7, 0.6, 0.5);\n";
        changedInclude.Dependencies[0].ContentHash = PortableShaderContract::Sha256(changedInclude.Dependencies[0].Content);
        const PortableShaderPackage includeOutput = compiler.Compile(changedInclude);
        PortableShaderRequest changedOptions = request;
        changedOptions.Options = { "-O3" };
        const PortableShaderPackage optionOutput = compiler.Compile(changedOptions);
        const bool appliedInputs = defineOutput.Succeeded() && includeOutput.Succeeded() && optionOutput.Succeeded()
            && (defineOutput.Dxil != first.Dxil || defineOutput.Spirv != first.Spirv)
            && (includeOutput.Dxil != first.Dxil || includeOutput.Spirv != first.Spirv)
            && (optionOutput.Dxil != first.Dxil || optionOutput.Spirv != first.Spirv);

        PortableShaderRequest unsupportedOption = request;
        unsupportedOption.Options = { "--not-an-admitted-option" };
        const PortableShaderPackage optionFailure = compiler.Compile(unsupportedOption);
        PortableShaderRequest undeclaredInclude = request;
        undeclaredInclude.Dependencies.clear();
        const PortableShaderPackage dependencyFailure = compiler.Compile(undeclaredInclude);
        PortableShaderRequest staleDependencyHash = request;
        staleDependencyHash.Dependencies[0].Content += "// changed";
        const PortableShaderPackage staleHashFailure = compiler.Compile(staleDependencyHash);
        PortableShaderRequest unusedDependency = request;
        PortableShaderDependency unused;
        unused.Path = "unused.inc";
        unused.Content = "static const float Unused = 1.0;\n";
        unused.ContentHash = PortableShaderContract::Sha256(unused.Content);
        unusedDependency.Dependencies.push_back(unused);
        const PortableShaderPackage unusedDependencyFailure = compiler.Compile(unusedDependency);

        PortableShaderRequest vertexRequest;
        vertexRequest.SourceName = "portable-vertex-fixture.slang";
        vertexRequest.Source = R"(
cbuffer View : register(b0, space0) { row_major float4x4 ViewProjection; };
struct VertexInput { float3 Position : POSITION; float2 TexCoord : TEXCOORD1; };
float4 main(VertexInput input) : SV_Position
{
    return mul(float4(input.Position + float3(input.TexCoord, 0.0), 1.0), ViewProjection);
}
)";
        vertexRequest.EntryPoint = "main";
        vertexRequest.Stage = RHI::ShaderStage::Vertex;
        vertexRequest.Targets = request.Targets;
        vertexRequest.CompilerIdentity = request.CompilerIdentity;
        vertexRequest.CompilerVersion = request.CompilerVersion;
        vertexRequest.CompilerPackageHash = request.CompilerPackageHash;
        vertexRequest.DownstreamCompilerPackageHash = request.DownstreamCompilerPackageHash;
        vertexRequest.ExpectedLayout = {
            { "View", 'b', 0, 0, RHI::ShaderStage::Vertex, "ConstantBuffer", "struct{ViewProjection:float32x4x4:row-major@0}", 1, 64, 0, 0 }
        };
        vertexRequest.ExpectedVertexInputs = {
            { "Position", "POSITION", 0, 0, "float32x3", 12, 1, 3 },
            { "TexCoord", "TEXCOORD", 1, 1, "float32x2", 8, 1, 2 }
        };
        const PortableShaderPackage vertexOutput = compiler.Compile(vertexRequest);
        if (!vertexOutput.Succeeded()) printShaderDiagnostics(vertexOutput);
        const bool vertexInterface = vertexOutput.Succeeded()
            && vertexOutput.VertexInputs == vertexRequest.ExpectedVertexInputs;

        PortableShaderRequest invalidSource = request;
        invalidSource.Source = "this is not valid Slang";
        const PortableShaderPackage sourceFailure = compiler.Compile(invalidSource);
        PortableShaderRequest invalidEntry = request;
        invalidEntry.EntryPoint = "missingEntry";
        const PortableShaderPackage entryFailure = compiler.Compile(invalidEntry);
        PortableShaderRequest mismatchedLayout = request;
        mismatchedLayout.ExpectedLayout[1].Register = 3;
        const PortableShaderPackage layoutFailure = compiler.Compile(mismatchedLayout);
        PortableShaderRequest mismatchedConvention = request;
        mismatchedConvention.Conventions.VulkanYFlip = false;
        const PortableShaderPackage conventionFailure = compiler.Compile(mismatchedConvention);
#ifndef _WIN32
        PortableShaderRequest unavailableDxil = request;
        unavailableDxil.Targets = { PortableShaderTarget::Dxil, PortableShaderTarget::Spirv };
        const PortableShaderPackage dxilFailure = compiler.Compile(unavailableDxil);
        const bool hostTargetContract = !dxilFailure.Succeeded() && !dxilFailure.Diagnostics.empty()
            && dxilFailure.Diagnostics.front().Message.find("DXIL target is unavailable on this host") != std::string::npos;
#else
        PortableShaderRequest incompletePackage = request;
        incompletePackage.Targets = { PortableShaderTarget::Dxil };
        const PortableShaderPackage incompleteFailure = compiler.Compile(incompletePackage);
        const bool hostTargetContract = !incompleteFailure.Succeeded() && !incompleteFailure.Diagnostics.empty()
            && incompleteFailure.Diagnostics.front().Message.find("requires the admitted paired DXIL+SPIR-V package") != std::string::npos;
#endif

        std::filesystem::remove_all(cacheDirectory, error);
        return Expect(compiled, "Slang produces the admitted host target package and linked-program reflection")
            & Expect(deterministic, "repeated Slang compilation returns deterministic cached bytes and reflection")
            & Expect(appliedInputs, "Slang defines, controlled includes, and optimization options change generated artifacts")
            & Expect(!optionFailure.Succeeded(), "unsupported compiler options return structured failures")
            & Expect(!dependencyFailure.Succeeded(), "undeclared include closure returns structured failure")
            & Expect(!staleHashFailure.Succeeded(), "stale dependency hashes return structured failure")
            & Expect(!unusedDependencyFailure.Succeeded(), "unused declared dependencies are rejected from the resolved closure")
            & Expect(vertexInterface, "Slang normalizes the vertex entry-point interface across DXIL and SPIR-V")
            & Expect(!sourceFailure.Succeeded() && !sourceFailure.Diagnostics.empty(), "invalid Slang source returns structured diagnostics")
            & Expect(!entryFailure.Succeeded() && !entryFailure.Diagnostics.empty(), "unknown Slang entry point returns structured diagnostics")
            & Expect(!layoutFailure.Succeeded() && !layoutFailure.Diagnostics.empty(), "real reflected layout rejects an expected-layout mismatch")
            & Expect(!conventionFailure.Succeeded() && !conventionFailure.Diagnostics.empty(),
                "a convention mismatch fails before artifact publication")
            & Expect(hostTargetContract, "unsupported host shader targets fail with explicit diagnostics");
    }

    Engine::PortableShaderPackage MakeSyntheticShaderPackage(const Engine::PortableShaderRequest& request, Engine::u8 seed)
    {
        Engine::PortableShaderPackage package;
        package.Key = Engine::PortableShaderContract::CacheKey(request);
        package.Dxil = { seed, static_cast<Engine::u8>(seed + 1) };
        package.Spirv = { static_cast<Engine::u8>(seed + 2), static_cast<Engine::u8>(seed + 3) };
        package.Reflection = request.ExpectedLayout;
        return package;
    }

    bool TestAsyncShaderPackagePublicationIsNonblockingDeduplicatedAndAtomic()
    {
        using namespace Engine;
        JobSystem& jobs = JobSystem::Get();
        jobs.Shutdown();
        jobs.Initialize(1);

        std::mutex gateMutex;
        std::condition_variable gate;
        bool entered = false;
        bool release = false;
        std::atomic<int> compileCalls = 0;
        AsyncShaderPackageService service([&](const PortableShaderRequest& request)
        {
            ++compileCalls;
            std::unique_lock lock(gateMutex);
            entered = true;
            gate.notify_all();
            gate.wait(lock, [&]() { return release; });
            return MakeSyntheticShaderPackage(request, 11);
        });

        PortableShaderRequest request;
        request.SourceName = "async-fixture.slang";
        request.Source = "float4 main() : SV_Target { return 1; }";
        request.EntryPoint = "main";
        request.Stage = RHI::ShaderStage::Pixel;
        request.Targets = { PortableShaderTarget::Dxil, PortableShaderTarget::Spirv };
        request.CompilerIdentity = "ControlledCompiler";

        const auto begin = std::chrono::steady_clock::now();
        const ShaderPackageRequestHandle first = service.Request(request);
        const auto requestDuration = std::chrono::steady_clock::now() - begin;
        const ShaderPackageRequestHandle duplicate = service.Request(request);
        {
            std::unique_lock lock(gateMutex);
            gate.wait_for(lock, std::chrono::seconds(2), [&]() { return entered; });
        }
        const ShaderPackageRequestResult pending = service.Poll(first);
        {
            std::scoped_lock lock(gateMutex);
            release = true;
        }
        gate.notify_all();
        jobs.WaitIdle();

        const ShaderPackageRequestResult published = service.Poll(first);
        const ShaderPackageRequestHandle cacheRequest = service.Request(request);
        const ShaderPackageRequestResult cacheHit = service.Poll(cacheRequest);
        const bool nonblocking = requestDuration < std::chrono::milliseconds(100);
        const bool deduplicated = first.Id == duplicate.Id && compileCalls == 1;
        const bool atomic = pending.Status == ShaderPackageRequestStatus::Pending && !pending.Package
            && published.Status == ShaderPackageRequestStatus::Success && published.Package
            && published.Package->Succeeded();
        const bool cacheReused = cacheHit.Status == ShaderPackageRequestStatus::CacheHit
            && cacheHit.Package == published.Package;

        AsyncShaderPackageService integrityService([](const PortableShaderRequest& value)
        {
            PortableShaderPackage package = MakeSyntheticShaderPackage(value, 19);
            package.Dxil.clear();
            return package;
        }, ShaderPackageExecutionMode::DeterministicInline);
        const ShaderPackageRequestResult incompletePublication = integrityService.Poll(integrityService.Request(request));
        PortableShaderRequest spirvOnlyRequest = request;
        spirvOnlyRequest.Targets = { PortableShaderTarget::Spirv };
        const ShaderPackageRequestResult spirvOnlyPublication = integrityService.Poll(integrityService.Request(spirvOnlyRequest));
        const bool publicationCompleteness = incompletePublication.Status == ShaderPackageRequestStatus::Failure
            && incompletePublication.Diagnostic.Message.find("missing requested DXIL artifact") != std::string::npos
            && spirvOnlyPublication.Succeeded() && spirvOnlyPublication.Package
            && spirvOnlyPublication.Package->Dxil.empty() && !spirvOnlyPublication.Package->Spirv.empty();

        service.Shutdown();
        integrityService.Shutdown();
        jobs.Shutdown();
        return Expect(nonblocking, "asynchronous shader Request returns before controlled compiler completion")
            && Expect(deduplicated, "identical in-flight shader keys share one compiler job")
            && Expect(atomic, "poll exposes no partial package before one immutable successful publication")
            && Expect(cacheReused, "completed shader packages publish a structured service cache hit")
            && Expect(publicationCompleteness,
                "async publication rejects missing requested artifacts and accepts SPIR-V-only packages");
    }

    bool TestAsyncShaderFailureRetentionInlineEquivalenceAndShutdownSafety()
    {
        using namespace Engine;
        PortableShaderRequest request;
        request.SourceName = "retention-fixture.slang";
        request.Source = "float4 main() : SV_Target { return 1; }";
        request.EntryPoint = "main";
        request.Stage = RHI::ShaderStage::Pixel;
        request.Targets = { PortableShaderTarget::Dxil, PortableShaderTarget::Spirv };

        const auto deterministicCompiler = [](const PortableShaderRequest& value)
        {
            return MakeSyntheticShaderPackage(value, 29);
        };
        AsyncShaderPackageService inlineService(deterministicCompiler, ShaderPackageExecutionMode::DeterministicInline);
        const ShaderPackageRequestResult inlineResult = inlineService.Poll(inlineService.Request(request));
        const PortableShaderPackage direct = deterministicCompiler(request);
        const bool inlineEquivalent = inlineResult.Succeeded()
            && inlineResult.Package->Dxil == direct.Dxil && inlineResult.Package->Spirv == direct.Spirv;

        std::shared_ptr<const PortableShaderPackage> active = inlineResult.Package;
        PortableShaderRequest failingRequest = request;
        failingRequest.Source += " invalid";
        AsyncShaderPackageService failingService([](const PortableShaderRequest& value)
        {
            PortableShaderPackage failed;
            failed.Key = PortableShaderContract::CacheKey(value);
            failed.Diagnostics.push_back({ value.SourceName, value.EntryPoint, "DXIL,SPIR-V", "ControlledCompiler", "injected compile failure" });
            return failed;
        }, ShaderPackageExecutionMode::DeterministicInline);
        const ShaderPackageRequestResult failed = failingService.Poll(failingService.Request(failingRequest));
        if (failed.Succeeded())
            active = failed.Package;
        const bool retained = failed.Status == ShaderPackageRequestStatus::Failure
            && active == inlineResult.Package && active && active->Succeeded();

        JobSystem& jobs = JobSystem::Get();
        jobs.Shutdown();
        jobs.Initialize(1);
        std::mutex gateMutex;
        std::condition_variable gate;
        bool entered = false;
        bool release = false;
        AsyncShaderPackageService cancellingService([&](const PortableShaderRequest& value)
        {
            std::unique_lock lock(gateMutex);
            entered = true;
            gate.notify_all();
            gate.wait(lock, [&]() { return release; });
            return MakeSyntheticShaderPackage(value, 41);
        });
        const ShaderPackageRequestHandle cancellationHandle = cancellingService.Request(request);
        {
            std::unique_lock lock(gateMutex);
            gate.wait_for(lock, std::chrono::seconds(2), [&]() { return entered; });
        }
        cancellingService.Shutdown();
        const ShaderPackageRequestResult cancelledBeforeCompletion = cancellingService.Poll(cancellationHandle);
        {
            std::scoped_lock lock(gateMutex);
            release = true;
        }
        gate.notify_all();
        jobs.WaitIdle();
        const ShaderPackageRequestResult cancelledAfterCompletion = cancellingService.Poll(cancellationHandle);
        jobs.Shutdown();
        const bool shutdownSafe = cancelledBeforeCompletion.Status == ShaderPackageRequestStatus::Cancelled
            && cancelledAfterCompletion.Status == ShaderPackageRequestStatus::Cancelled
            && !cancelledAfterCompletion.Package;

        int throwingCompileCalls = 0;
        AsyncShaderPackageService throwingService([&](const PortableShaderRequest& value)
        {
            if (++throwingCompileCalls == 1)
                throw std::runtime_error("injected compiler exception");
            return MakeSyntheticShaderPackage(value, 53);
        }, ShaderPackageExecutionMode::DeterministicInline);
        const ShaderPackageRequestHandle throwingHandle = throwingService.Request(request);
        const ShaderPackageRequestResult throwingFailure = throwingService.Poll(throwingHandle);
        const ShaderPackageRequestHandle retryHandle = throwingService.Request(request);
        const ShaderPackageRequestResult retrySuccess = throwingService.Poll(retryHandle);
        const bool exceptionRetry = throwingFailure.Status == ShaderPackageRequestStatus::Failure
            && throwingFailure.Diagnostic.Message.find("injected compiler exception") != std::string::npos
            && retryHandle.Id != throwingHandle.Id && retrySuccess.Succeeded()
            && throwingCompileCalls == 2;

        return Expect(inlineEquivalent, "deterministic inline mode uses equivalent compile and publication semantics")
            && Expect(retained, "a failed refresh cannot replace the consumer's last valid package")
            && Expect(shutdownSafe, "shutdown cancels publication and ignores late worker completion safely")
            && Expect(exceptionRetry, "compiler exceptions publish terminal failure, clear in-flight state, and permit retry");
    }

    Engine::RHI::TextureDescription MakeGraphTexture(std::string name, Engine::RHI::ResourceState initialState = Engine::RHI::ResourceState::Common)
    {
        Engine::RHI::TextureDescription description;
        description.DebugName = std::move(name);
        description.Extent = { 32, 32 };
        description.TextureFormat = Engine::RHI::Format::R8G8B8A8Unorm;
        description.InitialState = initialState;
        return description;
    }

    bool TestRenderGraphOrdersHazardsAndLifetimesDeterministically()
    {
        using namespace Engine;
        using namespace Engine::RHI;

        RenderGraph graph;
        const auto texture = graph.AddTexture(MakeGraphTexture("graph-order"));
        const auto writer = graph.AddPass("Writer");
        const auto reader = graph.AddPass("Reader");
        const auto independent = graph.AddPass("Independent");
        graph.AddWrite(writer, texture, ResourceState::RenderTarget);
        graph.AddRead(reader, texture);
        graph.AddDependency(independent, writer);

        const RenderGraph::CompileResult result = graph.Compile();
        const bool ordered = result.Success && result.Passes.size() == 3
            && result.Passes[0].Pass.Index == independent.Index
            && result.Passes[1].Pass.Index == writer.Index
            && result.Passes[2].Pass.Index == reader.Index;
        const bool hazards = result.Dependencies.size() == 2
            && result.Dependencies[0].Producer.Index == independent.Index
            && result.Dependencies[0].Consumer.Index == writer.Index
            && result.Dependencies[1].Producer.Index == writer.Index
            && result.Dependencies[1].Consumer.Index == reader.Index
            && result.Dependencies[1].Resource.Index == texture.Index;
        const bool lifetime = result.ResourceLifetimes.size() == 1
            && result.ResourceLifetimes[0].Used
            && result.ResourceLifetimes[0].FirstPass == 1
            && result.ResourceLifetimes[0].LastPass == 2;

        RenderGraph stableGraph;
        stableGraph.AddPass("First");
        stableGraph.AddPass("Second");
        stableGraph.AddPass("Third");
        const RenderGraph::CompileResult stable = stableGraph.Compile();
        const bool stableIndependentOrder = stable.Success && stable.Passes.size() == 3
            && stable.Passes[0].Pass.Index == 0 && stable.Passes[1].Pass.Index == 1 && stable.Passes[2].Pass.Index == 2;

        return Expect(ordered, "explicit ordering and RAW hazards produce deterministic topological pass order")
            && Expect(hazards, "the compiler records explicit and resource-hazard dependency edges")
            && Expect(lifetime, "resource lifetime intervals use compiled order rather than registration order")
            && Expect(stableIndependentOrder, "independent passes retain stable registration order");
    }

    bool TestRenderGraphTracksRawWarWawBarriersAndQueueTransitions()
    {
        using namespace Engine;
        using namespace Engine::RHI;

        RenderGraph graph;
        const auto texture = graph.AddTexture(MakeGraphTexture("graph-hazards"));
        const auto upload = graph.AddPass("Upload", QueueType::Copy);
        const auto sample = graph.AddPass("Sample", QueueType::Graphics);
        const auto overwrite = graph.AddPass("Overwrite", QueueType::Graphics);
        graph.AddWrite(upload, texture, ResourceState::CopyDest);
        graph.AddRead(sample, texture, ResourceState::ShaderResource, ShaderStage::Pixel);
        graph.AddWrite(overwrite, texture, ResourceState::RenderTarget);

        const RenderGraph::CompileResult result = graph.Compile();
        const bool hazards = result.Success && result.Dependencies.size() == 3
            && result.Dependencies[0].Producer.Index == upload.Index && result.Dependencies[0].Consumer.Index == sample.Index
            && result.Dependencies[1].Producer.Index == upload.Index && result.Dependencies[1].Consumer.Index == overwrite.Index
            && result.Dependencies[2].Producer.Index == sample.Index && result.Dependencies[2].Consumer.Index == overwrite.Index;
        const bool barriers = result.Barriers.size() == 3
            && result.Barriers[0].Pass.Index == upload.Index && result.Barriers[0].Before == ResourceState::Common && result.Barriers[0].After == ResourceState::CopyDest
            && result.Barriers[1].Pass.Index == sample.Index && result.Barriers[1].Before == ResourceState::CopyDest && result.Barriers[1].After == ResourceState::ShaderResource
            && result.Barriers[2].Pass.Index == overwrite.Index && result.Barriers[2].Before == ResourceState::ShaderResource && result.Barriers[2].After == ResourceState::RenderTarget;
        const bool queueTransition = result.QueueTransitions.size() == 1
            && result.QueueTransitions[0].Producer.Index == upload.Index && result.QueueTransitions[0].Consumer.Index == sample.Index
            && result.QueueTransitions[0].SourceQueue == QueueType::Copy && result.QueueTransitions[0].DestinationQueue == QueueType::Graphics
            && result.QueueTransitions[0].Before == ResourceState::CopyDest && result.QueueTransitions[0].After == ResourceState::ShaderResource;

        RenderGraph readWriteGraph;
        const auto imported = readWriteGraph.AddTexture(MakeGraphTexture("imported"), RenderGraph::ResourceLifetimeKind::Imported);
        const auto readWrite = readWriteGraph.AddPass("ReadWrite");
        readWriteGraph.AddReadWrite(readWrite, imported, ResourceState::UnorderedAccess, ShaderStage::Compute);
        const RenderGraph::CompileResult readWriteResult = readWriteGraph.Compile();
        const bool readWriteDeclaration = readWriteResult.Success && readWriteResult.Barriers.size() == 1
            && readWriteResult.Barriers[0].After == ResourceState::UnorderedAccess;

        return Expect(hazards, "RAW, WAW, and WAR hazards become ordered dependency records")
            && Expect(barriers, "state barriers are derived deterministically from ordered uses")
            && Expect(queueTransition, "cross-queue ordered uses produce an ownership and synchronization record")
            && Expect(readWriteDeclaration, "read-write declarations preserve explicit unordered-access and stage intent");
    }

    bool TestRenderGraphRejectsInvalidDeclarationsAndCycles()
    {
        using namespace Engine;
        using namespace Engine::RHI;

        RenderGraph readBeforeWrite;
        const auto transient = readBeforeWrite.AddTexture(MakeGraphTexture("transient"));
        const auto reader = readBeforeWrite.AddPass("Reader");
        readBeforeWrite.AddRead(reader, transient);
        const RenderGraph::CompileResult unresolved = readBeforeWrite.Compile();

        RenderGraph invalidUse;
        const auto pass = invalidUse.AddPass("Invalid");
        invalidUse.AddRead(pass, { 77 });
        const RenderGraph::CompileResult invalidHandle = invalidUse.Compile();

        RenderGraph duplicateUse;
        const auto duplicateTexture = duplicateUse.AddTexture(MakeGraphTexture("duplicate"), RenderGraph::ResourceLifetimeKind::Imported);
        const auto duplicatePass = duplicateUse.AddPass("Duplicate");
        duplicateUse.AddRead(duplicatePass, duplicateTexture);
        duplicateUse.AddWrite(duplicatePass, duplicateTexture, ResourceState::RenderTarget);
        const RenderGraph::CompileResult duplicate = duplicateUse.Compile();

        RenderGraph cycleGraph;
        const auto first = cycleGraph.AddPass("First");
        const auto second = cycleGraph.AddPass("Second");
        cycleGraph.AddDependency(first, second);
        cycleGraph.AddDependency(second, first);
        const RenderGraph::CompileResult cycle = cycleGraph.Compile();

        return Expect(!unresolved.Success && unresolved.Error.find("before any write") != std::string::npos, "transient reads without a producer are rejected")
            && Expect(!invalidHandle.Success && invalidHandle.Error.find("invalid resource") != std::string::npos, "invalid resource handles are rejected")
            && Expect(!duplicate.Success && duplicate.Error.find("same resource") != std::string::npos, "ambiguous duplicate declarations are rejected")
            && Expect(!cycle.Success && cycle.Error.find("cycle") != std::string::npos, "explicit dependency cycles are rejected");
    }

    bool TestRhiBufferTransitionContract()
    {
        using namespace Engine::RHI;
        const BufferUsage copyBuffer = static_cast<BufferUsage>(
            static_cast<Engine::u32>(BufferUsage::CopySource) | static_cast<Engine::u32>(BufferUsage::CopyDest));
        const BufferUsage readOnlyBuffer = static_cast<BufferUsage>(
            static_cast<Engine::u32>(BufferUsage::Vertex) | static_cast<Engine::u32>(BufferUsage::Constant));

        return Expect(IsBufferStateCompatible(copyBuffer, BufferCpuAccess::None, ResourceState::CopyDest), "copy destination buffers accept CopyDest")
            && Expect(IsBufferStateCompatible(copyBuffer, BufferCpuAccess::None, ResourceState::CopySource), "copy source buffers accept CopySource")
            && Expect(!IsBufferStateCompatible(copyBuffer, BufferCpuAccess::None, ResourceState::ShaderResource), "copy-only buffers reject read-only shader state")
            && Expect(IsBufferStateCompatible(readOnlyBuffer, BufferCpuAccess::None, ResourceState::ShaderResource), "vertex and constant buffers accept portable read-only state")
            && Expect(!IsBufferStateCompatible(readOnlyBuffer, BufferCpuAccess::None, ResourceState::UnorderedAccess), "read-only buffers reject UAV state")
            && Expect(!IsBufferStateCompatible(BufferUsage::Storage, BufferCpuAccess::Write, ResourceState::UnorderedAccess), "CPU-visible buffers reject transitions")
            && Expect(!IsBufferStateCompatible(BufferUsage::Storage, BufferCpuAccess::None, ResourceState::RenderTarget), "buffer-incompatible texture state is rejected")
            && Expect(!IsBufferStateCompatible(BufferUsage::Storage, BufferCpuAccess::None, ResourceState::Unknown), "unknown state is rejected");
    }

    bool TestRhiCompletionTokenContract()
    {
        using namespace Engine::RHI;
        const CompletionToken invalid {};
        const CompletionToken missingDevice { 0, 1 };
        const CompletionToken missingSubmission { 1, 0 };
        const CompletionToken issued { 7, 11 };
        return Expect(!invalid.IsValid(), "zero completion token is invalid")
            && Expect(!missingDevice.IsValid(), "completion token requires a device identity")
            && Expect(!missingSubmission.IsValid(), "completion token requires an issued submission identity")
            && Expect(issued.IsValid(), "completion token preserves opaque nonzero device and submission identities");
    }

    class RenderGraphTestTexture final : public Engine::RHI::Texture
    {
    public:
        RenderGraphTestTexture(Engine::u64 ownerId, Engine::RHI::TextureDescription description,
            Engine::RHI::ResourceState committedState)
            : m_OwnerId(ownerId), m_Description(std::move(description)), m_State(committedState) {}

        const Engine::RHI::TextureDescription& GetDescription() const override { return m_Description; }
        Engine::u64 GetOwnerId() const { return m_OwnerId; }
        Engine::RHI::ResourceState GetState() const { return m_State; }
        void SetState(Engine::RHI::ResourceState state) { m_State = state; }
        Engine::RHI::QueueType GetQueueOwner() const { return m_QueueOwner; }
        void SetQueueOwner(Engine::RHI::QueueType owner) { m_QueueOwner = owner; }

    private:
        Engine::u64 m_OwnerId = 0;
        Engine::RHI::TextureDescription m_Description;
        Engine::RHI::ResourceState m_State = Engine::RHI::ResourceState::Unknown;
        Engine::RHI::QueueType m_QueueOwner = Engine::RHI::QueueType::Graphics;
    };

    class RenderGraphTestBuffer final : public Engine::RHI::Buffer
    {
    public:
        RenderGraphTestBuffer(Engine::u64 ownerId, Engine::RHI::BufferDescription description,
            Engine::RHI::ResourceState committedState)
            : m_OwnerId(ownerId), m_Description(std::move(description)), m_State(committedState) {}

        const Engine::RHI::BufferDescription& GetDescription() const override { return m_Description; }
        void* Map() override { return nullptr; }
        void Unmap() override {}
        Engine::u64 GetOwnerId() const { return m_OwnerId; }
        Engine::RHI::ResourceState GetState() const { return m_State; }
        void SetState(Engine::RHI::ResourceState state) { m_State = state; }
        Engine::RHI::QueueType GetQueueOwner() const { return m_QueueOwner; }
        void SetQueueOwner(Engine::RHI::QueueType owner) { m_QueueOwner = owner; }

    private:
        Engine::u64 m_OwnerId = 0;
        Engine::RHI::BufferDescription m_Description;
        Engine::RHI::ResourceState m_State = Engine::RHI::ResourceState::Unknown;
        Engine::RHI::QueueType m_QueueOwner = Engine::RHI::QueueType::Graphics;
    };

    class RenderGraphTestCommandList final : public Engine::RHI::CommandList
    {
    public:
        RenderGraphTestCommandList(Engine::u32 id, Engine::RHI::QueueType queue, int& beginCount, std::vector<Engine::u32>& begunIds,
            std::vector<std::string>& events)
            : m_Id(id), m_Queue(queue), m_BeginCount(beginCount), m_BegunIds(begunIds), m_Events(events) {}

        Engine::RHI::QueueType GetQueueType() const override { return m_Queue; }
        bool Begin() override
        {
            if (m_Recording) return false;
            m_Recording = true;
            m_Closed = false;
            m_TextureTransitions.clear();
            m_BufferTransitions.clear();
            m_BufferOwnershipOperations.clear();
            m_TextureOwnershipOperations.clear();
            ++m_BeginCount;
            m_BegunIds.push_back(m_Id);
            return true;
        }
        bool End() override
        {
            if (!m_Recording) return false;
            m_Recording = false;
            m_Closed = true;
            return true;
        }
        void BeginDebugMarker(std::string_view) override {}
        void EndDebugMarker() override {}
        bool BindViewportOutputs(Engine::RHI::Texture&, Engine::RHI::Texture*) override { return m_Recording; }
        bool ClearViewportOutputs(const Engine::RHI::ViewportClear&) override { return m_Recording; }
        bool TransitionTexture(Engine::RHI::Texture& texture, Engine::RHI::ResourceState state) override
        {
            auto* resource = dynamic_cast<RenderGraphTestTexture*>(&texture);
            if (!m_Recording || !resource || state == Engine::RHI::ResourceState::Unknown) return false;
            m_TextureTransitions.push_back({ resource, state });
            m_Events.push_back("texture:" + resource->GetDescription().DebugName + ":" + Engine::RenderGraph::ToString(state));
            return true;
        }
        bool TransitionBuffer(Engine::RHI::Buffer& buffer, Engine::RHI::ResourceState state) override
        {
            auto* resource = dynamic_cast<RenderGraphTestBuffer*>(&buffer);
            if (!m_Recording || !resource || state == Engine::RHI::ResourceState::Unknown) return false;
            m_BufferTransitions.push_back({ resource, state });
            m_Events.push_back("buffer:" + resource->GetDescription().DebugName + ":" + Engine::RenderGraph::ToString(state));
            return true;
        }
        bool ReleaseBufferOwnership(const Engine::RHI::BufferOwnershipRelease& release) override
        {
            if (!m_Recording || !release.Resource || std::any_of(m_BufferOwnershipOperations.begin(), m_BufferOwnershipOperations.end(),
                [&](const auto& operation) { return operation.Resource == release.Resource; })) return false;
            m_BufferOwnershipOperations.push_back({ BufferOwnershipOperationKind::Release, release, {} });
            return true;
        }
        bool AcquireBufferOwnership(const Engine::RHI::BufferOwnershipAcquire& acquire) override
        {
            if (!m_Recording || !acquire.Resource || std::any_of(m_BufferOwnershipOperations.begin(), m_BufferOwnershipOperations.end(),
                [&](const auto& operation) { return operation.Resource == acquire.Resource; })) return false;
            m_BufferOwnershipOperations.push_back({ BufferOwnershipOperationKind::Acquire, {}, acquire });
            return true;
        }
        bool ReleaseTextureOwnership(const Engine::RHI::TextureOwnershipRelease& release) override
        {
            if (!m_Recording || !release.Resource || std::any_of(m_TextureOwnershipOperations.begin(), m_TextureOwnershipOperations.end(),
                [&](const auto& operation) { return operation.Resource == release.Resource; })) return false;
            m_TextureOwnershipOperations.push_back({ TextureOwnershipOperationKind::Release, release, {} });
            return true;
        }
        bool AcquireTextureOwnership(const Engine::RHI::TextureOwnershipAcquire& acquire) override
        {
            if (!m_Recording || !acquire.Resource || std::any_of(m_TextureOwnershipOperations.begin(), m_TextureOwnershipOperations.end(),
                [&](const auto& operation) { return operation.Resource == acquire.Resource; })) return false;
            m_TextureOwnershipOperations.push_back({ TextureOwnershipOperationKind::Acquire, {}, acquire });
            return true;
        }
        void SetGraphicsPipeline(Engine::RHI::Pipeline&) override {}
        void SetGraphicsConstantBuffer(Engine::u32, Engine::RHI::Buffer&) override {}
        void SetViewport(const Engine::RHI::Viewport&) override {}
        void SetScissorRect(const Engine::RHI::ScissorRect&) override {}
        void SetVertexBuffer(Engine::u32, Engine::RHI::Buffer&) override {}
        void SetIndexBuffer(Engine::RHI::Buffer&, Engine::RHI::IndexFormat) override {}
        bool CopyBuffer(Engine::RHI::Buffer&, Engine::u64, Engine::RHI::Buffer&, Engine::u64, Engine::u64) override { return m_Recording; }
        void DrawIndexed(Engine::u32, Engine::u32, Engine::u32, int, Engine::u32) override {}
        void ResetQueryPool(Engine::RHI::QueryPool&, Engine::u32, Engine::u32) override {}
        void WriteTimestamp(Engine::RHI::QueryPool&, Engine::u32) override {}
        void ResolveQueryPool(Engine::RHI::QueryPool&, Engine::u32, Engine::u32) override {}

        Engine::u32 GetId() const { return m_Id; }
        bool Commit()
        {
            if (!m_Closed) return false;
            for (const TextureTransition& transition : m_TextureTransitions) transition.Resource->SetState(transition.State);
            for (const BufferTransition& transition : m_BufferTransitions) transition.Resource->SetState(transition.State);
            m_Closed = false;
            return true;
        }
        enum class BufferOwnershipOperationKind { Release, Acquire };
        struct BufferOwnershipOperation
        {
            BufferOwnershipOperationKind Kind;
            Engine::RHI::BufferOwnershipRelease Release;
            Engine::RHI::BufferOwnershipAcquire Acquire;
            Engine::RHI::Buffer* Resource = nullptr;
            BufferOwnershipOperation(BufferOwnershipOperationKind kind, Engine::RHI::BufferOwnershipRelease release,
                Engine::RHI::BufferOwnershipAcquire acquire)
                : Kind(kind), Release(release), Acquire(acquire), Resource(kind == BufferOwnershipOperationKind::Release ? release.Resource : acquire.Resource) {}
        };
        enum class TextureOwnershipOperationKind { Release, Acquire };
        struct TextureOwnershipOperation
        {
            TextureOwnershipOperationKind Kind;
            Engine::RHI::TextureOwnershipRelease Release;
            Engine::RHI::TextureOwnershipAcquire Acquire;
            Engine::RHI::Texture* Resource = nullptr;
            TextureOwnershipOperation(TextureOwnershipOperationKind kind, Engine::RHI::TextureOwnershipRelease release,
                Engine::RHI::TextureOwnershipAcquire acquire)
                : Kind(kind), Release(release), Acquire(acquire), Resource(kind == TextureOwnershipOperationKind::Release ? release.Resource : acquire.Resource) {}
        };
        const std::vector<BufferOwnershipOperation>& BufferOwnershipOperations() const { return m_BufferOwnershipOperations; }
        const std::vector<TextureOwnershipOperation>& TextureOwnershipOperations() const { return m_TextureOwnershipOperations; }

    private:
        struct TextureTransition { RenderGraphTestTexture* Resource = nullptr; Engine::RHI::ResourceState State = Engine::RHI::ResourceState::Unknown; };
        struct BufferTransition { RenderGraphTestBuffer* Resource = nullptr; Engine::RHI::ResourceState State = Engine::RHI::ResourceState::Unknown; };
        Engine::u32 m_Id = 0;
        Engine::RHI::QueueType m_Queue = Engine::RHI::QueueType::Graphics;
        int& m_BeginCount;
        std::vector<Engine::u32>& m_BegunIds;
        std::vector<std::string>& m_Events;
        bool m_Recording = false;
        bool m_Closed = false;
        std::vector<TextureTransition> m_TextureTransitions;
        std::vector<BufferTransition> m_BufferTransitions;
        std::vector<BufferOwnershipOperation> m_BufferOwnershipOperations;
        std::vector<TextureOwnershipOperation> m_TextureOwnershipOperations;
    };

    class RenderGraphTestDevice final : public Engine::RHI::Device
    {
    public:
        explicit RenderGraphTestDevice(Engine::u64 ownerId = 7001, bool computeIndependent = false, bool copyIndependent = false)
            : m_OwnerId(ownerId), m_ComputeIndependent(computeIndependent), m_CopyIndependent(copyIndependent)
        {
            m_Completions.push_back(Engine::RHI::CompletionStatus::Invalid);
        }

        const Engine::RHI::DeviceDescription& GetDescription() const override { return m_Description; }
        const Engine::RHI::DeviceCapabilities& GetCapabilities() const override { return m_Capabilities; }
        Engine::RHI::QueueResolution ResolveQueue(Engine::RHI::QueueType requested) const override
        {
            const Engine::RHI::QueueType effective = requested == Engine::RHI::QueueType::Compute && !m_ComputeIndependent
                ? Engine::RHI::QueueType::Graphics
                : requested == Engine::RHI::QueueType::Copy && !m_CopyIndependent ? Engine::RHI::QueueType::Graphics : requested;
            return { requested, effective, requested == Engine::RHI::QueueType::Graphics || requested == effective };
        }
        Engine::Scope<Engine::RHI::Buffer> CreateBuffer(const Engine::RHI::BufferDescription& description) override
        {
            return Engine::CreateScope<RenderGraphTestBuffer>(m_OwnerId, description, description.InitialState);
        }
        Engine::Scope<Engine::RHI::Texture> CreateTexture(const Engine::RHI::TextureDescription& description) override
        {
            return Engine::CreateScope<RenderGraphTestTexture>(m_OwnerId, description, description.InitialState);
        }
        bool OwnsResource(const Engine::RHI::Buffer* resource) const override
        {
            const auto* buffer = dynamic_cast<const RenderGraphTestBuffer*>(resource);
            return buffer && buffer->GetOwnerId() == m_OwnerId;
        }
        bool OwnsResource(const Engine::RHI::Texture* resource) const override
        {
            const auto* texture = dynamic_cast<const RenderGraphTestTexture*>(resource);
            return texture && texture->GetOwnerId() == m_OwnerId;
        }
        bool QueryResourceState(const Engine::RHI::Buffer* resource, Engine::RHI::ResourceState& state) const override
        {
            const auto* buffer = dynamic_cast<const RenderGraphTestBuffer*>(resource);
            if (!OwnsResource(resource) || buffer->GetState() == Engine::RHI::ResourceState::Unknown) return false;
            state = buffer->GetState();
            return true;
        }
        bool QueryResourceState(const Engine::RHI::Texture* resource, Engine::RHI::ResourceState& state) const override
        {
            const auto* texture = dynamic_cast<const RenderGraphTestTexture*>(resource);
            if (!OwnsResource(resource) || texture->GetState() == Engine::RHI::ResourceState::Unknown) return false;
            state = texture->GetState();
            return true;
        }
        bool QueryBufferQueueOwner(const Engine::RHI::Buffer* resource, Engine::RHI::QueueType& owner) const override
        { const auto* buffer = dynamic_cast<const RenderGraphTestBuffer*>(resource); if (!OwnsResource(resource) || !buffer) return false; owner = buffer->GetQueueOwner(); return true; }
        bool QueryTextureQueueOwner(const Engine::RHI::Texture* resource, Engine::RHI::QueueType& owner) const override
        { const auto* texture = dynamic_cast<const RenderGraphTestTexture*>(resource); if (!OwnsResource(resource) || !texture) return false; owner = texture->GetQueueOwner(); return true; }
        Engine::Scope<Engine::RHI::Shader> CreateShader(const Engine::RHI::ShaderDescription&) override { return nullptr; }
        Engine::Scope<Engine::RHI::Pipeline> CreatePipeline(const Engine::RHI::PipelineDescription&) override { return nullptr; }
        Engine::Scope<Engine::RHI::QueryPool> CreateQueryPool(const Engine::RHI::QueryPoolDescription&) override { return nullptr; }
        Engine::Scope<Engine::RHI::CommandList> CreateCommandList(Engine::RHI::QueueType queue, std::string_view) override
        {
            const Engine::u32 id = ++CreatedCommandListCount;
            return Engine::CreateScope<RenderGraphTestCommandList>(id, queue, BeginCount, BegunCommandListIds, Events);
        }
        bool UploadBuffer(Engine::RHI::Buffer&, const void*, Engine::u64, Engine::u64) override { return false; }
        bool ReadbackTexture(Engine::RHI::Texture&, Engine::RHI::TextureReadback&) override { return false; }
        Engine::RHI::CompletionToken Submit(Engine::RHI::CommandList& commandList) override
        {
            return Submit(commandList, {});
        }
        Engine::RHI::CompletionToken Submit(Engine::RHI::CommandList& commandList,
            const std::vector<Engine::RHI::CompletionToken>& dependencies) override
        {
            auto* list = dynamic_cast<RenderGraphTestCommandList*>(&commandList);
            const Engine::u64 submissionId = m_Completions.size();
            const Engine::RHI::SubmissionDependencyError dependencyError = Engine::RHI::ValidateSubmissionDependencies(
                m_OwnerId, submissionId, dependencies,
                [this](const Engine::RHI::CompletionToken& dependency)
                {
                    return dependency.SubmissionId > 0 && dependency.SubmissionId < m_Completions.size();
                });
            if (!list || dependencyError != Engine::RHI::SubmissionDependencyError::None)
                return {};
            const auto hasDependency = [&](const Engine::RHI::CompletionToken& token) { return std::any_of(dependencies.begin(), dependencies.end(), [&](const auto& item) { return item.DeviceId == token.DeviceId && item.SubmissionId == token.SubmissionId; }); };
            const auto bufferOperationValid = [&](const RenderGraphTestCommandList::BufferOwnershipOperation& operation)
            {
                return operation.Resource && (operation.Kind == RenderGraphTestCommandList::BufferOwnershipOperationKind::Release
                    ? operation.Release.SourceQueue == list->GetQueueType()
                    : hasDependency(operation.Acquire.ReleaseToken) && operation.Acquire.DestinationQueue == list->GetQueueType());
            };
            const auto textureOperationValid = [&](const RenderGraphTestCommandList::TextureOwnershipOperation& operation)
            {
                return operation.Resource && (operation.Kind == RenderGraphTestCommandList::TextureOwnershipOperationKind::Release
                    ? operation.Release.SourceQueue == list->GetQueueType()
                    : hasDependency(operation.Acquire.ReleaseToken) && operation.Acquire.DestinationQueue == list->GetQueueType());
            };
            if (std::any_of(list->BufferOwnershipOperations().begin(), list->BufferOwnershipOperations().end(),
                [&](const auto& operation) { return !bufferOperationValid(operation); })
                || std::any_of(list->TextureOwnershipOperations().begin(), list->TextureOwnershipOperations().end(),
                    [&](const auto& operation) { return !textureOperationValid(operation); })) return {};
            for (const Engine::RHI::CompletionToken& dependency : dependencies)
            {
                DependencyOrder.push_back(dependency);
                if (ResolveQueue(list->GetQueueType()).Effective
                    == m_SubmissionQueues[static_cast<size_t>(dependency.SubmissionId)])
                    ++ElidedDependencyCount;
                else
                    ++GpuWaitDependencyCount;
            }
            if (!list->Commit()) return {};
            m_Completions.push_back(Engine::RHI::CompletionStatus::Incomplete);
            m_SubmissionQueues.push_back(ResolveQueue(list->GetQueueType()).Effective);
            for (const auto& operation : list->BufferOwnershipOperations())
            {
                PublishedBufferOwnershipOperations.push_back(operation.Resource);
                if (operation.Kind == RenderGraphTestCommandList::BufferOwnershipOperationKind::Acquire)
                    if (auto* buffer = dynamic_cast<RenderGraphTestBuffer*>(operation.Resource))
                    {
                        buffer->SetState(operation.Acquire.After);
                        buffer->SetQueueOwner(ResolveQueue(operation.Acquire.DestinationQueue).Effective);
                    }
            }
            for (const auto& operation : list->TextureOwnershipOperations())
            {
                PublishedTextureOwnershipOperations.push_back(operation.Resource);
                if (operation.Kind == RenderGraphTestCommandList::TextureOwnershipOperationKind::Acquire)
                    if (auto* texture = dynamic_cast<RenderGraphTestTexture*>(operation.Resource))
                    {
                        texture->SetState(operation.Acquire.After);
                        texture->SetQueueOwner(ResolveQueue(operation.Acquire.DestinationQueue).Effective);
                    }
            }
            ++SubmitCount;
            ++NativeSubmissionCount;
            SubmittedCommandListIds.push_back(list->GetId());
            return { m_OwnerId, submissionId };
        }
        Engine::RHI::CompletionStatus QueryCompletion(const Engine::RHI::CompletionToken& token) override
        {
            if (token.DeviceId != m_OwnerId || token.SubmissionId == 0 || token.SubmissionId >= m_Completions.size()) return Engine::RHI::CompletionStatus::Invalid;
            return m_Completions[static_cast<size_t>(token.SubmissionId)];
        }
        bool WaitForCompletion(const Engine::RHI::CompletionToken& token, Engine::u32) override
        {
            return QueryCompletion(token) == Engine::RHI::CompletionStatus::Complete;
        }
        bool SubmitAndWait(Engine::RHI::CommandList& commandList) override
        {
            const Engine::RHI::CompletionToken token = Submit(commandList);
            if (!token.IsValid()) return false;
            SetCompletion(token, Engine::RHI::CompletionStatus::Complete);
            return true;
        }
        void WaitIdle() override {}

        void SetCompletion(const Engine::RHI::CompletionToken& token, Engine::RHI::CompletionStatus status)
        {
            if (token.DeviceId == m_OwnerId && token.SubmissionId > 0 && token.SubmissionId < m_Completions.size())
                m_Completions[static_cast<size_t>(token.SubmissionId)] = status;
        }

        int BeginCount = 0;
        int SubmitCount = 0;
        int NativeSubmissionCount = 0;
        int ElidedDependencyCount = 0;
        int GpuWaitDependencyCount = 0;
        Engine::u32 CreatedCommandListCount = 0;
        std::vector<Engine::u32> BegunCommandListIds;
        std::vector<Engine::u32> SubmittedCommandListIds;
        std::vector<Engine::RHI::CompletionToken> DependencyOrder;
        std::vector<std::string> Events;
        std::vector<Engine::RHI::Buffer*> PublishedBufferOwnershipOperations;
        std::vector<Engine::RHI::Texture*> PublishedTextureOwnershipOperations;

    private:
        Engine::u64 m_OwnerId = 0;
        bool m_ComputeIndependent = false;
        bool m_CopyIndependent = false;
        Engine::RHI::DeviceDescription m_Description;
        Engine::RHI::DeviceCapabilities m_Capabilities;
        std::vector<Engine::RHI::CompletionStatus> m_Completions;
        std::vector<Engine::RHI::QueueType> m_SubmissionQueues { Engine::RHI::QueueType::Graphics };
    };

    bool TestRhiSubmissionDependencyValidation()
    {
        using namespace Engine::RHI;
        const Engine::u64 deviceId = 91;
        const Engine::u64 prospectiveSubmissionId = 10;
        const auto issued = [](const CompletionToken& token)
        {
            return token.SubmissionId == 1 || token.SubmissionId == 2 || token.SubmissionId == 4;
        };
        return Expect(ValidateSubmissionDependencies(deviceId, prospectiveSubmissionId, { { deviceId, 1 } }, issued)
                == SubmissionDependencyError::None, "one prior issued dependency is accepted")
            && Expect(ValidateSubmissionDependencies(deviceId, prospectiveSubmissionId, { { deviceId, 1 }, { deviceId, 4 } }, issued)
                == SubmissionDependencyError::None, "multiple prior dependencies retain input order and are accepted")
            && Expect(ValidateSubmissionDependencies(deviceId, prospectiveSubmissionId, { {} }, issued)
                == SubmissionDependencyError::Zero, "zero dependency is rejected")
            && Expect(ValidateSubmissionDependencies(deviceId, prospectiveSubmissionId, { { deviceId + 1, 1 } }, issued)
                == SubmissionDependencyError::ForeignDevice, "foreign-device dependency is rejected")
            && Expect(ValidateSubmissionDependencies(deviceId, prospectiveSubmissionId, { { deviceId, 3 } }, issued)
                == SubmissionDependencyError::Unissued, "unissued prior identity is rejected")
            && Expect(ValidateSubmissionDependencies(deviceId, prospectiveSubmissionId, { { deviceId, 1 }, { deviceId, 1 } }, issued)
                == SubmissionDependencyError::Duplicate, "duplicate dependency is rejected")
            && Expect(ValidateSubmissionDependencies(deviceId, prospectiveSubmissionId, { { deviceId, 11 } }, issued)
                == SubmissionDependencyError::Forward, "forward dependency is rejected")
            && Expect(ValidateSubmissionDependencies(deviceId, prospectiveSubmissionId, { { deviceId, 10 } }, issued)
                == SubmissionDependencyError::ImpossibleSelf, "prospective self dependency is rejected")
            && Expect(ValidateSubmissionDependencies(deviceId, prospectiveSubmissionId, { { deviceId, 10 }, { deviceId, 1 } }, issued)
                != SubmissionDependencyError::None, "the prior-only token model rejects a cycle-forming edge");
    }

    bool TestRhiQueueTopologyDependencySubmissionContract()
    {
        using namespace Engine::RHI;
        constexpr Engine::u64 ownerId = 8101;
        RenderGraphTestDevice device(ownerId, true, false);
        const QueueResolution graphics = device.ResolveQueue(QueueType::Graphics);
        const QueueResolution compute = device.ResolveQueue(QueueType::Compute);
        const QueueResolution copy = device.ResolveQueue(QueueType::Copy);

        Engine::Scope<CommandList> copyList = device.CreateCommandList(QueueType::Copy, "dependency-copy");
        Engine::Scope<CommandList> graphicsList = device.CreateCommandList(QueueType::Graphics, "dependency-graphics");
        Engine::Scope<CommandList> computeList = device.CreateCommandList(QueueType::Compute, "dependency-compute");
        const bool closed = copyList && graphicsList && computeList && copyList->Begin() && copyList->End()
            && graphicsList->Begin() && graphicsList->End() && computeList->Begin() && computeList->End();
        const CompletionToken copyToken = closed ? device.Submit(*copyList) : CompletionToken {};
        const CompletionToken graphicsToken = copyToken.IsValid() ? device.Submit(*graphicsList, { copyToken }) : CompletionToken {};
        const CompletionToken computeToken = graphicsToken.IsValid() ? device.Submit(*computeList, { copyToken, graphicsToken }) : CompletionToken {};

        BufferDescription description;
        description.DebugName = "dependency-state-publication";
        description.SizeBytes = sizeof(Engine::u32);
        description.Usage = static_cast<BufferUsage>(static_cast<Engine::u32>(BufferUsage::CopySource) | static_cast<Engine::u32>(BufferUsage::CopyDest));
        description.InitialState = ResourceState::CopyDest;
        Engine::Scope<Buffer> buffer = device.CreateBuffer(description);
        Engine::Scope<CommandList> failureList = device.CreateCommandList(QueueType::Graphics, "dependency-validation-failure");
        const bool pending = buffer && failureList && failureList->Begin()
            && failureList->TransitionBuffer(*buffer, ResourceState::CopySource) && failureList->End();
        ResourceState observed = ResourceState::Unknown;
        const int submissionsBeforeFailure = device.NativeSubmissionCount;
        const bool invalidRejected = pending
            && !device.Submit(*failureList, { {} }).IsValid()
            && !device.Submit(*failureList, { { ownerId + 1, 1 } }).IsValid()
            && !device.Submit(*failureList, { { ownerId, 99 } }).IsValid()
            && !device.Submit(*failureList, { copyToken, copyToken }).IsValid()
            && !device.Submit(*failureList, { { ownerId, 4 } }).IsValid();
        const bool unpublished = invalidRejected && device.NativeSubmissionCount == submissionsBeforeFailure
            && device.QueryResourceState(buffer.get(), observed) && observed == ResourceState::CopyDest;
        const CompletionToken accepted = unpublished ? device.Submit(*failureList, { graphicsToken }) : CompletionToken {};
        const bool published = accepted.IsValid() && device.QueryResourceState(buffer.get(), observed) && observed == ResourceState::CopySource;

        device.SetCompletion(copyToken, CompletionStatus::Complete);
        const bool independentRetirement = device.QueryCompletion(copyToken) == CompletionStatus::Complete
            && device.QueryCompletion(graphicsToken) == CompletionStatus::Incomplete
            && device.QueryCompletion(computeToken) == CompletionStatus::Incomplete;
        device.SetCompletion(graphicsToken, CompletionStatus::Complete);
        const bool queryableAfterRetirement = device.QueryCompletion(copyToken) == CompletionStatus::Complete
            && device.QueryCompletion(graphicsToken) == CompletionStatus::Complete;

        return Expect(graphics.Requested == QueueType::Graphics && graphics.Effective == QueueType::Graphics && graphics.Independent,
                "graphics resolves to its enabled independent queue")
            && Expect(compute.Requested == QueueType::Compute && compute.Effective == QueueType::Compute && compute.Independent,
                "enabled compute resolves independently")
            && Expect(copy.Requested == QueueType::Copy && copy.Effective == QueueType::Graphics && !copy.Independent,
                "unavailable copy resolves to explicit graphics fallback")
            && Expect(copyToken.IsValid() && graphicsToken.IsValid() && computeToken.IsValid(), "one and multiple prior dependencies submit")
            && Expect(device.DependencyOrder.size() == 4 && device.DependencyOrder[1].SubmissionId == copyToken.SubmissionId
                && device.DependencyOrder[2].SubmissionId == graphicsToken.SubmissionId, "multiple dependencies preserve caller order")
            && Expect(device.ElidedDependencyCount == 2, "same-effective-queue dependencies are elided")
            && Expect(device.GpuWaitDependencyCount == 2, "distinct-effective-queue dependencies enqueue GPU waits in input order")
            && Expect(unpublished, "dependency failure issues no native submission and publishes no pending state")
            && Expect(published, "a later valid dependency publishes pending state exactly at accepted submission")
            && Expect(independentRetirement && queryableAfterRetirement, "producer and consumer tokens retire independently and remain queryable");
    }

    Engine::RHI::TextureDescription MakeExecutionTexture(std::string name,
        Engine::RHI::ResourceState initialState = Engine::RHI::ResourceState::CopyDest)
    {
        Engine::RHI::TextureDescription description;
        description.DebugName = std::move(name);
        description.Extent = { 8, 8 };
        description.TextureFormat = Engine::RHI::Format::R8G8B8A8Unorm;
        description.Usage = static_cast<Engine::RHI::TextureUsage>(
            static_cast<Engine::u32>(Engine::RHI::TextureUsage::RenderTarget)
            | static_cast<Engine::u32>(Engine::RHI::TextureUsage::CopySource)
            | static_cast<Engine::u32>(Engine::RHI::TextureUsage::CopyDest));
        description.InitialState = initialState;
        return description;
    }

    Engine::RHI::BufferDescription MakeExecutionBuffer(std::string name,
        Engine::RHI::ResourceState initialState = Engine::RHI::ResourceState::CopyDest)
    {
        Engine::RHI::BufferDescription description;
        description.DebugName = std::move(name);
        description.SizeBytes = 64;
        description.Usage = static_cast<Engine::RHI::BufferUsage>(
            static_cast<Engine::u32>(Engine::RHI::BufferUsage::CopySource)
            | static_cast<Engine::u32>(Engine::RHI::BufferUsage::CopyDest));
        description.InitialState = initialState;
        return description;
    }

    bool TestRenderGraphExecutorRejectsBindingsBeforeRecording()
    {
        using namespace Engine;
        using namespace Engine::RHI;
        int callbacks = 0;
        RenderGraph graph;
        const TextureDescription description = MakeExecutionTexture("binding");
        const auto resource = graph.AddTexture(description, RenderGraph::ResourceLifetimeKind::Imported);
        const auto pass = graph.AddPass("Read");
        graph.AddRead(pass, resource, ResourceState::CopySource, ShaderStage::Pixel);
        graph.SetPassCallback(pass, [&callbacks](RenderGraph::ExecutionContext&) { ++callbacks; return true; });
        const RenderGraph::CompileResult compiled = graph.Compile();

        RenderGraphTestDevice foreignDevice(8001);
        RenderGraphTestTexture foreign(8002, description, ResourceState::CopyDest);
        const bool foreignBound = graph.BindTexture(resource, foreign);
        const RenderGraph::ExecuteResult foreignResult = graph.Execute(foreignDevice, compiled);

        RenderGraph mismatchGraph;
        const auto mismatchResource = mismatchGraph.AddTexture(description, RenderGraph::ResourceLifetimeKind::Imported);
        const auto mismatchPass = mismatchGraph.AddPass("Read");
        mismatchGraph.AddRead(mismatchPass, mismatchResource, ResourceState::CopySource, ShaderStage::Pixel);
        mismatchGraph.SetPassCallback(mismatchPass, [&callbacks](RenderGraph::ExecutionContext&) { ++callbacks; return true; });
        RenderGraphTestDevice mismatchDevice(8003);
        RenderGraphTestTexture mismatch(8003, description, ResourceState::Common);
        const bool mismatchBound = mismatchGraph.BindTexture(mismatchResource, mismatch);
        const RenderGraph::ExecuteResult mismatchResult = mismatchGraph.Execute(mismatchDevice, mismatchGraph.Compile());

        RenderGraph invalidDescriptionGraph;
        const auto invalidResource = invalidDescriptionGraph.AddTexture(description, RenderGraph::ResourceLifetimeKind::Imported);
        const auto invalidPass = invalidDescriptionGraph.AddPass("Read");
        invalidDescriptionGraph.AddRead(invalidPass, invalidResource, ResourceState::CopySource, ShaderStage::Pixel);
        invalidDescriptionGraph.SetPassCallback(invalidPass, [&callbacks](RenderGraph::ExecutionContext&) { ++callbacks; return true; });
        TextureDescription wrongDescription = description;
        wrongDescription.Extent.Width += 1;
        RenderGraphTestTexture wrongDescriptionTexture(8004, wrongDescription, ResourceState::CopyDest);
        RenderGraphTestDevice invalidDescriptionDevice(8004);
        const bool invalidDescriptionBound = invalidDescriptionGraph.BindTexture(invalidResource, wrongDescriptionTexture);
        const RenderGraph::ExecuteResult invalidDescriptionResult = invalidDescriptionGraph.Execute(invalidDescriptionDevice, invalidDescriptionGraph.Compile());

        return Expect(compiled.Success && foreignBound && !foreignResult.Success, "foreign resource binding is rejected by the exact device before execution")
            && Expect(mismatchBound && !mismatchResult.Success, "imported committed-state mismatch is rejected before execution")
            && Expect(!invalidDescriptionBound && !invalidDescriptionResult.Success, "description mismatch cannot become a physical graph binding")
            && Expect(callbacks == 0, "binding validation failures invoke no pass callback")
            && Expect(foreignDevice.BeginCount == 0 && mismatchDevice.BeginCount == 0 && invalidDescriptionDevice.BeginCount == 0,
                "binding validation failures begin no command recording")
            && Expect(foreignDevice.SubmitCount == 0 && mismatchDevice.SubmitCount == 0 && invalidDescriptionDevice.SubmitCount == 0,
                "binding validation failures submit no work");
    }

    bool TestRenderGraphExecutorStopsAfterCallbackFailure()
    {
        using namespace Engine;
        using namespace Engine::RHI;
        RenderGraph graph;
        const TextureDescription description = MakeExecutionTexture("failure", ResourceState::Common);
        const auto resource = graph.AddTexture(description, RenderGraph::ResourceLifetimeKind::Imported);
        const auto failing = graph.AddPass("Failing");
        const auto later = graph.AddPass("Later");
        graph.AddRead(failing, resource, ResourceState::Common, ShaderStage::Pixel);
        graph.AddRead(later, resource, ResourceState::Common, ShaderStage::Pixel);
        graph.AddDependency(failing, later);
        int failingCallbacks = 0;
        int laterCallbacks = 0;
        graph.SetPassCallback(failing, [&failingCallbacks](RenderGraph::ExecutionContext&) { ++failingCallbacks; return false; });
        graph.SetPassCallback(later, [&laterCallbacks](RenderGraph::ExecutionContext&) { ++laterCallbacks; return true; });
        RenderGraphTestTexture texture(8101, description, ResourceState::Common);
        RenderGraphTestDevice device(8101);
        const bool bound = graph.BindTexture(resource, texture);
        const RenderGraph::ExecuteResult result = graph.Execute(device, graph.Compile());
        return Expect(bound && !result.Success && result.Error.find("callback failed") != std::string::npos, "callback failure fails graph execution explicitly")
            && Expect(failingCallbacks == 1 && laterCallbacks == 0, "callback failure prevents every later callback")
            && Expect(device.BeginCount == 1 && device.SubmitCount == 0, "callback failure records no submission")
            && Expect(!result.Completion.IsValid(), "callback failure publishes no completion token or success");
    }

    bool TestRenderGraphExecutorOrdersBarriersAndRestrictsContext()
    {
        using namespace Engine;
        using namespace Engine::RHI;
        RenderGraph graph;
        const TextureDescription colorDescription = MakeExecutionTexture("color");
        const BufferDescription bufferDescription = MakeExecutionBuffer("data");
        const TextureDescription hiddenDescription = MakeExecutionTexture("hidden");
        const auto color = graph.AddTexture(colorDescription, RenderGraph::ResourceLifetimeKind::Imported);
        const auto data = graph.AddBuffer(bufferDescription, RenderGraph::ResourceLifetimeKind::Imported);
        const auto hidden = graph.AddTexture(hiddenDescription, RenderGraph::ResourceLifetimeKind::Imported);
        const auto writer = graph.AddPass("Writer");
        const auto reader = graph.AddPass("Reader");
        graph.AddWrite(writer, color, ResourceState::RenderTarget);
        graph.AddWrite(writer, data, ResourceState::CopyDest);
        graph.AddRead(reader, color, ResourceState::CopySource, ShaderStage::Pixel);
        graph.AddRead(reader, data, ResourceState::CopySource, ShaderStage::Pixel);

        RenderGraphTestDevice device(8201);
        RenderGraphTestTexture colorTexture(8201, colorDescription, ResourceState::CopyDest);
        RenderGraphTestBuffer dataBuffer(8201, bufferDescription, ResourceState::CopyDest);
        RenderGraphTestTexture hiddenTexture(8201, hiddenDescription, ResourceState::CopyDest);
        bool restricted = false;
        bool acceptedPrefixVisible = false;
        graph.SetPassCallback(writer, [&](RenderGraph::ExecutionContext& context)
        {
            restricted = context.GetTexture(color) == &colorTexture && context.GetBuffer(data) == &dataBuffer
                && context.GetBuffer(color) == nullptr && context.GetTexture(data) == nullptr
                && context.GetTexture(hidden) == nullptr && context.GetBuffer(hidden) == nullptr;
            device.Events.push_back("callback:Writer");
            return restricted;
        });
        graph.SetPassCallback(reader, [&](RenderGraph::ExecutionContext&)
        {
            ResourceState textureState = ResourceState::Unknown;
            ResourceState bufferState = ResourceState::Unknown;
            acceptedPrefixVisible = device.QueryResourceState(&colorTexture, textureState) && textureState == ResourceState::RenderTarget
                && device.QueryResourceState(&dataBuffer, bufferState) && bufferState == ResourceState::CopyDest;
            device.Events.push_back("callback:Reader");
            return acceptedPrefixVisible;
        });
        const bool bound = graph.BindTexture(color, colorTexture) && graph.BindBuffer(data, dataBuffer) && graph.BindTexture(hidden, hiddenTexture);
        const RenderGraph::CompileResult compiled = graph.Compile();
        const RenderGraph::ExecuteResult result = graph.Execute(device, compiled);
        const std::vector<std::string> expectedEvents {
            "texture:color:RenderTarget", "callback:Writer", "texture:color:CopySource",
            "buffer:data:CopySource", "callback:Reader"
        };
        ResourceState finalTexture = ResourceState::Unknown;
        ResourceState finalBuffer = ResourceState::Unknown;
        const bool finalStates = device.QueryResourceState(&colorTexture, finalTexture) && finalTexture == ResourceState::CopySource
            && device.QueryResourceState(&dataBuffer, finalBuffer) && finalBuffer == ResourceState::CopySource;
        return Expect(bound && compiled.Success && compiled.Barriers.size() == 3 && result.Success, "compiled two-pass texture and buffer graph executes successfully")
            && Expect(restricted, "execution context rejects undeclared and wrong-kind resource access")
            && Expect(acceptedPrefixVisible, "each accepted pass publishes its prefix state before dependent recording")
            && Expect(device.Events == expectedEvents, "barriers and callbacks execute in deterministic compiled order")
            && Expect(device.SubmitCount == 2 && result.Completions.size() == 2 && result.Completion.IsValid(), "successful execution submits each compiled pass deterministically")
            && Expect(finalStates, "successful submission publishes the compiled final texture and buffer states");
    }

    bool TestRenderGraphExecutorPoolExhaustionAndExactRetirement()
    {
        using namespace Engine;
        using namespace Engine::RHI;
        RenderGraph graph;
        const TextureDescription description = MakeExecutionTexture("pool", ResourceState::Common);
        const auto resource = graph.AddTexture(description, RenderGraph::ResourceLifetimeKind::Imported);
        const auto pass = graph.AddPass("Use");
        graph.AddRead(pass, resource, ResourceState::Common, ShaderStage::Pixel);
        int callbacks = 0;
        graph.SetPassCallback(pass, [&callbacks](RenderGraph::ExecutionContext&) { ++callbacks; return true; });
        RenderGraphTestTexture texture(8301, description, ResourceState::Common);
        RenderGraphTestDevice device(8301);
        const bool bound = graph.BindTexture(resource, texture);
        const RenderGraph::CompileResult compiled = graph.Compile();
        const RenderGraph::ExecuteResult first = graph.Execute(device, compiled);
        const RenderGraph::ExecuteResult second = graph.Execute(device, compiled);
        const RenderGraph::ExecuteResult third = graph.Execute(device, compiled);
        const RenderGraph::ExecuteResult exhausted = graph.Execute(device, compiled);
        device.SetCompletion(second.Completion, CompletionStatus::Complete);
        const RenderGraph::ExecuteResult reused = graph.Execute(device, compiled);

        RenderGraph completionFailureGraph;
        const auto completionResource = completionFailureGraph.AddTexture(description, RenderGraph::ResourceLifetimeKind::Imported);
        const auto completionPass = completionFailureGraph.AddPass("Use");
        completionFailureGraph.AddRead(completionPass, completionResource, ResourceState::Common, ShaderStage::Pixel);
        int completionCallbacks = 0;
        completionFailureGraph.SetPassCallback(completionPass, [&completionCallbacks](RenderGraph::ExecutionContext&) { ++completionCallbacks; return true; });
        RenderGraphTestTexture completionTexture(8302, description, ResourceState::Common);
        RenderGraphTestDevice completionDevice(8302);
        completionFailureGraph.BindTexture(completionResource, completionTexture);
        const RenderGraph::CompileResult completionCompiled = completionFailureGraph.Compile();
        const RenderGraph::ExecuteResult completionFirst = completionFailureGraph.Execute(completionDevice, completionCompiled);
        completionDevice.SetCompletion(completionFirst.Completion, CompletionStatus::Failed);
        const RenderGraph::ExecuteResult completionFailed = completionFailureGraph.Execute(completionDevice, completionCompiled);

        RenderGraph perPassGraph;
        const auto perPassResource = perPassGraph.AddTexture(description, RenderGraph::ResourceLifetimeKind::Imported);
        int perPassCallbacks = 0;
        for (int index = 0; index < 4; ++index)
        {
            const auto perPass = perPassGraph.AddPass("PerPass" + std::to_string(index));
            perPassGraph.AddRead(perPass, perPassResource, ResourceState::Common, ShaderStage::Pixel);
            perPassGraph.SetPassCallback(perPass, [&perPassCallbacks](RenderGraph::ExecutionContext&) { ++perPassCallbacks; return true; });
        }
        RenderGraphTestTexture perPassTexture(8303, description, ResourceState::Common);
        RenderGraphTestDevice perPassDevice(8303);
        const bool perPassBound = perPassGraph.BindTexture(perPassResource, perPassTexture);
        const RenderGraph::CompileResult perPassCompiled = perPassGraph.Compile();
        const RenderGraph::ExecuteResult perPassFirst = perPassBound ? perPassGraph.Execute(perPassDevice, perPassCompiled) : RenderGraph::ExecuteResult {};
        const RenderGraph::ExecuteResult perPassSecond = perPassFirst.Success ? perPassGraph.Execute(perPassDevice, perPassCompiled) : RenderGraph::ExecuteResult {};
        const RenderGraph::ExecuteResult perPassThird = perPassSecond.Success ? perPassGraph.Execute(perPassDevice, perPassCompiled) : RenderGraph::ExecuteResult {};
        const RenderGraph::ExecuteResult perPassExhausted = perPassThird.Success ? perPassGraph.Execute(perPassDevice, perPassCompiled) : RenderGraph::ExecuteResult {};
        for (const CompletionToken& token : perPassSecond.Completions)
            perPassDevice.SetCompletion(token, CompletionStatus::Complete);
        const RenderGraph::ExecuteResult perPassReused = !perPassExhausted.Success ? perPassGraph.Execute(perPassDevice, perPassCompiled) : RenderGraph::ExecuteResult {};
        const bool perPassContexts = perPassFirst.Success && perPassSecond.Success && perPassThird.Success
            && perPassFirst.Completions.size() == 4 && perPassSecond.Completions.size() == 4 && perPassThird.Completions.size() == 4
            && !perPassExhausted.Success && perPassExhausted.AcceptedPassCount == 0
            && perPassReused.Success && perPassReused.ReusedRetiredContext
            && perPassDevice.CreatedCommandListCount == 12 && perPassDevice.SubmitCount == 16 && perPassCallbacks == 16
            && perPassDevice.SubmittedCommandListIds.size() == 16
            && perPassDevice.SubmittedCommandListIds[0] != perPassDevice.SubmittedCommandListIds[1]
            && perPassDevice.SubmittedCommandListIds[1] != perPassDevice.SubmittedCommandListIds[2]
            && perPassDevice.SubmittedCommandListIds[2] != perPassDevice.SubmittedCommandListIds[3]
            && perPassDevice.SubmittedCommandListIds[4] != perPassDevice.SubmittedCommandListIds[5]
            && perPassDevice.SubmittedCommandListIds[5] != perPassDevice.SubmittedCommandListIds[6]
            && perPassDevice.SubmittedCommandListIds[6] != perPassDevice.SubmittedCommandListIds[7]
            && perPassDevice.SubmittedCommandListIds[12] == perPassDevice.SubmittedCommandListIds[4]
            && perPassDevice.SubmittedCommandListIds[13] == perPassDevice.SubmittedCommandListIds[5]
            && perPassDevice.SubmittedCommandListIds[14] == perPassDevice.SubmittedCommandListIds[6]
            && perPassDevice.SubmittedCommandListIds[15] == perPassDevice.SubmittedCommandListIds[7];

        return Expect(bound && first.Success && second.Success && third.Success, "three bounded recording contexts accept three in-flight submissions")
            && Expect(first.RecordingContextIndex == 0 && second.RecordingContextIndex == 1 && third.RecordingContextIndex == 2,
                "in-flight submissions occupy distinct bounded contexts")
            && Expect(!exhausted.Success && exhausted.Error.find("in flight") != std::string::npos, "fourth execution reports pool exhaustion")
            && Expect(device.CreatedCommandListCount == 3 && device.BeginCount == 4 && device.SubmitCount == 4 && callbacks == 4,
                "exhaustion records nothing and one later retired reuse records exactly once")
            && Expect(reused.Success && reused.ReusedRetiredContext && reused.RecordingContextIndex == second.RecordingContextIndex,
                "only the context guarded by the exact completed token is reused")
            && Expect(device.SubmittedCommandListIds.size() == 4 && device.SubmittedCommandListIds[3] == device.SubmittedCommandListIds[1],
                "retired reuse re-records the same command-list object")
            && Expect(completionFirst.Success && !completionFailed.Success && completionCallbacks == 1
                && completionDevice.BeginCount == 1 && completionDevice.SubmitCount == 1,
                "completion failure prevents later callbacks, recording, submission, and success")
            && Expect(perPassContexts,
                "four pass identities record while earlier tokens are incomplete, then only exact same-pass retired contexts reuse after the three-context bound");
    }

    bool TestRenderGraphTransientAllocationReuseAndExactRetirement()
    {
        using namespace Engine;
        using namespace Engine::RHI;
        BufferDescription description = MakeExecutionBuffer("transient-pool");
        description.SizeBytes = 64;
        description.InitialState = ResourceState::CopyDest;
        RenderGraph graph;
        const auto first = graph.AddBuffer(description);
        const auto second = graph.AddBuffer(description);
        const auto firstPass = graph.AddPass("Transient First");
        const auto secondPass = graph.AddPass("Transient Second");
        graph.AddWrite(firstPass, first, ResourceState::CopyDest);
        graph.AddWrite(secondPass, second, ResourceState::CopyDest);
        Buffer* firstPhysical = nullptr;
        Buffer* secondPhysical = nullptr;
        graph.SetPassCallback(firstPass, [&](RenderGraph::ExecutionContext& context) { firstPhysical = context.GetBuffer(first); return firstPhysical != nullptr; });
        graph.SetPassCallback(secondPass, [&](RenderGraph::ExecutionContext& context) { secondPhysical = context.GetBuffer(second); return secondPhysical != nullptr; });
        RenderGraphTestDevice device(8501);
        const RenderGraph::CompileResult compiled = graph.Compile();
        const RenderGraph::ExecuteResult initial = graph.Execute(device, compiled);
        const bool lifetimeReuse = initial.Success && firstPhysical == secondPhysical
            && initial.TransientAllocationMode == CapabilityPath::NonAliasedGpuRetiredPool
            && initial.TransientResourceCount == 2 && initial.EstimatedTransientAllocatedBytes == 64 && initial.EstimatedTransientPooledBytes == 64;

        // Completing only the final token is insufficient: the pool records
        // every pass token that touched this physical allocation.
        device.SetCompletion(initial.Completion, CompletionStatus::Complete);
        const RenderGraph::ExecuteResult incompleteReuse = graph.Execute(device, compiled);
        const bool blockedByExactToken = incompleteReuse.Success && incompleteReuse.EstimatedTransientAllocatedBytes == 64
            && incompleteReuse.ReusedRetiredTransientCount == 0 && incompleteReuse.EstimatedTransientPooledBytes == 128;
        for (const CompletionToken& token : initial.Completions)
            device.SetCompletion(token, CompletionStatus::Complete);
        const RenderGraph::ExecuteResult retiredReuse = graph.Execute(device, compiled);
        const bool exactTokenRetired = retiredReuse.Success && retiredReuse.EstimatedTransientAllocatedBytes == 0
            && retiredReuse.ReusedRetiredTransientCount == 1 && retiredReuse.EstimatedTransientPooledBytes == 128;
        for (const CompletionToken& token : retiredReuse.Completions)
            device.SetCompletion(token, CompletionStatus::Complete);
        const RenderGraph::ExecuteResult repeatedRetiredReuse = graph.Execute(device, compiled);
        const bool repeatedReuse = repeatedRetiredReuse.Success && repeatedRetiredReuse.EstimatedTransientAllocatedBytes == 0
            && repeatedRetiredReuse.ReusedRetiredTransientCount == 1 && repeatedRetiredReuse.EstimatedTransientPooledBytes == 128;
        return Expect(compiled.Success && lifetimeReuse, "transient lifetimes bind compatible sequential resources to one peak-cost physical allocation")
            && Expect(blockedByExactToken, "an incomplete exact transient completion token prevents physical resource reuse")
            && Expect(exactTokenRetired && repeatedReuse, "a transient physical resource becomes repeatedly reusable only after every exact token retires");
    }

    bool TestRenderGraphTransientAcceptedPrefixRetainsRetirement()
    {
        using namespace Engine;
        using namespace Engine::RHI;
        const BufferDescription description = MakeExecutionBuffer("transient-accepted-prefix");
        RenderGraph graph;
        const auto resource = graph.AddBuffer(description);
        const auto accepted = graph.AddPass("Accepted transient pass");
        const auto failing = graph.AddPass("Failing suffix");
        graph.AddWrite(accepted, resource, ResourceState::CopyDest);
        graph.AddDependency(accepted, failing);
        std::vector<Buffer*> physicals;
        graph.SetPassCallback(accepted, [&](RenderGraph::ExecutionContext& context) { physicals.push_back(context.GetBuffer(resource)); return physicals.back() != nullptr; });
        graph.SetPassCallback(failing, [](RenderGraph::ExecutionContext&) { return false; });
        RenderGraphTestDevice device(8502);
        const RenderGraph::CompileResult compiled = graph.Compile();
        const RenderGraph::ExecuteResult firstFailure = graph.Execute(device, compiled);
        const RenderGraph::ExecuteResult blockedRetry = graph.Execute(device, compiled);
        const bool retainedPrefix = !firstFailure.Success && firstFailure.AcceptedPassCount == 1 && firstFailure.Completions.size() == 1
            && !blockedRetry.Success && blockedRetry.AcceptedPassCount == 1 && blockedRetry.EstimatedTransientAllocatedBytes == description.SizeBytes
            && blockedRetry.ReusedRetiredTransientCount == 0 && physicals.size() >= 2 && physicals[0] != physicals[1];
        device.SetCompletion(firstFailure.Completion, CompletionStatus::Complete);
        const RenderGraph::ExecuteResult retiredRetry = graph.Execute(device, compiled);
        const bool retiredReuse = !retiredRetry.Success && retiredRetry.AcceptedPassCount == 1
            && retiredRetry.EstimatedTransientAllocatedBytes == 0 && retiredRetry.ReusedRetiredTransientCount == 1
            && physicals.size() >= 3 && physicals[2] == physicals[0];
        return Expect(compiled.Success && retainedPrefix, "an accepted transient prefix retains its incomplete exact token after a later graph failure")
            && Expect(retiredReuse, "a failed accepted prefix transient allocation is reusable only after its exact token retires");
    }

    bool TestRenderGraphTransientLogicalTextureCost()
    {
        using namespace Engine;
        using namespace Engine::RHI;
        TextureDescription rg32;
        rg32.DebugName = "logical-rg32";
        rg32.Extent = { 4, 4 };
        rg32.TextureFormat = Format::R32G32Float;
        rg32.Usage = TextureUsage::CopyDest;
        rg32.InitialState = ResourceState::CopyDest;
        rg32.MipLevels = 3;
        TextureDescription rgb32 = rg32;
        rgb32.DebugName = "logical-rgb32";
        rgb32.Extent = { 2, 1 };
        rgb32.TextureFormat = Format::R32G32B32Float;
        rgb32.MipLevels = 2;
        RenderGraph graph;
        const auto first = graph.AddTexture(rg32);
        const auto second = graph.AddTexture(rgb32);
        BufferDescription unusedDescription = MakeExecutionBuffer("unused-transient");
        const auto unused = graph.AddBuffer(unusedDescription);
        const auto firstPass = graph.AddPass("logical-rg32-pass");
        const auto secondPass = graph.AddPass("logical-rgb32-pass");
        graph.AddWrite(firstPass, first, ResourceState::CopyDest);
        graph.AddWrite(secondPass, second, ResourceState::CopyDest);
        graph.SetPassCallback(firstPass, [](RenderGraph::ExecutionContext& context) { return context.GetTexture({ 0 }) != nullptr; });
        graph.SetPassCallback(secondPass, [](RenderGraph::ExecutionContext& context) { return context.GetTexture({ 1 }) != nullptr; });
        RenderGraphTestDevice device(8503);
        const RenderGraph::ExecuteResult result = graph.Execute(device, graph.Compile());
        return Expect(result.Success && result.TransientResourceCount == 2 && result.EstimatedTransientAllocatedBytes == 204 && result.EstimatedTransientPooledBytes == 204,
            "unused transient declarations contribute no allocation or token failure while texture estimates sum mip extents and use correct R32G32/R32G32B32 byte sizes");
    }

    bool TestRenderGraphExecutorCrossQueueOwnershipAndFallback()
    {
        using namespace Engine;
        using namespace Engine::RHI;
        const TextureDescription textureDescription = MakeExecutionTexture("cross-texture", ResourceState::CopyDest);
        const BufferDescription firstBufferDescription = MakeExecutionBuffer("cross-buffer-a", ResourceState::CopyDest);
        const BufferDescription secondBufferDescription = MakeExecutionBuffer("cross-buffer-b", ResourceState::CopyDest);
        auto execute = [&](bool independent)
        {
            RenderGraph graph;
            const auto texture = graph.AddTexture(textureDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const auto firstBuffer = graph.AddBuffer(firstBufferDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const auto secondBuffer = graph.AddBuffer(secondBufferDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const auto graphics = graph.AddPass("Graphics", QueueType::Graphics);
            const auto copy = graph.AddPass("Copy", QueueType::Copy);
            graph.AddWrite(graphics, texture, ResourceState::CopyDest);
            graph.AddWrite(graphics, firstBuffer, ResourceState::CopyDest);
            graph.AddWrite(graphics, secondBuffer, ResourceState::CopyDest);
            graph.AddRead(copy, texture, ResourceState::CopySource, ShaderStage::Compute);
            graph.AddRead(copy, firstBuffer, ResourceState::CopySource, ShaderStage::Compute);
            graph.AddRead(copy, secondBuffer, ResourceState::CopySource, ShaderStage::Compute);
            int callbacks = 0;
            graph.SetPassCallback(graphics, [&](RenderGraph::ExecutionContext&) { ++callbacks; return true; });
            graph.SetPassCallback(copy, [&](RenderGraph::ExecutionContext&) { ++callbacks; return true; });
            RenderGraphTestDevice device(8401 + independent, false, independent);
            RenderGraphTestTexture physicalTexture(8401 + independent, textureDescription, ResourceState::CopyDest);
            RenderGraphTestBuffer firstPhysicalBuffer(8401 + independent, firstBufferDescription, ResourceState::CopyDest);
            RenderGraphTestBuffer secondPhysicalBuffer(8401 + independent, secondBufferDescription, ResourceState::CopyDest);
            const bool bound = graph.BindTexture(texture, physicalTexture) && graph.BindBuffer(firstBuffer, firstPhysicalBuffer)
                && graph.BindBuffer(secondBuffer, secondPhysicalBuffer);
            const RenderGraph::ExecuteResult result = bound ? graph.Execute(device, graph.Compile()) : RenderGraph::ExecuteResult {};
            QueueType textureOwner = QueueType::Graphics, firstBufferOwner = QueueType::Graphics, secondBufferOwner = QueueType::Graphics;
            ResourceState textureState = ResourceState::Unknown, firstBufferState = ResourceState::Unknown, secondBufferState = ResourceState::Unknown;
            const bool owners = device.QueryTextureQueueOwner(&physicalTexture, textureOwner)
                && device.QueryBufferQueueOwner(&firstPhysicalBuffer, firstBufferOwner)
                && device.QueryBufferQueueOwner(&secondPhysicalBuffer, secondBufferOwner)
                && device.QueryResourceState(&physicalTexture, textureState)
                && device.QueryResourceState(&firstPhysicalBuffer, firstBufferState)
                && device.QueryResourceState(&secondPhysicalBuffer, secondBufferState);
            return Expect(result.Success && result.AcceptedPassCount == 2 && result.Completions.size() == 2 && callbacks == 2,
                    independent ? "independent graphics-to-copy graph accepts both queue submissions" : "graphics fallback graph accepts both ordered submissions")
                && Expect(device.GpuWaitDependencyCount == (independent ? 1 : 0) && device.ElidedDependencyCount == (independent ? 0 : 1),
                    "resolved queue topology selects GPU wait only for an independent effective queue")
                && Expect(owners && textureOwner == (independent ? QueueType::Copy : QueueType::Graphics)
                    && firstBufferOwner == (independent ? QueueType::Copy : QueueType::Graphics)
                    && secondBufferOwner == (independent ? QueueType::Copy : QueueType::Graphics)
                    && textureState == ResourceState::CopySource && firstBufferState == ResourceState::CopySource
                    && secondBufferState == ResourceState::CopySource,
                    "texture and both buffers publish the exact accepted acquire state and owner")
                && Expect(independent ? device.DependencyOrder.size() == 1
                        && device.PublishedBufferOwnershipOperations.size() == 4
                        && device.PublishedTextureOwnershipOperations.size() == 2
                        && device.PublishedBufferOwnershipOperations[0] == &firstPhysicalBuffer
                        && device.PublishedBufferOwnershipOperations[1] == &secondPhysicalBuffer
                        && device.PublishedBufferOwnershipOperations[2] == &firstPhysicalBuffer
                        && device.PublishedBufferOwnershipOperations[3] == &secondPhysicalBuffer
                        && device.PublishedTextureOwnershipOperations[0] == &physicalTexture
                        && device.PublishedTextureOwnershipOperations[1] == &physicalTexture
                    : device.PublishedBufferOwnershipOperations.empty() && device.PublishedTextureOwnershipOperations.empty(),
                    "cross-queue releases and acquires publish in recorded order with one deduplicated producer token");
        };
        RenderGraphTestDevice invalidDevice(8403, false, true);
        RenderGraphTestBuffer firstInvalidBuffer(8403, firstBufferDescription, ResourceState::CopyDest);
        RenderGraphTestBuffer secondInvalidBuffer(8403, secondBufferDescription, ResourceState::CopyDest);
        Engine::Scope<CommandList> invalidList = invalidDevice.CreateCommandList(QueueType::Graphics, "invalid-ownership-batch");
        const bool recordedInvalidBatch = invalidList && invalidList->Begin()
            && invalidList->ReleaseBufferOwnership({ &firstInvalidBuffer, QueueType::Graphics, QueueType::Copy, ResourceState::CopyDest, ResourceState::CopySource })
            && invalidList->ReleaseBufferOwnership({ &secondInvalidBuffer, QueueType::Copy, QueueType::Graphics, ResourceState::CopyDest, ResourceState::CopySource })
            && invalidList->End();
        ResourceState firstInvalidState = ResourceState::Unknown, secondInvalidState = ResourceState::Unknown;
        const bool noPartialPublication = recordedInvalidBatch && !invalidDevice.Submit(*invalidList).IsValid()
            && invalidDevice.NativeSubmissionCount == 0 && invalidDevice.PublishedBufferOwnershipOperations.empty()
            && invalidDevice.QueryResourceState(&firstInvalidBuffer, firstInvalidState)
            && invalidDevice.QueryResourceState(&secondInvalidBuffer, secondInvalidState)
            && firstInvalidState == ResourceState::CopyDest && secondInvalidState == ResourceState::CopyDest;
        return execute(true) && execute(false)
            && Expect(noPartialPublication, "one invalid ownership operation rejects the complete batch before native acceptance or publication");
    }

    bool TestRenderGraphExecutorRejectsMissingProducerTokenBeforeConsumer()
    {
        using namespace Engine;
        using namespace Engine::RHI;
        const TextureDescription description = MakeExecutionTexture("missing-producer", ResourceState::Common);
        RenderGraph graph;
        const auto resource = graph.AddTexture(description, RenderGraph::ResourceLifetimeKind::Imported);
        const auto prefix = graph.AddPass("Prefix");
        const auto consumer = graph.AddPass("Consumer");
        const auto absentProducer = graph.AddPass("AbsentProducer");
        graph.AddRead(prefix, resource, ResourceState::Common, ShaderStage::Pixel);
        graph.AddRead(consumer, resource, ResourceState::Common, ShaderStage::Pixel);
        graph.AddRead(absentProducer, resource, ResourceState::Common, ShaderStage::Pixel);
        graph.AddDependency(prefix, consumer);
        int prefixCallbacks = 0, consumerCallbacks = 0, absentCallbacks = 0;
        graph.SetPassCallback(prefix, [&prefixCallbacks](RenderGraph::ExecutionContext&) { ++prefixCallbacks; return true; });
        graph.SetPassCallback(consumer, [&consumerCallbacks](RenderGraph::ExecutionContext&) { ++consumerCallbacks; return true; });
        graph.SetPassCallback(absentProducer, [&absentCallbacks](RenderGraph::ExecutionContext&) { ++absentCallbacks; return true; });
        RenderGraph::CompileResult compiled = graph.Compile();
        compiled.Dependencies.push_back({ absentProducer, consumer, resource });
        RenderGraphTestTexture texture(8404, description, ResourceState::Common);
        RenderGraphTestDevice device(8404);
        const bool bound = graph.BindTexture(resource, texture);
        const RenderGraph::ExecuteResult result = bound ? graph.Execute(device, compiled) : RenderGraph::ExecuteResult {};
        ResourceState observed = ResourceState::Unknown;
        return Expect(compiled.Success && !result.Success && result.Error.find("no accepted producer token") != std::string::npos,
                "a declared dependency without an accepted producer token fails before the consumer")
            && Expect(prefixCallbacks == 1 && consumerCallbacks == 0 && absentCallbacks == 0,
                "missing producer-token failure invokes neither consumer nor later callback")
            && Expect(result.AcceptedPassCount == 1 && result.Completions.size() == 1 && device.SubmitCount == 1
                && device.QueryResourceState(&texture, observed) && observed == ResourceState::Common,
                "missing producer-token failure preserves the already accepted prefix without later submission");
    }

    class OwnershipTestBuffer final : public Engine::RHI::Buffer
    {
    public:
        explicit OwnershipTestBuffer(Engine::u64 ownerId, Engine::RHI::ResourceState state = Engine::RHI::ResourceState::CopyDest) : m_OwnerId(ownerId), m_State(state) {}
        const Engine::RHI::BufferDescription& GetDescription() const override { return m_Description; }
        void* Map() override { return nullptr; }
        void Unmap() override {}
        Engine::u64 OwnerId() const { return m_OwnerId; }
        Engine::RHI::ResourceState State() const { return m_State; }
    private:
        Engine::RHI::BufferDescription m_Description;
        Engine::u64 m_OwnerId;
        Engine::RHI::ResourceState m_State;
    };

    class OwnershipTestTexture final : public Engine::RHI::Texture
    {
    public:
        explicit OwnershipTestTexture(Engine::u64 ownerId, Engine::RHI::ResourceState state = Engine::RHI::ResourceState::CopyDest) : m_OwnerId(ownerId), m_State(state) {}
        const Engine::RHI::TextureDescription& GetDescription() const override { return m_Description; }
        Engine::u64 OwnerId() const { return m_OwnerId; }
        Engine::RHI::ResourceState State() const { return m_State; }
    private:
        Engine::RHI::TextureDescription m_Description;
        Engine::u64 m_OwnerId;
        Engine::RHI::ResourceState m_State;
    };

    class ForeignOwnershipTestBuffer final : public Engine::RHI::Buffer
    {
    public:
        const Engine::RHI::BufferDescription& GetDescription() const override { return m_Description; }
        void* Map() override { return nullptr; }
        void Unmap() override {}
    private:
        Engine::RHI::BufferDescription m_Description;
    };

    class OwnershipTestDevice final : public Engine::RHI::Device
    {
    public:
        explicit OwnershipTestDevice(Engine::u64 ownerId) : m_OwnerId(ownerId) {}
        const Engine::RHI::DeviceDescription& GetDescription() const override { return m_Description; }
        const Engine::RHI::DeviceCapabilities& GetCapabilities() const override { return m_Capabilities; }
        Engine::Scope<Engine::RHI::Buffer> CreateBuffer(const Engine::RHI::BufferDescription&) override { return nullptr; }
        Engine::Scope<Engine::RHI::Texture> CreateTexture(const Engine::RHI::TextureDescription&) override { return nullptr; }
        bool OwnsResource(const Engine::RHI::Buffer* resource) const override { const auto* buffer = dynamic_cast<const OwnershipTestBuffer*>(resource); return buffer && buffer->OwnerId() == m_OwnerId; }
        bool OwnsResource(const Engine::RHI::Texture* resource) const override { const auto* texture = dynamic_cast<const OwnershipTestTexture*>(resource); return texture && texture->OwnerId() == m_OwnerId; }
        bool QueryResourceState(const Engine::RHI::Buffer* resource, Engine::RHI::ResourceState& state) const override
        {
            const auto* buffer = dynamic_cast<const OwnershipTestBuffer*>(resource);
            if (!buffer || !OwnsResource(resource) || buffer->State() == Engine::RHI::ResourceState::Unknown) return false;
            state = buffer->State();
            return true;
        }
        bool QueryResourceState(const Engine::RHI::Texture* resource, Engine::RHI::ResourceState& state) const override
        {
            const auto* texture = dynamic_cast<const OwnershipTestTexture*>(resource);
            if (!texture || !OwnsResource(resource) || texture->State() == Engine::RHI::ResourceState::Unknown) return false;
            state = texture->State();
            return true;
        }
        Engine::Scope<Engine::RHI::Shader> CreateShader(const Engine::RHI::ShaderDescription&) override { return nullptr; }
        Engine::Scope<Engine::RHI::Pipeline> CreatePipeline(const Engine::RHI::PipelineDescription&) override { return nullptr; }
        Engine::Scope<Engine::RHI::QueryPool> CreateQueryPool(const Engine::RHI::QueryPoolDescription&) override { return nullptr; }
        Engine::Scope<Engine::RHI::CommandList> CreateCommandList(Engine::RHI::QueueType, std::string_view) override { return nullptr; }
        bool UploadBuffer(Engine::RHI::Buffer&, const void*, Engine::u64, Engine::u64) override { return false; }
        bool ReadbackTexture(Engine::RHI::Texture&, Engine::RHI::TextureReadback&) override { return false; }
        Engine::RHI::CompletionToken Submit(Engine::RHI::CommandList&) override { return {}; }
        Engine::RHI::CompletionStatus QueryCompletion(const Engine::RHI::CompletionToken&) override { return Engine::RHI::CompletionStatus::Invalid; }
        bool WaitForCompletion(const Engine::RHI::CompletionToken&, Engine::u32) override { return false; }
        bool SubmitAndWait(Engine::RHI::CommandList&) override { return false; }
        void WaitIdle() override {}
    private:
        Engine::RHI::DeviceDescription m_Description;
        Engine::RHI::DeviceCapabilities m_Capabilities;
        Engine::u64 m_OwnerId;
    };

    bool TestRhiResourceOwnershipContract()
    {
        OwnershipTestDevice first(101), second(202);
        OwnershipTestBuffer ownBuffer(101), foreignBuffer(202);
        OwnershipTestTexture ownTexture(101), foreignTexture(202);
        OwnershipTestBuffer unknownBuffer(101, Engine::RHI::ResourceState::Unknown);
        OwnershipTestTexture unknownTexture(101, Engine::RHI::ResourceState::Unknown);
        ForeignOwnershipTestBuffer foreignBackendBuffer;
        Engine::RHI::ResourceState observed = Engine::RHI::ResourceState::Unknown;
        return Expect(first.OwnsResource(&ownBuffer), "a device accepts its own buffer")
            && Expect(first.OwnsResource(&ownTexture), "a device accepts its own texture")
            && Expect(!first.OwnsResource(&foreignBuffer), "a same-backend different-device buffer is rejected")
            && Expect(!first.OwnsResource(&foreignTexture), "a same-backend different-device texture is rejected")
            && Expect(!first.OwnsResource(&foreignBackendBuffer), "a foreign-backend buffer is rejected")
            && Expect(!first.OwnsResource(static_cast<const Engine::RHI::Buffer*>(nullptr)), "a null buffer is rejected")
            && Expect(!first.OwnsResource(static_cast<const Engine::RHI::Texture*>(nullptr)), "a null texture is rejected")
            && Expect(!second.OwnsResource(&ownBuffer), "ownership is exact-device rather than description-equivalent")
            && Expect(first.QueryResourceState(&ownBuffer, observed) && observed == Engine::RHI::ResourceState::CopyDest, "an owned buffer exposes only its tracked state")
            && Expect(first.QueryResourceState(&ownTexture, observed) && observed == Engine::RHI::ResourceState::CopyDest, "an owned texture exposes only its tracked state")
            && Expect(!first.QueryResourceState(&foreignBuffer, observed) && !first.QueryResourceState(&foreignTexture, observed), "foreign same-backend resources are rejected")
            && Expect(!first.QueryResourceState(&foreignBackendBuffer, observed), "foreign backend resources are rejected")
            && Expect(!first.QueryResourceState(static_cast<const Engine::RHI::Buffer*>(nullptr), observed)
                && !first.QueryResourceState(static_cast<const Engine::RHI::Texture*>(nullptr), observed), "null resources are rejected")
            && Expect(!first.QueryResourceState(&unknownBuffer, observed) && !first.QueryResourceState(&unknownTexture, observed), "unknown states are rejected");
    }

    class BufferOwnershipTestBuffer final : public Engine::RHI::Buffer
    {
    public:
        explicit BufferOwnershipTestBuffer(std::string name = "ownership")
        {
            m_Description.DebugName = std::move(name);
            m_Description.SizeBytes = 4096;
            m_Description.Usage = static_cast<Engine::RHI::BufferUsage>(
                static_cast<Engine::u32>(Engine::RHI::BufferUsage::CopySource)
                | static_cast<Engine::u32>(Engine::RHI::BufferUsage::CopyDest));
            m_Description.InitialState = Engine::RHI::ResourceState::CopyDest;
        }
        const Engine::RHI::BufferDescription& GetDescription() const override { return m_Description; }
        void* Map() override { return nullptr; }
        void Unmap() override {}
    private:
        Engine::RHI::BufferDescription m_Description;
    };

    class BufferOwnershipContractList final : public Engine::RHI::CommandList
    {
    public:
        BufferOwnershipContractList(Engine::RHI::BufferOwnershipTracker& tracker, Engine::RHI::QueueType requested,
            bool copyIndependent, bool computeIndependent)
            : m_Tracker(tracker), m_Requested(requested), m_CopyIndependent(copyIndependent), m_ComputeIndependent(computeIndependent) {}

        Engine::RHI::QueueType GetQueueType() const override { return m_Requested; }
        bool Begin() override { if (m_Recording) return false; m_Recording = true; m_Operation.reset(); return true; }
        bool End() override { if (!m_Recording) return false; m_Recording = false; return true; }
        void BeginDebugMarker(std::string_view) override {}
        void EndDebugMarker() override {}
        bool BindViewportOutputs(Engine::RHI::Texture&, Engine::RHI::Texture*) override { return false; }
        bool ClearViewportOutputs(const Engine::RHI::ViewportClear&) override { return false; }
        bool TransitionTexture(Engine::RHI::Texture&, Engine::RHI::ResourceState) override { return false; }
        bool TransitionBuffer(Engine::RHI::Buffer& buffer, Engine::RHI::ResourceState state) override
        {
            return m_Recording && m_Tracker.CanUse(&buffer)
                && Engine::RHI::IsBufferStateCompatible(buffer.GetDescription().Usage, buffer.GetDescription().CpuAccess, state);
        }
        bool ReleaseBufferOwnership(const Engine::RHI::BufferOwnershipRelease& release) override
        {
            if (!m_Recording || m_Operation) return false;
            Engine::RHI::RecordedBufferOwnershipOperation operation;
            if (!m_Tracker.RecordRelease(release, Resolve(m_Requested), Resolve(release.SourceQueue),
                Resolve(release.DestinationQueue), operation)) return false;
            m_Operation = operation;
            return true;
        }
        bool AcquireBufferOwnership(const Engine::RHI::BufferOwnershipAcquire& acquire) override
        {
            if (!m_Recording || m_Operation) return false;
            Engine::RHI::RecordedBufferOwnershipOperation operation;
            if (!m_Tracker.RecordAcquire(acquire, Resolve(m_Requested), Resolve(acquire.SourceQueue),
                Resolve(acquire.DestinationQueue), operation)) return false;
            m_Operation = operation;
            ++AcquireTransitionCount;
            return true;
        }
        void SetGraphicsPipeline(Engine::RHI::Pipeline&) override {}
        void SetGraphicsConstantBuffer(Engine::u32, Engine::RHI::Buffer&) override {}
        void SetViewport(const Engine::RHI::Viewport&) override {}
        void SetScissorRect(const Engine::RHI::ScissorRect&) override {}
        void SetVertexBuffer(Engine::u32, Engine::RHI::Buffer&) override {}
        void SetIndexBuffer(Engine::RHI::Buffer&, Engine::RHI::IndexFormat) override {}
        bool CopyBuffer(Engine::RHI::Buffer& destination, Engine::u64, Engine::RHI::Buffer& source, Engine::u64, Engine::u64) override
        {
            return m_Recording && m_Tracker.CanUse(&destination) && m_Tracker.CanUse(&source);
        }
        void DrawIndexed(Engine::u32, Engine::u32, Engine::u32, int, Engine::u32) override {}
        void ResetQueryPool(Engine::RHI::QueryPool&, Engine::u32, Engine::u32) override {}
        void WriteTimestamp(Engine::RHI::QueryPool&, Engine::u32) override {}
        void ResolveQueryPool(Engine::RHI::QueryPool&, Engine::u32, Engine::u32) override {}

        const std::optional<Engine::RHI::RecordedBufferOwnershipOperation>& Operation() const { return m_Operation; }
        int AcquireTransitionCount = 0;

    private:
        Engine::RHI::QueueType Resolve(Engine::RHI::QueueType requested) const
        {
            if (requested == Engine::RHI::QueueType::Copy && !m_CopyIndependent) return Engine::RHI::QueueType::Graphics;
            if (requested == Engine::RHI::QueueType::Compute && !m_ComputeIndependent) return Engine::RHI::QueueType::Graphics;
            return requested;
        }

        Engine::RHI::BufferOwnershipTracker& m_Tracker;
        Engine::RHI::QueueType m_Requested;
        bool m_CopyIndependent;
        bool m_ComputeIndependent;
        bool m_Recording = false;
        std::optional<Engine::RHI::RecordedBufferOwnershipOperation> m_Operation;
    };

    bool TestRhiBufferOwnershipLifecycleContract()
    {
        using namespace Engine::RHI;
        BufferOwnershipTracker tracker;
        BufferOwnershipTestBuffer buffer, foreign("foreign"), second("second");
        const bool registered = tracker.Register(buffer, QueueType::Graphics, ResourceState::CopyDest)
            && tracker.Register(second, QueueType::Graphics, ResourceState::CopyDest);
        BufferOwnershipRelease release { &buffer, QueueType::Graphics, QueueType::Copy,
            ResourceState::CopyDest, ResourceState::CopySource };
        BufferOwnershipContractList source(tracker, QueueType::Graphics, true, true);
        QueueType owner = QueueType::Copy;
        ResourceState state = ResourceState::Unknown;

        const bool invalidRecording = source.Begin()
            && !source.ReleaseBufferOwnership({ nullptr, QueueType::Graphics, QueueType::Copy, ResourceState::CopyDest, ResourceState::CopySource })
            && !source.ReleaseBufferOwnership({ &foreign, QueueType::Graphics, QueueType::Copy, ResourceState::CopyDest, ResourceState::CopySource })
            && !source.ReleaseBufferOwnership({ &buffer, QueueType::Graphics, QueueType::Graphics, ResourceState::CopyDest, ResourceState::CopySource })
            && !source.ReleaseBufferOwnership({ &buffer, QueueType::Copy, QueueType::Graphics, ResourceState::CopyDest, ResourceState::CopySource })
            && !source.ReleaseBufferOwnership({ &buffer, QueueType::Graphics, QueueType::Copy, ResourceState::Common, ResourceState::CopySource })
            && !source.ReleaseBufferOwnership({ &buffer, QueueType::Graphics, QueueType::Copy, ResourceState::CopyDest, ResourceState::RenderTarget });
        const bool privateRecording = source.ReleaseBufferOwnership(release)
            && !source.ReleaseBufferOwnership(release)
            && tracker.QueryOwner(&buffer, owner) && owner == QueueType::Graphics
            && tracker.QueryState(&buffer, state) && state == ResourceState::CopyDest
            && !tracker.HasPending(&buffer) && source.End();
        const bool releaseValidated = source.Operation() && tracker.ValidateSubmission(*source.Operation(), {});
        const bool failedReleasePrivate = releaseValidated && !tracker.PublishRelease(*source.Operation(), {})
            && !tracker.HasPending(&buffer) && tracker.CanUse(&buffer);
        const CompletionToken releaseToken { 71, 5 };
        const bool releaseAccepted = tracker.PublishRelease(*source.Operation(), releaseToken)
            && tracker.HasPending(&buffer) && !tracker.CanUse(&buffer) && !tracker.CanDestroy(&buffer)
            && !tracker.QueryOwner(&buffer, owner) && !tracker.QueryState(&buffer, state)
            && !tracker.PublishOrdinaryState(buffer, ResourceState::Common);

        BufferOwnershipContractList blockedSource(tracker, QueueType::Graphics, true, true);
        BufferOwnershipContractList destination(tracker, QueueType::Copy, true, true);
        BufferOwnershipAcquire acquire;
        static_cast<BufferOwnershipRelease&>(acquire) = release;
        acquire.ReleaseToken = releaseToken;
        BufferOwnershipAcquire wrongToken = acquire;
        wrongToken.ReleaseToken = { 72, 5 };
        BufferOwnershipAcquire zeroToken = acquire;
        zeroToken.ReleaseToken = {};
        BufferOwnershipAcquire mismatch = acquire;
        mismatch.After = ResourceState::Common;
        BufferOwnershipContractList wrongDestination(tracker, QueueType::Graphics, true, true);
        const bool pendingGuards = blockedSource.Begin() && !blockedSource.TransitionBuffer(buffer, ResourceState::Common)
            && !blockedSource.CopyBuffer(second, 0, buffer, 0, 4) && !blockedSource.ReleaseBufferOwnership(release)
            && blockedSource.End() && wrongDestination.Begin() && !wrongDestination.AcquireBufferOwnership(acquire)
            && wrongDestination.End() && destination.Begin() && !destination.AcquireBufferOwnership(zeroToken)
            && !destination.AcquireBufferOwnership(wrongToken) && !destination.AcquireBufferOwnership(mismatch)
            && destination.AcquireBufferOwnership(acquire) && !destination.AcquireBufferOwnership(acquire) && destination.End();
        const bool failedAcquirePersistent = destination.Operation()
            && !tracker.ValidateSubmission(*destination.Operation(), {})
            && !tracker.ValidateSubmission(*destination.Operation(), { { 72, 5 } })
            && !tracker.ValidateSubmission(*destination.Operation(), { releaseToken, releaseToken })
            && tracker.HasPending(&buffer) && !tracker.CanUse(&buffer);
        const bool acquireAccepted = tracker.ValidateSubmission(*destination.Operation(), { releaseToken })
            && tracker.PublishAcquire(*destination.Operation()) && !tracker.HasPending(&buffer)
            && tracker.QueryOwner(&buffer, owner) && owner == QueueType::Copy
            && tracker.QueryState(&buffer, state) && state == ResourceState::CopySource
            && tracker.CanUse(&buffer) && tracker.CanDestroy(&buffer) && destination.AcquireTransitionCount == 1;

        return Expect(registered && invalidRecording, "ownership release rejects null foreign same-effective wrong-queue state and usage inputs")
            && Expect(privateRecording && failedReleasePrivate, "release recording and failed acceptance publish no owner state or pending transfer")
            && Expect(releaseAccepted, "accepted release publishes only a token-bound pending transfer")
            && Expect(pendingGuards, "pending transfer blocks ordinary transition copy second release and duplicate acquire recording")
            && Expect(failedAcquirePersistent, "missing foreign and duplicate release-token dependencies preserve pending state")
            && Expect(acquireAccepted, "accepted exact-token acquire publishes destination owner and state exactly once");
    }

    bool TestRhiBufferOwnershipRecoveryAndAdapterSeams()
    {
        using namespace Engine::RHI;
        BufferOwnershipTracker tracker;
        BufferOwnershipTestBuffer first("recover-first"), second("recover-second"), fallback("fallback");
        tracker.Register(first, QueueType::Graphics, ResourceState::CopyDest);
        tracker.Register(second, QueueType::Graphics, ResourceState::CopyDest);
        tracker.Register(fallback, QueueType::Graphics, ResourceState::CopyDest);
        const BufferOwnershipRelease firstRelease { &first, QueueType::Graphics, QueueType::Copy,
            ResourceState::CopyDest, ResourceState::CopySource };
        const BufferOwnershipRelease secondRelease { &second, QueueType::Graphics, QueueType::Compute,
            ResourceState::CopyDest, ResourceState::Common };
        RecordedBufferOwnershipOperation firstOperation, secondOperation, fallbackOperation;
        const CompletionToken firstToken { 81, 7 }, secondToken { 81, 8 };
        const bool pending = tracker.RecordRelease(firstRelease, QueueType::Graphics, QueueType::Graphics, QueueType::Copy, firstOperation)
            && tracker.RecordRelease(secondRelease, QueueType::Graphics, QueueType::Graphics, QueueType::Compute, secondOperation)
            && tracker.PublishRelease(firstOperation, firstToken) && tracker.PublishRelease(secondOperation, secondToken);
        PendingBufferOwnershipTransfer pendingSnapshot;
        const bool compensationPlan = tracker.QueryPending(&first, pendingSnapshot)
            && pendingSnapshot.Resource == &first && pendingSnapshot.Source == QueueType::Graphics
            && pendingSnapshot.Destination == QueueType::Copy && pendingSnapshot.Before == ResourceState::CopyDest
            && pendingSnapshot.After == ResourceState::CopySource && pendingSnapshot.ReleaseToken.DeviceId == firstToken.DeviceId
            && pendingSnapshot.ReleaseToken.SubmissionId == firstToken.SubmissionId;
        QueueType owner = QueueType::Copy;
        ResourceState state = ResourceState::Unknown;
        const bool exactRecovery = !tracker.Recover(first, secondToken, CompletionStatus::Complete)
            && !tracker.Recover(first, firstToken, CompletionStatus::Incomplete)
            && tracker.Recover(first, firstToken, CompletionStatus::Complete)
            && tracker.QueryOwner(&first, owner) && owner == QueueType::Graphics
            && tracker.QueryState(&first, state) && state == ResourceState::CopyDest
            && !tracker.HasPending(&first) && tracker.HasPending(&second)
            && tracker.Recover(second, secondToken, CompletionStatus::Complete) && !tracker.HasPending(&second);
        const BufferOwnershipRelease fallbackRelease { &fallback, QueueType::Graphics, QueueType::Copy,
            ResourceState::CopyDest, ResourceState::CopySource };
        const bool vulkanFallback = !tracker.RecordRelease(fallbackRelease, QueueType::Graphics,
            QueueType::Graphics, QueueType::Graphics, fallbackOperation) && !tracker.HasPending(&fallback);
        const bool liveRemoval = tracker.Unregister(fallback) && !tracker.IsLive(&fallback) && !tracker.CanUse(&fallback);
        const bool d3d12Policy = GetNVRHID3D12BufferOwnershipBarrier(BufferOwnershipOperationType::Release)
                == NVRHID3D12BufferOwnershipBarrier::None
            && GetNVRHID3D12BufferOwnershipBarrier(BufferOwnershipOperationType::Acquire)
                == NVRHID3D12BufferOwnershipBarrier::PortableStateTransition;
        return Expect(pending && compensationPlan && exactRecovery, "recovery snapshot preserves the exact token and reversible source destination state plan")
            && Expect(vulkanFallback && liveRemoval, "same-effective Vulkan-style graphics fallback never enters transfer state and dead wrappers unregister")
            && Expect(d3d12Policy, "D3D12 release emits no ownership barrier while acquire requires the portable state transition");
    }

    class TextureOwnershipTestTexture final : public Engine::RHI::Texture
    {
    public:
        explicit TextureOwnershipTestTexture(Engine::RHI::TextureDescription description = {})
        {
            if (description.Extent.Width == 0)
            {
                description.Extent = { 4, 4 };
                description.TextureFormat = Engine::RHI::Format::R8G8B8A8Unorm;
                description.Usage = static_cast<Engine::RHI::TextureUsage>(static_cast<Engine::u32>(Engine::RHI::TextureUsage::CopyDest) | static_cast<Engine::u32>(Engine::RHI::TextureUsage::CopySource));
            }
            m_Description = std::move(description);
        }
        const Engine::RHI::TextureDescription& GetDescription() const override { return m_Description; }
    private: Engine::RHI::TextureDescription m_Description;
    };

    bool TestRhiTextureOwnershipLifecycleContract()
    {
        using namespace Engine::RHI;
        TextureOwnershipTracker tracker;
        TextureOwnershipTestTexture texture, foreign;
        TextureDescription unsupportedDescription = texture.GetDescription();
        unsupportedDescription.MipLevels = 2;
        TextureOwnershipTestTexture unsupported(unsupportedDescription);
        TextureDescription unsupportedLayerDescription = texture.GetDescription();
        unsupportedLayerDescription.ArrayLayers = 2;
        TextureOwnershipTestTexture unsupportedLayer(unsupportedLayerDescription);
        TextureDescription unsupportedSampleDescription = texture.GetDescription();
        unsupportedSampleDescription.SampleCount = 2;
        TextureOwnershipTestTexture unsupportedSample(unsupportedSampleDescription);
        TextureDescription incompatibleDescription = texture.GetDescription();
        incompatibleDescription.Usage = TextureUsage::CopyDest;
        TextureOwnershipTestTexture incompatible(incompatibleDescription);
        TextureOwnershipTestTexture common;
        const TextureOwnershipRelease release { &texture, QueueType::Graphics, QueueType::Copy, ResourceState::CopyDest, ResourceState::CopySource };
        RecordedTextureOwnershipOperation op;
        const CompletionToken releaseToken { 73, 1 };
        TextureOwnershipAcquire acquire; static_cast<TextureOwnershipRelease&>(acquire) = release; acquire.ReleaseToken = releaseToken;
        QueueType owner = QueueType::Copy; ResourceState state = ResourceState::Unknown;
        const bool rejected = !tracker.Register(unsupported, QueueType::Graphics, ResourceState::CopyDest)
            && !tracker.Register(unsupportedLayer, QueueType::Graphics, ResourceState::CopyDest)
            && !tracker.Register(unsupportedSample, QueueType::Graphics, ResourceState::CopyDest)
            && !tracker.Register(foreign, QueueType::Graphics, ResourceState::Unknown)
            && tracker.Register(common, QueueType::Graphics, ResourceState::Common)
            && !tracker.RecordRelease(release, QueueType::Graphics, QueueType::Graphics, QueueType::Graphics, op)
            && !tracker.RecordRelease({ &foreign, QueueType::Graphics, QueueType::Copy, ResourceState::CopyDest, ResourceState::CopySource }, QueueType::Graphics, QueueType::Graphics, QueueType::Copy, op)
            && !tracker.Register(incompatible, QueueType::Graphics, ResourceState::CopySource)
            && tracker.Register(incompatible, QueueType::Graphics, ResourceState::CopyDest)
            && !tracker.RecordRelease({ &incompatible, QueueType::Graphics, QueueType::Copy, ResourceState::CopyDest, ResourceState::CopySource }, QueueType::Graphics, QueueType::Graphics, QueueType::Copy, op);
        const bool released = tracker.Register(texture, QueueType::Graphics, ResourceState::CopyDest)
            && tracker.RecordRelease(release, QueueType::Graphics, QueueType::Graphics, QueueType::Copy, op)
            && tracker.ValidateSubmission(op, {}) && tracker.PublishRelease(op, releaseToken)
            && tracker.HasPending(&texture) && !tracker.CanUse(&texture) && !tracker.CanDestroy(&texture)
            && !tracker.QueryOwner(&texture, owner) && !tracker.QueryState(&texture, state)
            && !tracker.PublishOrdinaryState(texture, ResourceState::CopySource);
        RecordedTextureOwnershipOperation acquireOp;
        const bool acquired = tracker.RecordAcquire(acquire, QueueType::Copy, QueueType::Graphics, QueueType::Copy, acquireOp)
            && !tracker.RecordAcquire({ &texture, QueueType::Graphics, QueueType::Copy, ResourceState::CopyDest, ResourceState::CopySource, { 73, 2 } }, QueueType::Copy, QueueType::Graphics, QueueType::Copy, op)
            && !tracker.ValidateSubmission(acquireOp, {}) && !tracker.ValidateSubmission(acquireOp, { releaseToken, releaseToken })
            && tracker.ValidateSubmission(acquireOp, { releaseToken })
            && tracker.PublishAcquire(acquireOp) && !tracker.HasPending(&texture)
            && tracker.QueryOwner(&texture, owner) && owner == QueueType::Copy
            && tracker.QueryState(&texture, state) && state == ResourceState::CopySource;
        PendingTextureOwnershipTransfer recoverySnapshot;
        const bool recoverySetup = tracker.PublishOrdinaryState(texture, ResourceState::CopyDest)
            && tracker.RecordRelease({ &texture, QueueType::Copy, QueueType::Graphics, ResourceState::CopyDest, ResourceState::CopySource },
                QueueType::Copy, QueueType::Copy, QueueType::Graphics, op)
            && tracker.PublishRelease(op, { 73, 2 }) && tracker.HasPending(&texture)
            && tracker.QueryPending(&texture, recoverySnapshot) && recoverySnapshot.Resource == &texture
            && recoverySnapshot.Source == QueueType::Copy && recoverySnapshot.Destination == QueueType::Graphics
            && recoverySnapshot.Before == ResourceState::CopyDest && recoverySnapshot.After == ResourceState::CopySource
            && recoverySnapshot.ReleaseToken.DeviceId == 73 && recoverySnapshot.ReleaseToken.SubmissionId == 2
            && !tracker.Recover(texture, { 73, 1 }, CompletionStatus::Complete)
            && !tracker.Recover(texture, { 73, 2 }, CompletionStatus::Incomplete)
            && tracker.Recover(texture, { 73, 2 }, CompletionStatus::Complete)
            && tracker.QueryOwner(&texture, owner) && owner == QueueType::Copy
            && tracker.QueryState(&texture, state) && state == ResourceState::CopyDest;
        const bool passed = rejected && released && acquired && recoverySetup;
        return passed;
    }

    bool TestVulkanQueueAdmissionPolicy()
    {
        using namespace Engine::RHI;
        const VulkanQueueAdmission fallback {};
        const VulkanQueueAdmission sameFamily { true, true, 3, 3, 3 };
        const VulkanQueueAdmission splitFamilies { true, true, 0, 2, 1 };
        const QueueResolution fallbackCopy = ResolveVulkanQueue(fallback, QueueType::Copy);
        const QueueResolution sameCompute = ResolveVulkanQueue(sameFamily, QueueType::Compute);
        const QueueResolution splitCopy = ResolveVulkanQueue(splitFamilies, QueueType::Copy);
        const bool resolution = ResolveVulkanQueue(fallback, QueueType::Graphics).Independent
            && fallbackCopy.Requested == QueueType::Copy && fallbackCopy.Effective == QueueType::Graphics
            && !fallbackCopy.Independent && sameCompute.Effective == QueueType::Compute && sameCompute.Independent
            && splitCopy.Effective == QueueType::Copy && splitCopy.Independent;
        const bool policy = VulkanQueuesMayShareResources(fallback, QueueType::Graphics, QueueType::Copy)
            && VulkanQueuesMayShareResources(sameFamily, QueueType::Graphics, QueueType::Compute)
            && !VulkanQueuesMayShareResources(splitFamilies, QueueType::Graphics, QueueType::Copy)
            && !VulkanQueuesMayShareResources(splitFamilies, QueueType::Copy, QueueType::Compute);
        return Expect(resolution && policy, "Vulkan queue admission resolves fallback and rejects only different-family shared resources");
    }

}

int main()
{
    Engine::Log::Init();

    const std::vector<std::pair<std::string_view, TestFunction>> tests = {
        { "Slang compiler emits validated portable shader packages", TestSlangShaderCompilerProducesPortableValidatedPackages },
        { "Async shader publication is nonblocking deduplicated and atomic", TestAsyncShaderPackagePublicationIsNonblockingDeduplicatedAndAtomic },
        { "Async shader failure retention inline equivalence and shutdown safety", TestAsyncShaderFailureRetentionInlineEquivalenceAndShutdownSafety },
        { "Render graph orders hazards and lifetimes deterministically", TestRenderGraphOrdersHazardsAndLifetimesDeterministically },
        { "Render graph tracks RAW WAR WAW barriers and queue transitions", TestRenderGraphTracksRawWarWawBarriersAndQueueTransitions },
        { "Render graph rejects invalid declarations and cycles", TestRenderGraphRejectsInvalidDeclarationsAndCycles },
        { "Render graph executor rejects invalid bindings before recording", TestRenderGraphExecutorRejectsBindingsBeforeRecording },
        { "Render graph executor stops after callback failure", TestRenderGraphExecutorStopsAfterCallbackFailure },
        { "Render graph executor orders barriers and restricts context", TestRenderGraphExecutorOrdersBarriersAndRestrictsContext },
        { "Render graph executor exhausts and reuses exact retired contexts", TestRenderGraphExecutorPoolExhaustionAndExactRetirement },
        { "Render graph transient allocation reuses compatible lifetimes after exact retirement", TestRenderGraphTransientAllocationReuseAndExactRetirement },
        { "Render graph transient accepted prefixes retain exact retirement", TestRenderGraphTransientAcceptedPrefixRetainsRetirement },
        { "Render graph transient logical texture estimates cover mips and formats", TestRenderGraphTransientLogicalTextureCost },
        { "Render graph executor translates cross-queue ownership and fallback", TestRenderGraphExecutorCrossQueueOwnershipAndFallback },
        { "Render graph executor preserves accepted prefix on missing producer token", TestRenderGraphExecutorRejectsMissingProducerTokenBeforeConsumer },
        { "RHI buffer transition contract rejects incompatible states", TestRhiBufferTransitionContract },
        { "RHI completion token contract rejects unissued identities", TestRhiCompletionTokenContract },
        { "RHI submission dependencies reject invalid token graphs", TestRhiSubmissionDependencyValidation },
        { "RHI queue topology submits dependencies without premature publication", TestRhiQueueTopologyDependencySubmissionContract },
        { "RHI resource-state query rejects null foreign and unknown resources", TestRhiResourceOwnershipContract },
        { "RHI buffer ownership lifecycle publishes only accepted exact-token pairs", TestRhiBufferOwnershipLifecycleContract },
        { "RHI buffer ownership recovery and adapter seams preserve fallback semantics", TestRhiBufferOwnershipRecoveryAndAdapterSeams },
        { "RHI texture ownership tracker preserves accepted-token pending publication", TestRhiTextureOwnershipLifecycleContract },
        { "Vulkan queue admission preserves fallback same-family and split-family policy", TestVulkanQueueAdmissionPolicy },
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
        { "Scene version 4 canonical persistence", TestSceneVersionFourCanonicalPersistence },
        { "Scene loads legacy absolute transforms", TestSceneLoadsLegacyAbsoluteTransforms },
        { "Scene rejects invalid version 4 world state", TestSceneRejectsInvalidVersionFourWorldState },
        { "Camera-relative large-world transform", TestCameraRelativeLargeWorldTransform },
        { "World grid canonicalization and bounds", TestWorldGridCanonicalizationAndBounds },
        { "Per-view sector-snapped origin tracking", TestPerViewSectorSnappedOriginTracking },
        { "Scene raster origin epoch invariance", TestSceneRasterOriginEpochInvariance },
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
        { "Portable shader contract validates deterministic cache and layouts", TestPortableShaderContract },
        { "Frame timing capability group rejects invalid timestamp lifecycle", TestFrameTimingCapabilityGroupRejectsInvalidTimestampLifecycle },
        { "Transient capability group selects placed aliasing only when both RHI features are usable", TestTransientCapabilityGroupSelectsPlacedAliasingOnlyWhenBothRhiFeaturesAreUsable },
        { "Transient capability group selects GPU-retired non-aliased fallback", TestTransientCapabilityGroupSelectsGpuRetiredNonAliasedFallback }
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
