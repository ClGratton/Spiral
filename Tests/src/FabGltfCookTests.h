#pragma once

namespace SpiralTests
{
    bool TestFabGltfPrepareBakesGeometryAgainstIndependentOracles();
    bool TestFabGltfPrepareMapsMaterialsTexturesRolesAndColorSpaces();
    bool TestFabGltfPrepareRejectsUnsupportedAndHostilePackagesWithoutOutput();
    bool TestFabGltfCookStagesDeterministicGenerationsAndStableIdentities();
    bool TestFabGltfCandidateConstructionIsFailureAtomicAndPreservesOldResolvers();
}
