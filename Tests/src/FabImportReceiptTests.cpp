#include "FabImportReceiptTests.h"

#include "Engine/Assets/FabImportReceipt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>

namespace Spiral::Tests
{
    namespace
    {
        std::string Digest(char value)
        {
            return std::string(64, value);
        }

        void RefreshGenerationIdentity(Engine::FabImportReceipt& receipt)
        {
            receipt.GenerationId = Engine::ComputeFabGenerationId(
                receipt.StreamId, receipt.SourceSha256, receipt.ExpandedTreeSha256);
        }

        void RefreshStreamIdentity(Engine::FabImportReceipt& receipt)
        {
            using namespace Engine;
            receipt.StreamId = ComputeFabStreamId(
                receipt.ProductIdentity, receipt.VersionOrDownloadLabel, receipt.PackageFormat);
            for (FabImportedAssetRecord& asset : receipt.Assets)
            {
                asset.Handle = ComputeFabStableAssetHandle(
                    receipt.StreamId, asset.Type, asset.SemanticRole);
                const std::string directory = asset.Type == AssetType::Mesh ? "meshes/" : "textures/";
                const std::string extension = asset.Type == AssetType::Mesh
                    ? ".spiralmesh" : ".rgba-fallback.spiraltexture";
                asset.GenerationRelativeCookedPath = directory + std::to_string(asset.Handle) + extension;
            }
            RefreshGenerationIdentity(receipt);
        }

        Engine::FabImportReceipt MakeReceipt()
        {
            using namespace Engine;
            FabImportReceipt receipt;
            receipt.ProductIdentity = "https://www.fab.com/listings/11111111-2222-3333-4444-555555555555";
            receipt.ProductName = "Ancient Statue";
            receipt.Publisher = "Example Studio";
            receipt.VersionOrDownloadLabel = "1.2-glb";
            receipt.PackageFormat = FabPackageFormat::Glb;
            receipt.LicenseFamily = FabLicenseFamily::FabStandard;
            receipt.LicenseTier = FabLicenseTier::Personal;
            receipt.MetadataConfirmedByUser = true;
            receipt.NoAI = FabMetadataFlag::Unknown;
            receipt.GeneratedWithAI = FabMetadataFlag::No;
            receipt.SourceDigestKind = FabDigestKind::Sha256;
            receipt.SourceSha256 = Digest('a');
            receipt.ExpandedTreeSha256 = Digest('b');
            receipt.ImporterVersion = "SpiralFabImporter/1";
            receipt.CookerVersion = "SpiralAssetCooker/3";
            receipt.DiagnosticAcquiredAtUtc = "2026-09-05T10:20:30Z";
            receipt.RawSourcePolicy = FabRawSourcePolicy::ExcludedFromProject;
            receipt.Relation = FabGenerationRelation::Initial;
            receipt.Assets = {
                { 0, AssetType::Texture, "Texture.BaseColor", "Textures/base.png", {}, Digest('d') },
                { 0, AssetType::Mesh, "Mesh.Primary", "Models/statue.glb", {}, Digest('c') }
            };
            RefreshStreamIdentity(receipt);
            return receipt;
        }

