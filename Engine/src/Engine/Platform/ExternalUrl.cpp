#include "Engine/Platform/ExternalUrl.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <string>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
    #include <shellapi.h>
#elif defined(__linux__) || defined(__APPLE__)
    #include <fcntl.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace Engine
{
    namespace
    {
        constexpr std::string_view kHttpsScheme = "https://";
        constexpr size_t kMaximumExternalUrlBytes = 2048;

        bool IsAsciiHostCharacter(char character)
        {
            const unsigned char value = static_cast<unsigned char>(character);
            return std::isalnum(value) != 0 || character == '-' || character == '.';
        }

        std::string LowerAscii(std::string_view value)
        {
            std::string result(value);
            std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
            return result;
        }

#if defined(__linux__) || defined(__APPLE__)
        bool WriteExecError(int descriptor, int error)
        {
            const char* bytes = reinterpret_cast<const char*>(&error);
            size_t remaining = sizeof(error);
            while (remaining > 0)
            {
                const ssize_t written = ::write(descriptor, bytes, remaining);
                if (written > 0)
                {
                    bytes += written;
                    remaining -= static_cast<size_t>(written);
                    continue;
                }
                if (written < 0 && errno == EINTR)
                    continue;
                return false;
            }
            return true;
        }

        bool LaunchDetached(const char* executable, const std::string& url, std::string& outError)
        {
            int errorPipe[2] { -1, -1 };
            if (::pipe(errorPipe) != 0)
            {
                outError = "could not create the external-navigation status pipe";
                return false;
            }
            if (::fcntl(errorPipe[1], F_SETFD, FD_CLOEXEC) != 0)
            {
                const int pipeError = errno;
                ::close(errorPipe[0]);
                ::close(errorPipe[1]);
                outError = "could not protect the external-navigation status pipe: "
                    + std::string(std::strerror(pipeError));
                return false;
            }

            const pid_t supervisor = ::fork();
            if (supervisor < 0)
            {
                const int forkError = errno;
                ::close(errorPipe[0]);
                ::close(errorPipe[1]);
                outError = "could not create the external-navigation process: "
                    + std::string(std::strerror(forkError));
                return false;
            }

            if (supervisor == 0)
            {
                ::close(errorPipe[0]);
                if (::setsid() < 0)
                {
                    const int launchError = errno;
                    WriteExecError(errorPipe[1], launchError);
                    _exit(126);
                }

                const pid_t detached = ::fork();
                if (detached < 0)
                {
                    const int launchError = errno;
                    WriteExecError(errorPipe[1], launchError);
                    _exit(126);
                }
                if (detached > 0)
                {
                    ::close(errorPipe[1]);
                    _exit(0);
                }

                ::execl(executable, executable, url.c_str(), static_cast<char*>(nullptr));
                const int launchError = errno;
                WriteExecError(errorPipe[1], launchError);
                _exit(127);
            }

            ::close(errorPipe[1]);
            int supervisorStatus = 0;
            while (::waitpid(supervisor, &supervisorStatus, 0) < 0)
            {
                if (errno == EINTR)
                    continue;
                const int waitError = errno;
                ::close(errorPipe[0]);
                outError = "could not retire the external-navigation supervisor: "
                    + std::string(std::strerror(waitError));
                return false;
            }

            int launchError = 0;
            size_t received = 0;
            char* bytes = reinterpret_cast<char*>(&launchError);
            while (received < sizeof(launchError))
            {
                const ssize_t readCount = ::read(
                    errorPipe[0], bytes + received, sizeof(launchError) - received);
                if (readCount > 0)
                {
                    received += static_cast<size_t>(readCount);
                    continue;
                }
                if (readCount < 0 && errno == EINTR)
                    continue;
                break;
            }
            ::close(errorPipe[0]);

            if (received != 0)
            {
                outError = received == sizeof(launchError)
                    ? "could not launch the system browser: " + std::string(std::strerror(launchError))
                    : "could not read the external-navigation launch result";
                return false;
            }
            if (!WIFEXITED(supervisorStatus) || WEXITSTATUS(supervisorStatus) != 0)
            {
                outError = "external-navigation supervisor failed before browser launch";
                return false;
            }

            outError.clear();
            return true;
        }
#endif
    }

    bool IsAllowedExternalHttpsUrl(std::string_view url, std::string_view requiredHost)
    {
        if (url.size() <= kHttpsScheme.size() || url.size() > kMaximumExternalUrlBytes
            || url.substr(0, kHttpsScheme.size()) != kHttpsScheme || requiredHost.empty())
        {
            return false;
        }
        for (char character : url)
        {
            const unsigned char value = static_cast<unsigned char>(character);
            if (value < 0x21 || value > 0x7e || character == '\\')
                return false;
        }
        if (!std::all_of(requiredHost.begin(), requiredHost.end(), IsAsciiHostCharacter)
            || requiredHost.front() == '.' || requiredHost.back() == '.'
            || requiredHost.find("..") != std::string_view::npos)
        {
            return false;
        }

        const size_t authorityStart = kHttpsScheme.size();
        const size_t authorityEnd = url.find_first_of("/?#", authorityStart);
        const std::string_view authority = url.substr(
            authorityStart,
            authorityEnd == std::string_view::npos ? std::string_view::npos : authorityEnd - authorityStart);
        if (authority.empty() || authority.find('@') != std::string_view::npos
            || authority.find(':') != std::string_view::npos
            || !std::all_of(authority.begin(), authority.end(), IsAsciiHostCharacter))
        {
            return false;
        }

        return LowerAscii(authority) == LowerAscii(requiredHost);
    }

    bool OpenExternalHttpsUrl(
        std::string_view url, std::string_view requiredHost, std::string& outError)
    {
        if (!IsAllowedExternalHttpsUrl(url, requiredHost))
        {
            outError = "external navigation requires an HTTPS URL on the declared host";
            return false;
        }
        const std::string stableUrl(url);

#if defined(_WIN32)
        const int wideCount = ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, stableUrl.c_str(), -1, nullptr, 0);
        if (wideCount <= 0)
        {
            outError = "external-navigation URL is not valid UTF-8";
            return false;
        }
        std::wstring wideUrl(static_cast<size_t>(wideCount), L'\0');
        if (::MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, stableUrl.c_str(), -1,
                wideUrl.data(), wideCount) != wideCount)
        {
            outError = "could not convert the external-navigation URL";
            return false;
        }
        const HINSTANCE result = ::ShellExecuteW(
            nullptr, L"open", wideUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
        {
            outError = "the system browser rejected external navigation";
            return false;
        }
        outError.clear();
        return true;
#elif defined(__linux__)
        return LaunchDetached("/usr/bin/xdg-open", stableUrl, outError);
#elif defined(__APPLE__)
        return LaunchDetached("/usr/bin/open", stableUrl, outError);
#else
        outError = "external navigation is unsupported on this platform";
        return false;
#endif
    }
}
