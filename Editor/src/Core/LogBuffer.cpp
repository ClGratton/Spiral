#include "LogBuffer.h"

#include "TextLimits.h"

#include <algorithm>

namespace SpiralEditor
{
    namespace
    {
        char FoldAscii(char c)
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle)
        {
            const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                [](char a, char b) { return FoldAscii(a) == FoldAscii(b); });
            return it != haystack.end();
        }
    }

    std::string_view LogSeverityName(LogSeverity severity)
    {
        switch (severity)
        {
        case LogSeverity::Trace: return "Trace";
        case LogSeverity::Info: return "Info";
        case LogSeverity::Warn: return "Warn";
        case LogSeverity::Error: return "Error";
        }
        return "Unknown";
    }

    LogBuffer::LogBuffer(size_t capacity)
        : m_Capacity(std::clamp<size_t>(capacity, 1, kMaximumCapacity))
    {
        m_Ring.resize(m_Capacity);
    }

    Engine::u64 LogBuffer::Append(const LogRecord& record)
    {
        auto entry = std::make_shared<LogEntry>();
        const LogSeverity severity = static_cast<size_t>(record.Severity) < kLogSeverityCount ? record.Severity : LogSeverity::Error;
        entry->Severity = severity;
        entry->TimestampNanoseconds = record.TimestampNanoseconds;
        entry->Truncated = record.Source.size() > kMaximumLogSourceBytes || record.Message.size() > kMaximumLogMessageBytes;
        entry->Source.assign(TruncateUtf8(record.Source, kMaximumLogSourceBytes));
        entry->Message.assign(TruncateUtf8(record.Message, kMaximumLogMessageBytes));

        std::shared_ptr<const LogEntry> evicted;
        Engine::u64 id = 0;
        {
            std::lock_guard lock(m_Mutex);
            id = m_NextId++;
            entry->Id = id;
            if (m_Count == m_Capacity)
            {
                evicted = std::move(m_Ring[m_Head]);
                --m_Retained[static_cast<size_t>(evicted->Severity)];
                m_Ring[m_Head] = std::move(entry);
                m_Head = (m_Head + 1) % m_Capacity;
                ++m_Evicted;
            }
            else
            {
                m_Ring[(m_Head + m_Count) % m_Capacity] = std::move(entry);
                ++m_Count;
            }
            ++m_Retained[static_cast<size_t>(severity)];
            m_Revision.fetch_add(1, std::memory_order_release);
        }
        return id;
    }

    void LogBuffer::Clear()
    {
        std::vector<std::shared_ptr<const LogEntry>> released;
        {
            std::lock_guard lock(m_Mutex);
            released.reserve(m_Count);
            for (size_t i = 0; i < m_Count; ++i)
                released.push_back(std::move(m_Ring[(m_Head + i) % m_Capacity]));
            m_Head = 0;
            m_Count = 0;
            m_Retained = {};
            m_Revision.fetch_add(1, std::memory_order_release);
        }
    }

    bool LogBuffer::Matches(const LogEntry& entry, const LogFilter& filter)
    {
        if ((filter.SeverityMask & LogSeverityBit(entry.Severity)) == 0)
            return false;
        return filter.Text.empty() || ContainsIgnoreCase(entry.Message, filter.Text) || ContainsIgnoreCase(entry.Source, filter.Text);
    }

    LogSnapshot LogBuffer::Snapshot(const LogFilter& filter) const
    {
        LogSnapshot snapshot;
        {
            std::lock_guard lock(m_Mutex);
            snapshot.Entries.reserve(m_Count);
            for (size_t i = 0; i < m_Count; ++i)
                snapshot.Entries.push_back(m_Ring[(m_Head + i) % m_Capacity]);
            snapshot.LastId = m_NextId - 1;
            snapshot.Evicted = m_Evicted;
            snapshot.RetainedBySeverity = m_Retained;
        }
        std::erase_if(snapshot.Entries, [&](const std::shared_ptr<const LogEntry>& entry) { return !Matches(*entry, filter); });
        return snapshot;
    }
}
