#pragma once

namespace SpiralTests
{
    bool TestAssetRegistryRejectsUnsaveableMetadataAndNormalizesLinearly();
    bool TestAssetWatcherUsesExplicitProjectRootAndReportsEveryTransition();
    bool TestAssetWatcherScalesLinearlyWithAssetCount();
    bool TestGltfImporterRejectsUnsupportedEncodingsAndEscapingBuffers();
    bool TestMeshAndTextureArtifactsRejectDeclaredCountsTheFileCannotSupply();
    bool TestMeshAndTextureArtifactStoresPublishAtomicallyAcrossProcesses();
    bool TestMaterialAssetSavesNonFiniteAndPreciseValuesRoundTripExactly();
}
