#pragma once

#include "Engine/Core/Base.h"

#include <condition_variable>
#include <functional>
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

    private:
        struct Job
        {
            JobHandle Handle;
            std::function<void()> Function;
            std::string Name;
        };

        JobSystem() = default;
        ~JobSystem();

        void WorkerLoop(u32 workerIndex);

    private:
        mutable std::mutex m_Mutex;
        std::condition_variable m_WorkAvailable;
        std::condition_variable m_Idle;
        std::queue<Job> m_Jobs;
        std::vector<std::thread> m_Workers;
        u64 m_NextJobId = 1;
        u32 m_ActiveJobs = 0;
        bool m_Running = false;
        bool m_AcceptingJobs = false;
    };
}
