#include "BrowserDownloadPolicy.h"

#include <algorithm>
#include <cstdio>

namespace Fab
{
    namespace
    {
        constexpr std::string_view kFallbackName = "download";
        constexpr size_t kMaximumExtensionBytes = 16;
        constexpr size_t kMinimumNameBudget = 8;

        bool IsKeptCharacter(char character)
        {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9') || character == '.' || character == '-'
                || character == '_';
        }

        std::string UpperAscii(std::string_view text)
        {
            std::string result(text);
            for (char& character : result)
            {
                if (character >= 'a' && character <= 'z')
                    character = static_cast<char>(character - 'a' + 'A');
            }
            return result;
        }

        bool IsWindowsReservedStem(std::string_view stem)
        {
            const std::string upper = UpperAscii(stem);
            if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL")
                return true;
            return upper.size() == 4 && upper[3] >= '1' && upper[3] <= '9'
                && (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0);
        }

        bool IsPlainSegment(const std::string& segment)
        {
            return !segment.empty() && segment != "." && segment != ".."
                && std::all_of(segment.begin(), segment.end(), IsKeptCharacter);
        }

        std::string HexSegment(Engine::u64 value, int digits)
        {
            char buffer[17] {};
            std::snprintf(buffer, sizeof(buffer), "%0*llx", digits, static_cast<unsigned long long>(value));
            return buffer;
        }
    }

    std::string SanitizeDownloadFileName(std::string_view suggested, size_t maximumBytes)
    {
        maximumBytes = std::max(maximumBytes, kMinimumNameBudget);
        const size_t separator = suggested.find_last_of("/\\");
        std::string name(separator == std::string_view::npos ? suggested : suggested.substr(separator + 1));
        for (char& character : name)
        {
            if (!IsKeptCharacter(character))
                character = '_';
        }

        const size_t firstKept = name.find_first_not_of('.');
        if (firstKept == std::string::npos)
            return std::string(kFallbackName);
        name.erase(0, firstKept);
        name.erase(name.find_last_not_of('.') + 1);

        std::string stem = name;
        std::string extension;
        const size_t lastDot = name.rfind('.');
        if (lastDot != std::string::npos)
        {
            stem = name.substr(0, lastDot);
            extension = name.substr(lastDot + 1, std::min(kMaximumExtensionBytes, maximumBytes - 2));
            for (char& character : extension)
            {
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character - 'A' + 'a');
            }
        }
        std::replace(stem.begin(), stem.end(), '.', '_');
        if (stem.empty())
            stem = kFallbackName;

        const size_t budget = maximumBytes - (extension.empty() ? 0 : extension.size() + 1);
        if (stem.size() > budget)
            stem.resize(budget);
        if (IsWindowsReservedStem(stem))
        {
            stem.insert(stem.begin(), '_');
            if (stem.size() > budget)
                stem.pop_back();
        }
        return extension.empty() ? stem : stem + "." + extension;
    }

    std::string_view ExtensionOfSanitizedName(std::string_view sanitizedName)
    {
        const size_t lastDot = sanitizedName.rfind('.');
        return lastDot == std::string_view::npos ? std::string_view() : sanitizedName.substr(lastDot + 1);
    }

    BrowserDownloadPolicy::BrowserDownloadPolicy(BrowserDownloadLimits limits)
        : m_Limits(std::move(limits))
    {
    }

    BrowserDownloadDecision BrowserDownloadPolicy::Evaluate(const BrowserDownloadOffer& offer, Engine::u64 nonce)
    {
        BrowserDownloadDecision decision;
        decision.SanitizedName = SanitizeDownloadFileName(offer.SuggestedName, m_Limits.MaximumNameBytes);

        const std::string_view extension = ExtensionOfSanitizedName(decision.SanitizedName);
        const bool extensionAllowed = !extension.empty()
            && std::find(m_Limits.AllowedExtensions.begin(), m_Limits.AllowedExtensions.end(), extension)
                != m_Limits.AllowedExtensions.end();
        if (!extensionAllowed)
        {
            decision.Verdict = BrowserDownloadVerdict::RejectExtension;
            return decision;
        }
        if (offer.TotalBytes > m_Limits.MaximumBytes)
        {
            decision.Verdict = BrowserDownloadVerdict::RejectSize;
            return decision;
        }

        decision.Verdict = BrowserDownloadVerdict::Accept;
        decision.StagingDirectoryName = "d" + HexSegment(m_Sequence++, 8) + "-" + HexSegment(nonce, 16);
        decision.RelativeStagedPath = std::filesystem::path(decision.StagingDirectoryName) / decision.SanitizedName;
        return decision;
    }

    std::filesystem::path BrowserDownloadPolicy::ResolveStagedPath(
        const std::filesystem::path& stagingRoot, const BrowserDownloadDecision& decision, std::string& error)
    {
        if (stagingRoot.empty() || !IsPlainSegment(decision.StagingDirectoryName) || !IsPlainSegment(decision.SanitizedName)
            || decision.Verdict != BrowserDownloadVerdict::Accept)
        {
            error = "download decision is not an accepted decision naming plain staging segments";
            return {};
        }
        error.clear();
        return stagingRoot / decision.StagingDirectoryName / decision.SanitizedName;
    }

    std::string BrowserDownloadPolicy::FormatLogLine(const BrowserDownloadDecision& decision, Engine::u64 totalBytes)
    {
        const char* verdict = "accepted";
        if (decision.Verdict == BrowserDownloadVerdict::RejectExtension)
            verdict = "blocked-type";
        else if (decision.Verdict == BrowserDownloadVerdict::RejectSize)
            verdict = "blocked-size";
        return std::string("download ") + verdict + " name=" + decision.SanitizedName
            + " declaredBytes=" + std::to_string(totalBytes);
    }
}
