#pragma once

#include "Engine/Renderer/Renderer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

namespace Engine
{
    // This artifact intentionally records engine observations only. Unknown
    // presentation/display/input fields stay unknown until an external runner
    // supplies measured evidence; they are never inferred from Present.
    struct FramePacingBenchmarkCondition
    {
        static constexpr u32 SchemaVersion = 1;
        std::string Backend;
        std::string Adapter;
        std::string AdapterStableId;
        double TargetFramesPerSecond = 0.0;
        u32 WarmupFrames = 0;
        ResolvedFramePacingPolicy Policy;
        std::string PresentationMode = "unknown";
        std::string SyncMode = "unknown";
        std::string VrrMode = "unknown";
        std::string TearingMode = "unknown";
    };

    struct FramePacingBenchmarkSummary
    {
        size_t SampleCount = 0;
        double StartToStartP50Milliseconds = 0.0;
        double StartToStartP95Milliseconds = 0.0;
        double StartToStartP99Milliseconds = 0.0;
        double CpuActiveP50Milliseconds = 0.0;
        double CpuActiveP95Milliseconds = 0.0;
        double CpuActiveP99Milliseconds = 0.0;
        double IntentionalWaitP50Milliseconds = 0.0;
        double IntentionalWaitP95Milliseconds = 0.0;
        double IntentionalWaitP99Milliseconds = 0.0;
        // A low is 1000 / worst-tail frametime: 1% uses p99, 0.1% uses p99.9.
        double OnePercentLowFramesPerSecond = 0.0;
        double PointOnePercentLowFramesPerSecond = 0.0;
        size_t DeadlineMissCount = 0;
        double DeadlineOvershootP99Milliseconds = 0.0;
    };

    struct FramePacingBenchmarkSnapshot
    {
        FramePacingBenchmarkCondition Condition;
        std::vector<RendererFrameTiming> Frames;
        FramePacingBenchmarkSummary Summary;
    };

    class FramePacingBenchmarkCapture
    {
    public:
        explicit FramePacingBenchmarkCapture(size_t capacity = 4096) : m_Capacity(std::max<size_t>(1, capacity)) {}

        void Begin(FramePacingBenchmarkCondition condition)
        {
            m_Condition = std::move(condition);
            m_Frames.clear();
            Publish();
        }

        void Record(const RendererFrameTiming& timing)
        {
            if (m_Frames.size() == m_Capacity)
                m_Frames.erase(m_Frames.begin());
            m_Frames.push_back(timing);
            Publish();
        }

        std::shared_ptr<const FramePacingBenchmarkSnapshot> GetSnapshot() const { return m_Snapshot; }

        static double Percentile(const std::vector<double>& values, double percentile)
        {
            if (values.empty()) return 0.0;
            std::vector<double> sorted = values;
            std::sort(sorted.begin(), sorted.end());
            const size_t index = static_cast<size_t>(std::ceil(percentile * static_cast<double>(sorted.size()))) - 1;
            return sorted[std::min(index, sorted.size() - 1)];
        }

        static bool WriteArtifacts(const FramePacingBenchmarkSnapshot& snapshot, const std::filesystem::path& directory, std::string& error)
        {
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (ec) { error = "could not create benchmark directory"; return false; }
            const std::string csv = Csv(snapshot);
            const std::string json = Json(snapshot);
            return WriteAtomically(directory / "frame-pacing-benchmark.csv", csv, error)
                && WriteAtomically(directory / "frame-pacing-benchmark.json", json, error);
        }

    private:
        static std::string Escape(std::string_view value)
        {
            std::string result;
            for (const unsigned char c : value)
            {
                if (c == '"') result += "\\\"";
                else if (c == '\\') result += "\\\\";
                else if (c == '\b') result += "\\b";
                else if (c == '\f') result += "\\f";
                else if (c == '\n') result += "\\n";
                else if (c == '\r') result += "\\r";
                else if (c == '\t') result += "\\t";
                else if (c < 0x20)
                {
                    constexpr char hex[] = "0123456789abcdef";
                    result += "\\u00";
                    result += hex[(c >> 4) & 0xf];
                    result += hex[c & 0xf];
                }
                else result += static_cast<char>(c);
            }
            return result;
        }

        static std::string CsvEscape(std::string_view value)
        {
            std::string result = "\"";
            for (const char c : value) { if (c == '"') result += "\"\""; else result += c; }
            return result + '"';
        }

