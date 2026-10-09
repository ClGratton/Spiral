#pragma once

namespace SpiralTests
{
    // Integration tests for the Scene reinsert/clone contract the Editor relies on for
    // delete, duplicate and paste with undo. EngineTests.cpp owns registration.
    bool TestSceneRestoreEntityUndoesDeletesByteExactly();
    bool TestSceneRestoreEntityRejectsInvalidRequestsFailureAtomically();
    bool TestSceneCloneEntityCopiesComponentsUnderFreshIdsAndUndoesExactly();
    bool TestSceneRestoreEntityIdCounterOrderingCameraAndPasteRules();
}
