#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Jobs/JobSystem.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Engine
{
    using FrameTaskId = u32;
    constexpr FrameTaskId kInvalidFrameTaskId = static_cast<FrameTaskId>(-1);

    enum class FrameTaskStatus
    {
        Pending,
        Running,
        Succeeded,
        Failed,
        Skipped
    };

    enum class FrameTaskExecutionMode
    {
        Parallel,
        DeterministicSingleThread
    };

    enum class FrameTaskLane
    {
        Worker,
        CallingThread
    };

    enum class FrameTaskProfilePhase
    {
        Begin,
        End
    };

    struct FrameTaskProfileEvent
    {
        FrameTaskId Task = kInvalidFrameTaskId;
        u64 FrameIndex = 0;
        std::string Name;
        FrameTaskProfilePhase Phase = FrameTaskProfilePhase::Begin;
        FrameTaskStatus Status = FrameTaskStatus::Pending;
        std::thread::id Thread;
        u32 WorkerIndex = kInvalidJobWorkerIndex;
        double DurationMicroseconds = 0.0;
    };

    using FrameTaskProfileHook = std::function<void(const FrameTaskProfileEvent&)>;

    class FramePublicationState
    {
    public:
        virtual ~FramePublicationState() = default;

    private:
        virtual void Commit() = 0;
        virtual void Abort() = 0;

        friend class FrameTaskGraph;
    };

    struct FrameTaskExecutionOptions
    {
        FrameTaskExecutionMode Mode = FrameTaskExecutionMode::Parallel;
        u64 FrameIndex = 0;
        FrameTaskProfileHook ProfileHook;
    };

    struct FrameTaskDescription
    {
        std::string Name;
        std::function<void()> Execute;
        std::shared_ptr<FramePublicationState> Publication;
        std::vector<FrameTaskId> Dependencies;
        FrameTaskLane Lane = FrameTaskLane::Worker;
    };

    struct FrameTaskGraphResult
    {
        std::vector<FrameTaskStatus> TaskStatuses;
        std::vector<std::string> TaskErrors;
        std::string GraphError;

        bool Succeeded() const;
    };

    template <typename T>
    class FramePublication
    {
    public:
        FramePublication()
            : m_State(std::make_shared<State>())
        {
        }

        void Stage(T value)
        {
            std::scoped_lock lock(m_State->Mutex);
            m_State->Staged = std::move(value);
        }

        std::shared_ptr<const T> Read() const
        {
            std::scoped_lock lock(m_State->Mutex);
            return m_State->Published;
        }

        std::shared_ptr<FramePublicationState> GetState() const { return m_State; }

    private:
        struct State final : FramePublicationState
        {
        private:
            friend class FramePublication<T>;

            void Commit() override
            {
                std::scoped_lock lock(Mutex);
                if (!Staged)
                    throw std::logic_error("frame publication has no staged value");

                std::shared_ptr<const T> published = std::make_shared<const T>(std::move(*Staged));
                Published = std::move(published);
                Staged.reset();
            }

            void Abort() override
            {
                std::scoped_lock lock(Mutex);
                Staged.reset();
            }

            mutable std::mutex Mutex;
            std::optional<T> Staged;
            std::shared_ptr<const T> Published;
        };

        std::shared_ptr<State> m_State;
    };

    class FrameTaskGraph
    {
    public:
        FrameTaskId AddTask(FrameTaskDescription description);
        FrameTaskGraphResult Execute(JobSystem& jobSystem, const FrameTaskExecutionOptions& options = {}) const;

        size_t GetTaskCount() const { return m_Tasks.size(); }

    private:
        std::vector<FrameTaskDescription> m_Tasks;
    };
}
