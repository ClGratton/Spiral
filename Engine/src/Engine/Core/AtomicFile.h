#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Engine
{
    // Writes bytes to an exclusively-created sibling and replaces the target in
    // one namespace operation. Callers retain authority over their schema and
    // whether the parent directory is trusted/private. On success,
    // outDirectoryDurable (when supplied) reports whether the replacement's
    // directory entry was confirmed durable; false means the new file is visible
    // but a power loss may still resurrect the previous one.
    bool WriteFileAtomically(const std::filesystem::path& path, std::string_view bytes,
        std::string& outError, bool* outDirectoryDurable = nullptr);

    enum class DirectoryPublishStatus
    {
        Published,
        PublishedDurabilityUnconfirmed,
        AlreadyExists,
        Unsupported,
        Failed
    };

    // Moves a fully written directory to a final path that must not exist, in one
    // namespace operation that never replaces an existing entry (Linux
    // renameat2 RENAME_NOREPLACE). A filesystem or platform that cannot provide
    // that guarantee returns Unsupported and changes nothing; there is no
    // replacing fallback. Both paths must be on one filesystem. The final path's
    // parent is created when missing. outError is cleared on Published* only.
    DirectoryPublishStatus PublishDirectoryNoReplace(const std::filesystem::path& stagedDirectory,
        const std::filesystem::path& finalDirectory, std::string& outError);
}
