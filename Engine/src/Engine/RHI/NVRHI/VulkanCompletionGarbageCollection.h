#pragma once

#include "Engine/RHI/CompletionToken.h"

namespace Engine::RHI
{
    // A Vulkan completion record remains queryable for the device lifetime.
    // Native NVRHI command-buffer references may be collected exactly once,
    // when that record first crosses from Incomplete to a terminal status.
    inline bool ShouldCollectVulkanNativeGarbage(
        CompletionStatus previous, CompletionStatus observed)
    {
        return previous == CompletionStatus::Incomplete
            && (observed == CompletionStatus::Complete || observed == CompletionStatus::Failed);
    }
}
