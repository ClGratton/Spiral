#pragma once

namespace SpiralTests
{
    bool TestSelectionModelAgainstReferenceModel();
    bool TestSelectionModelScriptedSequences();
    bool TestEntityNamingPolicy();
    bool TestHierarchyModelFilterLocksReorderAndRename();
    bool TestEntityClipboardRoundTripAndCorruption();
    bool TestLayoutCodecRoundTripAndGrammar();
    bool TestLayoutCodecRejectsHostileInputAtomically();
    bool TestLayoutStoreOperationsAndLegacyMigration();
    bool TestLayoutPersistenceIsAtomicAndFailClosed();
}
