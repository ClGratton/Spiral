#pragma once

#include "Fab/FabImportController.h"

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/FabImportReceipt.h"
#include "Engine/Assets/FabProjectState.h"
#include "Engine/Assets/MaterialAsset.h"
#include "Engine/Core/Base.h"
#include "Engine/Scene/Scene.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// Editor-side glue between the live project and the Engine-owned Fab import
// authorities. Nothing here knows ImGui or EditorLayer: it builds the value-copy
// project context a controller call needs, swaps a committed result into the
// Editor's project state, and runs the full-hash project validation on a worker.
// The Editor remains responsible for the live-state sequencing around these
// calls (history, renderer publication, watcher, Scene swap).
namespace FabEditor
{
    // Where the project lives on disk. Every manifest path resolves beneath Root.
    struct ProjectLocation
    {
        std::filesystem::path Root;
        // Strict portable path of the manifest beneath Root.
        std::string ManifestRelativePath;
    };

    // Project state the Editor tracks beside its registry, materials and Scene.
    struct ProjectFabState
    {
        Engine::FabReceiptCollection Receipts;
        // Project-relative path of the receipt collection; empty when none.
        std::string ReceiptsPath;
        Engine::u64 Revision = 0;
        // SHA-256 of the manifest bytes the Editor last read or wrote.
        std::string ManifestSha256;
        // none, passed, or failed: the structural check that ran at the last open.
        std::string StructuralStatus = "none";
        std::string StructuralMessage;
    };

    struct ContextInputs
    {
        ProjectLocation Location;
        const Engine::AssetRegistry* Registry = nullptr;
        const ProjectFabState* FabProject = nullptr;
        const Engine::Scene* Scene = nullptr;
        std::optional<Fab::FabAssignmentTarget> Assignment;
        // Private staging used to serialize the Scene bytes.
        std::filesystem::path ScratchRoot;
    };

    // Reads the exact manifest bytes from disk and refuses a manifest that no
    // longer matches the bytes the Editor last read or wrote, so a commit can
    // never silently overwrite an external change.
    bool BuildImportContext(const ContextInputs& inputs, Fab::FabImportProjectContext& out, std::string& error);

    // The Scene codec is file-based; these bounce through a private staging directory.
    bool SerializeScene(const Engine::Scene& scene, const std::filesystem::path& scratchRoot,
        std::string& outBytes, std::string& error);
    bool DeserializeScene(std::string_view bytes, const std::filesystem::path& scratchRoot,
        Engine::Scene& outScene, std::string& error);

    // Reads and hashes the manifest as the commit authority will.
    bool ReadManifestSha256(const ProjectLocation& location, std::string& outSha256, std::string& error);

    struct AdoptionTargets
    {
        Engine::AssetRegistry& Registry;
        Engine::MaterialLibrary& Materials;
        ProjectFabState& FabProject;
        std::string& ScenePath;
        std::string& AssetRegistryPath;
    };

    // Swaps a committed project change into the Editor's project state: registry,
    // receipts, the new immutable Material, committed manifest paths, revision and
    // manifest digest. It does not touch the Scene, history, renderer or watcher.
    bool AdoptCommitResult(const Fab::FabImportCommitResult& result, AdoptionTargets targets, std::string& error);

    // Full-hash project validation (plus the read-only orphan report) on a worker.
    class ProjectValidator
    {
    public:
        enum class State
        {
            None,
            Running,
            Passed,
            Failed
        };

        ProjectValidator() = default;
        ~ProjectValidator();
        ProjectValidator(const ProjectValidator&) = delete;
        ProjectValidator& operator=(const ProjectValidator&) = delete;

        // Main thread. Refuses while a validation is running.
        bool Start(const ProjectLocation& location, std::string& error);
        // Main thread, once per frame: publishes a finished worker result.
        void Poll();
        void Shutdown();

        State GetState() const { return m_State; }
        const std::string& GetMessage() const { return m_Message; }
        static const char* ToString(State state);

    private:
        struct Job
        {
            std::atomic<bool> Cancel { false };
            std::mutex Mutex;
            std::condition_variable Finished;
            bool Done = false;
            bool Passed = false;
            std::string Message;
        };

        std::shared_ptr<Job> m_Job;
        State m_State = State::None;
        std::string m_Message;
    };
}
