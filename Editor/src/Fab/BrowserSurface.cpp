#include "BrowserSurface.h"

namespace Fab
{
    namespace
    {
        bool IsCanonicalAbsolute(const std::filesystem::path& path)
        {
            return !path.empty() && path.is_absolute() && path.has_filename() && path.lexically_normal() == path;
        }

        bool IsInside(const std::filesystem::path& child, const std::filesystem::path& parent)
        {
            const std::filesystem::path relative = child.lexically_relative(parent);
            return !relative.empty() && *relative.begin() != "..";
        }
    }

    bool ValidateBrowserSurfaceConfig(const BrowserSurfaceConfig& config, std::string& error)
    {
        if (!IsCanonicalAbsolute(config.ProfileDir))
        {
            error = "profile directory must be a non-empty, absolute, normalized path without a trailing separator";
            return false;
        }
        if (!IsCanonicalAbsolute(config.DownloadStagingDir))
        {
            error = "download staging directory must be a non-empty, absolute, normalized path without a trailing separator";
            return false;
        }
        if (IsInside(config.ProfileDir, config.DownloadStagingDir) || IsInside(config.DownloadStagingDir, config.ProfileDir))
        {
            error = "profile and download staging directories must be distinct and not nested";
            return false;
        }
        if (!config.HelperPath.empty() && !IsCanonicalAbsolute(config.HelperPath))
        {
            error = "helper path must be absolute and normalized when set";
            return false;
        }
        if (!config.ResourceDir.empty() && !IsCanonicalAbsolute(config.ResourceDir))
        {
            error = "resource directory must be absolute and normalized when set";
            return false;
        }
        if (config.MaxFps < 1 || config.MaxFps > 60)
        {
            error = "frame-rate cap must be between 1 and 60";
            return false;
        }
        error.clear();
        return true;
    }
}
