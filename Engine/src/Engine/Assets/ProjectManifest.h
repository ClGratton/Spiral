#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/ColorPipelineSettings.h"
#include "Engine/Renderer/FramePacingPolicy.h"
#include "Engine/Renderer/PresentationPolicy.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace Engine
{
    inline constexpr u32 kProjectManifestFormatVersion = 7;
    inline constexpr u64 kMaximumProjectManifestBytes = 1ull << 20;
    inline constexpr u64 kMaximumProjectManifestPathBytes = 4096;

    // The project manifest is the sole commit pointer for a project revision.
    // Format 7 adds FabReceipts and ProjectRevision. Formats 1 through 6 migrate
    // deterministically: no receipt collection and revision 0.
    //
    // The first five members keep the pre-extraction aggregate shape so existing
    // positional initializers remain valid.
    struct ProjectManifest
    {
        std::string ScenePath;
        std::string AssetRegistryPath;
        Engine::FramePacingPolicy FramePacingPolicy;
        Engine::PresentationPolicy PresentationPolicy = Engine::PresentationPolicy::Synchronized;
        RendererColorPipelineSettings ColorPipelineSettings;

        // Empty means the project owns no Fab receipt collection (formats 1-6).
        // Nonempty must be a strict portable project-relative path.
        std::string FabReceiptsPath = {};
        // Monotonic commit counter. Ordinary saves preserve it; only a project
        // commit advances it, by exactly one.
        u64 ProjectRevision = 0;

        bool operator==(const ProjectManifest& other) const
        {
            return ScenePath == other.ScenePath && AssetRegistryPath == other.AssetRegistryPath
                && FramePacingPolicy.Mode == other.FramePacingPolicy.Mode
                && FramePacingPolicy.SmoothTargetFramesPerSecond
                    == other.FramePacingPolicy.SmoothTargetFramesPerSecond
                && PresentationPolicy == other.PresentationPolicy
                && ColorPipelineSettings == other.ColorPipelineSettings
                && FabReceiptsPath == other.FabReceiptsPath && ProjectRevision == other.ProjectRevision;
        }
    };

    // A strict portable project-relative path: nonempty, relative, '/'-separated,
    // ASCII, no '.', '..', empty, reserved or drive/stream segments. Shares its
    // grammar with AssetRegistry::IsValidCookedRoot.
    bool IsPortableProjectRelativePath(std::string_view path);

    // Serialization validates exactly what deserialization validates and always
    // emits the current format. outBytes is replaced only on success.
    bool SerializeProjectManifest(
        const ProjectManifest& manifest, std::string& outBytes, std::string& outError);
    // Parsing is transactional: outManifest is replaced only on success. Input
    // larger than kMaximumProjectManifestBytes, embedded NUL or control bytes,
    // unknown or duplicate keys, wrong-version keys, missing required keys,
    // malformed or out-of-range values, and path escapes are rejected.
    bool DeserializeProjectManifest(
        std::string_view bytes, ProjectManifest& outManifest, std::string& outError);

    // Store atomically replaces this one file. It is not a project commit.
    bool StoreProjectManifest(const std::filesystem::path& path, const ProjectManifest& manifest,
        std::string& outError, bool* outDirectoryDurable = nullptr);
    // Bounded read: more than kMaximumProjectManifestBytes is an error. A project
    // commit's ExpectedManifestSha256 is the SHA-256 of exactly these bytes.
    bool ReadProjectManifestBytes(
        const std::filesystem::path& path, std::string& outBytes, std::string& outError);
    bool LoadProjectManifest(
        const std::filesystem::path& path, ProjectManifest& outManifest, std::string& outError);
}
