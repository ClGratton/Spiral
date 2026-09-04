#include "Engine/Assets/FabImportReceipt.h"

#include "Engine/Core/AtomicFile.h"
#include "Engine/Core/Sha256.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

namespace Engine
{
    namespace
    {
        constexpr size_t kMaxReceiptAssets = 16 * 1024;
        constexpr size_t kMaxCollectionReceipts = 4 * 1024;
        constexpr size_t kMaxCollectionAssets = 256 * 1024;
        constexpr size_t kMaxReceiptBytes = 16 * 1024 * 1024;
        constexpr size_t kMaxCollectionBytes = 64 * 1024 * 1024;
        constexpr size_t kMaxCollectionLines = 512 * 1024;
        constexpr size_t kMaxShortTextBytes = 4096;
        constexpr size_t kMaxAttributionBytes = 16 * 1024;

        using ReceiptKey = std::pair<std::string, std::string>;

        bool Fail(std::string& outError, std::string message)
        {
            outError = std::move(message);
            return false;
        }

        bool IsAsciiWhitespace(unsigned char value)
        {
            return value == ' ' || value == '\t' || value == '\n'
                || value == '\r' || value == '\f' || value == '\v';
        }

        bool IsAsciiAlphaNumeric(unsigned char value)
        {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
                || (value >= '0' && value <= '9');
        }

        bool IsValidText(std::string_view value, bool required, size_t maximumBytes)
        {
            if (value.empty())
                return !required;
            if (value.size() > maximumBytes
                || IsAsciiWhitespace(static_cast<unsigned char>(value.front()))
                || IsAsciiWhitespace(static_cast<unsigned char>(value.back())))
                return false;
            for (const unsigned char character : value)
                if (character < 0x20 || character == 0x7f)
                    return false;
            return true;
        }

        bool IsCanonicalFabUrl(std::string_view value)
        {
            constexpr std::string_view prefix = "https://www.fab.com/listings/";
            if (!value.starts_with(prefix))
                return false;
            const std::string_view listing = value.substr(prefix.size());
            if (listing.empty() || listing.size() > 256)
                return false;
            for (const unsigned char character : listing)
                if (!IsAsciiAlphaNumeric(character) && character != '-' && character != '_')
                    return false;
            return true;
        }

        bool IsProductIdentity(std::string_view value)
        {
            return IsCanonicalFabUrl(value);
        }

        bool IsSecureUrl(std::string_view value)
        {
            constexpr std::string_view prefix = "https://";
            if (!IsValidText(value, true, kMaxShortTextBytes) || !value.starts_with(prefix))
                return false;
            if (value.find('\\') != std::string_view::npos
                || std::any_of(value.begin(), value.end(), [](unsigned char character)
                    { return IsAsciiWhitespace(character); }))
                return false;
            const size_t hostEnd = value.find('/', prefix.size());
            const std::string_view host = value.substr(prefix.size(), hostEnd - prefix.size());
            if (host.empty() || host.find('@') != std::string_view::npos || host.find(':') != std::string_view::npos)
                return false;
            for (const unsigned char character : host)
                if (!IsAsciiAlphaNumeric(character) && character != '-' && character != '.')
                    return false;
            return host.front() != '.' && host.back() != '.'
                && host.find("..") == std::string_view::npos;
        }

        bool IsLowerSha256(std::string_view value)
        {
            if (value.size() != 64)
                return false;
            return std::all_of(value.begin(), value.end(), [](unsigned char character)
            {
                return (character >= '0' && character <= '9')
                    || (character >= 'a' && character <= 'f');
            });
        }

        std::string AsciiCaseFold(std::string_view value)
        {
            std::string result(value);
            for (char& character : result)
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character + ('a' - 'A'));
            return result;
        }

        bool IsWindowsReservedName(std::string_view segment)
        {
            const std::string folded = AsciiCaseFold(segment.substr(0, segment.find('.')));
            if (folded == "con" || folded == "prn" || folded == "aux" || folded == "nul")
                return true;
            return folded.size() == 4 && folded[3] >= '1' && folded[3] <= '9'
                && (folded.starts_with("com") || folded.starts_with("lpt"));
        }

        bool IsSafeRelativePath(std::string_view value)
        {
            if (!IsValidText(value, true, 1024)
                || value.front() == '/' || value.back() == '/'
                || value.find('\\') != std::string_view::npos
                || value.find(':') != std::string_view::npos)
                return false;

            size_t offset = 0;
            size_t depth = 0;
            while (offset < value.size())
            {
                const size_t separator = value.find('/', offset);
                const size_t end = separator == std::string_view::npos ? value.size() : separator;
                const std::string_view component = value.substr(offset, end - offset);
                if (component.empty() || component == "." || component == ".."
                    || component.size() > 255 || ++depth > 16
                    || component.back() == '.' || component.back() == ' '
                    || IsWindowsReservedName(component))
                    return false;
                for (unsigned char character : component)
                    if (character < 0x20 || character > 0x7e || character == '<'
                        || character == '>' || character == '"' || character == '|'
                        || character == '?' || character == '*')
                        return false;
                offset = end + 1;
            }
            return true;
        }

        bool IsLeapYear(int year)
        {
            return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        }

        bool ParseFixedDecimal(std::string_view value, size_t offset, size_t count, int& output)
        {
            output = 0;
            for (size_t index = 0; index < count; ++index)
            {
                const char character = value[offset + index];
                if (character < '0' || character > '9')
                    return false;
                output = output * 10 + character - '0';
            }
            return true;
        }

