#include "BrowserSurface.h"

#include <algorithm>

namespace Fab
{
    namespace
    {
        constexpr size_t kMaximumAcceptLanguageTags = 8;
        constexpr std::string_view kFallbackAcceptLanguages = "en-US,en";

        bool IsAsciiLetter(char character)
        {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
        }

        bool IsAsciiDigit(char character)
        {
            return character >= '0' && character <= '9';
        }

        void AppendUnique(std::vector<std::string>& tags, std::string tag)
        {
            if (std::find(tags.begin(), tags.end(), tag) == tags.end())
                tags.push_back(std::move(tag));
        }

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

    std::string BuildAcceptLanguageList(
        std::string_view language, std::string_view lcAll, std::string_view lcMessages, std::string_view lang)
    {
        std::string_view source;
        for (const std::string_view candidate : { language, lcAll, lcMessages, lang })
        {
            if (!candidate.empty())
            {
                source = candidate;
                break;
            }
        }

        std::vector<std::string> tags;
        size_t start = 0;
        while (start <= source.size() && tags.size() < kMaximumAcceptLanguageTags)
        {
            const size_t colon = source.find(':', start);
            std::string_view entry = source.substr(start, colon == std::string_view::npos ? std::string_view::npos : colon - start);
            start = colon == std::string_view::npos ? source.size() + 1 : colon + 1;

            entry = entry.substr(0, entry.find_first_of(".@"));
            const size_t underscore = entry.find('_');
            const std::string_view languagePart = entry.substr(0, underscore);
            const std::string_view regionPart = underscore == std::string_view::npos ? std::string_view() : entry.substr(underscore + 1);
            const bool languageOk = (languagePart.size() == 2 || languagePart.size() == 3)
                && std::all_of(languagePart.begin(), languagePart.end(), IsAsciiLetter);
            const bool regionOk = underscore == std::string_view::npos
                || (regionPart.size() == 2 && std::all_of(regionPart.begin(), regionPart.end(), IsAsciiLetter))
                || (regionPart.size() == 3 && std::all_of(regionPart.begin(), regionPart.end(), IsAsciiDigit));
            if (!languageOk || !regionOk)
                continue;

            std::string base(languagePart);
            for (char& character : base)
                character = static_cast<char>(character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
            if (!regionPart.empty())
            {
                std::string region(regionPart);
                for (char& character : region)
                    character = static_cast<char>(character >= 'a' && character <= 'z' ? character - 'a' + 'A' : character);
                AppendUnique(tags, base + "-" + region);
            }
            AppendUnique(tags, base);
        }
        if (tags.empty())
            return std::string(kFallbackAcceptLanguages);
        if (tags.size() > kMaximumAcceptLanguageTags)
            tags.resize(kMaximumAcceptLanguageTags);

        std::string joined;
        for (const std::string& tag : tags)
        {
            if (!joined.empty())
                joined += ',';
            joined += tag;
        }
        return joined;
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
