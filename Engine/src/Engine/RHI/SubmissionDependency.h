#pragma once

#include "Engine/RHI/CompletionToken.h"

#include <unordered_set>
#include <vector>

namespace Engine::RHI
{
    enum class SubmissionDependencyError
    {
        None,
        Zero,
        ForeignDevice,
        Unissued,
        Duplicate,
        Forward,
        ImpossibleSelf
    };

    template <typename IsIssued>
    SubmissionDependencyError ValidateSubmissionDependencies(
        u64 deviceId,
        u64 prospectiveSubmissionId,
        const std::vector<CompletionToken>& dependencies,
        IsIssued&& isIssued)
    {
        std::unordered_set<u64> seen;
        for (const CompletionToken& dependency : dependencies)
        {
            if (!dependency.IsValid())
                return SubmissionDependencyError::Zero;
            if (dependency.DeviceId != deviceId)
                return SubmissionDependencyError::ForeignDevice;
            if (dependency.SubmissionId == prospectiveSubmissionId)
                return SubmissionDependencyError::ImpossibleSelf;
            if (dependency.SubmissionId > prospectiveSubmissionId)
                return SubmissionDependencyError::Forward;
            if (!seen.insert(dependency.SubmissionId).second)
                return SubmissionDependencyError::Duplicate;
            if (!isIssued(dependency))
                return SubmissionDependencyError::Unissued;
        }
        return SubmissionDependencyError::None;
    }
}
