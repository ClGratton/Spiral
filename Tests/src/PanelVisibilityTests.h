#pragma once

namespace SpiralTests
{
    // Tests for the pure panel visibility codec (Editor/src/Layout/PanelVisibility).
    // EngineTests.cpp owns registration.
    bool TestPanelVisibilityRoundTripsEveryCombinationAndMatchesHandWrittenText();
    bool TestPanelVisibilityRejectsMalformedFilesWithoutChangingTheOutput();
    bool TestPanelVisibilityMutatedFilesAreRejectedOrSelfConsistent();
}