        static const char* PhaseName(RendererFrameLifecyclePhase phase)
        {
            switch (phase) { case RendererFrameLifecyclePhase::FrameStart: return "FrameStart"; case RendererFrameLifecyclePhase::InputSimulation: return "InputSimulation"; case RendererFrameLifecyclePhase::RenderSubmission: return "RenderSubmission"; case RendererFrameLifecyclePhase::PresentBegin: return "PresentBegin"; case RendererFrameLifecyclePhase::PresentEnd: return "PresentEnd"; case RendererFrameLifecyclePhase::IntentionalPacingWait: return "IntentionalPacingWait"; case RendererFrameLifecyclePhase::GpuCompletionObservation: return "GpuCompletionObservation"; } return "Unknown";
        }
        static const char* WaitName(RendererFrameWaitKind kind)
        {
            switch (kind) { case RendererFrameWaitKind::IntentionalPacing: return "IntentionalPacing"; case RendererFrameWaitKind::MandatoryDxgiFrameLatency: return "MandatoryDxgiFrameLatency"; case RendererFrameWaitKind::MandatoryVulkanAcquire: return "MandatoryVulkanAcquire"; case RendererFrameWaitKind::MandatoryVulkanFence: return "MandatoryVulkanFence"; } return "Unknown";
        }

        static double Overshoot(const RendererFrameWaitTiming& wait)
        {
            return wait.DeadlineMissed ? std::max(0.0, wait.ActualReleaseMilliseconds - wait.RequestedDeadlineMilliseconds) : 0.0;
        }

        static FramePacingBenchmarkSummary Summarize(const std::vector<RendererFrameTiming>& frames)
        {
            std::vector<double> cadence, active, intentional, overshoots;
            size_t misses = 0;
            for (const RendererFrameTiming& frame : frames)
            {
                if (frame.StartToStartMilliseconds > 0.0) cadence.push_back(frame.StartToStartMilliseconds);
                active.push_back(frame.CpuActiveMilliseconds);
                intentional.push_back(frame.IntentionalPacingMilliseconds);
                for (const RendererFrameWaitTiming& wait : frame.Waits)
                    if (wait.Kind == RendererFrameWaitKind::IntentionalPacing && wait.DeadlineMissed)
                    { ++misses; overshoots.push_back(Overshoot(wait)); }
            }
            FramePacingBenchmarkSummary summary;
            summary.SampleCount = cadence.size();
            summary.StartToStartP50Milliseconds = Percentile(cadence, .50);
            summary.StartToStartP95Milliseconds = Percentile(cadence, .95);
            summary.StartToStartP99Milliseconds = Percentile(cadence, .99);
            summary.CpuActiveP50Milliseconds = Percentile(active, .50); summary.CpuActiveP95Milliseconds = Percentile(active, .95); summary.CpuActiveP99Milliseconds = Percentile(active, .99);
            summary.IntentionalWaitP50Milliseconds = Percentile(intentional, .50); summary.IntentionalWaitP95Milliseconds = Percentile(intentional, .95); summary.IntentionalWaitP99Milliseconds = Percentile(intentional, .99);
            const double one = Percentile(cadence, .99), pointOne = Percentile(cadence, .999);
            summary.OnePercentLowFramesPerSecond = one > 0.0 ? 1000.0 / one : 0.0;
            summary.PointOnePercentLowFramesPerSecond = pointOne > 0.0 ? 1000.0 / pointOne : 0.0;
            summary.DeadlineMissCount = misses;
            summary.DeadlineOvershootP99Milliseconds = Percentile(overshoots, .99);
            return summary;
        }

