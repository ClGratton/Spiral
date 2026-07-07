#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Engine
{
    class CrashHandler
    {
    public:
        static void Install();
        static void SetApplicationName(std::string_view applicationName);
        static std::filesystem::path WriteReport(std::string_view reason, std::string_view details = {});

    private:
        static std::string BuildReport(std::string_view reason, std::string_view details);
    };
}
