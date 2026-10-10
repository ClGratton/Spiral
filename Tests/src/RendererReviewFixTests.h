#pragma once

namespace SpiralTests
{
    bool TestRetainedResolverCacheDecodesOncePerSnapshotAndInvalidatesOnPublication();
    bool TestClusteredLightGridLocalLightBoundsCoverEveryPixelInsideTheSphere();
    bool TestShadowTexelLatticeIsFixedInTheWorldUnderCameraTranslation();
    bool TestSceneLightPayloadSlotCapacityBuckets();
    bool TestPendingPresentationPolicyDecisionDefersInsteadOfFailing();
    bool TestPresentationTransitionCommitIsIdempotentAndOncePerRequest();
    bool TestSuboptimalRecreationGateRequestsOncePerExtent();
    bool TestDeadlineWaiterChargesOnlyTheWaitingThreadsCpu();
    bool TestFramePacingCaptureAmendAndRecordCostIsIndependentOfCaptureSize();
    bool TestSceneSkyAtmospherePreparationCostIsReported();
}
