#include "Engine/Jobs/FrameTaskGraph.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <queue>

namespace Engine
{
    namespace
    {
        struct GraphSchedule
        {
            std::vector<FrameTaskId> TopologicalOrder;
            std::vector<std::vector<FrameTaskId>> Levels;
            std::string Error;
        };

        GraphSchedule BuildSchedule(const std::vector<FrameTaskDescription>& tasks)
        {
            GraphSchedule schedule;
            std::vector<u32> dependencyCounts(tasks.size(), 0);
            std::vector<std::vector<FrameTaskId>> dependents(tasks.size());

            for (FrameTaskId taskId = 0; taskId < tasks.size(); ++taskId)
            {
                const FrameTaskDescription& task = tasks[taskId];
                if (!task.Execute)
                {
                    schedule.Error = "task '" + task.Name + "' has no execute callback";
                    return schedule;
                }
                if (task.Publication)
                {
                    for (FrameTaskId previousTask = 0; previousTask < taskId; ++previousTask)
                    {
                        if (tasks[previousTask].Publication == task.Publication)
                        {
                            schedule.Error = "frame publication has more than one producer";
                            return schedule;
                        }
                    }
                }

                for (FrameTaskId dependency : task.Dependencies)
                {
                    if (dependency >= tasks.size())
                    {
                        schedule.Error = "task '" + task.Name + "' has an invalid dependency";
                        return schedule;
                    }
                    if (dependency == taskId)
                    {
                        schedule.Error = "task '" + task.Name + "' depends on itself";
                        return schedule;
                    }
                    if (std::count(task.Dependencies.begin(), task.Dependencies.end(), dependency) > 1)
                    {
                        schedule.Error = "task '" + task.Name + "' declares a duplicate dependency";
                        return schedule;
                    }

                    ++dependencyCounts[taskId];
                    dependents[dependency].push_back(taskId);
                }
            }

            std::priority_queue<FrameTaskId, std::vector<FrameTaskId>, std::greater<>> ready;
            for (FrameTaskId taskId = 0; taskId < tasks.size(); ++taskId)
            {
                if (dependencyCounts[taskId] == 0)
                    ready.push(taskId);
            }

            while (!ready.empty())
            {
                const FrameTaskId taskId = ready.top();
                ready.pop();
                schedule.TopologicalOrder.push_back(taskId);
                for (FrameTaskId dependent : dependents[taskId])
                {
                    if (--dependencyCounts[dependent] == 0)
                        ready.push(dependent);
                }
            }

            if (schedule.TopologicalOrder.size() != tasks.size())
            {
                schedule.Error = "frame task graph contains a dependency cycle";
                return schedule;
            }

            std::vector<size_t> taskLevels(tasks.size(), 0);
            size_t maximumLevel = 0;
            for (FrameTaskId taskId : schedule.TopologicalOrder)
            {
                for (FrameTaskId dependency : tasks[taskId].Dependencies)
                    taskLevels[taskId] = std::max(taskLevels[taskId], taskLevels[dependency] + 1);
                maximumLevel = std::max(maximumLevel, taskLevels[taskId]);
            }

            if (!tasks.empty())
                schedule.Levels.resize(maximumLevel + 1);
            for (FrameTaskId taskId : schedule.TopologicalOrder)
                schedule.Levels[taskLevels[taskId]].push_back(taskId);

            return schedule;
        }
    }

    bool FrameTaskGraphResult::Succeeded() const
    {
        if (!GraphError.empty())
            return false;

        return std::all_of(TaskStatuses.begin(), TaskStatuses.end(), [](FrameTaskStatus status)
        {
            return status == FrameTaskStatus::Succeeded;
        });
    }

    FrameTaskId FrameTaskGraph::AddTask(FrameTaskDescription description)
    {
        const FrameTaskId id = static_cast<FrameTaskId>(m_Tasks.size());
        m_Tasks.push_back(std::move(description));
        return id;
    }

