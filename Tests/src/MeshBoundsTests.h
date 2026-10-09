#pragma once

namespace SpiralTests
{
    // Integration tests for the Assets mesh-bounds query. EngineTests.cpp owns registration.
    bool TestMeshBoundsMatchHandComputedValuesAndOnlyCoverDrawnGeometry();
    bool TestMeshBoundsGeneratedMeshesMatchBruteForceAndTransformsContainGeometry();
    bool TestMeshBoundsResolveCookedArtifactsAndFailClosed();
    bool TestMeshBoundsTransformedSceneInstancesMatchHandComputedBoxes();
}
