#pragma once

#include "Engine/Core/Base.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Fab
{
    enum class BrowserDownloadVerdict
    {
        Accept,
        RejectExtension,
        RejectSize
    };

    struct BrowserDownloadLimits
    {
        // Matches FabArchiveLimits::MaximumCompressedBytes, the largest archive
        // the importer will admit; a larger download can never be imported.
        Engine::u64 MaximumBytes = 4ull * 1024ull * 1024ull * 1024ull;
        size_t MaximumNameBytes = 128;
        std::vector<std::string> AllowedExtensions { "zip", "glb", "gltf" };
    };

    // Deliberately carries no URL. A download URL can be a signed CDN link and
    // is treated as a credential, so it never reaches this policy, its
    // decisions, or any log line built from them.
    struct BrowserDownloadOffer
    {
        std::string_view SuggestedName;
        Engine::u64 TotalBytes = 0; // 0 when the server did not declare a length.
    };

    struct BrowserDownloadDecision
    {
        BrowserDownloadVerdict Verdict = BrowserDownloadVerdict::RejectExtension;
        std::string SanitizedName;
        // Set only for Accept: one directory segment that is unique for the
        // life of the policy and, through the nonce, across sessions.
        std::string StagingDirectoryName;
        // StagingDirectoryName / SanitizedName; empty unless accepted.
        std::filesystem::path RelativeStagedPath;
    };

    // Returns a non-empty, portable, traversal-free file name: the last path
    // component only, characters outside [A-Za-z0-9._-] replaced by '_', no
    // leading or trailing dots, every dot except the last replaced by '_'
    // (defeating double-extension tricks), extension lower-cased, Windows
    // reserved device names prefixed with '_', and the whole name capped at
    // maximumBytes with the extension preserved. A name with no extension
    // comes back without one. maximumBytes below 8 is raised to 8, and the
    // extension is capped at 16 bytes (and at maximumBytes - 2).
    std::string SanitizeDownloadFileName(std::string_view suggested, size_t maximumBytes = 128);

    // Lower-case extension of an already sanitized name, without the dot, or
    // empty.
    std::string_view ExtensionOfSanitizedName(std::string_view sanitizedName);

    class BrowserDownloadPolicy
    {
    public:
        BrowserDownloadPolicy() = default;
        explicit BrowserDownloadPolicy(BrowserDownloadLimits limits);

        // The nonce should be random per call so staging directory names are
        // also unique across processes; uniqueness within this instance is
        // guaranteed by the internal sequence even if the nonce repeats.
        BrowserDownloadDecision Evaluate(const BrowserDownloadOffer& offer, Engine::u64 nonce);

        // False once receivedBytes exceeds the limit; the host cancels then.
        bool WithinSizeLimit(Engine::u64 receivedBytes) const { return receivedBytes <= m_Limits.MaximumBytes; }

        const BrowserDownloadLimits& Limits() const { return m_Limits; }

        // root / directory / name for an accepted decision whose two segments are
        // plain [A-Za-z0-9._-] names other than "." and "..", which is what keeps
        // the result inside root. Empty path plus error otherwise, including for a
        // decision that was not accepted.
        static std::filesystem::path ResolveStagedPath(
            const std::filesystem::path& stagingRoot, const BrowserDownloadDecision& decision, std::string& error);

        // One line built only from the sanitized name, the verdict, and the
        // declared size; never the suggested name, URL, or staging root.
        static std::string FormatLogLine(const BrowserDownloadDecision& decision, Engine::u64 totalBytes);

    private:
        BrowserDownloadLimits m_Limits;
        Engine::u64 m_Sequence = 0;
    };
}