        bool IsUtcTimestamp(std::string_view value)
        {
            if (value.size() != 20 || value[4] != '-' || value[7] != '-'
                || value[10] != 'T' || value[13] != ':' || value[16] != ':' || value[19] != 'Z')
                return false;
            int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
            if (!ParseFixedDecimal(value, 0, 4, year) || !ParseFixedDecimal(value, 5, 2, month)
                || !ParseFixedDecimal(value, 8, 2, day) || !ParseFixedDecimal(value, 11, 2, hour)
                || !ParseFixedDecimal(value, 14, 2, minute) || !ParseFixedDecimal(value, 17, 2, second)
                || year == 0 || month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59)
                return false;
            constexpr std::array<int, 12> monthDays { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            const int maximumDay = monthDays[static_cast<size_t>(month - 1)]
                + (month == 2 && IsLeapYear(year) ? 1 : 0);
            return day >= 1 && day <= maximumDay;
        }

        bool IsReceiptAssetType(AssetType type)
        {
            return type == AssetType::Mesh || type == AssetType::Material || type == AssetType::Texture;
        }

        bool IsSemanticRole(std::string_view value)
        {
            if (!IsValidText(value, true, 256))
                return false;
            for (const unsigned char character : value)
                if (!IsAsciiAlphaNumeric(character) && character != '-' && character != '_'
                    && character != '.' && character != ':')
                    return false;
            return true;
        }

        FabImportReceipt CanonicalReceipt(FabImportReceipt receipt)
        {
            std::sort(receipt.Assets.begin(), receipt.Assets.end(), [](const auto& left, const auto& right)
            {
                return std::tie(left.Handle, left.SemanticRole, left.LogicalPath,
                           left.GenerationRelativeCookedPath, left.ArtifactSha256)
                    < std::tie(right.Handle, right.SemanticRole, right.LogicalPath,
                           right.GenerationRelativeCookedPath, right.ArtifactSha256);
            });
            return receipt;
        }

        bool FitsReceiptSizeBound(const FabImportReceipt& receipt)
        {
            size_t estimate = 4096;
            const auto add = [&estimate](size_t byteCount)
            {
                if (byteCount > kMaxReceiptBytes - estimate)
                    return false;
                estimate += byteCount;
                return true;
            };
            const auto addQuoted = [&estimate](size_t byteCount)
            {
                if (byteCount > (kMaxReceiptBytes - estimate) / 2)
                    return false;
                estimate += byteCount * 2;
                return true;
            };
            if (!addQuoted(receipt.ProductIdentity.size()) || !addQuoted(receipt.ProductName.size())
                || !addQuoted(receipt.Publisher.size()) || !addQuoted(receipt.VersionOrDownloadLabel.size())
                || !addQuoted(receipt.AttributionText.size()) || !addQuoted(receipt.AttributionLink.size())
                || !addQuoted(receipt.ImporterVersion.size()) || !addQuoted(receipt.CookerVersion.size())
                || !addQuoted(receipt.StreamId.size()) || !addQuoted(receipt.GenerationId.size())
                || !addQuoted(receipt.RelatedStreamId.size()) || !addQuoted(receipt.RelatedGenerationId.size()))
                return false;
            for (const FabImportedAssetRecord& asset : receipt.Assets)
                if (!addQuoted(asset.SemanticRole.size()) || !addQuoted(asset.LogicalPath.size())
                    || !addQuoted(asset.GenerationRelativeCookedPath.size()) || !add(256))
                    return false;
            return true;
        }

        FabReceiptCollection CanonicalCollection(FabReceiptCollection collection)
        {
            for (FabImportReceipt& receipt : collection.Receipts)
                receipt = CanonicalReceipt(std::move(receipt));
            std::sort(collection.Receipts.begin(), collection.Receipts.end(), [](const auto& left, const auto& right)
            {
                return std::tie(left.ProductIdentity, left.StreamId, left.GenerationId)
                    < std::tie(right.ProductIdentity, right.StreamId, right.GenerationId);
            });
            return collection;
        }

        bool IsAtEnd(std::istringstream& stream)
        {
            stream >> std::ws;
            return stream.eof();
        }

        bool ReadQuotedLine(std::string_view line, std::string_view expectedKey, std::string& output)
        {
            std::istringstream stream { std::string(line) };
            stream.imbue(std::locale::classic());
            std::string key;
            return static_cast<bool>(stream >> key >> std::quoted(output))
                && key == expectedKey && IsAtEnd(stream);
        }

        bool ReadTokenLine(std::string_view line, std::string_view expectedKey, std::string& output)
        {
            std::istringstream stream { std::string(line) };
            stream.imbue(std::locale::classic());
            std::string key;
            return static_cast<bool>(stream >> key >> output) && key == expectedKey && IsAtEnd(stream);
        }

        bool ReadCountLine(std::string_view line, std::string_view expectedKey, size_t& output)
        {
            std::istringstream stream { std::string(line) };
            stream.imbue(std::locale::classic());
            std::string key;
            std::string token;
            if (!(stream >> key >> token) || key != expectedKey || !IsAtEnd(stream) || token.empty())
                return false;
            size_t parsed = 0;
            const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), parsed);
            if (error != std::errc {} || end != token.data() + token.size())
                return false;
            output = parsed;
            return true;
        }

        bool SplitLines(
            std::string_view bytes, size_t maximumBytes, size_t maximumLines,
            std::vector<std::string_view>& lines)
        {
            if (bytes.empty() || bytes.size() > maximumBytes || bytes.back() != '\n')
                return false;
            lines.clear();
            size_t offset = 0;
            while (offset < bytes.size())
            {
                const size_t newline = bytes.find('\n', offset);
                if (newline == std::string_view::npos)
                    return false;
                const std::string_view line = bytes.substr(offset, newline - offset);
                if (line.empty() || line.find('\r') != std::string_view::npos
                    || lines.size() >= maximumLines)
                    return false;
                lines.push_back(line);
                offset = newline + 1;
            }
            return true;
        }

        bool ParsePackageFormat(std::string_view value, FabPackageFormat& output)
        {
            if (value == "glTF") output = FabPackageFormat::Gltf;
            else if (value == "GLB") output = FabPackageFormat::Glb;
            else return false;
            return true;
        }

        bool ParseLicenseFamily(std::string_view value, FabLicenseFamily& output)
        {
            if (value == "FabStandard") output = FabLicenseFamily::FabStandard;
            else if (value == "CC-BY") output = FabLicenseFamily::CreativeCommonsAttribution;
            else if (value == "LegacyUnrealMarketplace") output = FabLicenseFamily::LegacyUnrealMarketplace;
            else if (value == "ReferenceOnly") output = FabLicenseFamily::ReferenceOnly;
            else return false;
            return true;
        }

        bool ParseLicenseTier(std::string_view value, FabLicenseTier& output)
        {
            if (value == "NotApplicable") output = FabLicenseTier::NotApplicable;
            else if (value == "Personal") output = FabLicenseTier::Personal;
            else if (value == "Professional") output = FabLicenseTier::Professional;
            else return false;
            return true;
        }

        bool ParseMetadataFlag(std::string_view value, FabMetadataFlag& output)
        {
            if (value == "Unknown") output = FabMetadataFlag::Unknown;
            else if (value == "No") output = FabMetadataFlag::No;
            else if (value == "Yes") output = FabMetadataFlag::Yes;
            else return false;
            return true;
        }

        bool ParseDigestKind(std::string_view value, FabDigestKind& output)
        {
            if (value != "SHA-256")
                return false;
            output = FabDigestKind::Sha256;
            return true;
        }

        bool ParseRawSourcePolicy(std::string_view value, FabRawSourcePolicy& output)
        {
            if (value == "ExcludedFromProject") output = FabRawSourcePolicy::ExcludedFromProject;
            else if (value == "PrivateProjectOnly") output = FabRawSourcePolicy::PrivateProjectOnly;
            else return false;
            return true;
        }

        bool ParseRelation(std::string_view value, FabGenerationRelation& output)
        {
            if (value == "Initial") output = FabGenerationRelation::Initial;
            else if (value == "SourceReplacement") output = FabGenerationRelation::SourceReplacement;
            else if (value == "ProductUpdate") output = FabGenerationRelation::ProductUpdate;
            else return false;
            return true;
        }

        bool SameStreamProvenance(const FabImportReceipt& left, const FabImportReceipt& right)
        {
            return left.ProductIdentity == right.ProductIdentity
                && left.ProductName == right.ProductName
                && left.Publisher == right.Publisher
                && left.VersionOrDownloadLabel == right.VersionOrDownloadLabel
                && left.PackageFormat == right.PackageFormat
                && left.LicenseFamily == right.LicenseFamily
                && left.LicenseTier == right.LicenseTier
                && left.AttributionText == right.AttributionText
                && left.AttributionLink == right.AttributionLink
                && left.MetadataConfirmedByUser == right.MetadataConfirmedByUser
                && left.NoAI == right.NoAI
                && left.GeneratedWithAI == right.GeneratedWithAI
                && left.SourceDigestKind == right.SourceDigestKind
                && left.RawSourcePolicy == right.RawSourcePolicy;
        }

        bool SameReimportIdentity(const FabImportReceipt& left, const FabImportReceipt& right)
        {
            return left.ProductIdentity == right.ProductIdentity
                && left.VersionOrDownloadLabel == right.VersionOrDownloadLabel
                && left.PackageFormat == right.PackageFormat
                && left.SourceDigestKind == right.SourceDigestKind
                && left.SourceSha256 == right.SourceSha256;
        }

        bool ReuseCompatible(const FabImportReceipt& left, const FabImportReceipt& right)
        {
            const FabImportReceipt canonicalLeft = CanonicalReceipt(left);
            const FabImportReceipt canonicalRight = CanonicalReceipt(right);
            return SameStreamProvenance(canonicalLeft, canonicalRight)
                && canonicalLeft.ExpandedTreeSha256 == canonicalRight.ExpandedTreeSha256
                && canonicalLeft.ImporterVersion == canonicalRight.ImporterVersion
                && canonicalLeft.CookerVersion == canonicalRight.CookerVersion
                && canonicalLeft.Relation == canonicalRight.Relation
                && canonicalLeft.RelatedStreamId == canonicalRight.RelatedStreamId
                && canonicalLeft.RelatedGenerationId == canonicalRight.RelatedGenerationId
                && canonicalLeft.Assets == canonicalRight.Assets;
        }

        bool SameStableAssetSet(const FabImportReceipt& left, const FabImportReceipt& right)
        {
            if (left.Assets.size() != right.Assets.size())
                return false;
            const FabImportReceipt canonicalLeft = CanonicalReceipt(left);
            const FabImportReceipt canonicalRight = CanonicalReceipt(right);
            for (size_t index = 0; index < canonicalLeft.Assets.size(); ++index)
            {
                const FabImportedAssetRecord& a = canonicalLeft.Assets[index];
                const FabImportedAssetRecord& b = canonicalRight.Assets[index];
                if (a.Handle != b.Handle || a.Type != b.Type || a.SemanticRole != b.SemanticRole
                    || a.LogicalPath != b.LogicalPath)
                    return false;
            }
            return true;
        }

        const FabImportReceipt* FindReceipt(
            const FabReceiptCollection& collection, std::string_view streamId, std::string_view generationId)
        {
            const auto found = std::find_if(collection.Receipts.begin(), collection.Receipts.end(),
                [streamId, generationId](const FabImportReceipt& receipt)
                {
                    return receipt.StreamId == streamId && receipt.GenerationId == generationId;
                });
            return found == collection.Receipts.end() ? nullptr : &(*found);
        }

        const FabImportReceipt* FindStreamTip(
            const FabReceiptCollection& collection, std::string_view streamId)
        {
            std::set<std::string> generationsWithChildren;
            for (const FabImportReceipt& receipt : collection.Receipts)
                if (receipt.Relation == FabGenerationRelation::SourceReplacement
                    && receipt.RelatedStreamId == streamId)
                    generationsWithChildren.insert(receipt.RelatedGenerationId);
            const FabImportReceipt* tip = nullptr;
            for (const FabImportReceipt& candidate : collection.Receipts)
            {
                if (candidate.StreamId != streamId
                    || generationsWithChildren.contains(candidate.GenerationId))
                    continue;
                if (tip)
                    return nullptr;
                tip = &candidate;
            }
            return tip;
        }

        const FabImportReceipt* FindProductTip(
            const FabReceiptCollection& collection, std::string_view productIdentity)
        {
            std::set<ReceiptKey> generationsWithChildren;
            for (const FabImportReceipt& receipt : collection.Receipts)
                if (receipt.Relation == FabGenerationRelation::SourceReplacement
                    || receipt.Relation == FabGenerationRelation::ProductUpdate)
                    generationsWithChildren.emplace(
                        receipt.RelatedStreamId, receipt.RelatedGenerationId);
            const FabImportReceipt* productTip = nullptr;
            for (const FabImportReceipt& receipt : collection.Receipts)
            {
                if (receipt.ProductIdentity != productIdentity
                    || generationsWithChildren.contains(ReceiptKey {
                        receipt.StreamId, receipt.GenerationId }))
                    continue;
                if (productTip)
                    return nullptr;
                productTip = &receipt;
            }
            return productTip;
        }

        FabReceiptDecision Decision(FabReceiptDecisionKind kind, std::string diagnostic,
            const FabImportReceipt* existing = nullptr)
        {
            FabReceiptDecision result;
            result.Kind = kind;
            result.Diagnostic = std::move(diagnostic);
            if (existing)
            {
                result.ExistingStreamId = existing->StreamId;
                result.ExistingGenerationId = existing->GenerationId;
            }
            return result;
        }

        bool WriteBytesAtomically(
            const std::filesystem::path& path, std::string_view bytes, size_t maximumBytes,
            std::string& outError)
        {
            if (path.empty() || bytes.empty() || bytes.size() > maximumBytes)
                return Fail(outError, "Fab receipt destination or byte count is invalid");

            return WriteFileAtomically(path, bytes, outError);
        }

        bool ReadBoundedFile(
            const std::filesystem::path& path, size_t maximumBytes, std::string& output,
            std::string& outError)
        {
            std::error_code filesystemError;
            const std::uintmax_t byteCount = std::filesystem::file_size(path, filesystemError);
            if (filesystemError || byteCount == 0 || byteCount > maximumBytes)
                return Fail(outError, "Fab receipt file size is invalid");
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return Fail(outError, "could not open the Fab receipt");
            std::string bytes(static_cast<size_t>(byteCount), '\0');
            if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
                return Fail(outError, "could not read the complete Fab receipt");
            char trailing = 0;
            if (input.get(trailing))
                return Fail(outError, "Fab receipt changed while it was read");
            output = std::move(bytes);
            return true;
        }

        std::string HashFabFields(std::string_view domain,
            std::initializer_list<std::string_view> fields)
        {
            Sha256Builder hash;
            hash.Update(domain);
            hash.Update("\n");
            for (std::string_view field : fields)
            {
                hash.Update(field);
                hash.Update("\n");
            }
            return hash.FinalizeHex();
        }
    }

    const char* ToString(FabPackageFormat value)
    {
        switch (value)
        {
            case FabPackageFormat::Gltf: return "glTF";
            case FabPackageFormat::Glb: return "GLB";
            case FabPackageFormat::Unknown: break;
        }
        return "Unknown";
    }

    const char* ToString(FabLicenseFamily value)
    {
        switch (value)
        {
            case FabLicenseFamily::FabStandard: return "FabStandard";
            case FabLicenseFamily::CreativeCommonsAttribution: return "CC-BY";
            case FabLicenseFamily::LegacyUnrealMarketplace: return "LegacyUnrealMarketplace";
            case FabLicenseFamily::ReferenceOnly: return "ReferenceOnly";
            case FabLicenseFamily::Unknown: break;
        }
        return "Unknown";
    }

    const char* ToString(FabLicenseTier value)
    {
        switch (value)
        {
            case FabLicenseTier::NotApplicable: return "NotApplicable";
            case FabLicenseTier::Personal: return "Personal";
            case FabLicenseTier::Professional: return "Professional";
            case FabLicenseTier::Unknown: break;
        }
        return "Unknown";
    }

    const char* ToString(FabMetadataFlag value)
    {
        switch (value)
        {
            case FabMetadataFlag::Unknown: return "Unknown";
            case FabMetadataFlag::No: return "No";
            case FabMetadataFlag::Yes: return "Yes";
        }
        return "Invalid";
    }

    const char* ToString(FabDigestKind value)
    {
        return value == FabDigestKind::Sha256 ? "SHA-256" : "Unknown";
    }

    const char* ToString(FabRawSourcePolicy value)
    {
        switch (value)
        {
            case FabRawSourcePolicy::ExcludedFromProject: return "ExcludedFromProject";
            case FabRawSourcePolicy::PrivateProjectOnly: return "PrivateProjectOnly";
            case FabRawSourcePolicy::Unknown: break;
        }
        return "Unknown";
    }

    const char* ToString(FabGenerationRelation value)
    {
        switch (value)
        {
            case FabGenerationRelation::Initial: return "Initial";
            case FabGenerationRelation::SourceReplacement: return "SourceReplacement";
            case FabGenerationRelation::ProductUpdate: return "ProductUpdate";
            case FabGenerationRelation::Unknown: break;
        }
        return "Unknown";
    }

    const char* ToString(FabReceiptDecisionKind value)
    {
        switch (value)
        {
            case FabReceiptDecisionKind::InvalidCandidate: return "InvalidCandidate";
            case FabReceiptDecisionKind::CorruptPriorState: return "CorruptPriorState";
            case FabReceiptDecisionKind::ExactReuse: return "ExactReuse";
            case FabReceiptDecisionKind::AddNewStream: return "AddNewStream";
            case FabReceiptDecisionKind::ReplaceSameStreamSource: return "ReplaceSameStreamSource";
            case FabReceiptDecisionKind::AddProductUpdateStream: return "AddProductUpdateStream";
            case FabReceiptDecisionKind::Conflict: return "Conflict";
        }
        return "InvalidCandidate";
    }

    std::string ComputeFabStreamId(std::string_view productIdentity,
        std::string_view versionOrDownloadLabel, FabPackageFormat format)
    {
        if (!IsProductIdentity(productIdentity)
            || !IsValidText(versionOrDownloadLabel, true, kMaxShortTextBytes)
            || (format != FabPackageFormat::Gltf && format != FabPackageFormat::Glb))
            return {};
        return HashFabFields("SpiralFabStreamV1",
            { productIdentity, versionOrDownloadLabel, ToString(format) });
    }

    std::string ComputeFabGenerationId(std::string_view streamId,
        std::string_view sourceSha256, std::string_view expandedTreeSha256)
    {
        if (!IsLowerSha256(streamId) || !IsLowerSha256(sourceSha256)
            || !IsLowerSha256(expandedTreeSha256))
            return {};
        return HashFabFields("SpiralFabGenerationV1",
            { streamId, sourceSha256, expandedTreeSha256 });
    }

    AssetHandle ComputeFabStableAssetHandle(
        std::string_view streamId, AssetType type, std::string_view semanticRole)
    {
        if (!IsLowerSha256(streamId) || !IsReceiptAssetType(type) || !IsSemanticRole(semanticRole))
            return kInvalidAssetHandle;
        Sha256Builder hash;
        hash.Update("SpiralFabAssetV1\n");
        hash.Update(streamId);
        hash.Update("\n");
        hash.Update(ToString(type));
        hash.Update("\n");
        hash.Update(semanticRole);
        hash.Update("\n");
        const Sha256Builder::Digest digest = hash.FinalizeBytes();
        AssetHandle handle = 0;
        for (size_t index = 0; index < sizeof(handle); ++index)
            handle = (handle << 8) | digest[index];
        return handle == kInvalidAssetHandle ? 1 : handle;
    }

    bool ValidateFabImportReceipt(const FabImportReceipt& receipt, std::string& outError)
    {
        if (receipt.SchemaVersion != kFabImportReceiptSchemaVersion)
            return Fail(outError, "unsupported Fab import receipt schema");
        if (!IsProductIdentity(receipt.ProductIdentity))
            return Fail(outError, "Fab product identity is empty, unsafe, or not a canonical Fab URL");
        if (!IsValidText(receipt.ProductName, false, kMaxShortTextBytes)
            || !IsValidText(receipt.Publisher, true, kMaxShortTextBytes)
            || !IsValidText(receipt.VersionOrDownloadLabel, true, kMaxShortTextBytes)
            || !IsValidText(receipt.ImporterVersion, true, kMaxShortTextBytes)
            || !IsValidText(receipt.CookerVersion, true, kMaxShortTextBytes))
            return Fail(outError, "Fab receipt has empty or malformed identity metadata");
        if (receipt.PackageFormat != FabPackageFormat::Gltf && receipt.PackageFormat != FabPackageFormat::Glb)
            return Fail(outError, "Fab receipt package format is unknown");
        if (!receipt.MetadataConfirmedByUser)
            return Fail(outError, "Fab receipt metadata was not explicitly confirmed by the user");
        if (receipt.NoAI != FabMetadataFlag::Unknown && receipt.NoAI != FabMetadataFlag::No
            && receipt.NoAI != FabMetadataFlag::Yes)
            return Fail(outError, "Fab receipt NoAI metadata is invalid");
        if (receipt.GeneratedWithAI != FabMetadataFlag::Unknown && receipt.GeneratedWithAI != FabMetadataFlag::No
            && receipt.GeneratedWithAI != FabMetadataFlag::Yes)
            return Fail(outError, "Fab receipt generated-with-AI metadata is invalid");
        if (receipt.LicenseFamily == FabLicenseFamily::ReferenceOnly)
            return Fail(outError, "Fab Reference Only content cannot be imported");
        if (receipt.LicenseFamily == FabLicenseFamily::FabStandard)
        {
            if (receipt.LicenseTier != FabLicenseTier::Personal
                && receipt.LicenseTier != FabLicenseTier::Professional)
                return Fail(outError, "Fab Standard license requires a Personal or Professional tier");
        }
        else if (receipt.LicenseFamily == FabLicenseFamily::CreativeCommonsAttribution)
        {
            if (receipt.LicenseTier != FabLicenseTier::NotApplicable)
                return Fail(outError, "CC-BY must not declare a Fab Standard tier");
            if (!IsValidText(receipt.AttributionText, true, kMaxAttributionBytes)
                || !IsSecureUrl(receipt.AttributionLink))
                return Fail(outError, "CC-BY requires nonempty attribution text and a secure attribution link");
        }
        else if (receipt.LicenseFamily == FabLicenseFamily::LegacyUnrealMarketplace)
        {
            if (receipt.LicenseTier != FabLicenseTier::NotApplicable)
                return Fail(outError, "legacy Unreal Marketplace provenance must not declare a Fab tier");
        }
        else
            return Fail(outError, "Fab receipt license family is unknown");
        if (!receipt.AttributionText.empty()
            && !IsValidText(receipt.AttributionText, false, kMaxAttributionBytes))
            return Fail(outError, "Fab attribution text is malformed");
        if (!receipt.AttributionLink.empty() && !IsSecureUrl(receipt.AttributionLink))
            return Fail(outError, "Fab attribution link is insecure or malformed");
        if (receipt.SourceDigestKind != FabDigestKind::Sha256
            || !IsLowerSha256(receipt.SourceSha256)
            || !IsLowerSha256(receipt.ExpandedTreeSha256))
            return Fail(outError, "Fab receipt digests must be lowercase SHA-256 values");
        const std::string expectedStreamId = ComputeFabStreamId(
            receipt.ProductIdentity, receipt.VersionOrDownloadLabel, receipt.PackageFormat);
        const std::string expectedGenerationId = ComputeFabGenerationId(
            expectedStreamId, receipt.SourceSha256, receipt.ExpandedTreeSha256);
        if (receipt.StreamId != expectedStreamId || receipt.GenerationId != expectedGenerationId
            || !IsUtcTimestamp(receipt.DiagnosticAcquiredAtUtc))
            return Fail(outError, "Fab receipt stream, generation, or acquisition identity is malformed");
        if (receipt.RawSourcePolicy != FabRawSourcePolicy::ExcludedFromProject
            && receipt.RawSourcePolicy != FabRawSourcePolicy::PrivateProjectOnly)
            return Fail(outError, "Fab raw-source policy is unknown");

        switch (receipt.Relation)
        {
            case FabGenerationRelation::Initial:
                if (!receipt.RelatedStreamId.empty() || !receipt.RelatedGenerationId.empty())
                    return Fail(outError, "initial Fab generation cannot name a related generation");
                break;
            case FabGenerationRelation::SourceReplacement:
                if (receipt.RelatedStreamId != receipt.StreamId
                    || !IsLowerSha256(receipt.RelatedGenerationId)
                    || receipt.RelatedGenerationId == receipt.GenerationId)
                    return Fail(outError, "Fab source replacement must name another generation in the same stream");
                break;
            case FabGenerationRelation::ProductUpdate:
                if (!IsLowerSha256(receipt.RelatedStreamId)
                    || !IsLowerSha256(receipt.RelatedGenerationId)
                    || receipt.RelatedStreamId == receipt.StreamId)
                    return Fail(outError, "Fab product update must name a generation in another stream");
                break;
            case FabGenerationRelation::Unknown:
            default:
                return Fail(outError, "Fab generation relation is unknown");
        }

        if (receipt.Assets.empty() || receipt.Assets.size() > kMaxReceiptAssets)
            return Fail(outError, "Fab receipt asset count is invalid");
        std::set<AssetHandle> handles;
        std::set<std::string> roles;
        std::set<std::string> logicalPaths;
        std::set<std::string> cookedPaths;
        for (const FabImportedAssetRecord& asset : receipt.Assets)
        {
            if (asset.Handle == kInvalidAssetHandle || !IsReceiptAssetType(asset.Type)
                || !IsSemanticRole(asset.SemanticRole)
                || !IsSafeRelativePath(asset.LogicalPath)
                || !IsSafeRelativePath(asset.GenerationRelativeCookedPath)
                || !IsLowerSha256(asset.ArtifactSha256)
                || asset.Handle != ComputeFabStableAssetHandle(
                    receipt.StreamId, asset.Type, asset.SemanticRole))
                return Fail(outError, "Fab receipt contains a malformed asset record");
            if (!handles.insert(asset.Handle).second || !roles.insert(asset.SemanticRole).second
                || !logicalPaths.insert(AsciiCaseFold(asset.LogicalPath)).second
                || !cookedPaths.insert(AsciiCaseFold(asset.GenerationRelativeCookedPath)).second)
                return Fail(outError, "Fab receipt contains duplicate handles, roles, or paths");
        }
        if (!FitsReceiptSizeBound(receipt))
            return Fail(outError, "Fab receipt exceeds the bounded serialized size");

        outError.clear();
        return true;
    }

    bool SerializeFabImportReceipt(
        const FabImportReceipt& receipt, std::string& outBytes, std::string& outError)
    {
        if (!ValidateFabImportReceipt(receipt, outError))
            return false;
        const FabImportReceipt canonical = CanonicalReceipt(receipt);
        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << "SpiralFabImportReceipt " << kFabImportReceiptSchemaVersion << '\n'
            << "ProductIdentity " << std::quoted(canonical.ProductIdentity) << '\n'
            << "ProductName " << std::quoted(canonical.ProductName) << '\n'
            << "Publisher " << std::quoted(canonical.Publisher) << '\n'
            << "VersionOrDownloadLabel " << std::quoted(canonical.VersionOrDownloadLabel) << '\n'
            << "PackageFormat " << ToString(canonical.PackageFormat) << '\n'
            << "LicenseFamily " << ToString(canonical.LicenseFamily) << '\n'
            << "LicenseTier " << ToString(canonical.LicenseTier) << '\n'
            << "AttributionText " << std::quoted(canonical.AttributionText) << '\n'
            << "AttributionLink " << std::quoted(canonical.AttributionLink) << '\n'
            << "MetadataConfirmedByUser " << (canonical.MetadataConfirmedByUser ? "true" : "false") << '\n'
            << "NoAI " << ToString(canonical.NoAI) << '\n'
            << "GeneratedWithAI " << ToString(canonical.GeneratedWithAI) << '\n'
            << "SourceDigestKind " << ToString(canonical.SourceDigestKind) << '\n'
            << "SourceSHA256 " << canonical.SourceSha256 << '\n'
            << "ExpandedTreeSHA256 " << canonical.ExpandedTreeSha256 << '\n'
            << "ImporterVersion " << std::quoted(canonical.ImporterVersion) << '\n'
            << "CookerVersion " << std::quoted(canonical.CookerVersion) << '\n'
            << "StreamId " << std::quoted(canonical.StreamId) << '\n'
            << "GenerationId " << std::quoted(canonical.GenerationId) << '\n'
            << "DiagnosticAcquiredAtUTC " << std::quoted(canonical.DiagnosticAcquiredAtUtc) << '\n'
            << "RawSourcePolicy " << ToString(canonical.RawSourcePolicy) << '\n'
            << "Relation " << ToString(canonical.Relation) << '\n'
            << "RelatedStreamId " << std::quoted(canonical.RelatedStreamId) << '\n'
            << "RelatedGenerationId " << std::quoted(canonical.RelatedGenerationId) << '\n'
            << "AssetCount " << canonical.Assets.size() << '\n';
        for (const FabImportedAssetRecord& asset : canonical.Assets)
            output << "Asset " << asset.Handle << ' ' << ToString(asset.Type) << ' '
                << std::quoted(asset.SemanticRole) << ' ' << std::quoted(asset.LogicalPath) << ' '
                << std::quoted(asset.GenerationRelativeCookedPath) << ' ' << asset.ArtifactSha256 << '\n';
        output << "End\n";
        const std::string candidate = output.str();
        if (candidate.size() > kMaxReceiptBytes)
            return Fail(outError, "Fab receipt exceeds the bounded serialized size");
        outBytes = candidate;
        outError.clear();
        return true;
    }

    bool DeserializeFabImportReceipt(
        std::string_view bytes, FabImportReceipt& outReceipt, std::string& outError)
    {
        if (bytes.size() > kMaxReceiptBytes)
            return Fail(outError, "Fab receipt exceeds the bounded serialized size");
        std::vector<std::string_view> lines;
        if (!SplitLines(bytes, kMaxReceiptBytes, kMaxReceiptAssets + 27, lines)
            || lines.size() < 27)
            return Fail(outError, "Fab receipt framing is malformed or truncated");

        FabImportReceipt candidate;
        size_t cursor = 0;
        size_t version = 0;
        std::string token;
        if (!ReadCountLine(lines[cursor++], "SpiralFabImportReceipt", version)
            || version != kFabImportReceiptSchemaVersion
            || !ReadQuotedLine(lines[cursor++], "ProductIdentity", candidate.ProductIdentity)
            || !ReadQuotedLine(lines[cursor++], "ProductName", candidate.ProductName)
            || !ReadQuotedLine(lines[cursor++], "Publisher", candidate.Publisher)
            || !ReadQuotedLine(lines[cursor++], "VersionOrDownloadLabel", candidate.VersionOrDownloadLabel)
            || !ReadTokenLine(lines[cursor++], "PackageFormat", token)
            || !ParsePackageFormat(token, candidate.PackageFormat)
            || !ReadTokenLine(lines[cursor++], "LicenseFamily", token)
            || !ParseLicenseFamily(token, candidate.LicenseFamily)
            || !ReadTokenLine(lines[cursor++], "LicenseTier", token)
            || !ParseLicenseTier(token, candidate.LicenseTier)
            || !ReadQuotedLine(lines[cursor++], "AttributionText", candidate.AttributionText)
            || !ReadQuotedLine(lines[cursor++], "AttributionLink", candidate.AttributionLink)
            || !ReadTokenLine(lines[cursor++], "MetadataConfirmedByUser", token)
            || token != "true"
            || !ReadTokenLine(lines[cursor++], "NoAI", token)
            || !ParseMetadataFlag(token, candidate.NoAI)
            || !ReadTokenLine(lines[cursor++], "GeneratedWithAI", token)
            || !ParseMetadataFlag(token, candidate.GeneratedWithAI)
            || !ReadTokenLine(lines[cursor++], "SourceDigestKind", token)
            || !ParseDigestKind(token, candidate.SourceDigestKind)
            || !ReadTokenLine(lines[cursor++], "SourceSHA256", candidate.SourceSha256)
            || !ReadTokenLine(lines[cursor++], "ExpandedTreeSHA256", candidate.ExpandedTreeSha256)
            || !ReadQuotedLine(lines[cursor++], "ImporterVersion", candidate.ImporterVersion)
            || !ReadQuotedLine(lines[cursor++], "CookerVersion", candidate.CookerVersion)
            || !ReadQuotedLine(lines[cursor++], "StreamId", candidate.StreamId)
            || !ReadQuotedLine(lines[cursor++], "GenerationId", candidate.GenerationId)
            || !ReadQuotedLine(lines[cursor++], "DiagnosticAcquiredAtUTC", candidate.DiagnosticAcquiredAtUtc)
            || !ReadTokenLine(lines[cursor++], "RawSourcePolicy", token)
            || !ParseRawSourcePolicy(token, candidate.RawSourcePolicy)
            || !ReadTokenLine(lines[cursor++], "Relation", token)
            || !ParseRelation(token, candidate.Relation)
            || !ReadQuotedLine(lines[cursor++], "RelatedStreamId", candidate.RelatedStreamId)
            || !ReadQuotedLine(lines[cursor++], "RelatedGenerationId", candidate.RelatedGenerationId))
            return Fail(outError, "Fab receipt contains an unknown, duplicate, malformed, or out-of-order field");
        candidate.MetadataConfirmedByUser = true;
        candidate.SchemaVersion = static_cast<u32>(version);

        size_t assetCount = 0;
        if (cursor >= lines.size() || !ReadCountLine(lines[cursor++], "AssetCount", assetCount)
            || assetCount == 0 || assetCount > kMaxReceiptAssets
            || lines.size() != cursor + assetCount + 1)
            return Fail(outError, "Fab receipt asset count or framing is malformed");
        candidate.Assets.resize(assetCount);
        for (FabImportedAssetRecord& asset : candidate.Assets)
        {
            std::istringstream stream { std::string(lines[cursor++]) };
            stream.imbue(std::locale::classic());
            std::string key;
            std::string type;
            if (!(stream >> key >> asset.Handle >> type >> std::quoted(asset.SemanticRole)
                >> std::quoted(asset.LogicalPath) >> std::quoted(asset.GenerationRelativeCookedPath)
                >> asset.ArtifactSha256) || key != "Asset" || !IsAtEnd(stream))
                return Fail(outError, "Fab receipt asset record is malformed");
            asset.Type = ParseAssetType(type);
        }
        if (lines[cursor] != "End" || !ValidateFabImportReceipt(candidate, outError))
            return lines[cursor] == "End" ? false : Fail(outError, "Fab receipt has unknown trailing data");

        candidate = CanonicalReceipt(std::move(candidate));
        std::string canonicalBytes;
        if (!SerializeFabImportReceipt(candidate, canonicalBytes, outError) || canonicalBytes != bytes)
            return Fail(outError, "Fab receipt is not in canonical schema-1 form");
        outReceipt = std::move(candidate);
        outError.clear();
        return true;
    }

    bool StoreFabImportReceipt(
        const std::filesystem::path& path, const FabImportReceipt& receipt, std::string& outError)
    {
        std::string bytes;
        return SerializeFabImportReceipt(receipt, bytes, outError)
            && WriteBytesAtomically(path, bytes, kMaxReceiptBytes, outError);
    }

    bool LoadFabImportReceipt(
        const std::filesystem::path& path, FabImportReceipt& outReceipt, std::string& outError)
    {
        std::string bytes;
        return ReadBoundedFile(path, kMaxReceiptBytes, bytes, outError)
            && DeserializeFabImportReceipt(bytes, outReceipt, outError);
    }

    bool ValidateFabReceiptCollection(const FabReceiptCollection& collection, std::string& outError)
    {
        if (collection.SchemaVersion != kFabReceiptCollectionSchemaVersion)
            return Fail(outError, "unsupported Fab receipt collection schema");
        if (collection.Receipts.size() > kMaxCollectionReceipts)
            return Fail(outError, "Fab receipt collection exceeds its receipt bound");

        std::map<ReceiptKey, const FabImportReceipt*> receiptsByKey;
        std::map<std::string, std::vector<const FabImportReceipt*>> streams;
        std::map<std::string, std::set<std::string>> productStreams;
        std::map<AssetHandle, std::pair<std::string, FabImportedAssetRecord>> globalAssets;
        std::map<std::pair<std::string, std::string>, FabImportedAssetRecord> streamRoles;
        std::map<std::pair<std::string, std::string>, FabImportedAssetRecord> streamLogicalPaths;
        std::set<std::tuple<std::string, std::string, FabPackageFormat, std::string>> reimportIdentities;
        size_t totalAssetCount = 0;
        for (const FabImportReceipt& receipt : collection.Receipts)
        {
            if (!ValidateFabImportReceipt(receipt, outError))
                return false;
            const ReceiptKey key { receipt.StreamId, receipt.GenerationId };
            if (!receiptsByKey.emplace(key, &receipt).second)
                return Fail(outError, "Fab receipt collection has a duplicate stream/generation key");
            streams[receipt.StreamId].push_back(&receipt);
            productStreams[receipt.ProductIdentity].insert(receipt.StreamId);
            if (receipt.Assets.size() > kMaxCollectionAssets - totalAssetCount)
                return Fail(outError, "Fab receipt collection exceeds its total asset-record bound");
            totalAssetCount += receipt.Assets.size();
            if (!reimportIdentities.emplace(receipt.ProductIdentity,
                    receipt.VersionOrDownloadLabel, receipt.PackageFormat, receipt.SourceSha256).second)
                return Fail(outError, "Fab receipt collection persists an exact reimport identity more than once");
            for (const FabImportedAssetRecord& asset : receipt.Assets)
            {
                const auto found = globalAssets.find(asset.Handle);
                if (found == globalAssets.end())
                    globalAssets.emplace(asset.Handle, std::make_pair(receipt.StreamId, asset));
                else if (found->second.first != receipt.StreamId)
                    return Fail(outError, "Fab asset handle aliases different provenance streams");
                else if (found->second.second.Type != asset.Type
                    || found->second.second.SemanticRole != asset.SemanticRole
                    || found->second.second.LogicalPath != asset.LogicalPath)
                    return Fail(outError, "Fab asset handle changes stable type, role, or logical path within a stream");

                const auto [role, insertedRole] = streamRoles.emplace(
                    std::make_pair(receipt.StreamId, asset.SemanticRole), asset);
                if (!insertedRole && (role->second.Handle != asset.Handle
                    || role->second.Type != asset.Type || role->second.LogicalPath != asset.LogicalPath))
                    return Fail(outError, "Fab semantic role changes stable asset identity within a stream");
                const auto [logicalPath, insertedPath] = streamLogicalPaths.emplace(
                    std::make_pair(receipt.StreamId, asset.LogicalPath), asset);
                if (!insertedPath && (logicalPath->second.Handle != asset.Handle
                    || logicalPath->second.Type != asset.Type || logicalPath->second.SemanticRole != asset.SemanticRole))
                    return Fail(outError, "Fab logical path changes stable asset identity within a stream");
            }
        }

        std::map<std::string, const FabImportReceipt*> streamTips;
        std::map<ReceiptKey, const FabImportReceipt*> productChildren;
        for (const auto& [streamId, streamReceipts] : streams)
        {
            const FabImportReceipt* root = nullptr;
            std::map<std::string, const FabImportReceipt*> replacementChildren;
            for (const FabImportReceipt* receipt : streamReceipts)
            {
                if (receipt->Relation == FabGenerationRelation::Initial
                    || receipt->Relation == FabGenerationRelation::ProductUpdate)
                {
                    if (root)
                        return Fail(outError, "Fab provenance stream has multiple roots");
                    root = receipt;
                    continue;
                }
                const auto parent = receiptsByKey.find({ receipt->RelatedStreamId, receipt->RelatedGenerationId });
                if (parent == receiptsByKey.end() || parent->second->StreamId != streamId
                    || !SameStreamProvenance(*parent->second, *receipt)
                    || !SameStableAssetSet(*parent->second, *receipt)
                    || parent->second->SourceSha256 == receipt->SourceSha256)
                    return Fail(outError, "Fab source replacement has an impossible parent or unchanged source");
                if (!replacementChildren.emplace(receipt->RelatedGenerationId, receipt).second)
                    return Fail(outError, "Fab provenance stream forks one source generation");
            }
            if (!root)
                return Fail(outError, "Fab provenance stream has no root");
            size_t visited = 1;
            const FabImportReceipt* tip = root;
            for (;;)
            {
                const auto child = replacementChildren.find(tip->GenerationId);
                if (child == replacementChildren.end())
                    break;
                tip = child->second;
                if (++visited > streamReceipts.size())
                    return Fail(outError, "Fab provenance stream contains a generation cycle");
            }
            if (visited != streamReceipts.size())
                return Fail(outError, "Fab provenance stream contains an unreachable generation");
            streamTips.emplace(streamId, tip);
        }

        for (const auto& stream : streams)
        {
            const FabImportReceipt* root = nullptr;
            for (const FabImportReceipt* receipt : stream.second)
                if (receipt->Relation != FabGenerationRelation::SourceReplacement)
                    root = receipt;
            if (!root || root->Relation != FabGenerationRelation::ProductUpdate)
                continue;
            const auto parent = receiptsByKey.find({ root->RelatedStreamId, root->RelatedGenerationId });
            const auto parentTip = streamTips.find(root->RelatedStreamId);
            if (parent == receiptsByKey.end() || parentTip == streamTips.end()
                || parent->second != parentTip->second
                || parent->second->ProductIdentity != root->ProductIdentity
                || (parent->second->VersionOrDownloadLabel == root->VersionOrDownloadLabel
                    && parent->second->PackageFormat == root->PackageFormat))
                return Fail(outError, "Fab product update has an impossible or unchanged parent");
            const ReceiptKey parentKey { root->RelatedStreamId, root->RelatedGenerationId };
            if (!productChildren.emplace(parentKey, root).second)
                return Fail(outError, "Fab product provenance forks one accepted generation");
        }

        for (const auto& [productIdentity, productStreamIds] : productStreams)
        {
            const FabImportReceipt* initialRoot = nullptr;
            for (const std::string& streamId : productStreamIds)
                for (const FabImportReceipt* receipt : streams.at(streamId))
                    if (receipt->Relation == FabGenerationRelation::Initial)
                    {
                        if (initialRoot)
                            return Fail(outError, "Fab product provenance has multiple initial streams");
                        initialRoot = receipt;
                    }
            if (!initialRoot)
                return Fail(outError, "Fab product provenance has no initial stream");

            size_t visitedStreams = 1;
            const FabImportReceipt* tip = streamTips.at(initialRoot->StreamId);
            for (;;)
            {
                const auto child = productChildren.find({ tip->StreamId, tip->GenerationId });
                if (child == productChildren.end())
                    break;
                if (child->second->ProductIdentity != productIdentity)
                    return Fail(outError, "Fab product update crosses product identities");
                tip = streamTips.at(child->second->StreamId);
                if (++visitedStreams > productStreamIds.size())
                    return Fail(outError, "Fab product provenance contains an update cycle");
            }
            if (visitedStreams != productStreamIds.size())
                return Fail(outError, "Fab product provenance contains an unreachable update stream");
        }

        outError.clear();
        return true;
    }

    bool SerializeFabReceiptCollection(
        const FabReceiptCollection& collection, std::string& outBytes, std::string& outError)
    {
        if (!ValidateFabReceiptCollection(collection, outError))
            return false;
        const FabReceiptCollection canonical = CanonicalCollection(collection);
        std::string candidate = "SpiralFabReceiptCollection "
            + std::to_string(kFabReceiptCollectionSchemaVersion) + "\nReceiptCount "
            + std::to_string(canonical.Receipts.size()) + "\n";
        for (const FabImportReceipt& receipt : canonical.Receipts)
        {
            std::string receiptBytes;
            if (!SerializeFabImportReceipt(receipt, receiptBytes, outError))
                return false;
            constexpr size_t boundaryBytes = sizeof("ReceiptBegin\nReceiptEnd\n") - 1;
            if (receiptBytes.size() > kMaxCollectionBytes - candidate.size()
                || boundaryBytes > kMaxCollectionBytes - candidate.size() - receiptBytes.size())
                return Fail(outError, "Fab receipt collection exceeds the bounded serialized size");
            candidate += "ReceiptBegin\n";
            candidate += receiptBytes;
            candidate += "ReceiptEnd\n";
        }
        constexpr size_t endBytes = sizeof("End\n") - 1;
        if (candidate.size() > kMaxCollectionBytes - endBytes)
            return Fail(outError, "Fab receipt collection exceeds the bounded serialized size");
        candidate += "End\n";
        outBytes = candidate;
        outError.clear();
        return true;
    }

    bool DeserializeFabReceiptCollection(
        std::string_view bytes, FabReceiptCollection& outCollection, std::string& outError)
    {
        std::vector<std::string_view> lines;
        if (!SplitLines(bytes, kMaxCollectionBytes, kMaxCollectionLines, lines)
            || lines.size() < 3)
            return Fail(outError, "Fab receipt collection framing is malformed or truncated");
        size_t cursor = 0;
        size_t version = 0;
        size_t receiptCount = 0;
        if (!ReadCountLine(lines[cursor++], "SpiralFabReceiptCollection", version)
            || version != kFabReceiptCollectionSchemaVersion
            || !ReadCountLine(lines[cursor++], "ReceiptCount", receiptCount)
            || receiptCount > kMaxCollectionReceipts)
            return Fail(outError, "Fab receipt collection header is malformed or unsupported");

        FabReceiptCollection candidate;
        candidate.SchemaVersion = static_cast<u32>(version);
        candidate.Receipts.reserve(receiptCount);
        for (size_t receiptIndex = 0; receiptIndex < receiptCount; ++receiptIndex)
        {
            if (cursor >= lines.size() || lines[cursor++] != "ReceiptBegin")
                return Fail(outError, "Fab receipt collection is missing a receipt boundary");
            std::string receiptBytes;
            while (cursor < lines.size() && lines[cursor] != "ReceiptEnd")
            {
                receiptBytes.append(lines[cursor]);
                receiptBytes.push_back('\n');
                ++cursor;
            }
            if (cursor >= lines.size() || lines[cursor++] != "ReceiptEnd")
                return Fail(outError, "Fab receipt collection has a truncated receipt boundary");
            FabImportReceipt receipt;
            if (!DeserializeFabImportReceipt(receiptBytes, receipt, outError))
                return false;
            candidate.Receipts.push_back(std::move(receipt));
        }
        if (cursor >= lines.size() || lines[cursor++] != "End" || cursor != lines.size())
            return Fail(outError, "Fab receipt collection has unknown trailing data");
        if (!ValidateFabReceiptCollection(candidate, outError))
            return false;
        candidate = CanonicalCollection(std::move(candidate));
        std::string canonicalBytes;
        if (!SerializeFabReceiptCollection(candidate, canonicalBytes, outError) || canonicalBytes != bytes)
            return Fail(outError, "Fab receipt collection is not in canonical schema-1 form");
        outCollection = std::move(candidate);
        outError.clear();
        return true;
    }

    bool StoreFabReceiptCollection(
        const std::filesystem::path& path, const FabReceiptCollection& collection, std::string& outError)
    {
        std::string bytes;
        return SerializeFabReceiptCollection(collection, bytes, outError)
            && WriteBytesAtomically(path, bytes, kMaxCollectionBytes, outError);
    }

    bool LoadFabReceiptCollection(
        const std::filesystem::path& path, FabReceiptCollection& outCollection, std::string& outError)
    {
        std::string bytes;
        return ReadBoundedFile(path, kMaxCollectionBytes, bytes, outError)
            && DeserializeFabReceiptCollection(bytes, outCollection, outError);
    }

    FabReceiptDecision ClassifyFabImportReceipt(
        const FabReceiptCollection& collection, const FabImportReceipt& candidate)
    {
        std::string error;
        if (!ValidateFabReceiptCollection(collection, error))
            return Decision(FabReceiptDecisionKind::CorruptPriorState, std::move(error));
        if (!ValidateFabImportReceipt(candidate, error))
            return Decision(FabReceiptDecisionKind::InvalidCandidate, std::move(error));

        if (const FabImportReceipt* exactKey = FindReceipt(collection, candidate.StreamId, candidate.GenerationId))
        {
            if (SameReimportIdentity(*exactKey, candidate) && ReuseCompatible(*exactKey, candidate))
                return Decision(FabReceiptDecisionKind::ExactReuse, "the exact receipt already exists", exactKey);
            return Decision(FabReceiptDecisionKind::Conflict,
                "the stream/generation key already names different receipt data", exactKey);
        }

        for (const FabImportReceipt& existing : collection.Receipts)
        {
            if (!SameReimportIdentity(existing, candidate))
                continue;
            if (ReuseCompatible(existing, candidate))
                return Decision(FabReceiptDecisionKind::ExactReuse,
                    "the same source and compatible provenance already have accepted artifacts", &existing);
            return Decision(FabReceiptDecisionKind::Conflict,
                "the same reimport identity has incompatible provenance, license, tools, or artifacts", &existing);
        }

        const bool streamExists = std::any_of(collection.Receipts.begin(), collection.Receipts.end(),
            [&candidate](const FabImportReceipt& receipt) { return receipt.StreamId == candidate.StreamId; });
        if (streamExists)
        {
            const FabImportReceipt* tip = FindStreamTip(collection, candidate.StreamId);
            if (!tip || candidate.Relation != FabGenerationRelation::SourceReplacement
                || candidate.RelatedStreamId != candidate.StreamId
                || candidate.RelatedGenerationId != tip->GenerationId
                || !SameStreamProvenance(*tip, candidate)
                || tip->SourceSha256 == candidate.SourceSha256)
                return Decision(FabReceiptDecisionKind::Conflict,
                    "same-stream import is not a valid changed-source replacement", tip);

            FabReceiptCollection proposed = collection;
            proposed.Receipts.push_back(candidate);
            if (!ValidateFabReceiptCollection(proposed, error))
                return Decision(FabReceiptDecisionKind::Conflict, std::move(error), tip);
            return Decision(FabReceiptDecisionKind::ReplaceSameStreamSource,
                "changed source can replace the current generation after caller transaction success", tip);
        }

        const bool productExists = std::any_of(collection.Receipts.begin(), collection.Receipts.end(),
            [&candidate](const FabImportReceipt& receipt)
            {
                return receipt.ProductIdentity == candidate.ProductIdentity;
            });
        FabReceiptDecisionKind acceptedKind = FabReceiptDecisionKind::AddNewStream;
        const FabImportReceipt* related = nullptr;
        if (productExists)
        {
            related = FindProductTip(collection, candidate.ProductIdentity);
            if (!related || candidate.Relation != FabGenerationRelation::ProductUpdate
                || candidate.RelatedStreamId != related->StreamId
                || candidate.RelatedGenerationId != related->GenerationId
                || (candidate.VersionOrDownloadLabel == related->VersionOrDownloadLabel
                    && candidate.PackageFormat == related->PackageFormat))
                return Decision(FabReceiptDecisionKind::Conflict,
                    "existing product requires an explicitly related version/format update stream", related);
            acceptedKind = FabReceiptDecisionKind::AddProductUpdateStream;
        }
        else if (candidate.Relation != FabGenerationRelation::Initial)
            return Decision(FabReceiptDecisionKind::Conflict,
                "a new product must begin with an initial provenance stream");

        FabReceiptCollection proposed = collection;
        proposed.Receipts.push_back(candidate);
        if (!ValidateFabReceiptCollection(proposed, error))
            return Decision(FabReceiptDecisionKind::Conflict, std::move(error), related);
        return Decision(acceptedKind,
            acceptedKind == FabReceiptDecisionKind::AddNewStream
                ? "new product provenance stream can be added after caller transaction success"
                : "product update stream can be added after caller transaction success",
            related);
    }

    bool AddFabImportReceipt(
        FabReceiptCollection& collection, const FabImportReceipt& candidate,
        FabReceiptDecision& outDecision, std::string& outError)
    {
        FabReceiptDecision decision = ClassifyFabImportReceipt(collection, candidate);
        if (decision.Kind == FabReceiptDecisionKind::ExactReuse)
        {
            outDecision = std::move(decision);
            outError.clear();
            return true;
        }
        if (decision.Kind != FabReceiptDecisionKind::AddNewStream
            && decision.Kind != FabReceiptDecisionKind::ReplaceSameStreamSource
            && decision.Kind != FabReceiptDecisionKind::AddProductUpdateStream)
        {
            outError = decision.Diagnostic;
            outDecision = std::move(decision);
            return false;
        }

        FabReceiptCollection proposed = collection;
        proposed.Receipts.push_back(candidate);
        if (!ValidateFabReceiptCollection(proposed, outError))
        {
            outDecision = Decision(FabReceiptDecisionKind::Conflict, outError);
            return false;
        }
        proposed = CanonicalCollection(std::move(proposed));
        collection = std::move(proposed);
        outDecision = std::move(decision);
        outError.clear();
        return true;
    }
}
