#pragma once

#include "Engine/Core/Base.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Fab
{
    enum class FabIntakeKind
    {
        Zip,
        Glb,
        Gltf,
        Folder,
        Unsupported
    };

    enum class FabIntakeReason
    {
        None,
        Missing,
        NotRegularObject, // symlink, FIFO, device, socket, or reparse point
        Empty,
        TooLarge,
        ExtensionContentMismatch,
        UnrecognizedContent,
        IncompleteZip,
        FolderWithoutGltf,
        FolderTooLarge,
        TooManyPaths,
        IoError
    };

    struct FabIntakeLimits
    {
        Engine::u64 MaximumFileBytes = 4ull * 1024ull * 1024ull * 1024ull;
        Engine::u64 MaximumGltfJsonBytes = 64ull * 1024ull * 1024ull;
        Engine::u64 MaximumFolderEntries = 4096;
        Engine::u32 MaximumFolderDepth = 16;
        size_t MaximumPathsPerSubmission = 32;
    };

    struct FabIntakeClassification
    {
        FabIntakeKind Kind = FabIntakeKind::Unsupported;
        FabIntakeReason Reason = FabIntakeReason::None;
        Engine::u64 SizeBytes = 0;
    };

    // Routing classification for an explicitly supplied path (drop, completed
    // download, or typed control). Content decides: a file is Zip, Glb, or Gltf
    // by its leading/trailing structure, and a recognised extension that
    // contradicts the content is refused rather than trusted. The path is
    // opened without following a final symlink and only through a regular
    // file descriptor; directories are walked without following links and
    // within entry/depth bounds. This is a UX router and not a security gate:
    // the Engine snapshot and ZIP authorities re-validate everything after it.
    // On non-POSIX hosts the final-component check precedes the open and is
    // therefore not race-free; that platform is unqualified.
    FabIntakeClassification ClassifyFabIntakePath(
        const std::filesystem::path& path, const FabIntakeLimits& limits = {});

    enum class FabIntakeOrigin
    {
        Drop,
        Download,
        Typed
    };

    struct FabIntakeRequest
    {
        std::filesystem::path Path;
        FabIntakeOrigin Origin = FabIntakeOrigin::Drop;
        FabIntakeKind Kind = FabIntakeKind::Unsupported;
        Engine::u64 SizeBytes = 0;
    };

    struct FabIntakeRejection
    {
        // File name only, with non-printable bytes replaced; safe to print.
        std::string DisplayName;
        FabIntakeReason Reason = FabIntakeReason::None;
    };

    struct FabIntakePlan
    {
        std::vector<FabIntakeRequest> Accepted;
        std::vector<FabIntakeRejection> Rejected;
    };

    // Classifies UTF-8 paths in order, drops exact duplicates after lexical
    // normalisation, and rejects everything beyond the per-submission cap.
    FabIntakePlan PlanFabIntake(FabIntakeOrigin origin, std::span<const std::string> utf8Paths,
        const FabIntakeLimits& limits = {});
}
