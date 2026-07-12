#include "Engine/Core/Log.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/Scene/Scene.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>
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

    bool TestSceneRoundTrip()
    {
        const std::filesystem::path path = TestFilePath("scene-round-trip.spiral");
        Engine::Scene source("Round Trip");
        Engine::CameraComponent camera = source.GetMainCamera();
        camera.BackgroundColor = { 0.21f, 0.34f, 0.55f };
        source.SetMainCamera(camera);
        const Engine::Entity cube = source.CreateEntity("Cube");
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
        const Engine::MeshRendererComponent* loadedMesh = loaded.TryGetMeshRendererComponent(loadedCube);
        return Expect(result, "a valid scene saves and loads")
            && Expect(loaded.GetName() == "Round Trip", "the scene name round trips")
            && Expect(loadedMesh && loadedMesh->MeshAsset == 42 && loadedMesh->MaterialAsset == 84, "mesh handles round trip")
            && Expect(loaded.GetMainCamera().BackgroundColor.X == 0.21f
                && loaded.GetMainCamera().BackgroundColor.Y == 0.34f
                && loaded.GetMainCamera().BackgroundColor.Z == 0.55f, "camera background color round trips");
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
}

int main()
{
    Engine::Log::Init();

    const std::vector<std::pair<std::string_view, TestFunction>> tests = {
        { "JobSystem contains worker exceptions", TestJobSystemContainsWorkerExceptions },
        { "JobSystem inline fallback is reentrant", TestJobSystemInlineFallbackIsReentrant },
        { "Scene round trip", TestSceneRoundTrip },
        { "Scene rejects truncated components", TestSceneRejectsTruncatedComponent },
        { "Scene loads version-one camera", TestSceneLoadsVersionOneCamera },
        { "Scene rejects duplicate entities", TestSceneRejectsDuplicateEntities }
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
