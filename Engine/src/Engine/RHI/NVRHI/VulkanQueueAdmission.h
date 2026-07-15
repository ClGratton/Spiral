#pragma once

#include "Engine/RHI/Device.h"

namespace Engine::RHI
{
    struct VulkanQueueAdmission
    {
        bool ComputeEnabled = false;
        bool CopyEnabled = false;
        u32 GraphicsFamily = 0;
        u32 ComputeFamily = 0;
        u32 CopyFamily = 0;
    };
    inline QueueResolution ResolveVulkanQueue(const VulkanQueueAdmission& a, QueueType q)
    {
        const bool enabled = q == QueueType::Graphics || (q == QueueType::Compute && a.ComputeEnabled)
            || (q == QueueType::Copy && a.CopyEnabled);
        return { q, enabled ? q : QueueType::Graphics, enabled };
    }
    inline bool VulkanQueuesMayShareResources(const VulkanQueueAdmission& a, QueueType s, QueueType d)
    {
        const QueueType source = ResolveVulkanQueue(a, s).Effective;
        const QueueType destination = ResolveVulkanQueue(a, d).Effective;
        if (source == destination)
            return true;
        const auto family = [&a](QueueType q) {
            return q == QueueType::Compute ? a.ComputeFamily
                : q == QueueType::Copy ? a.CopyFamily : a.GraphicsFamily;
        };
        return family(source) == family(destination);
    }
}
