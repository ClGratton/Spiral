#pragma once

namespace SpiralTests
{
    bool TestApplicationMinimizeLatchesOneUiDecisionAndBacksOff();
    bool TestApplicationShutdownCancelsLayerJobsBeforeJoiningWorkers();
    bool TestLayerStackRollsBackALayerWhoseAttachThrows();
    bool TestAtomicFilePreservesModeFollowsSymlinkedParentAndBoundsTemporaryNames();
    bool TestFrameTaskGraphRunsWorkerTasksWhenEveryWorkerIsBusy();
    bool TestJobSystemWaitIdleOutlastsJobCaptureDestruction();
    bool TestProfileScopesCanShareABlock();
}
