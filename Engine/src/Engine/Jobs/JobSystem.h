#pragma once

#include "Engine/Core/Base.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace Engine
{
    struct JobHandle
    {
        u64 Id = 0;
    };

    constexpr u32 kInvalidJobWorkerIndex = static_cast<u32>(-1);

    struct JobSystemStatistics
    {
        u64 SubmittedJobs = 0;
        u64 CompletedJobs = 0;
        u64 StolenJobs = 0;
    };

    class JobSystem
    {
    public:
        static JobSystem& Get();

        void Initialize(u32 workerCount = 0);
        void Shutdown();

        JobHandle Submit(std::function<void()> function, std::string name = {});
        void WaitIdle();

        bool IsRunning() const;
        u32 GetWorkerCount() const;
        u32 GetCurrentWorkerIndex() const;
        JobSystemStatistics GetStatistics() const;

    private:
        struct Job
        {
            JobHandle Handle;
            std::function<void()> Function;
            std::string Name;
        };

        struct Worker
        {
            std::mutex Mutex;
            std::deque<Job> Jobs;
            std::thread Thread;
        };

        JobSystem() = default;
        ~JobSystem();

        void WorkerLoop(u32 workerIndex);
        bool TryAcquireJob(u32 workerIndex, Job& outJob);

    private:
        mutable std::mutex m_LifecycleMutex;
        mutable std::mutex m_Mutex;
        std::condition_variable m_WorkAvailable;
        std::condition_variable m_Idle;
        std::queue<Job> m_InjectedJobs;
        std::vector<std::unique_ptr<Worker>> m_Workers;
        u64 m_NextJobId = 1;
        u64 m_QueuedJobs = 0;
        u32 m_ActiveJobs = 0;
        JobSystemStatistics m_Statistics;
        bool m_Running = false;
        bool m_AcceptingJobs = false;
    };
}
