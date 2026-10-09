#pragma once

#include "Engine/Core/Base.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace SpiralEditor
{
    // Same order as Engine::Log::Level so a sink adapter converts by value.
    enum class LogSeverity : Engine::u8
    {
        Trace,
        Info,
        Warn,
        Error
    };

    inline constexpr size_t kLogSeverityCount = 4;

    constexpr Engine::u8 LogSeverityBit(LogSeverity severity)
    {
        return static_cast<Engine::u8>(1u << static_cast<Engine::u8>(severity));
    }

    inline constexpr Engine::u8 kAllLogSeverities = 0xF;

    std::string_view LogSeverityName(LogSeverity severity);

    // What a producer hands in. The buffer keeps its own copy; nothing in the record has to outlive Append.
    struct LogRecord
    {
        LogSeverity Severity = LogSeverity::Info;
        std::string_view Source;  // subsystem tag such as "Editor" or "Renderer"; truncated at kMaximumLogSourceBytes
        std::string_view Message; // truncated at kMaximumLogMessageBytes
        Engine::u64 TimestampNanoseconds = 0; // producer's clock, opaque to the buffer
    };

    inline constexpr size_t kMaximumLogSourceBytes = 64;
    inline constexpr size_t kMaximumLogMessageBytes = 4096;

    // Immutable once published, so a snapshot stays valid after its entry is evicted.
    struct LogEntry
    {
        Engine::u64 Id = 0; // 1-based, strictly increasing, never reused; Clear does not reset it
        LogSeverity Severity = LogSeverity::Info;
        std::string Source;
        std::string Message;
        Engine::u64 TimestampNanoseconds = 0;
        bool Truncated = false; // the message or source was cut at its limit
    };

    struct LogFilter
    {
        Engine::u8 SeverityMask = kAllLogSeverities;
        // Case-insensitive ASCII substring of Source or Message; empty matches everything.
        std::string Text;
    };

    struct LogSnapshot
    {
        std::vector<std::shared_ptr<const LogEntry>> Entries; // ascending Id, newest last
        Engine::u64 LastId = 0;                                // newest Id ever appended, whatever the filter
        Engine::u64 Evicted = 0;                               // entries pushed out by capacity since construction
        std::array<Engine::u64, kLogSeverityCount> RetainedBySeverity {}; // retained, before filtering
    };

    // Bounded ring of structured log entries.
    //
    // Threading: any thread may Append and any thread may read. A single std::mutex guards the ring; an
    // append allocates one entry outside the lock and holds the lock only for a pointer move, and a
    // snapshot copies shared pointers, not strings. A lock-free ring of variable-size entries was rejected
    // on a measurement (x86-64, -O2, 80-byte messages): an append costs about 60 ns uncontended and about
    // 150 ns wall per append with eight producers appending in a tight loop, far below the Engine log's own
    // formatting and write cost, and a 10,000-entry snapshot costs about 80 us. Append never calls out of
    // the buffer while locked and must not be called from code that holds a lock which a reader could wait on.
    class LogBuffer
    {
    public:
        static constexpr size_t kMaximumCapacity = 1u << 20;

        // Capacity is clamped to [1, kMaximumCapacity].
        explicit LogBuffer(size_t capacity = 10000);

        // Returns the id assigned to the entry.
        Engine::u64 Append(const LogRecord& record);

        // Drops every retained entry. Ids keep counting up and the eviction total is unchanged.
        void Clear();

        // Entries matching the filter, plus counters, all from one consistent moment.
        LogSnapshot Snapshot(const LogFilter& filter = {}) const;

        // Cheap change detector for per-frame polling: increases on every Append and Clear.
        Engine::u64 Revision() const { return m_Revision.load(std::memory_order_acquire); }

        size_t Capacity() const { return m_Capacity; }

        // The filter predicate, exposed so the console can reuse it on a retained snapshot.
        static bool Matches(const LogEntry& entry, const LogFilter& filter);

    private:
        const size_t m_Capacity;
        mutable std::mutex m_Mutex;
        std::vector<std::shared_ptr<const LogEntry>> m_Ring;
        size_t m_Head = 0;  // index of the oldest entry once the ring is full
        size_t m_Count = 0; // retained entries
        Engine::u64 m_NextId = 1;
        Engine::u64 m_Evicted = 0;
        std::array<Engine::u64, kLogSeverityCount> m_Retained {};
        std::atomic<Engine::u64> m_Revision { 0 };
    };
}
