#include "Engine/Core/Log.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/RHI/Capability.h"
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
        { "Scene rejects duplicate entities", TestSceneRejectsDuplicateEntities },
        { "Capability state keeps lifecycle stages distinct", TestCapabilityStateKeepsLifecycleStagesDistinct },
        { "Capability selection retains fallbacks and rejections", TestCapabilitySelectionRetainsFallbacksAndRejections },
        { "Capability selection validates format usage and stable ranking", TestCapabilitySelectionValidatesFormatUsageAndStableRanking },
        { "Capability selection rejects API limits and synchronization", TestCapabilitySelectionRejectsApiLimitsAndSynchronization },
        { "Capability selection honors strict preference", TestCapabilitySelectionHonorsStrictPreference }
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