        static std::string Csv(const FramePacingBenchmarkSnapshot& snapshot)
        {
            std::ostringstream out; out << std::setprecision(12);
            out << "schema,backend,adapter,adapterStableId,targetFps,effectiveTargetFps,warmupFrames,projectMode,runtimeOverride,mode,candidate,behavior,presentationMode,sync,vrr,tearing,frame,startToStartMs,cpuTotalMs,cpuActiveMs,intentionalWaitMs,gpuCompleteFrame,lifecycleJson,waitsJson,display,replacementDrop,inputLatency,gpuHeadroom\n";
            for (const RendererFrameTiming& f : snapshot.Frames)
            {
                std::ostringstream lifecycle, waits;
                lifecycle << std::setprecision(12) << '[';
                waits << std::setprecision(12) << '[';
                for (size_t i=0;i<f.Lifecycle.size();++i) { if(i) lifecycle << ','; lifecycle << "{\"phase\":\"" << PhaseName(f.Lifecycle[i].Phase) << "\",\"ms\":" << f.Lifecycle[i].MillisecondsFromFrameStart << '}'; }
                for (size_t i=0;i<f.Waits.size();++i) { if(i) waits << ','; const auto& w=f.Waits[i]; waits << "{\"kind\":\"" << WaitName(w.Kind) << "\",\"applied\":" << (w.Applied?"true":"false") << ",\"ms\":" << w.Milliseconds << ",\"candidate\":\"" << ToString(w.Candidate) << "\",\"deadlineMissed\":" << (w.DeadlineMissed?"true":"false") << ",\"requestedDeadlineMs\":" << w.RequestedDeadlineMilliseconds << ",\"actualReleaseMs\":" << w.ActualReleaseMilliseconds << '}'; }
                lifecycle << ']'; waits << ']';
                out << FramePacingBenchmarkCondition::SchemaVersion << ',' << CsvEscape(snapshot.Condition.Backend) << ',' << CsvEscape(snapshot.Condition.Adapter)
                    << ',' << CsvEscape(snapshot.Condition.AdapterStableId) << ',' << snapshot.Condition.TargetFramesPerSecond << ','
                    << (snapshot.Condition.Policy.SmoothTargetFramesPerSecond ? std::to_string(*snapshot.Condition.Policy.SmoothTargetFramesPerSecond) : "unavailable")
                    << ',' << snapshot.Condition.WarmupFrames << ',' << CsvEscape(ToString(snapshot.Condition.Policy.ProjectMode)) << ','
                    << CsvEscape(ToString(snapshot.Condition.Policy.RuntimeOverride)) << ',' << CsvEscape(ToString(f.FramePacingPolicy.EffectiveMode))
                    << ',' << CsvEscape(ToString(f.FramePacingPolicy.Candidate)) << ',' << CsvEscape(f.FramePacingPolicy.Behavior)
                    << ',' << CsvEscape(snapshot.Condition.PresentationMode) << ',' << CsvEscape(snapshot.Condition.SyncMode)
                    << ',' << CsvEscape(snapshot.Condition.VrrMode) << ',' << CsvEscape(snapshot.Condition.TearingMode) << ',' << f.FrameIndex << ',' << f.StartToStartMilliseconds << ','
                    << f.CpuMilliseconds << ',' << f.CpuActiveMilliseconds << ',' << f.IntentionalPacingMilliseconds << ',' << (f.HasGpuCompletionObservation ? std::to_string(f.LastGpuCompletionObservedFrameIndex) : "unavailable")
                    << ',' << CsvEscape(lifecycle.str()) << ',' << CsvEscape(waits.str()) << ",unavailable,unavailable,unavailable,unavailable\n";
            }
            return out.str();
        }

