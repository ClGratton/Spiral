#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Engine
{
    // Writes bytes to an exclusively-created sibling and replaces the target in
    // one namespace operation. Callers retain authority over their schema and
    // whether the parent directory is trusted/private.
    bool WriteFileAtomically(
        const std::filesystem::path& path, std::string_view bytes, std::string& outError);
}
