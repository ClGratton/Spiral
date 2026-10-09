#pragma once

#include <cstddef>

namespace SpiralTests
{
    bool TestProjectCommitPublishesRevisionsAtomically();
    bool TestProjectCommitInjectedFailuresPreserveOldProject();
    bool TestProjectCommitGenerationPublicationIsCreateOnce();
    bool TestProjectCommitRejectsEscapesStaleBasesAndLockContention();
    bool TestProjectCommitConcurrentWritersAreExcluded();
    bool TestFabProjectStateDetectsTamperAndOrphans();
    bool TestFabImmutableMaterialLoad();

    // Measurement helper, not a registered test: commits one synthetic generation
    // whose mesh artifact is generationBytes long and prints the phase timings.
    bool MeasureProjectCommitLatency(size_t generationBytes);
}
