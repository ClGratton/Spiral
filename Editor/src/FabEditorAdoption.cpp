#include "FabEditorAdoption.h"

#include "Engine/Assets/FabZipStaging.h"
#include "Engine/Assets/ProjectManifest.h"
#include "Engine/Core/Sha256.h"
#include "Engine/Jobs/JobSystem.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

namespace FabEditor
{
    namespace fs = std::filesystem;
    using Engine::u64;

    bool ReadManifestSha256(const ProjectLocation& location, std::string& outSha256, std::string& error)
    {
        std::string bytes;
        if (!Engine::ReadProjectManifestBytes(location.Root / fs::path(location.ManifestRelativePath), bytes, error))
            return false;
        outSha256 = Engine::Sha256Builder::HashString(bytes);
        return true;
    }

    bool SerializeScene(const Engine::Scene& scene, const fs::path& scratchRoot, std::string& outBytes, std::string& error)
    {
        Engine::FabStagingDirectory directory;
        if (!Engine::PrepareFabStagingRoot(scratchRoot, error)
            || !Engine::FabStagingDirectory::Create(scratchRoot, "scene", directory, error))
            return false;
        const fs::path file = directory.GetPath() / "scene.spiral";
        std::string removeError;
        const auto finish = [&](bool result)
        {
            directory.Remove(removeError);
            return result;
        };
        if (!scene.SaveToFile(file))
        {
            error = "the Scene could not be serialized";
            return finish(false);
        }
        std::ifstream input(file, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (bytes.empty())
        {
            error = "the serialized Scene could not be read back";
            return finish(false);
        }
        outBytes = std::move(bytes);
        return finish(true);
    }

    bool DeserializeScene(std::string_view bytes, const fs::path& scratchRoot, Engine::Scene& outScene, std::string& error)
    {
        Engine::FabStagingDirectory directory;
        if (!Engine::PrepareFabStagingRoot(scratchRoot, error)
            || !Engine::FabStagingDirectory::Create(scratchRoot, "scene", directory, error))
            return false;
        const fs::path file = directory.GetPath() / "scene.spiral";
        std::string removeError;
        bool ok = false;
        {
            std::ofstream output(file, std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            ok = static_cast<bool>(output);
        }
        Engine::Scene scene;
        if (!ok)
            error = "the Scene bytes could not be staged";
        else if (!(ok = Engine::Scene::LoadFromFile(file, scene)))
            error = "the committed Scene could not be loaded";
        directory.Remove(removeError);
        if (ok)
            outScene = std::move(scene);
        return ok;
    }

    bool BuildImportContext(const ContextInputs& inputs, Fab::FabImportProjectContext& out, std::string& error)
    {
        if (!inputs.Registry || !inputs.FabProject || !inputs.Scene || inputs.Location.Root.empty())
        {
            error = "the project is not available";
            return false;
        }
        Fab::FabImportProjectContext context;
        context.ProjectRoot = inputs.Location.Root;
        context.ManifestRelativePath = inputs.Location.ManifestRelativePath;
        if (!Engine::IsPortableProjectRelativePath(context.ManifestRelativePath))
        {
            error = "this project's manifest path is not a strict project-relative path, so it cannot take a Fab import";
            return false;
        }
        std::string manifestError;
        if (!Engine::ReadProjectManifestBytes(
                context.ProjectRoot / fs::path(context.ManifestRelativePath), context.ManifestBytes, manifestError))
        {
            error = "the project manifest could not be read (save the project first): " + manifestError;
            return false;
        }
        if (!inputs.FabProject->ManifestSha256.empty()
            && Engine::Sha256Builder::HashString(context.ManifestBytes) != inputs.FabProject->ManifestSha256)
        {
            error = "the project manifest changed on disk since the Editor last read or wrote it";
            return false;
        }
        context.Registry = *inputs.Registry;
        context.Receipts = inputs.FabProject->Receipts;
        if (!SerializeScene(*inputs.Scene, inputs.ScratchRoot, context.SceneBytes, error))
            return false;
        context.Assignment = inputs.Assignment;
        out = std::move(context);
        return true;
    }

    bool AdoptCommitResult(const Fab::FabImportCommitResult& result, AdoptionTargets targets, std::string& error)
    {
        if (!result.ProjectChanged)
        {
            error = "the result changed nothing to adopt";
            return false;
        }
        const Engine::AssetMetadata* material = result.Registry.GetAsset(result.MaterialHandle);
        if (!material || !Engine::IsImmutableMaterialAsset(*material))
        {
            error = "the committed material is not an immutable package material";
            return false;
        }
        if (!targets.Materials.Set(result.MaterialHandle, result.Material))
        {
            error = "the committed material could not be added to the material library";
            return false;
        }
        targets.Registry = result.Registry;
        targets.FabProject.Receipts = result.Receipts;
        targets.FabProject.ReceiptsPath = result.Manifest.FabReceiptsPath;
        targets.FabProject.Revision = result.Manifest.ProjectRevision;
        targets.FabProject.ManifestSha256 = result.Commit.CommittedManifestSha256;
        targets.ScenePath = result.Manifest.ScenePath;
        targets.AssetRegistryPath = result.Manifest.AssetRegistryPath;
        return true;
    }

    const char* ProjectValidator::ToString(State state)
    {
        switch (state)
        {
            case State::None: return "none";
            case State::Running: return "running";
            case State::Passed: return "passed";
            case State::Failed: return "failed";
        }
        return "none";
    }

    ProjectValidator::~ProjectValidator()
    {
        Shutdown();
    }

    bool ProjectValidator::Start(const ProjectLocation& location, std::string& error)
    {
        if (m_State == State::Running)
        {
            error = "a project validation is already running";
            return false;
        }
        if (location.Root.empty() || location.ManifestRelativePath.empty())
        {
            error = "the project location is unknown";
            return false;
        }
        auto job = std::make_shared<Job>();
        m_Job = job;
        m_State = State::Running;
        m_Message = "validating every committed artifact hash";
        Engine::JobSystem::Get().Submit([job, location]()
        {
            Engine::FabProjectState state;
            Engine::FabProjectValidationOptions options;
            options.Level = Engine::FabProjectValidationLevel::FullHash;
            options.IsCancelled = [job]() { return job->Cancel.load(std::memory_order_acquire); };
            std::string message;
            bool passed = Engine::LoadFabProjectState(location.Root, location.ManifestRelativePath, options, state, message);
            if (passed)
            {
                Engine::FabProjectOrphanReport orphans;
                std::string orphanError;
                if (Engine::FindFabProjectOrphans(state, location.Root, location.ManifestRelativePath, orphans, orphanError))
                {
                    message = "passed: " + std::to_string(state.Receipts.Receipts.size()) + " receipt(s), "
                        + std::to_string(orphans.UnreferencedGenerations.size()) + " unreferenced generation(s), "
                        + std::to_string(orphans.TemporaryFiles.size()) + " temporary file(s)";
                }
                else
                {
                    message = "passed; the orphan report failed: " + orphanError;
                }
            }
            else if (job->Cancel.load(std::memory_order_acquire))
            {
                message = "cancelled";
            }
            std::scoped_lock lock(job->Mutex);
            job->Passed = passed;
            job->Message = std::move(message);
            job->Done = true;
            job->Finished.notify_all();
        }, "FabProjectValidation");
        return true;
    }

    void ProjectValidator::Poll()
    {
        if (m_State != State::Running || !m_Job)
            return;
        std::scoped_lock lock(m_Job->Mutex);
        if (!m_Job->Done)
            return;
        m_State = m_Job->Passed ? State::Passed : State::Failed;
        m_Message = m_Job->Message;
    }

    void ProjectValidator::Shutdown()
    {
        if (!m_Job)
            return;
        m_Job->Cancel.store(true, std::memory_order_release);
        std::unique_lock lock(m_Job->Mutex);
        m_Job->Finished.wait(lock, [this]() { return m_Job->Done; });
        if (m_State == State::Running)
        {
            m_State = State::Failed;
            m_Message = "cancelled";
        }
        lock.unlock();
        m_Job.reset();
    }
}
