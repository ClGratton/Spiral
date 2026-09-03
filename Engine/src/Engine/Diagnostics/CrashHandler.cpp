#include "Engine/Diagnostics/CrashHandler.h"

#include "Engine/Core/Log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#if defined(GE_PLATFORM_WINDOWS)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <Windows.h>
#else
    #include <execinfo.h>
    #include <fcntl.h>
    #include <signal.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace Engine
{
    namespace
    {
        std::mutex s_ReportMutex;
        std::string s_ApplicationName = "Spiral";
        std::atomic_bool s_Installed = false;
        std::uint32_t s_ReportSequence = 0;

#if !defined(GE_PLATFORM_WINDOWS)
        std::atomic<int> s_SignalReceiptFile = -1;
        std::atomic_flag s_SignalReceiptClaimed = ATOMIC_FLAG_INIT;
        std::filesystem::path s_SignalReceiptPath;
        static_assert(std::atomic<int>::is_always_lock_free,
            "The POSIX fatal-signal callback requires a lock-free descriptor publication.");

        constexpr char s_AbortReceipt[] =
            "SpiralFatalSignalReceiptV1 signal=SIGABRT disposition=reset-reraise enrichment=none\n";
        constexpr char s_FloatingPointReceipt[] =
            "SpiralFatalSignalReceiptV1 signal=SIGFPE disposition=reset-reraise enrichment=none\n";
        constexpr char s_IllegalInstructionReceipt[] =
            "SpiralFatalSignalReceiptV1 signal=SIGILL disposition=reset-reraise enrichment=none\n";
        constexpr char s_SegmentationViolationReceipt[] =
            "SpiralFatalSignalReceiptV1 signal=SIGSEGV disposition=reset-reraise enrichment=none\n";
        constexpr char s_UnknownSignalReceipt[] =
            "SpiralFatalSignalReceiptV1 signal=UNKNOWN disposition=reset-reraise enrichment=none\n";
        constexpr std::array<int, 4> s_PosixFatalSignals { SIGABRT, SIGFPE, SIGILL, SIGSEGV };

        struct SignalReceipt
        {
            const char* Data;
            std::size_t Size;
        };

        SignalReceipt GetSignalReceipt(int signal) noexcept
        {
            switch (signal)
            {
                case SIGABRT: return { s_AbortReceipt, sizeof(s_AbortReceipt) - 1 };
                case SIGFPE: return { s_FloatingPointReceipt, sizeof(s_FloatingPointReceipt) - 1 };
                case SIGILL: return { s_IllegalInstructionReceipt, sizeof(s_IllegalInstructionReceipt) - 1 };
                case SIGSEGV: return { s_SegmentationViolationReceipt, sizeof(s_SegmentationViolationReceipt) - 1 };
                default: return { s_UnknownSignalReceipt, sizeof(s_UnknownSignalReceipt) - 1 };
            }
        }

        void HandlePosixFatalSignal(int signal) noexcept
        {
            if (!s_SignalReceiptClaimed.test_and_set(std::memory_order_relaxed))
            {
                const int receiptFile = s_SignalReceiptFile.load(std::memory_order_relaxed);
                if (receiptFile >= 0)
                {
                    const SignalReceipt receipt = GetSignalReceipt(signal);
                    std::size_t written = 0;
                    for (int attempt = 0; attempt < 4 && written < receipt.Size; ++attempt)
                    {
                        const ssize_t result = ::write(receiptFile, receipt.Data + written, receipt.Size - written);
                        if (result <= 0)
                            break;
                        written += static_cast<std::size_t>(result);
                    }
                }
            }

            if (::kill(::getpid(), signal) != 0)
                ::_exit(128 + signal);
        }
#endif

        std::string SanitizeFilePart(std::string_view value)
        {
            std::string sanitized;
            sanitized.reserve(value.size());

            for (char character : value)
            {
                const bool isAlphaNumeric = (character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9');

                sanitized.push_back(isAlphaNumeric ? character : '-');
            }

            while (!sanitized.empty() && sanitized.back() == '-')
                sanitized.pop_back();

            return sanitized.empty() ? "Spiral" : sanitized;
        }

        std::string FormatTimestampForFile(std::chrono::system_clock::time_point timePoint)
        {
            const std::time_t time = std::chrono::system_clock::to_time_t(timePoint);
            std::tm localTime = {};

#if defined(_WIN32)
            localtime_s(&localTime, &time);
#else
            localtime_r(&time, &localTime);
#endif

            std::ostringstream stream;
            stream << std::put_time(&localTime, "%Y%m%d-%H%M%S");
            return stream.str();
        }

        std::string FormatTimestampForReport(std::chrono::system_clock::time_point timePoint)
        {
            const std::time_t time = std::chrono::system_clock::to_time_t(timePoint);
            std::tm localTime = {};

#if defined(_WIN32)
            localtime_s(&localTime, &time);
#else
            localtime_r(&time, &localTime);
#endif

            std::ostringstream stream;
            stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S %Z");
            return stream.str();
        }

        std::filesystem::path BuildReportPath()
        {
            const auto now = std::chrono::system_clock::now();
            const std::string fileName = SanitizeFilePart(s_ApplicationName) + "-" + FormatTimestampForFile(now) + "-" + std::to_string(++s_ReportSequence) + ".txt";
            return std::filesystem::path("output") / "crashes" / fileName;
        }

#if defined(GE_PLATFORM_WINDOWS)
        const char* SignalName(int signal)
        {
            switch (signal)
            {
                case SIGABRT: return "SIGABRT";
                case SIGFPE: return "SIGFPE";
                case SIGILL: return "SIGILL";
                case SIGINT: return "SIGINT";
                case SIGSEGV: return "SIGSEGV";
                case SIGTERM: return "SIGTERM";
            }

            return "Unknown signal";
        }
#endif

        std::string CaptureStackTrace()
        {
            std::ostringstream stream;
            stream << "Stack trace:" << '\n';

#if defined(GE_PLATFORM_WINDOWS)
            std::array<void*, 64> frames = {};
            const USHORT frameCount = CaptureStackBackTrace(0, static_cast<DWORD>(frames.size()), frames.data(), nullptr);

            if (frameCount == 0)
            {
                stream << "  <unavailable>" << '\n';
                return stream.str();
            }

            for (USHORT index = 0; index < frameCount; ++index)
                stream << "  #" << index << " " << frames[index] << '\n';
#else
            std::array<void*, 64> frames = {};
            const int frameCount = backtrace(frames.data(), static_cast<int>(frames.size()));
            char** symbols = backtrace_symbols(frames.data(), frameCount);

            if (!symbols)
            {
                stream << "  <unavailable>" << '\n';
                return stream.str();
            }

            for (int index = 0; index < frameCount; ++index)
                stream << "  #" << index << " " << symbols[index] << '\n';

            std::free(symbols);
#endif

            return stream.str();
        }

        std::string CurrentThreadId()
        {
            std::ostringstream stream;
            stream << std::this_thread::get_id();
            return stream.str();
        }

        void HandleTerminate()
        {
            std::string details = "std::terminate called";

            if (std::exception_ptr exception = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(exception);
                }
                catch (const std::exception& caught)
                {
                    details = caught.what();
                }
                catch (...)
                {
                    details = "Non-standard exception active during std::terminate";
                }
            }

            const auto path = CrashHandler::WriteReport("Fatal terminate", details);
            Log::Error("Fatal terminate. Crash report: ", path.string());
            std::_Exit(1);
        }

#if defined(GE_PLATFORM_WINDOWS)
        void HandleSignal(int signal)
        {
            std::ostringstream details;
            details << SignalName(signal) << " (" << signal << ")";

            const auto path = CrashHandler::WriteReport("Fatal signal", details.str());
            Log::Error("Fatal signal. Crash report: ", path.string());

            _exit(128 + signal);
        }
#endif

#if defined(GE_PLATFORM_WINDOWS)
        LONG WINAPI HandleWindowsException(EXCEPTION_POINTERS* exceptionPointers)
        {
            std::ostringstream details;
            details << "Exception code: 0x" << std::hex << exceptionPointers->ExceptionRecord->ExceptionCode << '\n';
            details << "Exception address: " << exceptionPointers->ExceptionRecord->ExceptionAddress;

            const auto path = CrashHandler::WriteReport("Unhandled Windows exception", details.str());
            Log::Error("Unhandled Windows exception. Crash report: ", path.string());
            return EXCEPTION_EXECUTE_HANDLER;
        }
#endif
    }

    void CrashHandler::Install()
    {
        bool expected = false;
        if (!s_Installed.compare_exchange_strong(expected, true))
            return;

        std::set_terminate(HandleTerminate);

#if defined(GE_PLATFORM_WINDOWS)
        std::signal(SIGABRT, HandleSignal);
        std::signal(SIGFPE, HandleSignal);
        std::signal(SIGILL, HandleSignal);
        std::signal(SIGSEGV, HandleSignal);
        std::signal(SIGTERM, HandleSignal);
        SetUnhandledExceptionFilter(HandleWindowsException);
#else
        std::error_code directoryError;
        const std::filesystem::path receiptDirectory = std::filesystem::path("output") / "crashes";
        std::filesystem::create_directories(receiptDirectory, directoryError);

        if (!directoryError)
        {
            const auto now = std::chrono::system_clock::now();
            for (std::uint32_t attempt = 1; attempt <= 32; ++attempt)
            {
                const std::string fileName = "Spiral-" + FormatTimestampForFile(now) + "-" +
                    std::to_string(getpid()) + "-" + std::to_string(attempt) + ".receipt";
                const std::filesystem::path path = receiptDirectory / fileName;
                const std::string nativePath = path.string();
                const int receiptFile = ::open(nativePath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                    S_IRUSR | S_IWUSR);
                if (receiptFile < 0)
                    continue;

                if (::fchmod(receiptFile, S_IRUSR | S_IWUSR) != 0)
                {
                    ::close(receiptFile);
                    std::error_code removeError;
                    std::filesystem::remove(path, removeError);
                    continue;
                }

                s_SignalReceiptPath = path;
                s_SignalReceiptFile.store(receiptFile, std::memory_order_release);
                break;
            }
        }

        struct sigaction action = {};
        action.sa_handler = HandlePosixFatalSignal;
        sigemptyset(&action.sa_mask);
        sigaddset(&action.sa_mask, SIGABRT);
        sigaddset(&action.sa_mask, SIGFPE);
        sigaddset(&action.sa_mask, SIGILL);
        sigaddset(&action.sa_mask, SIGSEGV);
        action.sa_flags = SA_RESETHAND | SA_NODEFER;

        bool handlersInstalled = true;
        for (const int signal : s_PosixFatalSignals)
            handlersInstalled = sigaction(signal, &action, nullptr) == 0 && handlersInstalled;

        if (s_SignalReceiptFile.load(std::memory_order_acquire) < 0)
            Log::Warn("POSIX fatal-signal receipt could not be armed; default signal termination remains available");
        if (!handlersInstalled)
            Log::Warn("One or more POSIX fatal-signal handlers could not be installed");
#endif

        Log::Info("Crash handler installed");
    }

    void CrashHandler::Shutdown()
    {
#if !defined(GE_PLATFORM_WINDOWS)
        struct sigaction defaultAction = {};
        defaultAction.sa_handler = SIG_DFL;
        sigemptyset(&defaultAction.sa_mask);

        bool handlersRestored = true;
        for (const int signal : s_PosixFatalSignals)
            handlersRestored = sigaction(signal, &defaultAction, nullptr) == 0 && handlersRestored;
        if (!handlersRestored)
        {
            Log::Warn("One or more POSIX fatal-signal dispositions could not be restored; receipt remains armed");
            return;
        }

        const int receiptFile = s_SignalReceiptFile.exchange(-1, std::memory_order_acq_rel);
        if (receiptFile >= 0)
            ::close(receiptFile);

        if (!s_SignalReceiptPath.empty())
        {
            std::error_code error;
            std::filesystem::remove(s_SignalReceiptPath, error);
            s_SignalReceiptPath.clear();
        }
#endif
    }

    void CrashHandler::SetApplicationName(std::string_view applicationName)
    {
        std::scoped_lock lock(s_ReportMutex);
        s_ApplicationName = applicationName.empty() ? "Spiral" : std::string(applicationName);
    }

    std::filesystem::path CrashHandler::WriteReport(std::string_view reason, std::string_view details)
    {
        std::scoped_lock lock(s_ReportMutex);

        const std::filesystem::path path = BuildReportPath();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);

        std::ofstream output(path, std::ios::out | std::ios::trunc);
        if (!output)
            return {};

        output << BuildReport(reason, details);
        return path;
    }

    std::string CrashHandler::BuildReport(std::string_view reason, std::string_view details)
    {
        const auto now = std::chrono::system_clock::now();

        std::ostringstream stream;
        stream << "Spiral crash report" << '\n';
        stream << "===================" << '\n';
        stream << "Application: " << s_ApplicationName << '\n';
        stream << "Reason: " << reason << '\n';
        stream << "Time: " << FormatTimestampForReport(now) << '\n';
        stream << "Thread: " << CurrentThreadId() << '\n';

#if defined(GE_PLATFORM_WINDOWS)
        stream << "Platform: Windows" << '\n';
        stream << "Process ID: " << GetCurrentProcessId() << '\n';
        stream << "Thread ID: " << GetCurrentThreadId() << '\n';
#elif defined(GE_PLATFORM_MACOS)
        stream << "Platform: macOS" << '\n';
        stream << "Process ID: " << getpid() << '\n';
#elif defined(GE_PLATFORM_LINUX)
        stream << "Platform: Linux" << '\n';
        stream << "Process ID: " << getpid() << '\n';
#else
        stream << "Platform: Unknown" << '\n';
#endif

#if defined(GE_DEBUG)
        stream << "Configuration: Debug" << '\n';
#elif defined(GE_RELEASE)
        stream << "Configuration: Release" << '\n';
#elif defined(GE_DIST)
        stream << "Configuration: Dist" << '\n';
#else
        stream << "Configuration: Unknown" << '\n';
#endif

        if (!details.empty())
            stream << '\n' << "Details:" << '\n' << details << '\n';

        stream << '\n' << CaptureStackTrace();
        return stream.str();
    }
}