        std::string ReadFile(const std::filesystem::path& path)
        {
            std::ifstream input(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }

        bool ReplaceOnce(std::string& text, std::string_view from, std::string_view to)
        {
            const size_t offset = text.find(from);
            if (offset == std::string::npos)
                return false;
            text.replace(offset, from.size(), to);
            return true;
        }

        bool CreateUniqueTemporaryRoot(std::filesystem::path& output)
        {
            static std::atomic<unsigned long long> sequence { 0 };
            std::error_code filesystemError;
            const std::filesystem::path temporaryDirectory
                = std::filesystem::temp_directory_path(filesystemError);
            if (filesystemError)
                return false;
            const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
            for (size_t attempt = 0; attempt < 16; ++attempt)
            {
                const std::filesystem::path candidate = temporaryDirectory
                    / ("spiral-fab-receipt-test-" + std::to_string(tick) + "-"
                        + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
                filesystemError.clear();
                if (std::filesystem::create_directory(candidate, filesystemError))
                {
                    output = candidate;
                    return true;
                }
                if (filesystemError)
                    return false;
            }
            return false;
        }
    }

    bool TestFabImportReceiptAuthority()
    {
        using namespace Engine;
        bool passed = true;
        const auto Check = [&passed](bool condition, std::string_view message)
        {
            if (!condition)
            {
                std::cerr << "Fab import receipt test failed: " << message << '\n';
                passed = false;
            }
        };

        FabImportReceipt receipt = MakeReceipt();
        std::string error;
        std::string bytes;
        Check(receipt.StreamId == "c5dd5cbc7d0f6ea45e74565c40dbbcaa687ea7877722284605c860b607e8ba18"
                && receipt.GenerationId == "e6a13e1dd20df25ddade3ea97b83265be1d9082f27e0002a6d328faa14da3cf0"
                && receipt.Assets[0].Handle == 3084667825722034383ull
                && receipt.Assets[1].Handle == 18215528761153011616ull,
            "stream, generation, and semantic handles match independent SHA-256 vectors");
        const std::string expected =
            "SpiralFabImportReceipt 1\n"
            "ProductIdentity \"https://www.fab.com/listings/11111111-2222-3333-4444-555555555555\"\n"
            "ProductName \"Ancient Statue\"\n"
            "Publisher \"Example Studio\"\n"
            "VersionOrDownloadLabel \"1.2-glb\"\n"
            "PackageFormat GLB\n"
            "LicenseFamily FabStandard\n"
            "LicenseTier Personal\n"
            "AttributionText \"\"\n"
            "AttributionLink \"\"\n"
            "MetadataConfirmedByUser true\n"
            "NoAI Unknown\n"
            "GeneratedWithAI No\n"
            "SourceDigestKind SHA-256\n"
            "SourceSHA256 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
            "ExpandedTreeSHA256 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
            "ImporterVersion \"SpiralFabImporter/1\"\n"
            "CookerVersion \"SpiralAssetCooker/3\"\n"
            "StreamId \"c5dd5cbc7d0f6ea45e74565c40dbbcaa687ea7877722284605c860b607e8ba18\"\n"
            "GenerationId \"e6a13e1dd20df25ddade3ea97b83265be1d9082f27e0002a6d328faa14da3cf0\"\n"
            "DiagnosticAcquiredAtUTC \"2026-09-05T10:20:30Z\"\n"
            "RawSourcePolicy ExcludedFromProject\n"
            "Relation Initial\n"
            "RelatedStreamId \"\"\n"
            "RelatedGenerationId \"\"\n"
            "AssetCount 2\n"
            "Asset 3084667825722034383 Texture \"Texture.BaseColor\" \"Textures/base.png\" "
            "\"textures/3084667825722034383.rgba-fallback.spiraltexture\" "
            "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\n"
            "Asset 18215528761153011616 Mesh \"Mesh.Primary\" \"Models/statue.glb\" "
            "\"meshes/18215528761153011616.spiralmesh\" "
            "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
            "End\n";
        Check(SerializeFabImportReceipt(receipt, bytes, error) && bytes == expected,
            "schema-1 serialization is exact and asset-order independent");
        FabImportReceipt reversedAssets = receipt;
        std::reverse(reversedAssets.Assets.begin(), reversedAssets.Assets.end());
        std::string reversedBytes;
        Check(SerializeFabImportReceipt(reversedAssets, reversedBytes, error)
                && reversedBytes == expected,
            "schema-1 asset ordering is independent of input order");
        FabImportReceipt roundTrip;
        Check(DeserializeFabImportReceipt(bytes, roundTrip, error)
                && SerializeFabImportReceipt(roundTrip, bytes, error) && bytes == expected,
            "canonical bytes round-trip exactly");

        FabImportReceipt sentinel = MakeReceipt();
        sentinel.ProductIdentity = "sentinel";
        const auto RejectsTransactionally = [&sentinel](std::string malformed)
        {
            FabImportReceipt output = sentinel;
            std::string parseError;
            return !DeserializeFabImportReceipt(malformed, output, parseError)
                && !parseError.empty() && output == sentinel;
        };
        std::string malformed = expected;
        malformed.pop_back();
        Check(RejectsTransactionally(malformed), "truncated framing is rejected transactionally");
        malformed = expected;
        ReplaceOnce(malformed, "SpiralFabImportReceipt 1", "SpiralFabImportReceipt 2");
        Check(RejectsTransactionally(malformed), "unknown schema is rejected transactionally");
        malformed = expected;
        ReplaceOnce(malformed, "Publisher ", "UnknownPublisherField ");
        Check(RejectsTransactionally(malformed), "unknown keys are rejected transactionally");
        malformed = expected;
        ReplaceOnce(malformed, "ProductName \"Ancient Statue\"", "ProductIdentity \"duplicate\"");
        Check(RejectsTransactionally(malformed), "duplicate keys are rejected transactionally");
        malformed = expected + "Unexpected true\n";
        Check(RejectsTransactionally(malformed), "trailing records are rejected transactionally");

        FabImportReceipt invalid = receipt;
        invalid.MetadataConfirmedByUser = false;
        Check(!ValidateFabImportReceipt(invalid, error), "unconfirmed metadata fails closed");
        invalid = receipt;
        invalid.LicenseTier = FabLicenseTier::NotApplicable;
        Check(!ValidateFabImportReceipt(invalid, error), "Fab Standard rejects an inapplicable tier");
        invalid = receipt;
        invalid.LicenseFamily = FabLicenseFamily::ReferenceOnly;
        Check(!ValidateFabImportReceipt(invalid, error), "Reference Only is never admitted");
        FabImportReceipt ccBy = receipt;
        ccBy.LicenseFamily = FabLicenseFamily::CreativeCommonsAttribution;
        ccBy.LicenseTier = FabLicenseTier::NotApplicable;
        Check(!ValidateFabImportReceipt(ccBy, error), "CC-BY rejects missing attribution");
        ccBy.AttributionText = "Example Studio, Ancient Statue, CC BY 4.0";
        ccBy.AttributionLink = "https://creativecommons.org/licenses/by/4.0/";
        Check(ValidateFabImportReceipt(ccBy, error), "CC-BY accepts complete secure attribution");
        ccBy.AttributionLink = "https://creativecommons.org/licenses/by/4.0/?source=fab#terms";
        Check(ValidateFabImportReceipt(ccBy, error), "secure attribution links may retain query and fragment context");
        FabImportReceipt legacy = receipt;
        legacy.LicenseFamily = FabLicenseFamily::LegacyUnrealMarketplace;
        legacy.LicenseTier = FabLicenseTier::NotApplicable;
        Check(ValidateFabImportReceipt(legacy, error),
            "legacy Unreal Marketplace provenance is represented without a Fab tier");
        ccBy.AttributionLink = "http://creativecommons.org/licenses/by/4.0/";
        Check(!ValidateFabImportReceipt(ccBy, error), "CC-BY rejects insecure attribution links");

        invalid = receipt;
        invalid.ProductIdentity = "http://www.fab.com/listings/11111111-2222-3333-4444-555555555555";
        Check(!ValidateFabImportReceipt(invalid, error), "insecure Fab URLs are rejected");
        invalid.ProductIdentity = "https://example.com/listings/11111111-2222-3333-4444-555555555555";
        Check(!ValidateFabImportReceipt(invalid, error), "non-Fab product URLs are rejected");
        invalid = receipt;
        invalid.ProductIdentity = "11111111-2222-3333-4444-555555555555";
        Check(!ValidateFabImportReceipt(invalid, error),
            "noncanonical bare listing identifiers cannot split one product identity");
        bool requiredFieldsRejected = true;
        for (std::string FabImportReceipt::* field : {
                 &FabImportReceipt::ProductIdentity, &FabImportReceipt::Publisher,
                 &FabImportReceipt::VersionOrDownloadLabel, &FabImportReceipt::ImporterVersion,
                 &FabImportReceipt::CookerVersion, &FabImportReceipt::StreamId,
                 &FabImportReceipt::GenerationId })
        {
            invalid = receipt;
            (invalid.*field).clear();
            requiredFieldsRejected = requiredFieldsRejected
                && !ValidateFabImportReceipt(invalid, error);
        }
        Check(requiredFieldsRejected, "all required declared identity fields reject empty values");
        invalid = receipt;
        invalid.PackageFormat = static_cast<FabPackageFormat>(99);
        Check(!ValidateFabImportReceipt(invalid, error), "unknown enums fail closed");
        invalid = receipt;
        invalid.SourceSha256 = Digest('A');
        Check(!ValidateFabImportReceipt(invalid, error), "uppercase SHA-256 is rejected");
        invalid = receipt;
        invalid.SourceSha256.pop_back();
        Check(!ValidateFabImportReceipt(invalid, error), "short SHA-256 is rejected");
        invalid = receipt;
        invalid.ExpandedTreeSha256 = Digest('F');
        Check(!ValidateFabImportReceipt(invalid, error), "uppercase expanded-tree digests are rejected");
        invalid = receipt;
        invalid.Assets.front().ArtifactSha256 = Digest('C');
        Check(!ValidateFabImportReceipt(invalid, error), "uppercase artifact digests are rejected");
        invalid = receipt;
        invalid.StreamId = Digest('0');
        Check(!ValidateFabImportReceipt(invalid, error), "caller-chosen stream identities are rejected");
        invalid = receipt;
        invalid.GenerationId = Digest('0');
        Check(!ValidateFabImportReceipt(invalid, error), "caller-chosen generation identities are rejected");
        invalid = receipt;
        invalid.Assets.front().Handle ^= 1;
        Check(!ValidateFabImportReceipt(invalid, error), "caller-chosen semantic asset handles are rejected");
        invalid = receipt;
        invalid.DiagnosticAcquiredAtUtc = "2026-02-30T10:20:30Z";
        Check(!ValidateFabImportReceipt(invalid, error), "impossible diagnostic timestamps are rejected");
        invalid = receipt;
        invalid.RelatedStreamId = invalid.StreamId;
        invalid.RelatedGenerationId = Digest('0');
        Check(!ValidateFabImportReceipt(invalid, error), "an initial generation cannot claim a parent");
        invalid = receipt;
        invalid.Relation = FabGenerationRelation::SourceReplacement;
        invalid.RelatedStreamId = Digest('0');
        invalid.RelatedGenerationId = Digest('1');
        Check(!ValidateFabImportReceipt(invalid, error), "a replacement cannot cross streams");
        for (std::string_view attack : { "../escape.glb", "/absolute/escape.glb", "safe//escape.glb",
                 "C:/escape.glb", "safe/CON", "safe/trailing. " })
        {
            invalid = receipt;
            invalid.Assets.front().LogicalPath = attack;
            Check(!ValidateFabImportReceipt(invalid, error), "unsafe logical paths are rejected");
            invalid = receipt;
            invalid.Assets.front().GenerationRelativeCookedPath = attack;
            Check(!ValidateFabImportReceipt(invalid, error), "unsafe generation-relative cooked paths are rejected");
        }
        invalid = receipt;
        invalid.Assets.front().LogicalPath = "models/STATUE.glb";
        Check(!ValidateFabImportReceipt(invalid, error), "portable case-fold path aliases are rejected");
        invalid = receipt;
        invalid.Assets.front().LogicalPath = std::string("safe/") + static_cast<char>(0x80) + ".glb";
        Check(!ValidateFabImportReceipt(invalid, error), "non-ASCII durable artifact paths are rejected");
        invalid = receipt;
        invalid.Assets[1] = invalid.Assets[0];
        Check(!ValidateFabImportReceipt(invalid, error), "duplicate semantic asset records are rejected");
        invalid = receipt;
        invalid.Assets[1].LogicalPath = invalid.Assets[0].LogicalPath;
        Check(!ValidateFabImportReceipt(invalid, error), "duplicate logical paths are rejected");
        invalid = receipt;
        invalid.Assets[1].GenerationRelativeCookedPath = invalid.Assets[0].GenerationRelativeCookedPath;
        Check(!ValidateFabImportReceipt(invalid, error), "duplicate cooked paths are rejected");

        Check(expected.find("/home/") == std::string::npos
                && expected.find("SourcePath") == std::string::npos
                && expected.find("Credential") == std::string::npos
                && expected.find("Cookie") == std::string::npos
                && expected.find("Authorization") == std::string::npos
                && expected.find("Payment") == std::string::npos
                && expected.find("Entitlement") == std::string::npos,
            "the durable schema has no selected-source or browser/commerce secret surface");
        invalid = receipt;
        invalid.ProductIdentity = "/home/user/Downloads/package.zip";
        Check(!ValidateFabImportReceipt(invalid, error), "a local source path cannot masquerade as product identity");

        FabReceiptCollection collection;
        FabReceiptDecision decision = ClassifyFabImportReceipt(collection, receipt);
        Check(decision.Kind == FabReceiptDecisionKind::AddNewStream,
            "a first product is classified as a new stream");
        Check(AddFabImportReceipt(collection, receipt, decision, error)
                && decision.Kind == FabReceiptDecisionKind::AddNewStream
                && collection.Receipts.size() == 1,
            "new stream publication succeeds transactionally");
        const FabReceiptCollection oneReceipt = collection;
        Check(ClassifyFabImportReceipt(collection, receipt).Kind == FabReceiptDecisionKind::ExactReuse,
            "an exact receipt is classified as reuse");
        FabImportReceipt sameBytes = receipt;
        sameBytes.DiagnosticAcquiredAtUtc = "2026-09-05T10:21:30Z";
        Check(ClassifyFabImportReceipt(collection, sameBytes).Kind == FabReceiptDecisionKind::ExactReuse,
            "same source and compatible provenance reuse accepted artifacts");
        Check(AddFabImportReceipt(collection, sameBytes, decision, error)
                && collection == oneReceipt,
            "exact reuse is an in-memory no-op");

        FabImportReceipt incompatible = sameBytes;
        incompatible.LicenseFamily = FabLicenseFamily::CreativeCommonsAttribution;
        incompatible.LicenseTier = FabLicenseTier::NotApplicable;
        incompatible.AttributionText = "Conflicting declaration";
        incompatible.AttributionLink = "https://creativecommons.org/licenses/by/4.0/";
        Check(ClassifyFabImportReceipt(collection, incompatible).Kind == FabReceiptDecisionKind::Conflict,
            "same bytes with incompatible license/provenance never silently alias");

        FabImportReceipt replacement = receipt;
        replacement.Relation = FabGenerationRelation::SourceReplacement;
        replacement.RelatedStreamId = receipt.StreamId;
        replacement.RelatedGenerationId = receipt.GenerationId;
        replacement.SourceSha256 = Digest('e');
        replacement.ExpandedTreeSha256 = Digest('f');
        RefreshGenerationIdentity(replacement);
        replacement.DiagnosticAcquiredAtUtc = "2026-09-05T11:20:30Z";
        replacement.Assets[0].ArtifactSha256 = Digest('1');
        replacement.Assets[1].ArtifactSha256 = Digest('2');
        FabImportReceipt incompleteReplacement = replacement;
        incompleteReplacement.Assets.pop_back();
        Check(ClassifyFabImportReceipt(collection, incompleteReplacement).Kind
                == FabReceiptDecisionKind::Conflict,
            "same-stream replacement cannot remove a stable semantic asset");
        Check(ClassifyFabImportReceipt(collection, replacement).Kind
                == FabReceiptDecisionKind::ReplaceSameStreamSource,
            "changed source with an exact parent is classified as same-stream replacement");
        Check(AddFabImportReceipt(collection, replacement, decision, error)
                && decision.Kind == FabReceiptDecisionKind::ReplaceSameStreamSource
                && collection.Receipts.size() == 2,
            "same-stream changed-source generation publishes only after validation");

        FabImportReceipt staleReplacement = replacement;
        staleReplacement.RelatedGenerationId = receipt.GenerationId;
        staleReplacement.SourceSha256 = Digest('3');
        RefreshGenerationIdentity(staleReplacement);
        Check(ClassifyFabImportReceipt(collection, staleReplacement).Kind == FabReceiptDecisionKind::Conflict,
            "a replacement cannot branch from a stale generation");

        FabImportReceipt update = replacement;
        update.VersionOrDownloadLabel = "2.0-glb";
        update.Relation = FabGenerationRelation::ProductUpdate;
        update.RelatedStreamId = replacement.StreamId;
        update.RelatedGenerationId = replacement.GenerationId;
        update.SourceSha256 = Digest('4');
        update.ExpandedTreeSha256 = Digest('5');
        update.Assets[0].ArtifactSha256 = Digest('6');
        update.Assets[1].ArtifactSha256 = Digest('7');
        RefreshStreamIdentity(update);
        Check(ClassifyFabImportReceipt(collection, update).Kind
                == FabReceiptDecisionKind::AddProductUpdateStream,
            "a related declared product update is classified as a new stream");
        Check(AddFabImportReceipt(collection, update, decision, error)
                && decision.Kind == FabReceiptDecisionKind::AddProductUpdateStream
                && collection.Receipts.size() == 3,
            "product update stream publishes transactionally");

        FabImportReceipt anotherProduct = receipt;
        anotherProduct.ProductIdentity =
            "https://www.fab.com/listings/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
        RefreshStreamIdentity(anotherProduct);
        Check(ClassifyFabImportReceipt(collection, anotherProduct).Kind
                == FabReceiptDecisionKind::AddNewStream,
            "a distinct product with distinct stable assets creates a new stream");

        FabReceiptCollection corrupt = collection;
        corrupt.Receipts.push_back(corrupt.Receipts.front());
        Check(ClassifyFabImportReceipt(corrupt, anotherProduct).Kind
                == FabReceiptDecisionKind::CorruptPriorState,
            "corrupt prior collection state is distinct from candidate conflict");
        const FabReceiptCollection beforeInvalidAdd = collection;
        invalid = receipt;
        invalid.MetadataConfirmedByUser = false;
        Check(!AddFabImportReceipt(collection, invalid, decision, error)
                && decision.Kind == FabReceiptDecisionKind::InvalidCandidate
                && collection == beforeInvalidAdd,
            "invalid addition preserves the complete prior collection");

        std::string collectionBytes;
        FabReceiptCollection loadedCollection;
        Check(SerializeFabReceiptCollection(collection, collectionBytes, error)
                && DeserializeFabReceiptCollection(collectionBytes, loadedCollection, error)
                && loadedCollection == collection,
            "the deterministic collection round-trips exact canonical bytes");
        FabReceiptCollection reversedCollection = collection;
        std::reverse(reversedCollection.Receipts.begin(), reversedCollection.Receipts.end());
        std::string reorderedBytes;
        Check(SerializeFabReceiptCollection(reversedCollection, reorderedBytes, error)
                && reorderedBytes == collectionBytes,
            "collection serialization is independent of in-memory receipt order");
        FabReceiptCollection sentinelCollection;
        sentinelCollection.Receipts.push_back(receipt);
        std::string malformedCollection = collectionBytes;
        ReplaceOnce(malformedCollection, "ReceiptCount 3", "ReceiptCount 4");
        loadedCollection = sentinelCollection;
        Check(!DeserializeFabReceiptCollection(malformedCollection, loadedCollection, error)
                && loadedCollection == sentinelCollection,
            "malformed collection parsing is transactional");

        std::filesystem::path temporaryRoot;
        const bool isolatedTemporaryRoot = CreateUniqueTemporaryRoot(temporaryRoot);
        const std::filesystem::path receiptPath = temporaryRoot / "receipt.spiralfab";
        const std::filesystem::path collectionPath = temporaryRoot / "receipts.spiralfab";
        std::error_code filesystemError;
        FabImportReceipt diskReceipt = sentinel;
        std::string diskReceiptBytes;
        const bool receiptStored = isolatedTemporaryRoot
            && StoreFabImportReceipt(receiptPath, receipt, error)
            && LoadFabImportReceipt(receiptPath, diskReceipt, error)
            && SerializeFabImportReceipt(diskReceipt, diskReceiptBytes, error);
        Check(receiptStored && diskReceiptBytes == expected,
            "strict individual receipt store/load preserves canonical bytes");
        const bool stored = isolatedTemporaryRoot
            && StoreFabReceiptCollection(collectionPath, collection, error);
        const std::string storedBytes = stored ? ReadFile(collectionPath) : std::string {};
        FabReceiptCollection diskLoaded;
        Check(stored && storedBytes == collectionBytes
                && LoadFabReceiptCollection(collectionPath, diskLoaded, error)
                && diskLoaded == collection,
            "explicit store/load uses an isolated file and preserves canonical bytes");
        if (stored)
        {
            FabReceiptCollection invalidCollection = collection;
            invalidCollection.SchemaVersion = 99;
            Check(!StoreFabReceiptCollection(collectionPath, invalidCollection, error)
                    && ReadFile(collectionPath) == storedBytes,
                "failed store validation preserves the prior destination");
            std::ofstream truncated(collectionPath, std::ios::binary | std::ios::trunc);
            truncated << "SpiralFabReceiptCollection 1\nReceiptCount 1\nReceiptBegin\n";
            truncated.close();
            diskLoaded = sentinelCollection;
            Check(!LoadFabReceiptCollection(collectionPath, diskLoaded, error)
                    && diskLoaded == sentinelCollection,
                "truncated disk load preserves the caller collection");
        }
        if (isolatedTemporaryRoot)
            std::filesystem::remove_all(temporaryRoot, filesystemError);

        return passed;
    }
}
