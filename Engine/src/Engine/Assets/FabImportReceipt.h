#pragma once

#include "Engine/Assets/AssetRegistry.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    inline constexpr u32 kFabImportReceiptSchemaVersion = 1;
    inline constexpr u32 kFabReceiptCollectionSchemaVersion = 1;

    enum class FabPackageFormat
    {
        Unknown,
        Gltf,
        Glb
    };

    enum class FabLicenseFamily
    {
        Unknown,
        FabStandard,
        CreativeCommonsAttribution,
        LegacyUnrealMarketplace,
        ReferenceOnly
    };

    enum class FabLicenseTier
    {
        Unknown,
        NotApplicable,
        Personal,
        Professional
    };

    // Fab can omit either declaration. Unknown is intentionally durable and
    // distinct from a confirmed No value; receipt construction must not guess.
    enum class FabMetadataFlag
    {
        Unknown,
        No,
        Yes
    };

    enum class FabDigestKind
    {
        Unknown,
        Sha256
    };

    enum class FabRawSourcePolicy
    {
        Unknown,
        ExcludedFromProject,
        PrivateProjectOnly
    };

    enum class FabGenerationRelation
    {
        Unknown,
        Initial,
        SourceReplacement,
        ProductUpdate
    };

    struct FabImportedAssetRecord
    {
        AssetHandle Handle = kInvalidAssetHandle;
        AssetType Type = AssetType::Unknown;
        std::string SemanticRole;
        std::string LogicalPath;
        std::string GenerationRelativeCookedPath;
        std::string ArtifactSha256;

        bool operator==(const FabImportedAssetRecord&) const = default;
    };

    struct FabImportReceipt
    {
        u32 SchemaVersion = kFabImportReceiptSchemaVersion;

        // ProductIdentity is the canonical https://www.fab.com/listings/<id>
        // URL. It is never a local source path and no local-path field exists
        // in this durable model.
        std::string ProductIdentity;
        std::string ProductName;
        std::string Publisher;
        std::string VersionOrDownloadLabel;
        FabPackageFormat PackageFormat = FabPackageFormat::Unknown;

        FabLicenseFamily LicenseFamily = FabLicenseFamily::Unknown;
        FabLicenseTier LicenseTier = FabLicenseTier::Unknown;
        std::string AttributionText;
        std::string AttributionLink;
        bool MetadataConfirmedByUser = false;
        FabMetadataFlag NoAI = FabMetadataFlag::Unknown;
        FabMetadataFlag GeneratedWithAI = FabMetadataFlag::Unknown;

        FabDigestKind SourceDigestKind = FabDigestKind::Unknown;
        std::string SourceSha256;
        std::string ExpandedTreeSha256;
        std::string ImporterVersion;
        std::string CookerVersion;

        std::string StreamId;
        std::string GenerationId;
        std::string DiagnosticAcquiredAtUtc;
        FabRawSourcePolicy RawSourcePolicy = FabRawSourcePolicy::Unknown;
        FabGenerationRelation Relation = FabGenerationRelation::Unknown;
        std::string RelatedStreamId;
        std::string RelatedGenerationId;

        std::vector<FabImportedAssetRecord> Assets;

        bool operator==(const FabImportReceipt&) const = default;
    };

    struct FabReceiptCollection
    {
        u32 SchemaVersion = kFabReceiptCollectionSchemaVersion;
        std::vector<FabImportReceipt> Receipts;

        bool operator==(const FabReceiptCollection&) const = default;
    };

    enum class FabReceiptDecisionKind
    {
        InvalidCandidate,
        CorruptPriorState,
        ExactReuse,
        AddNewStream,
        ReplaceSameStreamSource,
        AddProductUpdateStream,
        Conflict
    };

    struct FabReceiptDecision
    {
        FabReceiptDecisionKind Kind = FabReceiptDecisionKind::InvalidCandidate;
        std::string ExistingStreamId;
        std::string ExistingGenerationId;
        std::string Diagnostic;
    };

    const char* ToString(FabPackageFormat value);
    const char* ToString(FabLicenseFamily value);
    const char* ToString(FabLicenseTier value);
    const char* ToString(FabMetadataFlag value);
    const char* ToString(FabDigestKind value);
    const char* ToString(FabRawSourcePolicy value);
    const char* ToString(FabGenerationRelation value);
    const char* ToString(FabReceiptDecisionKind value);

    // IDs are domain-separated SHA-256 values over canonical newline-delimited
    // fields. Invalid inputs return an empty string/invalid handle.
    std::string ComputeFabStreamId(std::string_view productIdentity,
        std::string_view versionOrDownloadLabel, FabPackageFormat format);
    std::string ComputeFabGenerationId(std::string_view streamId,
        std::string_view sourceSha256, std::string_view expandedTreeSha256);
    AssetHandle ComputeFabStableAssetHandle(
        std::string_view streamId, AssetType type, std::string_view semanticRole);

    bool ValidateFabImportReceipt(const FabImportReceipt& receipt, std::string& outError);
    bool SerializeFabImportReceipt(
        const FabImportReceipt& receipt, std::string& outBytes, std::string& outError);
    bool DeserializeFabImportReceipt(
        std::string_view bytes, FabImportReceipt& outReceipt, std::string& outError);
    // Store atomically replaces this one destination file. It is deliberately
    // not a claim of a registry/artifact/Scene project transaction.
    bool StoreFabImportReceipt(
        const std::filesystem::path& path, const FabImportReceipt& receipt, std::string& outError);
    bool LoadFabImportReceipt(
        const std::filesystem::path& path, FabImportReceipt& outReceipt, std::string& outError);

    bool ValidateFabReceiptCollection(const FabReceiptCollection& collection, std::string& outError);
    bool SerializeFabReceiptCollection(
        const FabReceiptCollection& collection, std::string& outBytes, std::string& outError);
    bool DeserializeFabReceiptCollection(
        std::string_view bytes, FabReceiptCollection& outCollection, std::string& outError);
    // As above, atomicity is scoped to this one collection file.
    bool StoreFabReceiptCollection(
        const std::filesystem::path& path, const FabReceiptCollection& collection, std::string& outError);
    bool LoadFabReceiptCollection(
        const std::filesystem::path& path, FabReceiptCollection& outCollection, std::string& outError);

    // Classify is pure. Add performs an in-memory transactional publication:
    // exact reuse is a successful no-op, accepted new generations replace the
    // collection only after complete validation, and every other result leaves
    // the caller's collection unchanged.
    FabReceiptDecision ClassifyFabImportReceipt(
        const FabReceiptCollection& collection, const FabImportReceipt& candidate);
    bool AddFabImportReceipt(
        FabReceiptCollection& collection, const FabImportReceipt& candidate,
        FabReceiptDecision& outDecision, std::string& outError);
}
