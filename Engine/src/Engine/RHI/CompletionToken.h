#pragma once

#include "Engine/Core/Base.h"

namespace Engine::RHI
{
    // A device-owned submission identity. The fields deliberately carry no
    // backend synchronization object; only Device can interpret them.
    struct CompletionToken
    {
        u64 DeviceId = 0;
        u64 SubmissionId = 0;

        constexpr bool IsValid() const
        {
            return DeviceId != 0 && SubmissionId != 0;
        }
    };

    enum class CompletionStatus
    {
        Invalid,
        Incomplete,
        Complete,
        Failed
    };
}