        static std::string Json(const FramePacingBenchmarkSnapshot& s)
        {
            std::ostringstream out; out << std::setprecision(12);
            out << "{\n  \"schema\":1,\n  \"condition\":{\"backend\":\"" << Escape(s.Condition.Backend) << "\",\"adapter\":\"" << Escape(s.Condition.Adapter)
                << "\",\"adapterStableId\":\"" << Escape(s.Condition.AdapterStableId) << "\",\"targetFps\":" << s.Condition.TargetFramesPerSecond << ",\"warmupFrames\":" << s.Condition.WarmupFrames
                << ",\"projectMode\":\"" << ToString(s.Condition.Policy.ProjectMode) << "\",\"runtimeOverride\":\"" << ToString(s.Condition.Policy.RuntimeOverride)
                << "\",\"mode\":\"" << ToString(s.Condition.Policy.EffectiveMode) << "\",\"candidate\":\"" << ToString(s.Condition.Policy.Candidate)
                << "\",\"behavior\":\"" << Escape(s.Condition.Policy.Behavior) << "\",\"effectiveTargetFps\":"
                << (s.Condition.Policy.SmoothTargetFramesPerSecond ? std::to_string(*s.Condition.Policy.SmoothTargetFramesPerSecond) : "null")
                << ",\"presentationMode\":\"" << Escape(s.Condition.PresentationMode) << "\",\"sync\":\"" << Escape(s.Condition.SyncMode) << "\",\"vrr\":\"" << Escape(s.Condition.VrrMode) << "\",\"tearing\":\"" << Escape(s.Condition.TearingMode)
                << "\"},\n  \"summary\":{\"samples\":" << s.Summary.SampleCount << ",\"p50Ms\":" << s.Summary.StartToStartP50Milliseconds << ",\"p95Ms\":" << s.Summary.StartToStartP95Milliseconds << ",\"p99Ms\":" << s.Summary.StartToStartP99Milliseconds << ",\"cpuActiveP50Ms\":" << s.Summary.CpuActiveP50Milliseconds << ",\"cpuActiveP95Ms\":" << s.Summary.CpuActiveP95Milliseconds << ",\"cpuActiveP99Ms\":" << s.Summary.CpuActiveP99Milliseconds << ",\"intentionalWaitP50Ms\":" << s.Summary.IntentionalWaitP50Milliseconds << ",\"intentionalWaitP95Ms\":" << s.Summary.IntentionalWaitP95Milliseconds << ",\"intentionalWaitP99Ms\":" << s.Summary.IntentionalWaitP99Milliseconds << ",\"onePercentLowFps\":" << s.Summary.OnePercentLowFramesPerSecond << ",\"pointOnePercentLowFps\":" << s.Summary.PointOnePercentLowFramesPerSecond << ",\"deadlineMisses\":" << s.Summary.DeadlineMissCount << ",\"deadlineOvershootP99Ms\":" << s.Summary.DeadlineOvershootP99Milliseconds << "},\n  \"frames\":[\n";
            for (size_t i = 0; i < s.Frames.size(); ++i) { const auto& f = s.Frames[i]; out << "    {\"frame\":" << f.FrameIndex << ",\"startToStartMs\":" << f.StartToStartMilliseconds << ",\"cpuTotalMs\":" << f.CpuMilliseconds << ",\"cpuActiveMs\":" << f.CpuActiveMilliseconds << ",\"intentionalWaitMs\":" << f.IntentionalPacingMilliseconds << ",\"lifecycle\":["; for(size_t e=0;e<f.Lifecycle.size();++e){if(e)out<<',';out<<"{\"phase\":\""<<PhaseName(f.Lifecycle[e].Phase)<<"\",\"ms\":"<<f.Lifecycle[e].MillisecondsFromFrameStart<<'}';} out<<"],\"waits\":["; for(size_t w=0;w<f.Waits.size();++w){if(w)out<<',';const auto& x=f.Waits[w];out<<"{\"kind\":\""<<WaitName(x.Kind)<<"\",\"applied\":"<<(x.Applied?"true":"false")<<",\"ms\":"<<x.Milliseconds<<",\"candidate\":\""<<ToString(x.Candidate)<<"\",\"deadlineMissed\":"<<(x.DeadlineMissed?"true":"false")<<",\"requestedDeadlineMs\":"<<x.RequestedDeadlineMilliseconds<<",\"actualReleaseMs\":"<<x.ActualReleaseMilliseconds<<'}';} out<<"],\"gpuCompletionFrame\":"<<(f.HasGpuCompletionObservation?std::to_string(f.LastGpuCompletionObservedFrameIndex):"null")<<",\"display\":\"unavailable\",\"replacementDrop\":\"unavailable\",\"inputLatency\":\"unavailable\",\"gpuHeadroom\":\"unavailable\"}" << (i + 1 == s.Frames.size() ? "\n" : ",\n"); }
            return out.str() + "  ]\n}\n";
        }

        static bool WriteAtomically(const std::filesystem::path& path, const std::string& content, std::string& error)
        {
            const std::filesystem::path temp = path.string() + ".tmp";
            { std::ofstream out(temp, std::ios::binary | std::ios::trunc); out << content; if (!out) { error = "could not write benchmark artifact"; return false; } }
            std::error_code ec; std::filesystem::rename(temp, path, ec);
            if (ec) { std::filesystem::remove(path, ec); ec.clear(); std::filesystem::rename(temp, path, ec); }
            if (ec) { error = "could not publish benchmark artifact"; return false; }
            return true;
        }

        void Publish()
        {
            auto snapshot = std::make_shared<FramePacingBenchmarkSnapshot>();
            snapshot->Condition = m_Condition; snapshot->Frames = m_Frames; snapshot->Summary = Summarize(m_Frames); m_Snapshot = std::move(snapshot);
        }

        size_t m_Capacity;
        FramePacingBenchmarkCondition m_Condition;
        std::vector<RendererFrameTiming> m_Frames;
        std::shared_ptr<const FramePacingBenchmarkSnapshot> m_Snapshot;
    };
}