    FrameTaskGraphResult FrameTaskGraph::Execute(JobSystem& jobSystem, const FrameTaskExecutionOptions& options) const
    {
        FrameTaskGraphResult result;
        result.TaskStatuses.resize(m_Tasks.size(), FrameTaskStatus::Pending);
        result.TaskErrors.resize(m_Tasks.size());

        const GraphSchedule schedule = BuildSchedule(m_Tasks);
        if (!schedule.Error.empty())
        {
            result.GraphError = schedule.Error;
            std::fill(result.TaskStatuses.begin(), result.TaskStatuses.end(), FrameTaskStatus::Skipped);
            return result;
        }

        std::mutex profileMutex;
        const auto emitProfileEvent = [&](FrameTaskId taskId, FrameTaskProfilePhase phase,
                                          FrameTaskStatus status, double duration) noexcept
        {
            try
            {
                if (!options.ProfileHook)
                    return;

                FrameTaskProfileEvent event;
                event.Task = taskId;
                event.FrameIndex = options.FrameIndex;
                event.Name = m_Tasks[taskId].Name;
                event.Phase = phase;
                event.Status = status;
                event.Thread = std::this_thread::get_id();
                event.WorkerIndex = jobSystem.GetCurrentWorkerIndex();
                event.DurationMicroseconds = duration;
                std::scoped_lock lock(profileMutex);
                options.ProfileHook(event);
            }
            catch (...)
            {
            }
        };

        const auto runTask = [&](FrameTaskId taskId) noexcept
        {
            const FrameTaskDescription& task = m_Tasks[taskId];
            result.TaskStatuses[taskId] = FrameTaskStatus::Running;
            emitProfileEvent(taskId, FrameTaskProfilePhase::Begin, FrameTaskStatus::Running, 0.0);
            const auto start = std::chrono::steady_clock::now();

            try
            {
                task.Execute();
                if (task.Publication)
                    task.Publication->Commit();
                result.TaskStatuses[taskId] = FrameTaskStatus::Succeeded;
            }
            catch (const std::exception& exception)
            {
                if (task.Publication)
                {
                    try
                    {
                        task.Publication->Abort();
                    }
                    catch (...)
                    {
                    }
                }
                result.TaskStatuses[taskId] = FrameTaskStatus::Failed;
                try
                {
                    result.TaskErrors[taskId] = exception.what();
                }
                catch (...)
                {
                }
            }
            catch (...)
            {
                if (task.Publication)
                {
                    try
                    {
                        task.Publication->Abort();
                    }
                    catch (...)
                    {
                    }
                }
                result.TaskStatuses[taskId] = FrameTaskStatus::Failed;
                try
                {
                    result.TaskErrors[taskId] = "unknown exception";
                }
                catch (...)
                {
                }
            }

            const auto end = std::chrono::steady_clock::now();
            const double duration = std::chrono::duration<double, std::micro>(end - start).count();
            emitProfileEvent(taskId, FrameTaskProfilePhase::End, result.TaskStatuses[taskId], duration);
        };

        const auto dependenciesSucceeded = [&](FrameTaskId taskId)
        {
            return std::all_of(m_Tasks[taskId].Dependencies.begin(), m_Tasks[taskId].Dependencies.end(), [&](FrameTaskId dependency)
            {
                return result.TaskStatuses[dependency] == FrameTaskStatus::Succeeded;
            });
        };

        const auto skipTask = [&](FrameTaskId taskId) noexcept
        {
            result.TaskStatuses[taskId] = FrameTaskStatus::Skipped;
            try
            {
                result.TaskErrors[taskId] = "dependency did not succeed";
            }
            catch (...)
            {
            }
            emitProfileEvent(taskId, FrameTaskProfilePhase::End, FrameTaskStatus::Skipped, 0.0);
        };

        if (options.Mode == FrameTaskExecutionMode::DeterministicSingleThread
            || jobSystem.GetCurrentWorkerIndex() != kInvalidJobWorkerIndex)
        {
            for (FrameTaskId taskId : schedule.TopologicalOrder)
            {
                if (dependenciesSucceeded(taskId))
                    runTask(taskId);
                else
                    skipTask(taskId);
            }
            return result;
        }

        for (const std::vector<FrameTaskId>& level : schedule.Levels)
        {
            std::vector<FrameTaskId> workerTasks;
            std::vector<FrameTaskId> callingThreadTasks;
            for (FrameTaskId taskId : level)
            {
                if (dependenciesSucceeded(taskId))
                {
                    if (m_Tasks[taskId].Lane == FrameTaskLane::CallingThread)
                        callingThreadTasks.push_back(taskId);
                    else
                        workerTasks.push_back(taskId);
                }
                else
                    skipTask(taskId);
            }

            std::mutex completionMutex;
            std::condition_variable completionCondition;
            size_t remaining = 0;
            try
            {
                for (FrameTaskId taskId : workerTasks)
                {
                    const std::string jobName = "FrameTask:" + m_Tasks[taskId].Name;
                    {
                        std::scoped_lock lock(completionMutex);
                        ++remaining;
                    }
                    try
                    {
                        jobSystem.Submit([&, taskId]
                        {
                            runTask(taskId);
                            {
                                std::scoped_lock lock(completionMutex);
                                --remaining;
                            }
                            completionCondition.notify_one();
                        }, jobName);
                    }
                    catch (...)
                    {
                        std::scoped_lock lock(completionMutex);
                        --remaining;
                        throw;
                    }
                }
            }
            catch (const std::exception& exception)
            {
                std::unique_lock lock(completionMutex);
                completionCondition.wait(lock, [&]() { return remaining == 0; });
                result.GraphError = "frame task submission failed: " + std::string(exception.what());
                std::replace(result.TaskStatuses.begin(), result.TaskStatuses.end(), FrameTaskStatus::Pending, FrameTaskStatus::Skipped);
                return result;
            }
            catch (...)
            {
                std::unique_lock lock(completionMutex);
                completionCondition.wait(lock, [&]() { return remaining == 0; });
                result.GraphError = "frame task submission failed with an unknown exception";
                std::replace(result.TaskStatuses.begin(), result.TaskStatuses.end(), FrameTaskStatus::Pending, FrameTaskStatus::Skipped);
                return result;
            }

            for (FrameTaskId taskId : callingThreadTasks)
                runTask(taskId);

            std::unique_lock lock(completionMutex);
            completionCondition.wait(lock, [&]() { return remaining == 0; });
        }

        return result;
    }
}
