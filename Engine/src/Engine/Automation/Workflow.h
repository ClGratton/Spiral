#pragma once

#include <string>
#include <vector>

namespace Engine
{
    struct WorkflowStep
    {
        std::string Name;
        bool Completed = false;
    };

    struct WorkflowDefinition
    {
        std::string Name;
        std::vector<WorkflowStep> Steps;
    };
}
