// THE STATUS FAMILY's FIELD TABLES (SCRIPT-IN-ENGINE-1 2d): one per record records_status.h declares, every field
// named once in declaration order (records.h), by the member's own token (JAH_FIELD). ANSWERS ONLY (no command takes these
// records): no domain is declared on them. A field the engine adds to a tabled record is added HERE in the same commit
// (the guard breaks the build until it is).
#include "script/records_status.h"

namespace jahshaka { namespace engine { namespace script {

const Table<PublishedFrame::RiderTag> &Record<PublishedFrame::RiderTag>::table()
{
    static const Table<PublishedFrame::RiderTag> t({
        JAH_FIELD(PublishedFrame::RiderTag, rider),
        JAH_FIELD(PublishedFrame::RiderTag, position),
        JAH_FIELD(PublishedFrame::RiderTag, rotation),
        JAH_FIELD(PublishedFrame::RiderTag, scale),
        JAH_FIELD(PublishedFrame::RiderTag, owner),
        JAH_FIELD(PublishedFrame::RiderTag, bone),
    });
    return t;
}

const Table<PublishedFrame::Facts> &Record<PublishedFrame::Facts>::table()
{
    static const Table<PublishedFrame::Facts> t({
        JAH_FIELD(PublishedFrame::Facts, shares),
        JAH_FIELD(PublishedFrame::Facts, reflectors),
        JAH_FIELD(PublishedFrame::Facts, reflectorArms),
        JAH_FIELD(PublishedFrame::Facts, clips),
    });
    return t;
}

const Table<PublishedFrame::Facts::Share> &Record<PublishedFrame::Facts::Share>::table()
{
    static const Table<PublishedFrame::Facts::Share> t({
        JAH_FIELD(PublishedFrame::Facts::Share, follower),
        JAH_FIELD(PublishedFrame::Facts::Share, source),
    });
    return t;
}

const Table<PublishedFrame::Facts::NodeClips> &Record<PublishedFrame::Facts::NodeClips>::table()
{
    static const Table<PublishedFrame::Facts::NodeClips> t({
        JAH_FIELD(PublishedFrame::Facts::NodeClips, node),
        JAH_FIELD(PublishedFrame::Facts::NodeClips, clips),
    });
    return t;
}

const Table<PublishedFrame::Facts::Clip> &Record<PublishedFrame::Facts::Clip>::table()
{
    static const Table<PublishedFrame::Facts::Clip> t({
        JAH_FIELD(PublishedFrame::Facts::Clip, id),
        JAH_FIELD(PublishedFrame::Facts::Clip, name),
    });
    return t;
}

const Table<PublishedFrame::Completion> &Record<PublishedFrame::Completion>::table()
{
    static const Table<PublishedFrame::Completion> t({
        JAH_FIELD(PublishedFrame::Completion, seq),
        JAH_FIELD(PublishedFrame::Completion, token),
        JAH_FIELD(PublishedFrame::Completion, ok),
        JAH_FIELD(PublishedFrame::Completion, error),
        JAH_FIELD(PublishedFrame::Completion, frame),
    });
    return t;
}

const Table<GiStatus> &Record<GiStatus>::table()
{
    static const Table<GiStatus> t({
        JAH_FIELD(GiStatus, mode),
        JAH_FIELD(GiStatus, updateBudget),
        JAH_FIELD(GiStatus, probeCount),
        JAH_FIELD(GiStatus, probeGridBudgetBytes),
        JAH_FIELD(GiStatus, probeGridBudgetProbes),
        JAH_FIELD(GiStatus, probeGridBytes),
        JAH_FIELD(GiStatus, probesOverBudget),
        JAH_FIELD(GiStatus, pccBound),
        JAH_FIELD(GiStatus, vctBound),
        JAH_FIELD(GiStatus, boundsMin),
        JAH_FIELD(GiStatus, boundsMax),
        JAH_FIELD(GiStatus, voxelMetres),
        JAH_FIELD(GiStatus, probeRegionMin),
        JAH_FIELD(GiStatus, probeRegionMax),
        JAH_FIELD(GiStatus, probeFitMin),
        JAH_FIELD(GiStatus, probeFitMax),
        JAH_FIELD(GiStatus, probeHdr),
        JAH_FIELD(GiStatus, probeShadows),
        JAH_FIELD(GiStatus, probeCaptureSize),
        JAH_FIELD(GiStatus, probesDropped),
        JAH_FIELD(GiStatus, probeGridByRays),
        JAH_FIELD(GiStatus, probePlacements),
        JAH_FIELD(GiStatus, probeReplacements),
        JAH_FIELD(GiStatus, probeCapturesTotal),
        JAH_FIELD(GiStatus, probeUpdatesPerFrame),
        JAH_FIELD(GiStatus, probeShapeMin),
        JAH_FIELD(GiStatus, probeShapeMax),
        JAH_FIELD(GiStatus, worstProbeShapeCellRatio),
        JAH_FIELD(GiStatus, probesExceedingCell),
        JAH_FIELD(GiStatus, probesClampedToRegion),
        JAH_FIELD(GiStatus, probeGateCrossings),
        JAH_FIELD(GiStatus, cubemapProbeSlotsPerCell),
        JAH_FIELD(GiStatus, reusedLastRefresh),
        JAH_FIELD(GiStatus, ifdBound),
        JAH_FIELD(GiStatus, ifdProbes),
        JAH_FIELD(GiStatus, ifdProbesPerFrame),
        JAH_FIELD(GiStatus, ifdMin),
        JAH_FIELD(GiStatus, ifdMax),
        JAH_FIELD(GiStatus, ifdFollows),
        JAH_FIELD(GiStatus, ifdScrollProbes),
        JAH_FIELD(GiStatus, ifdScrolls),
        JAH_FIELD(GiStatus, ifdReplacements),
        JAH_FIELD(GiStatus, ifdTargetSamples),
        JAH_FIELD(GiStatus, ifdRestartDepth),
        JAH_FIELD(GiStatus, ifdMarkLag),
        JAH_FIELD(GiStatus, ifdOwedRestarts),
        JAH_FIELD(GiStatus, ifdOwedRefines),
        JAH_FIELD(GiStatus, ifdOwedIntegrations),
        JAH_FIELD(GiStatus, ifdWorking),
        JAH_FIELD(GiStatus, ifdMarkedByMoves),
        JAH_FIELD(GiStatus, ifdMarkedByRegion),
        JAH_FIELD(GiStatus, ifdMarkAlls),
        JAH_FIELD(GiStatus, ifdRestarts),
        JAH_FIELD(GiStatus, ifdReportsKept),
        JAH_FIELD(GiStatus, ifdDispatchedLastFrame),
        JAH_FIELD(GiStatus, ifdGrantLastFrame),
        JAH_FIELD(GiStatus, ifdReadUnheld),
        JAH_FIELD(GiStatus, ifdIntegrations),
        JAH_FIELD(GiStatus, giAtRest),
        JAH_FIELD(GiStatus, chainOwed),
        JAH_FIELD(GiStatus, regionFrame),
        JAH_FIELD(GiStatus, regionCount),
        JAH_FIELD(GiStatus, regionsByKind),
        JAH_FIELD(GiStatus, regionOverflow),
        JAH_FIELD(GiStatus, regionRestart),
        JAH_FIELD(GiStatus, regionMerges),
        JAH_FIELD(GiStatus, regionCloses),
        JAH_FIELD(GiStatus, regionClosesListing),
        JAH_FIELD(GiStatus, ssaoSuppressed),
        JAH_FIELD(GiStatus, ssaoSuppressedReason),
        JAH_FIELD(GiStatus, probeCapturesLastFrame),
        JAH_FIELD(GiStatus, probeCapturesDeferred),
        JAH_FIELD(GiStatus, staleProbes),
        JAH_FIELD(GiStatus, lastStaleReason),
        JAH_FIELD(GiStatus, staleSerial),
        JAH_FIELD(GiStatus, rebuilds),
        JAH_FIELD(GiStatus, giScans),
        JAH_FIELD(GiStatus, giScansFull),
        JAH_FIELD(GiStatus, giScanMicros),
        JAH_FIELD(GiStatus, giAabbReads),
        JAH_FIELD(GiStatus, cascades),
        JAH_FIELD(GiStatus, cascadeVoxelLod),
        JAH_FIELD(GiStatus, cascadesAwaitingCamera),
        JAH_FIELD(GiStatus, awaitingVoxelTextures),
        JAH_FIELD(GiStatus, cascadeProfileVr),
        JAH_FIELD(GiStatus, cascadeFullRebuilds),
        JAH_FIELD(GiStatus, cascadeDeferrals),
        JAH_FIELD(GiStatus, feedbackRests),
        JAH_FIELD(GiStatus, feedbackOwed),
        JAH_FIELD(GiStatus, chainInjectionRefusals),
        JAH_FIELD(GiStatus, chainInjectionsPeakFrame),
        JAH_FIELD(GiStatus, dragMovers),
        JAH_FIELD(GiStatus, dragMoverGestures),
        JAH_FIELD(GiStatus, schedulerPushes),
        JAH_FIELD(GiStatus, schedulerResolves),
        JAH_FIELD(GiStatus, settlePending),
        JAH_FIELD(GiStatus, settleStableFrames),
        JAH_FIELD(GiStatus, gestureActive),
        JAH_FIELD(GiStatus, cascadeDirtyMajority),
        JAH_FIELD(GiStatus, cascadeScrollAborts),
        JAH_FIELD(GiStatus, cascadeCommitShaderWaits),
        JAH_FIELD(GiStatus, relightCount),
        JAH_FIELD(GiStatus, cascadeTornCleared),
        JAH_FIELD(GiStatus, gather),
        JAH_FIELD(GiStatus, cards),
        JAH_FIELD(GiStatus, voxelSides),
    });
    return t;
}

const Table<GiStatus::CascadeStatus> &Record<GiStatus::CascadeStatus>::table()
{
    static const Table<GiStatus::CascadeStatus> t({
        JAH_FIELD(GiStatus::CascadeStatus, halfSize),
        JAH_FIELD(GiStatus::CascadeStatus, resolution),
        JAH_FIELD(GiStatus::CascadeStatus, cell),
        JAH_FIELD(GiStatus::CascadeStatus, step),
        JAH_FIELD(GiStatus::CascadeStatus, guaranteedRadius),
        JAH_FIELD(GiStatus::CascadeStatus, hitLevelRadius),
        JAH_FIELD(GiStatus::CascadeStatus, centre),
        JAH_FIELD(GiStatus::CascadeStatus, builtCam),
        JAH_FIELD(GiStatus::CascadeStatus, rebuilds),
        JAH_FIELD(GiStatus::CascadeStatus, pending),
        JAH_FIELD(GiStatus::CascadeStatus, scrolls),
        JAH_FIELD(GiStatus::CascadeStatus, scrollFrames),
        JAH_FIELD(GiStatus::CascadeStatus, scrolling),
        JAH_FIELD(GiStatus::CascadeStatus, tornCleared),
        JAH_FIELD(GiStatus::CascadeStatus, shellEnds),
        JAH_FIELD(GiStatus::CascadeStatus, commitLitSlab),
        JAH_FIELD(GiStatus::CascadeStatus, commitLitBand),
        JAH_FIELD(GiStatus::CascadeStatus, commitSlabCells),
        JAH_FIELD(GiStatus::CascadeStatus, commitInjectCells),
        JAH_FIELD(GiStatus::CascadeStatus, commitCounted),
        JAH_FIELD(GiStatus::CascadeStatus, deltaMarched),
        JAH_FIELD(GiStatus::CascadeStatus, feedbackActive),
        JAH_FIELD(GiStatus::CascadeStatus, feedbackOnPrior),
        JAH_FIELD(GiStatus::CascadeStatus, feedbackOwed),
        JAH_FIELD(GiStatus::CascadeStatus, feedbackPasses),
        JAH_FIELD(GiStatus::CascadeStatus, feedbackBricksRan),
        JAH_FIELD(GiStatus::CascadeStatus, feedbackStops),
        JAH_FIELD(GiStatus::CascadeStatus, feedbackResumes),
        JAH_FIELD(GiStatus::CascadeStatus, feedbackSaturated),
        JAH_FIELD(GiStatus::CascadeStatus, feedbackReadings),
        JAH_FIELD(GiStatus::CascadeStatus, feedbackCarried),
        JAH_FIELD(GiStatus::CascadeStatus, relightCarried),
        JAH_FIELD(GiStatus::CascadeStatus, relightRounds),
        JAH_FIELD(GiStatus::CascadeStatus, relightVoxels),
        JAH_FIELD(GiStatus::CascadeStatus, relightSlab),
        JAH_FIELD(GiStatus::CascadeStatus, regionRebuilds),
        JAH_FIELD(GiStatus::CascadeStatus, moverRegionRebuilds),
        JAH_FIELD(GiStatus::CascadeStatus, regionCells),
        JAH_FIELD(GiStatus::CascadeStatus, regionOwed),
        JAH_FIELD(GiStatus::CascadeStatus, marchOwed),
        JAH_FIELD(GiStatus::CascadeStatus, regionBouncePasses),
        JAH_FIELD(GiStatus::CascadeStatus, regionBounceLast),
        JAH_FIELD(GiStatus::CascadeStatus, lagLast),
        JAH_FIELD(GiStatus::CascadeStatus, lagMax),
        JAH_FIELD(GiStatus::CascadeStatus, items),
        JAH_FIELD(GiStatus::CascadeStatus, attached),
        JAH_FIELD(GiStatus::CascadeStatus, lastCpuMs),
        JAH_FIELD(GiStatus::CascadeStatus, voxelLevels),
        JAH_FIELD(GiStatus::CascadeStatus, voxelTriangles),
        JAH_FIELD(GiStatus::CascadeStatus, voxelDispatches),
        JAH_FIELD(GiStatus::CascadeStatus, voxelRecords),
        JAH_FIELD(GiStatus::CascadeStatus, voxelOverflow),
        JAH_FIELD(GiStatus::CascadeStatus, voxelGathers),
        JAH_FIELD(GiStatus::CascadeStatus, voxelReadingGather),
        JAH_FIELD(GiStatus::CascadeStatus, voxelReadoutFlushes),
        JAH_FIELD(GiStatus::CascadeStatus, lightsInRange),
        JAH_FIELD(GiStatus::CascadeStatus, lightsInjected),
        JAH_FIELD(GiStatus::CascadeStatus, lightCapacity),
        JAH_FIELD(GiStatus::CascadeStatus, lightsOverBudget),
        JAH_FIELD(GiStatus::CascadeStatus, lightsInRangeMax),
        JAH_FIELD(GiStatus::CascadeStatus, injectionsOverSlots),
        JAH_FIELD(GiStatus::CascadeStatus, bricks),
        JAH_FIELD(GiStatus::CascadeStatus, bricksDirectLast),
        JAH_FIELD(GiStatus::CascadeStatus, bricksBounceLast),
        JAH_FIELD(GiStatus::CascadeStatus, bricksActiveLast),
        JAH_FIELD(GiStatus::CascadeStatus, brickUpdates),
        JAH_FIELD(GiStatus::CascadeStatus, brickReadings),
        JAH_FIELD(GiStatus::CascadeStatus, brickReadingsSkipped),
        JAH_FIELD(GiStatus::CascadeStatus, foreignDirectMax),
        JAH_FIELD(GiStatus::CascadeStatus, foreignBounceMax),
        JAH_FIELD(GiStatus::CascadeStatus, foreignDirectMaxBrick),
        JAH_FIELD(GiStatus::CascadeStatus, foreignBounceMaxBrick),
        JAH_FIELD(GiStatus::CascadeStatus, foreignReadings),
        JAH_FIELD(GiStatus::CascadeStatus, foreignFirstFrame),
        JAH_FIELD(GiStatus::CascadeStatus, foreignFirstSkipped),
        JAH_FIELD(GiStatus::CascadeStatus, foreignFirstBrick),
        JAH_FIELD(GiStatus::CascadeStatus, foreignFirstWord),
        JAH_FIELD(GiStatus::CascadeStatus, foreignFirstCount),
        JAH_FIELD(GiStatus::CascadeStatus, foreignFirstCodes),
        JAH_FIELD(GiStatus::CascadeStatus, signReadings),
        JAH_FIELD(GiStatus::CascadeStatus, signFirstFrame),
        JAH_FIELD(GiStatus::CascadeStatus, signFirstBrick),
        JAH_FIELD(GiStatus::CascadeStatus, signFirstCount),
        JAH_FIELD(GiStatus::CascadeStatus, signFirstCodes),
    });
    return t;
}

const Table<GatherStatus> &Record<GatherStatus>::table()
{
    static const Table<GatherStatus> t({
        JAH_FIELD(GatherStatus, on),
        JAH_FIELD(GatherStatus, running),
        JAH_FIELD(GatherStatus, stride),
        JAH_FIELD(GatherStatus, octRes),
        JAH_FIELD(GatherStatus, raysPerProbe),
        JAH_FIELD(GatherStatus, probesX),
        JAH_FIELD(GatherStatus, probesY),
        JAH_FIELD(GatherStatus, probes),
        JAH_FIELD(GatherStatus, adaptive),
        JAH_FIELD(GatherStatus, adaptiveCap),
        JAH_FIELD(GatherStatus, adaptiveRequested),
        JAH_FIELD(GatherStatus, raysPerFrame),
        JAH_FIELD(GatherStatus, targetW),
        JAH_FIELD(GatherStatus, targetH),
        JAH_FIELD(GatherStatus, stereo),
        JAH_FIELD(GatherStatus, eyeProbesX),
        JAH_FIELD(GatherStatus, vramBytes),
        JAH_FIELD(GatherStatus, historyBytes),
        JAH_FIELD(GatherStatus, placeMs),
        JAH_FIELD(GatherStatus, traceMs),
        JAH_FIELD(GatherStatus, integrateMs),
        JAH_FIELD(GatherStatus, cpuMs),
        JAH_FIELD(GatherStatus, filterMs),
        JAH_FIELD(GatherStatus, temporal),
        JAH_FIELD(GatherStatus, historyAge),
        JAH_FIELD(GatherStatus, restFrames),
        JAH_FIELD(GatherStatus, sinceRestart),
        JAH_FIELD(GatherStatus, settleFrames),
        JAH_FIELD(GatherStatus, historyFrames),
        JAH_FIELD(GatherStatus, settled),
        JAH_FIELD(GatherStatus, held),
        JAH_FIELD(GatherStatus, cellsReached),
        JAH_FIELD(GatherStatus, cellsHeld),
        JAH_FIELD(GatherStatus, cellsActive),
        JAH_FIELD(GatherStatus, cellsRing),
        JAH_FIELD(GatherStatus, resumes),
        JAH_FIELD(GatherStatus, restLog),
        JAH_FIELD(GatherStatus, cameraCuts),
        JAH_FIELD(GatherStatus, foliageRuleAccepts),
        JAH_FIELD(GatherStatus, starvedDiffuse),
        JAH_FIELD(GatherStatus, starvedRough),
        JAH_FIELD(GatherStatus, shotValid),
        JAH_FIELD(GatherStatus, shotStarvedDiffuse),
        JAH_FIELD(GatherStatus, shotStarvedRough),
        JAH_FIELD(GatherStatus, irradiance),
        JAH_FIELD(GatherStatus, irradianceW),
        JAH_FIELD(GatherStatus, irradianceH),
        JAH_FIELD(GatherStatus, irradianceFrame),
        JAH_FIELD(GatherStatus, frame),
        JAH_FIELD(GatherStatus, historyCount),
        JAH_FIELD(GatherStatus, historyClass),
        JAH_FIELD(GatherStatus, historyDist),
        JAH_FIELD(GatherStatus, historyStreak),
        JAH_FIELD(GatherStatus, historyCountW),
        JAH_FIELD(GatherStatus, historyCountH),
        JAH_FIELD(GatherStatus, historyCountFrame),
        JAH_FIELD(GatherStatus, rays),
        JAH_FIELD(GatherStatus, raysFrame),
        JAH_FIELD(GatherStatus, adaptiveCells),
        JAH_FIELD(GatherStatus, error),
    });
    return t;
}

const Table<GatherRestRecord> &Record<GatherRestRecord>::table()
{
    static const Table<GatherRestRecord> t({
        JAH_FIELD(GatherRestRecord, frame),
        JAH_FIELD(GatherRestRecord, restK),
        JAH_FIELD(GatherRestRecord, reached),
        JAH_FIELD(GatherRestRecord, held),
        JAH_FIELD(GatherRestRecord, active),
        JAH_FIELD(GatherRestRecord, everyHeld),
        JAH_FIELD(GatherRestRecord, movingProbes),
        JAH_FIELD(GatherRestRecord, fastProbes),
        JAH_FIELD(GatherRestRecord, handoverM),
        JAH_FIELD(GatherRestRecord, refined),
    });
    return t;
}

const Table<GatherRay> &Record<GatherRay>::table()
{
    static const Table<GatherRay> t({
        JAH_FIELD(GatherRay, source),
        JAH_FIELD(GatherRay, cascade),
        JAH_FIELD(GatherRay, cellX),
        JAH_FIELD(GatherRay, cellY),
        JAH_FIELD(GatherRay, ray),
        JAH_FIELD(GatherRay, radiance),
        JAH_FIELD(GatherRay, distance),
        JAH_FIELD(GatherRay, hitPos),
        JAH_FIELD(GatherRay, hitNormal),
        JAH_FIELD(GatherRay, sunVisibility),
        JAH_FIELD(GatherRay, weight),
    });
    return t;
}

const Table<CardCacheStatus> &Record<CardCacheStatus>::table()
{
    static const Table<CardCacheStatus> t({
        JAH_FIELD(CardCacheStatus, built),
        JAH_FIELD(CardCacheStatus, pageSize),
        JAH_FIELD(CardCacheStatus, pages),
        JAH_FIELD(CardCacheStatus, pagesUsed),
        JAH_FIELD(CardCacheStatus, bytesPerTexel),
        JAH_FIELD(CardCacheStatus, bytes),
        JAH_FIELD(CardCacheStatus, emissiveFormat),
        JAH_FIELD(CardCacheStatus, instancesResident),
        JAH_FIELD(CardCacheStatus, cardsResident),
        JAH_FIELD(CardCacheStatus, residentHash),
        JAH_FIELD(CardCacheStatus, admitted),
        JAH_FIELD(CardCacheStatus, evicted),
        JAH_FIELD(CardCacheStatus, compactionMoves),
        JAH_FIELD(CardCacheStatus, requestOnlyPasses),
        JAH_FIELD(CardCacheStatus, requestOnlyKeysBuilt),
        JAH_FIELD(CardCacheStatus, cacheFrame),
        JAH_FIELD(CardCacheStatus, arrivalMarkNewest),
        JAH_FIELD(CardCacheStatus, demandReadEvery),
        JAH_FIELD(CardCacheStatus, demandReadLag),
        JAH_FIELD(CardCacheStatus, demandChangeFrame),
        JAH_FIELD(CardCacheStatus, demandFirst),
        JAH_FIELD(CardCacheStatus, demandNewest),
        JAH_FIELD(CardCacheStatus, demandOpenLag),
        JAH_FIELD(CardCacheStatus, lampTraces),
        JAH_FIELD(CardCacheStatus, lampTracedLastFrame),
        JAH_FIELD(CardCacheStatus, cardLampsMax),
        JAH_FIELD(CardCacheStatus, lampOverflowShareMax),
        JAH_FIELD(CardCacheStatus, cardAreaLampsMax),
        JAH_FIELD(CardCacheStatus, cardsMultiArea),
        JAH_FIELD(CardCacheStatus, cardsSharedFractional),
        JAH_FIELD(CardCacheStatus, lampRaysLastTrace),
        JAH_FIELD(CardCacheStatus, areaStrata),
        JAH_FIELD(CardCacheStatus, gather),
        JAH_FIELD(CardCacheStatus, gatherWaits),
        JAH_FIELD(CardCacheStatus, gatherByRays),
        JAH_FIELD(CardCacheStatus, indirectByCards),
        JAH_FIELD(CardCacheStatus, indirectByBound),
        JAH_FIELD(CardCacheStatus, visibleRelights),
        JAH_FIELD(CardCacheStatus, subQuantumRelights),
        JAH_FIELD(CardCacheStatus, unmeasuredRelights),
        JAH_FIELD(CardCacheStatus, readersHoldOwed),
        JAH_FIELD(CardCacheStatus, moveCodesLast),
        JAH_FIELD(CardCacheStatus, moveCodesFrame),
        JAH_FIELD(CardCacheStatus, restDrained),
        JAH_FIELD(CardCacheStatus, restDecisions),
        JAH_FIELD(CardCacheStatus, decisionUsedTexels),
        JAH_FIELD(CardCacheStatus, decisionFreeTexels),
        JAH_FIELD(CardCacheStatus, decisionAdmitted),
        JAH_FIELD(CardCacheStatus, decisionRefused),
        JAH_FIELD(CardCacheStatus, decisionReaders),
        JAH_FIELD(CardCacheStatus, decisionRefusedCards),
        JAH_FIELD(CardCacheStatus, windowDrains),
        JAH_FIELD(CardCacheStatus, workRoots),
        JAH_FIELD(CardCacheStatus, workRound),
        JAH_FIELD(CardCacheStatus, demandHits),
        JAH_FIELD(CardCacheStatus, queueLength),
        JAH_FIELD(CardCacheStatus, budgetTexels),
        JAH_FIELD(CardCacheStatus, capturesLastFrame),
        JAH_FIELD(CardCacheStatus, texelsLastFrame),
        JAH_FIELD(CardCacheStatus, captures),
        JAH_FIELD(CardCacheStatus, invalidTransform),
        JAH_FIELD(CardCacheStatus, invalidMaterial),
        JAH_FIELD(CardCacheStatus, captureMs),
        JAH_FIELD(CardCacheStatus, captureWorkspaceMs),
        JAH_FIELD(CardCacheStatus, captureCopyMs),
        JAH_FIELD(CardCacheStatus, captureTraceMs),
        JAH_FIELD(CardCacheStatus, cardRecords),
        JAH_FIELD(CardCacheStatus, instanceSlots),
        JAH_FIELD(CardCacheStatus, radianceFormat),
        JAH_FIELD(CardCacheStatus, lightBudgetTexels),
        JAH_FIELD(CardCacheStatus, relitLastFrame),
        JAH_FIELD(CardCacheStatus, relitTexelsLastFrame),
        JAH_FIELD(CardCacheStatus, relights),
        JAH_FIELD(CardCacheStatus, invalidRadiance),
        JAH_FIELD(CardCacheStatus, lightMs),
        JAH_FIELD(CardCacheStatus, indirectBudgetTexels),
        JAH_FIELD(CardCacheStatus, indirectLastFrame),
        JAH_FIELD(CardCacheStatus, indirectTexelsLastFrame),
        JAH_FIELD(CardCacheStatus, indirectRelights),
        JAH_FIELD(CardCacheStatus, invalidIndirect),
        JAH_FIELD(CardCacheStatus, indirectReported),
        JAH_FIELD(CardCacheStatus, indirectByRegion),
        JAH_FIELD(CardCacheStatus, indirectOverflows),
        JAH_FIELD(CardCacheStatus, indirectAnswered),
        JAH_FIELD(CardCacheStatus, reportsDropped),
        JAH_FIELD(CardCacheStatus, indirectOn),
        JAH_FIELD(CardCacheStatus, lightsInRange),
        JAH_FIELD(CardCacheStatus, lightsDropped),
        JAH_FIELD(CardCacheStatus, invalidSun),
        JAH_FIELD(CardCacheStatus, stillTraces),
        JAH_FIELD(CardCacheStatus, stillTracedLastFrame),
        JAH_FIELD(CardCacheStatus, stillTexelsLastFrame),
        JAH_FIELD(CardCacheStatus, stillPending),
        JAH_FIELD(CardCacheStatus, stillGpuMs),
        JAH_FIELD(CardCacheStatus, casterRetraces),
        JAH_FIELD(CardCacheStatus, moverCasters),
        JAH_FIELD(CardCacheStatus, moverTracedLastFrame),
        JAH_FIELD(CardCacheStatus, moverTexelsLastFrame),
        JAH_FIELD(CardCacheStatus, moverTraces),
        JAH_FIELD(CardCacheStatus, moverRetired),
        JAH_FIELD(CardCacheStatus, moverPending),
        JAH_FIELD(CardCacheStatus, moverPendingAge),
        JAH_FIELD(CardCacheStatus, moverGpuMs),
        JAH_FIELD(CardCacheStatus, relightGpuMs),
    });
    return t;
}

const Table<CardCacheStatus::GatherCounts> &Record<CardCacheStatus::GatherCounts>::table()
{
    static const Table<CardCacheStatus::GatherCounts> t({
        JAH_FIELD(CardCacheStatus::GatherCounts, dispatches),
        JAH_FIELD(CardCacheStatus::GatherCounts, rays),
        JAH_FIELD(CardCacheStatus::GatherCounts, card),
        JAH_FIELD(CardCacheStatus::GatherCounts, voxel),
        JAH_FIELD(CardCacheStatus::GatherCounts, backFaces),
        JAH_FIELD(CardCacheStatus::GatherCounts, noSample),
        JAH_FIELD(CardCacheStatus::GatherCounts, sky),
        JAH_FIELD(CardCacheStatus::GatherCounts, texels),
        JAH_FIELD(CardCacheStatus::GatherCounts, emptyTexels),
        JAH_FIELD(CardCacheStatus::GatherCounts, readsOverflowed),
        JAH_FIELD(CardCacheStatus::GatherCounts, buried),
    });
    return t;
}

const Table<CardCacheStatus::Refused> &Record<CardCacheStatus::Refused>::table()
{
    static const Table<CardCacheStatus::Refused> t({
        JAH_FIELD(CardCacheStatus::Refused, node),
        JAH_FIELD(CardCacheStatus::Refused, index),
        JAH_FIELD(CardCacheStatus::Refused, size),
        JAH_FIELD(CardCacheStatus::Refused, mark),
    });
    return t;
}

const Table<CardCacheStatus::WorkRoot> &Record<CardCacheStatus::WorkRoot>::table()
{
    static const Table<CardCacheStatus::WorkRoot> t({
        JAH_FIELD(CardCacheStatus::WorkRoot, kind),
        JAH_FIELD(CardCacheStatus::WorkRoot, frame),
    });
    return t;
}

const Table<GiStatus::VoxelSideCounts> &Record<GiStatus::VoxelSideCounts>::table()
{
    static const Table<GiStatus::VoxelSideCounts> t({
        JAH_FIELD(GiStatus::VoxelSideCounts, dispatches),
        JAH_FIELD(GiStatus::VoxelSideCounts, rays),
        JAH_FIELD(GiStatus::VoxelSideCounts, card),
        JAH_FIELD(GiStatus::VoxelSideCounts, voxel),
        JAH_FIELD(GiStatus::VoxelSideCounts, backFaces),
        JAH_FIELD(GiStatus::VoxelSideCounts, noSample),
        JAH_FIELD(GiStatus::VoxelSideCounts, sky),
        JAH_FIELD(GiStatus::VoxelSideCounts, sides),
        JAH_FIELD(GiStatus::VoxelSideCounts, emptySides),
        JAH_FIELD(GiStatus::VoxelSideCounts, waits),
        JAH_FIELD(GiStatus::VoxelSideCounts, routeChanges),
        JAH_FIELD(GiStatus::VoxelSideCounts, buried),
    });
    return t;
}

const Table<PublishedFrame::Atmosphere> &Record<PublishedFrame::Atmosphere>::table()
{
    static const Table<PublishedFrame::Atmosphere> t({
        JAH_FIELD(PublishedFrame::Atmosphere, on),
        JAH_FIELD(PublishedFrame::Atmosphere, rayleighScale),
        JAH_FIELD(PublishedFrame::Atmosphere, mieScale),
        JAH_FIELD(PublishedFrame::Atmosphere, ozone),
        JAH_FIELD(PublishedFrame::Atmosphere, planetRadiusKm),
        JAH_FIELD(PublishedFrame::Atmosphere, atmosphereHeightKm),
        JAH_FIELD(PublishedFrame::Atmosphere, observerKm),
    });
    return t;
}

const Table<HeightFogStatus> &Record<HeightFogStatus>::table()
{
    static const Table<HeightFogStatus> t({
        JAH_FIELD(HeightFogStatus, on),
        JAH_FIELD(HeightFogStatus, colour),
        JAH_FIELD(HeightFogStatus, colourFromSky),
    });
    return t;
}

const Table<PublishedFrame::Completeness> &Record<PublishedFrame::Completeness>::table()
{
    static const Table<PublishedFrame::Completeness> t({
        JAH_FIELD(PublishedFrame::Completeness, incomplete),
        JAH_FIELD(PublishedFrame::Completeness, terms),
        JAH_FIELD(PublishedFrame::Completeness, incompleteFrames),
        JAH_FIELD(PublishedFrame::Completeness, completionSerial),
    });
    return t;
}

const Table<CloudStatus> &Record<CloudStatus>::table()
{
    static const Table<CloudStatus> t({
        JAH_FIELD(CloudStatus, drawn),
        JAH_FIELD(CloudStatus, reason),
        JAH_FIELD(CloudStatus, changeCaptures),
        JAH_FIELD(CloudStatus, scrollCaptures),
        JAH_FIELD(CloudStatus, clockTicks),
        JAH_FIELD(CloudStatus, capturePeriodFrames),
        JAH_FIELD(CloudStatus, fieldBakes),
        JAH_FIELD(CloudStatus, scroll),
    });
    return t;
}

const Table<AtmosphereStatus> &Record<AtmosphereStatus>::table()
{
    static const Table<AtmosphereStatus> t({
        JAH_FIELD(AtmosphereStatus, on),
        JAH_FIELD(AtmosphereStatus, transmittanceBuilds),
        JAH_FIELD(AtmosphereStatus, multiScatterBuilds),
        JAH_FIELD(AtmosphereStatus, skyViewBuilds),
        JAH_FIELD(AtmosphereStatus, aerialBuilds),
        JAH_FIELD(AtmosphereStatus, deferredUpdates),
        JAH_FIELD(AtmosphereStatus, topIlluminance),
        JAH_FIELD(AtmosphereStatus, observerAltitudeM),
        JAH_FIELD(AtmosphereStatus, observerRebuilds),
        JAH_FIELD(AtmosphereStatus, environmentObserverM),
        JAH_FIELD(AtmosphereStatus, aerialBound),
    });
    return t;
}

const Table<RayQueryStatus> &Record<RayQueryStatus>::table()
{
    static const Table<RayQueryStatus> t({
        JAH_FIELD(RayQueryStatus, available),
        JAH_FIELD(RayQueryStatus, enabled),
        JAH_FIELD(RayQueryStatus, blasCount),
        JAH_FIELD(RayQueryStatus, instances),
        JAH_FIELD(RayQueryStatus, skinnedInstances),
        JAH_FIELD(RayQueryStatus, skinCaches),
        JAH_FIELD(RayQueryStatus, skinCacheBytes),
        JAH_FIELD(RayQueryStatus, skinBlasBytes),
        JAH_FIELD(RayQueryStatus, skinPasses),
        JAH_FIELD(RayQueryStatus, skinDispatches),
        JAH_FIELD(RayQueryStatus, skinBlasBuilds),
        JAH_FIELD(RayQueryStatus, skinRefits),
        JAH_FIELD(RayQueryStatus, skinLastItems),
        JAH_FIELD(RayQueryStatus, skinLastVertices),
        JAH_FIELD(RayQueryStatus, skinMs),
        JAH_FIELD(RayQueryStatus, skinRefitMs),
        JAH_FIELD(RayQueryStatus, skinCpuMs),
        JAH_FIELD(RayQueryStatus, skinReason),
        JAH_FIELD(RayQueryStatus, farInstances),
        JAH_FIELD(RayQueryStatus, levelBlasCount),
        JAH_FIELD(RayQueryStatus, levelBlasBytes),
        JAH_FIELD(RayQueryStatus, triangles),
        JAH_FIELD(RayQueryStatus, blasBytes),
        JAH_FIELD(RayQueryStatus, tlasBytes),
        JAH_FIELD(RayQueryStatus, tlasMs),
        JAH_FIELD(RayQueryStatus, blasMs),
        JAH_FIELD(RayQueryStatus, gatherMs),
        JAH_FIELD(RayQueryStatus, tlasJobDispatches),
        JAH_FIELD(RayQueryStatus, feedSlotVisits),
        JAH_FIELD(RayQueryStatus, updateLookups),
        JAH_FIELD(RayQueryStatus, updateRows),
        JAH_FIELD(RayQueryStatus, updateVisits),
        JAH_FIELD(RayQueryStatus, alphaMasksMade),
        JAH_FIELD(RayQueryStatus, alphaStaleServed),
        JAH_FIELD(RayQueryStatus, hitLights),
        JAH_FIELD(RayQueryStatus, hitMaterialRows),
        JAH_FIELD(RayQueryStatus, hitTableBuffers),
        JAH_FIELD(RayQueryStatus, reflectEmitters),
        JAH_FIELD(RayQueryStatus, reflectEmittersOver),
        JAH_FIELD(RayQueryStatus, reflectEmitterSetSerial),
        JAH_FIELD(RayQueryStatus, reflectEmitterTable),
        JAH_FIELD(RayQueryStatus, reflectPortals),
        JAH_FIELD(RayQueryStatus, reflectPick),
        JAH_FIELD(RayQueryStatus, reflectPickFrames),
        JAH_FIELD(RayQueryStatus, colourPick),
        JAH_FIELD(RayQueryStatus, colourPickFrames),
        JAH_FIELD(RayQueryStatus, reflectPortalTable),
        JAH_FIELD(RayQueryStatus, reflectNeeRays),
        JAH_FIELD(RayQueryStatus, reflectNeeHits),
        JAH_FIELD(RayQueryStatus, reflectRestNeedMax),
        JAH_FIELD(RayQueryStatus, reflectRestStopVMax),
        JAH_FIELD(RayQueryStatus, lastWasRefit),
        JAH_FIELD(RayQueryStatus, hitRecords),
        JAH_FIELD(RayQueryStatus, hitDropped),
        JAH_FIELD(RayQueryStatus, hitCapacity),
        JAH_FIELD(RayQueryStatus, escapeRecords),
        JAH_FIELD(RayQueryStatus, escapeDropped),
        JAH_FIELD(RayQueryStatus, escapeCapacity),
        JAH_FIELD(RayQueryStatus, escapeTraced),
        JAH_FIELD(RayQueryStatus, escapeCard),
        JAH_FIELD(RayQueryStatus, escapeVoxel),
        JAH_FIELD(RayQueryStatus, escapeDirect),
        JAH_FIELD(RayQueryStatus, escapeFar),
        JAH_FIELD(RayQueryStatus, escapeSky),
        JAH_FIELD(RayQueryStatus, escapeBack),
        JAH_FIELD(RayQueryStatus, escapeNoSample),
        JAH_FIELD(RayQueryStatus, escapeSaturated),
        JAH_FIELD(RayQueryStatus, panesOverflow),
        JAH_FIELD(RayQueryStatus, proceedStepsMax),
        JAH_FIELD(RayQueryStatus, raysRefused),
        JAH_FIELD(RayQueryStatus, hitViews),
        JAH_FIELD(RayQueryStatus, hitDecodeDraws),
        JAH_FIELD(RayQueryStatus, tlasBuilds),
        JAH_FIELD(RayQueryStatus, tlasRefits),
        JAH_FIELD(RayQueryStatus, blasBuilds),
        JAH_FIELD(RayQueryStatus, reflect),
        JAH_FIELD(RayQueryStatus, reflectRays),
        JAH_FIELD(RayQueryStatus, reflectMs),
        JAH_FIELD(RayQueryStatus, reflectMotionMs),
        JAH_FIELD(RayQueryStatus, reflectHistoryCount),
        JAH_FIELD(RayQueryStatus, reflectHistorySe),
        JAH_FIELD(RayQueryStatus, reflectHistoryW),
        JAH_FIELD(RayQueryStatus, reflectHistoryH),
        JAH_FIELD(RayQueryStatus, reflectHistoryFrame),
        JAH_FIELD(RayQueryStatus, reflectFrame),
        JAH_FIELD(RayQueryStatus, reflectRestFrames),
        JAH_FIELD(RayQueryStatus, reflectHeld),
        JAH_FIELD(RayQueryStatus, reflectRestActive),
        JAH_FIELD(RayQueryStatus, reflectRestCapped),
        JAH_FIELD(RayQueryStatus, reflectRests),
        JAH_FIELD(RayQueryStatus, reflectShotValid),
        JAH_FIELD(RayQueryStatus, reflectShotRestFrames),
        JAH_FIELD(RayQueryStatus, reflectShotHeld),
        JAH_FIELD(RayQueryStatus, reflectShotRests),
        JAH_FIELD(RayQueryStatus, reflectRestMoverFrames),
        JAH_FIELD(RayQueryStatus, reflectCameraCuts),
        JAH_FIELD(RayQueryStatus, reflectShapeRefusals),
    });
    return t;
}

const Table<RayQueryStatus::ReflectEmitter> &Record<RayQueryStatus::ReflectEmitter>::table()
{
    static const Table<RayQueryStatus::ReflectEmitter> t({
        JAH_FIELD(RayQueryStatus::ReflectEmitter, slot),
        JAH_FIELD(RayQueryStatus::ReflectEmitter, centre),
        JAH_FIELD(RayQueryStatus::ReflectEmitter, radius),
        JAH_FIELD(RayQueryStatus::ReflectEmitter, radiance),
    });
    return t;
}

const Table<RayQueryStatus::HitListView> &Record<RayQueryStatus::HitListView>::table()
{
    static const Table<RayQueryStatus::HitListView> t({
        JAH_FIELD(RayQueryStatus::HitListView, eyes),
        JAH_FIELD(RayQueryStatus::HitListView, photonTerm),
        JAH_FIELD(RayQueryStatus::HitListView, eyeWidth),
        JAH_FIELD(RayQueryStatus::HitListView, eyeHeight),
        JAH_FIELD(RayQueryStatus::HitListView, firstCapacity),
        JAH_FIELD(RayQueryStatus::HitListView, capacity),
        JAH_FIELD(RayQueryStatus::HitListView, records),
        JAH_FIELD(RayQueryStatus::HitListView, dropped),
    });
    return t;
}

const Table<SunContactStatus> &Record<SunContactStatus>::table()
{
    static const Table<SunContactStatus> t({
        JAH_FIELD(SunContactStatus, on),
        JAH_FIELD(SunContactStatus, running),
        JAH_FIELD(SunContactStatus, width),
        JAH_FIELD(SunContactStatus, height),
        JAH_FIELD(SunContactStatus, targetW),
        JAH_FIELD(SunContactStatus, targetH),
        JAH_FIELD(SunContactStatus, divisor),
        JAH_FIELD(SunContactStatus, rays),
        JAH_FIELD(SunContactStatus, range),
        JAH_FIELD(SunContactStatus, toSun),
        JAH_FIELD(SunContactStatus, gpuMs),
        JAH_FIELD(SunContactStatus, cpuMs),
        JAH_FIELD(SunContactStatus, reason),
    });
    return t;
}

const Table<AreaVisStatus> &Record<AreaVisStatus>::table()
{
    static const Table<AreaVisStatus> t({
        JAH_FIELD(AreaVisStatus, running),
        JAH_FIELD(AreaVisStatus, lights),
        JAH_FIELD(AreaVisStatus, strata),
        JAH_FIELD(AreaVisStatus, traceW),
        JAH_FIELD(AreaVisStatus, traceH),
        JAH_FIELD(AreaVisStatus, targetW),
        JAH_FIELD(AreaVisStatus, targetH),
        JAH_FIELD(AreaVisStatus, restFrames),
        JAH_FIELD(AreaVisStatus, held),
        JAH_FIELD(AreaVisStatus, rays),
        JAH_FIELD(AreaVisStatus, traceMs),
        JAH_FIELD(AreaVisStatus, resolveMs),
        JAH_FIELD(AreaVisStatus, cpuMs),
        JAH_FIELD(AreaVisStatus, vramBytes),
        JAH_FIELD(AreaVisStatus, reason),
        JAH_FIELD(AreaVisStatus, vis),
        JAH_FIELD(AreaVisStatus, visW),
        JAH_FIELD(AreaVisStatus, visH),
    });
    return t;
}

const Table<PublishedFrame::AtomHidden> &Record<PublishedFrame::AtomHidden>::table()
{
    static const Table<PublishedFrame::AtomHidden> t({
        JAH_FIELD(PublishedFrame::AtomHidden, valid),
        JAH_FIELD(PublishedFrame::AtomHidden, drawn),
        JAH_FIELD(PublishedFrame::AtomHidden, hidden),
    });
    return t;
}

const Table<MobilityStatus> &Record<MobilityStatus>::table()
{
    static const Table<MobilityStatus> t({
        JAH_FIELD(MobilityStatus, movableItems),
        JAH_FIELD(MobilityStatus, movableLights),
        JAH_FIELD(MobilityStatus, movableNodes),
    });
    return t;
}

const Table<RigStats> &Record<RigStats>::table()
{
    static const Table<RigStats> t({
        JAH_FIELD(RigStats, rigged),
        JAH_FIELD(RigStats, instances),
        JAH_FIELD(RigStats, shared),
        JAH_FIELD(RigStats, streamedBones),
    });
    return t;
}

const Table<SceneChangeStats> &Record<SceneChangeStats>::table()
{
    static const Table<SceneChangeStats> t({
        JAH_FIELD(SceneChangeStats, itemsCreated),
        JAH_FIELD(SceneChangeStats, itemsDestroyed),
        JAH_FIELD(SceneChangeStats, giInvalidations),
        JAH_FIELD(SceneChangeStats, probeBuildRestarts),
        JAH_FIELD(SceneChangeStats, engineAssignedIds),
    });
    return t;
}

const Table<GpuSceneStatus> &Record<GpuSceneStatus>::table()
{
    static const Table<GpuSceneStatus> t({
        JAH_FIELD(GpuSceneStatus, live),
        JAH_FIELD(GpuSceneStatus, slotCount),
        JAH_FIELD(GpuSceneStatus, capacity),
        JAH_FIELD(GpuSceneStatus, meshEntries),
        JAH_FIELD(GpuSceneStatus, lastDirty),
        JAH_FIELD(GpuSceneStatus, writes),
        JAH_FIELD(GpuSceneStatus, copyRuns),
        JAH_FIELD(GpuSceneStatus, updates),
        JAH_FIELD(GpuSceneStatus, grows),
        JAH_FIELD(GpuSceneStatus, scans),
        JAH_FIELD(GpuSceneStatus, aabbReads),
        JAH_FIELD(GpuSceneStatus, lastCopyMs),
        JAH_FIELD(GpuSceneStatus, lastScanMicros),
        JAH_FIELD(GpuSceneStatus, feedNotifies),
    });
    return t;
}

const Table<PublishedFrame::NodeCount> &Record<PublishedFrame::NodeCount>::table()
{
    static const Table<PublishedFrame::NodeCount> t({
        JAH_FIELD(PublishedFrame::NodeCount, node),
        JAH_FIELD(PublishedFrame::NodeCount, count),
    });
    return t;
}

const Table<PublishedFrame::RigPoses> &Record<PublishedFrame::RigPoses>::table()
{
    static const Table<PublishedFrame::RigPoses> t({
        JAH_FIELD(PublishedFrame::RigPoses, node),
        JAH_FIELD(PublishedFrame::RigPoses, poses),
    });
    return t;
}

const Table<BonePose> &Record<BonePose>::table()
{
    static const Table<BonePose> t({
        JAH_FIELD(BonePose, position),
        JAH_FIELD(BonePose, rotation),
        JAH_FIELD(BonePose, scale),
    });
    return t;
}

const Table<PublishedFrame::NodePose> &Record<PublishedFrame::NodePose>::table()
{
    static const Table<PublishedFrame::NodePose> t({
        JAH_FIELD(PublishedFrame::NodePose, node),
        JAH_FIELD(PublishedFrame::NodePose, position),
        JAH_FIELD(PublishedFrame::NodePose, rotation),
    });
    return t;
}

const Table<ViewFrames> &Record<ViewFrames>::table()
{
    static const Table<ViewFrames> t({
        JAH_FIELD(ViewFrames, id),
        JAH_FIELD(ViewFrames, name),
        JAH_FIELD(ViewFrames, presented),
        JAH_FIELD(ViewFrames, blank),
        JAH_FIELD(ViewFrames, width),
        JAH_FIELD(ViewFrames, height),
        JAH_FIELD(ViewFrames, sampleCount),
        JAH_FIELD(ViewFrames, pipGeneration),
        JAH_FIELD(ViewFrames, workspaceGeneration),
        JAH_FIELD(ViewFrames, globalsPushes),
        JAH_FIELD(ViewFrames, postFx),
        JAH_FIELD(ViewFrames, routeGather),
        JAH_FIELD(ViewFrames, routeIntegrate),
        JAH_FIELD(ViewFrames, routeReflect),
    });
    return t;
}

const Table<PublishedEngineFrame> &Record<PublishedEngineFrame>::table()
{
    static const Table<PublishedEngineFrame> t({
        JAH_FIELD(PublishedEngineFrame, frame),
        JAH_FIELD(PublishedEngineFrame, serial),
        JAH_FIELD(PublishedEngineFrame, recordApplied),
        JAH_FIELD(PublishedEngineFrame, handOver),
        JAH_FIELD(PublishedEngineFrame, stepSerialDrawn),
        JAH_FIELD(PublishedEngineFrame, poseSerial),
        JAH_FIELD(PublishedEngineFrame, displayTimeNs),
        JAH_FIELD(PublishedEngineFrame, presentAlpha),
        JAH_FIELD(PublishedEngineFrame, presentFree),
        JAH_FIELD(PublishedEngineFrame, input),
        JAH_FIELD(PublishedEngineFrame, vr),
        JAH_FIELD(PublishedEngineFrame, vrState),
        JAH_FIELD(PublishedEngineFrame, vrInfo),
        JAH_FIELD(PublishedEngineFrame, vrJoints),
        JAH_FIELD(PublishedEngineFrame, vrBindingBlocks),
        JAH_FIELD(PublishedEngineFrame, streaming),
        JAH_FIELD(PublishedEngineFrame, shaders),
        JAH_FIELD(PublishedEngineFrame, hasEnabledViews),
        JAH_FIELD(PublishedEngineFrame, paceOwesWork),
        JAH_FIELD(PublishedEngineFrame, streamingFrames),
        JAH_FIELD(PublishedEngineFrame, views),
        JAH_FIELD(PublishedEngineFrame, frameError),
        JAH_FIELD(PublishedEngineFrame, frameErrors),
        JAH_FIELD(PublishedEngineFrame, framesWithError),
        JAH_FIELD(PublishedEngineFrame, refusals),
        JAH_FIELD(PublishedEngineFrame, vrSessionLive),
        JAH_FIELD(PublishedEngineFrame, completions),
        JAH_FIELD(PublishedEngineFrame, deviceLost),
        JAH_FIELD(PublishedEngineFrame, gpuFault),
        JAH_FIELD(PublishedEngineFrame, monitor),
        JAH_FIELD(PublishedEngineFrame, shadow),
        JAH_FIELD(PublishedEngineFrame, render),
        JAH_FIELD(PublishedEngineFrame, publishMicros),
    });
    return t;
}

const Table<FrameInputStatus> &Record<FrameInputStatus>::table()
{
    static const Table<FrameInputStatus> t({
        JAH_FIELD(FrameInputStatus, drained),
        JAH_FIELD(FrameInputStatus, drainedEmpty),
        JAH_FIELD(FrameInputStatus, fieldsApplied),
        JAH_FIELD(FrameInputStatus, viewFieldsApplied),
        JAH_FIELD(FrameInputStatus, gestureActive),
        JAH_FIELD(FrameInputStatus, changesMoved),
        JAH_FIELD(FrameInputStatus, changesLights),
        JAH_FIELD(FrameInputStatus, changesIncomplete),
        JAH_FIELD(FrameInputStatus, nodeRecords),
        JAH_FIELD(FrameInputStatus, nodeTransforms),
        JAH_FIELD(FrameInputStatus, messages),
        JAH_FIELD(FrameInputStatus, messagesTotal),
        JAH_FIELD(FrameInputStatus, messagesRefused),
        JAH_FIELD(FrameInputStatus, nodeRecordsTotal),
        JAH_FIELD(FrameInputStatus, messagesCarried),
        JAH_FIELD(FrameInputStatus, drainsCut),
        JAH_FIELD(FrameInputStatus, drainMicros),
        JAH_FIELD(FrameInputStatus, drainNameMicros),
        JAH_FIELD(FrameInputStatus, drainRecordsMicros),
        JAH_FIELD(FrameInputStatus, drainInterpolateMicros),
        JAH_FIELD(FrameInputStatus, drainRecordLookups),
        JAH_FIELD(FrameInputStatus, drainNameVisits),
        JAH_FIELD(FrameInputStatus, drainNameLookups),
    });
    return t;
}

const Table<VrStatus> &Record<VrStatus>::table()
{
    static const Table<VrStatus> t({
        JAH_FIELD(VrStatus, state),
        JAH_FIELD(VrStatus, active),
        JAH_FIELD(VrStatus, frames),
        JAH_FIELD(VrStatus, rendered),
        JAH_FIELD(VrStatus, framesOffRenderThread),
        JAH_FIELD(VrStatus, displayTimeNs),
        JAH_FIELD(VrStatus, refreshHz),
        JAH_FIELD(VrStatus, missedDisplayPeriods),
        JAH_FIELD(VrStatus, ipd),
        JAH_FIELD(VrStatus, eyeWidth),
        JAH_FIELD(VrStatus, eyeHeight),
        JAH_FIELD(VrStatus, swapchainFormat),
        JAH_FIELD(VrStatus, colourEncodedOnce),
        JAH_FIELD(VrStatus, warmUpFrames),
        JAH_FIELD(VrStatus, warmUpMs),
        JAH_FIELD(VrStatus, mirror),
        JAH_FIELD(VrStatus, mirrorShowing),
        JAH_FIELD(VrStatus, worldScale),
        JAH_FIELD(VrStatus, asymmetricFov),
        JAH_FIELD(VrStatus, headPosition),
        JAH_FIELD(VrStatus, headRotation),
        JAH_FIELD(VrStatus, posesValid),
        JAH_FIELD(VrStatus, origin),
        JAH_FIELD(VrStatus, originYaw),
        JAH_FIELD(VrStatus, originSeq),
        JAH_FIELD(VrStatus, spaceChanges),
        JAH_FIELD(VrStatus, hiddenAreaSource),
        JAH_FIELD(VrStatus, hiddenAreaFraction),
        JAH_FIELD(VrStatus, hiddenAreaTriangles),
        JAH_FIELD(VrStatus, hands),
        JAH_FIELD(VrStatus, input),
        JAH_FIELD(VrStatus, inputFocused),
        JAH_FIELD(VrStatus, profile),
        JAH_FIELD(VrStatus, bindingProfiles),
        JAH_FIELD(VrStatus, bindingProfilesAccepted),
        JAH_FIELD(VrStatus, handActions),
        JAH_FIELD(VrStatus, handJoints),
        JAH_FIELD(VrStatus, handsEnabled),
    });
    return t;
}

const Table<VrPose> &Record<VrPose>::table()
{
    static const Table<VrPose> t({
        JAH_FIELD(VrPose, valid),
        JAH_FIELD(VrPose, position),
        JAH_FIELD(VrPose, rotation),
    });
    return t;
}

const Table<VrHandState> &Record<VrHandState>::table()
{
    static const Table<VrHandState> t({
        JAH_FIELD(VrHandState, valid),
        JAH_FIELD(VrHandState, aim),
        JAH_FIELD(VrHandState, grip),
        JAH_FIELD(VrHandState, manipPose),
        JAH_FIELD(VrHandState, select),
        JAH_FIELD(VrHandState, selectPressed),
        JAH_FIELD(VrHandState, grab),
        JAH_FIELD(VrHandState, grabPressed),
        JAH_FIELD(VrHandState, menuPressed),
        JAH_FIELD(VrHandState, stickX),
        JAH_FIELD(VrHandState, stickY),
        JAH_FIELD(VrHandState, stickPressed),
        JAH_FIELD(VrHandState, profile),
        JAH_FIELD(VrHandState, jointsTracked),
        JAH_FIELD(VrHandState, fromInjection),
    });
    return t;
}

const Table<VrProfileName> &Record<VrProfileName>::table()
{
    static const Table<VrProfileName> t({
        JAH_FIELD(VrProfileName, text).bytes(),
    });
    return t;
}

const Table<VrInfo> &Record<VrInfo>::table()
{
    static const Table<VrInfo> t({
        JAH_FIELD(VrInfo, available),
        JAH_FIELD(VrInfo, reason),
        JAH_FIELD(VrInfo, failure),
        JAH_FIELD(VrInfo, manifest),
        JAH_FIELD(VrInfo, manifestRuntime),
        JAH_FIELD(VrInfo, openxrCalls),
        JAH_FIELD(VrInfo, runtime),
        JAH_FIELD(VrInfo, runtimeVersion),
        JAH_FIELD(VrInfo, system),
        JAH_FIELD(VrInfo, apiMajor),
        JAH_FIELD(VrInfo, apiMinor),
        JAH_FIELD(VrInfo, eyeWidth),
        JAH_FIELD(VrInfo, eyeHeight),
        JAH_FIELD(VrInfo, refreshHz),
        JAH_FIELD(VrInfo, space),
        JAH_FIELD(VrInfo, visibilityMask),
        JAH_FIELD(VrInfo, depthLayer),
    });
    return t;
}

const Table<VrBindingBlock> &Record<VrBindingBlock>::table()
{
    static const Table<VrBindingBlock> t({
        JAH_FIELD(VrBindingBlock, profile),
        JAH_FIELD(VrBindingBlock, bindings),
        JAH_FIELD(VrBindingBlock, accepted),
    });
    return t;
}

const Table<StreamingWork> &Record<StreamingWork>::table()
{
    static const Table<StreamingWork> t({
        JAH_FIELD(StreamingWork, giStagesLeft),
        JAH_FIELD(StreamingWork, giStage),
        JAH_FIELD(StreamingWork, materialsAwaitingTexture),
        JAH_FIELD(StreamingWork, shadersCompiled),
        JAH_FIELD(StreamingWork, giRebuilds),
        JAH_FIELD(StreamingWork, textureDrains),
        JAH_FIELD(StreamingWork, textureDrainLanded),
    });
    return t;
}

const Table<AsyncShaderStats> &Record<AsyncShaderStats>::table()
{
    static const Table<AsyncShaderStats> t({
        JAH_FIELD(AsyncShaderStats, running),
        JAH_FIELD(AsyncShaderStats, threads),
        JAH_FIELD(AsyncShaderStats, pending),
        JAH_FIELD(AsyncShaderStats, completed),
        JAH_FIELD(AsyncShaderStats, failed),
        JAH_FIELD(AsyncShaderStats, placeholderDraws),
        JAH_FIELD(AsyncShaderStats, pendingSkips),
        JAH_FIELD(AsyncShaderStats, captureDeferredDraws),
        JAH_FIELD(AsyncShaderStats, compiledInBackground),
        JAH_FIELD(AsyncShaderStats, compiledInForeground),
        JAH_FIELD(AsyncShaderStats, paramsMisfits),
        JAH_FIELD(AsyncShaderStats, compileErrors),
        JAH_FIELD(AsyncShaderStats, computeParsedInFrame),
        JAH_FIELD(AsyncShaderStats, computePsoInFrame),
        JAH_FIELD(AsyncShaderStats, computeDestroyWaits),
        JAH_FIELD(AsyncShaderStats, computeDestroyWaitMs),
        JAH_FIELD(AsyncShaderStats, computeJoins),
        JAH_FIELD(AsyncShaderStats, computeJoinMs),
        JAH_FIELD(AsyncShaderStats, glslangCompiles),
        JAH_FIELD(AsyncShaderStats, glslangMs),
        JAH_FIELD(AsyncShaderStats, graphicsPsos),
        JAH_FIELD(AsyncShaderStats, graphicsPsoMs),
        JAH_FIELD(AsyncShaderStats, computePsos),
        JAH_FIELD(AsyncShaderStats, computePsoMs),
        JAH_FIELD(AsyncShaderStats, graphicsBuiltInFrame),
        JAH_FIELD(AsyncShaderStats, graphicsCompiledInFrame),
        JAH_FIELD(AsyncShaderStats, lowLevelBuiltInFrame),
        JAH_FIELD(AsyncShaderStats, rayBuildsJoinedWaiting),
        JAH_FIELD(AsyncShaderStats, rayBuildsInFlight),
        JAH_FIELD(AsyncShaderStats, computeDeferred),
        JAH_FIELD(AsyncShaderStats, rayBuildsLanded),
        JAH_FIELD(AsyncShaderStats, submitted),
        JAH_FIELD(AsyncShaderStats, aheadPasses),
        JAH_FIELD(AsyncShaderStats, aheadRun),
        JAH_FIELD(AsyncShaderStats, aheadPromoted),
        JAH_FIELD(AsyncShaderStats, aheadOverOwn),
        JAH_FIELD(AsyncShaderStats, submittedGraphics),
        JAH_FIELD(AsyncShaderStats, shaderBurst),
        JAH_FIELD(AsyncShaderStats, rayBurst),
    });
    return t;
}

const Table<AsyncShaderStats::Burst> &Record<AsyncShaderStats::Burst>::table()
{
    static const Table<AsyncShaderStats::Burst> t({
        JAH_FIELD(AsyncShaderStats::Burst, landed),
        JAH_FIELD(AsyncShaderStats::Burst, total),
    });
    return t;
}

const Table<PublishedEngineFrame::FrameError> &Record<PublishedEngineFrame::FrameError>::table()
{
    static const Table<PublishedEngineFrame::FrameError> t({
        JAH_FIELD(PublishedEngineFrame::FrameError, serial),
        JAH_FIELD(PublishedEngineFrame::FrameError, frame),
        JAH_FIELD(PublishedEngineFrame::FrameError, text),
    });
    return t;
}

const Table<PublishedEngineFrame::Refusal> &Record<PublishedEngineFrame::Refusal>::table()
{
    static const Table<PublishedEngineFrame::Refusal> t({
        JAH_FIELD(PublishedEngineFrame::Refusal, frame),
        JAH_FIELD(PublishedEngineFrame::Refusal, text),
    });
    return t;
}

const Table<MonitorStatus> &Record<MonitorStatus>::table()
{
    static const Table<MonitorStatus> t({
        JAH_FIELD(MonitorStatus, level),
        JAH_FIELD(MonitorStatus, attachedListeners),
        JAH_FIELD(MonitorStatus, ringCapacity),
        JAH_FIELD(MonitorStatus, ringFrames),
        JAH_FIELD(MonitorStatus, pendingEvents),
        JAH_FIELD(MonitorStatus, framesRecorded),
        JAH_FIELD(MonitorStatus, framesDropped),
        JAH_FIELD(MonitorStatus, eventsDropped),
        JAH_FIELD(MonitorStatus, gpuCompiled),
        JAH_FIELD(MonitorStatus, gpuSupported),
        JAH_FIELD(MonitorStatus, gpuActive),
        JAH_FIELD(MonitorStatus, gpuQueryPools),
        JAH_FIELD(MonitorStatus, gpuMarksDropped),
        JAH_FIELD(MonitorStatus, gpuFramesAgedOut),
        JAH_FIELD(MonitorStatus, gpuReason),
        JAH_FIELD(MonitorStatus, overheadMs),
    });
    return t;
}

const Table<ShadowStatus> &Record<ShadowStatus>::table()
{
    static const Table<ShadowStatus> t({
        JAH_FIELD(ShadowStatus, live),
        JAH_FIELD(ShadowStatus, resolution),
        JAH_FIELD(ShadowStatus, maps),
        JAH_FIELD(ShadowStatus, pssmSplits),
        JAH_FIELD(ShadowStatus, focusedMaps),
        JAH_FIELD(ShadowStatus, lightSlots),
        JAH_FIELD(ShadowStatus, casters),
        JAH_FIELD(ShadowStatus, budget),
        JAH_FIELD(ShadowStatus, requestedBudget),
        JAH_FIELD(ShadowStatus, atlasWidth),
        JAH_FIELD(ShadowStatus, atlasHeight),
        JAH_FIELD(ShadowStatus, atlasBytes),
        JAH_FIELD(ShadowStatus, reflectAtlasBytes),
        JAH_FIELD(ShadowStatus, probeAtlasBytes),
        JAH_FIELD(ShadowStatus, mapped),
        JAH_FIELD(ShadowStatus, unmapped),
        JAH_FIELD(ShadowStatus, countersMeasured),
        JAH_FIELD(ShadowStatus, shadowPassesLastFrame),
        JAH_FIELD(ShadowStatus, cachedMapRendersLastFrame),
        JAH_FIELD(ShadowStatus, shaderLightMismatches),
        JAH_FIELD(ShadowStatus, reflectPassesLastFrame),
        JAH_FIELD(ShadowStatus, probePassesLastFrame),
        JAH_FIELD(ShadowStatus, reflectLampPassesLastFrame),
        JAH_FIELD(ShadowStatus, probeLampPassesLastFrame),
        JAH_FIELD(ShadowStatus, cachedInstances),
        JAH_FIELD(ShadowStatus, uncachedInstances),
        JAH_FIELD(ShadowStatus, viewCached),
        JAH_FIELD(ShadowStatus, mapsDirtiedLastFrame),
        JAH_FIELD(ShadowStatus, atlasRebuilds),
        JAH_FIELD(ShadowStatus, casterWalkItems),
    });
    return t;
}

const Table<ShadowMapInfo> &Record<ShadowMapInfo>::table()
{
    static const Table<ShadowMapInfo> t({
        JAH_FIELD(ShadowMapInfo, slot),
        JAH_FIELD(ShadowMapInfo, node),
        JAH_FIELD(ShadowMapInfo, isCached),
        JAH_FIELD(ShadowMapInfo, dirty),
        JAH_FIELD(ShadowMapInfo, pssm),
        JAH_FIELD(ShadowMapInfo, passesLastFrame),
    });
    return t;
}

const Table<RenderStats> &Record<RenderStats>::table()
{
    static const Table<RenderStats> t({
        JAH_FIELD(RenderStats, metricsRecording),
        JAH_FIELD(RenderStats, fps),
        JAH_FIELD(RenderStats, frameMs),
        JAH_FIELD(RenderStats, lastMs),
        JAH_FIELD(RenderStats, p95Ms),
        JAH_FIELD(RenderStats, p99Ms),
        JAH_FIELD(RenderStats, bestMs),
        JAH_FIELD(RenderStats, worstMs),
        JAH_FIELD(RenderStats, draws),
        JAH_FIELD(RenderStats, batches),
        JAH_FIELD(RenderStats, triangles),
        JAH_FIELD(RenderStats, vertices),
        JAH_FIELD(RenderStats, instances),
        JAH_FIELD(RenderStats, gpuCountLagFrames),
        JAH_FIELD(RenderStats, incompletePsoRequests),
        JAH_FIELD(RenderStats, forwardPlusLights),
        JAH_FIELD(RenderStats, forwardPlusBudget),
        JAH_FIELD(RenderStats, forwardPlusOverBudget),
        JAH_FIELD(RenderStats, resourceAdvances),
    });
    return t;
}

const Table<MonitorRecord> &Record<MonitorRecord>::table()
{
    static const Table<MonitorRecord> t({
        JAH_FIELD(MonitorRecord, frame),
        JAH_FIELD(MonitorRecord, serial),
        JAH_FIELD(MonitorRecord, startMs),
        JAH_FIELD(MonitorRecord, totalMs),
        JAH_FIELD(MonitorRecord, cause),
        JAH_FIELD(MonitorRecord, onscreen),
        JAH_FIELD(MonitorRecord, scenesUpdated),
        JAH_FIELD(MonitorRecord, inputMoved),
        JAH_FIELD(MonitorRecord, inputGesture),
        JAH_FIELD(MonitorRecord, inputFieldsApplied),
        JAH_FIELD(MonitorRecord, stages),
        JAH_FIELD(MonitorRecord, passes),
        JAH_FIELD(MonitorRecord, cacheWork),
        JAH_FIELD(MonitorRecord, draws),
        JAH_FIELD(MonitorRecord, batches),
        JAH_FIELD(MonitorRecord, instances),
        JAH_FIELD(MonitorRecord, triangles),
        JAH_FIELD(MonitorRecord, probeCaptures),
        JAH_FIELD(MonitorRecord, cascadeRebuilds),
        JAH_FIELD(MonitorRecord, shadowPasses),
        JAH_FIELD(MonitorRecord, shadowPassesReflect),
        JAH_FIELD(MonitorRecord, shadowPassesProbe),
        JAH_FIELD(MonitorRecord, planarRenders),
        JAH_FIELD(MonitorRecord, shaderCompiles),
        JAH_FIELD(MonitorRecord, shaderCompileErrors),
        JAH_FIELD(MonitorRecord, incompleteScenes),
        JAH_FIELD(MonitorRecord, incompleteFramesMax),
        JAH_FIELD(MonitorRecord, metricsRecording),
        JAH_FIELD(MonitorRecord, orphanedPasses),
        JAH_FIELD(MonitorRecord, textureWaitMs),
        JAH_FIELD(MonitorRecord, gpuMs),
        JAH_FIELD(MonitorRecord, frameGpuMs),
        JAH_FIELD(MonitorRecord, gpuIdleMs),
        JAH_FIELD(MonitorRecord, unattributedGpuMs),
        JAH_FIELD(MonitorRecord, rowBubbleGpuMs),
        JAH_FIELD(MonitorRecord, widestGapMs),
        JAH_FIELD(MonitorRecord, widestGap),
        JAH_FIELD(MonitorRecord, gpuMarksDropped),
        JAH_FIELD(MonitorRecord, stagingFlushes),
        JAH_FIELD(MonitorRecord, stagingWraps),
        JAH_FIELD(MonitorRecord, stagingWaits),
        JAH_FIELD(MonitorRecord, stagingWaitSubmits),
        JAH_FIELD(MonitorRecord, stagingWaitMs),
        JAH_FIELD(MonitorRecord, stagingBytes),
        JAH_FIELD(MonitorRecord, queueFlushes),
        JAH_FIELD(MonitorRecord, queueFencedFlushes),
        JAH_FIELD(MonitorRecord, queueSelfDrains),
        JAH_FIELD(MonitorRecord, queueSelfDrainMs),
        JAH_FIELD(MonitorRecord, overheadMs),
    });
    return t;
}

const Table<FrameStage> &Record<FrameStage>::table()
{
    static const Table<FrameStage> t({
        JAH_FIELD(FrameStage, name),
        JAH_FIELD(FrameStage, ms),
    });
    return t;
}

const Table<FramePass> &Record<FramePass>::table()
{
    static const Table<FramePass> t({
        JAH_FIELD(FramePass, workspace),
        JAH_FIELD(FramePass, node),
        JAH_FIELD(FramePass, pass),
        JAH_FIELD(FramePass, bucket),
        JAH_FIELD(FramePass, shadowMapIdx),
        JAH_FIELD(FramePass, draws),
        JAH_FIELD(FramePass, batches),
        JAH_FIELD(FramePass, instances),
        JAH_FIELD(FramePass, triangles),
        JAH_FIELD(FramePass, orphaned),
        JAH_FIELD(FramePass, cpuMs),
        JAH_FIELD(FramePass, shadowMs),
        JAH_FIELD(FramePass, gpuMs),
        JAH_FIELD(FramePass, topLevel),
        JAH_FIELD(FramePass, selfGpuMs),
        JAH_FIELD(FramePass, selfMeasured),
        JAH_FIELD(FramePass, selfSubmitted),
        JAH_FIELD(FramePass, selfStaging),
        JAH_FIELD(FramePass, bubbleGpuMs),
        JAH_FIELD(FramePass, selfBubbleGpuMs),
        JAH_FIELD(FramePass, insideSubmitted),
        JAH_FIELD(FramePass, readsGi),
    });
    return t;
}

const Table<CacheWork> &Record<CacheWork>::table()
{
    static const Table<CacheWork> t({
        JAH_FIELD(CacheWork, cache),
        JAH_FIELD(CacheWork, reason),
        JAH_FIELD(CacheWork, id),
        JAH_FIELD(CacheWork, detail),
        JAH_FIELD(CacheWork, units),
        JAH_FIELD(CacheWork, ms),
        JAH_FIELD(CacheWork, gpuMs),
        JAH_FIELD(CacheWork, topLevel),
        JAH_FIELD(CacheWork, selfGpuMs),
        JAH_FIELD(CacheWork, selfMeasured),
        JAH_FIELD(CacheWork, selfSubmitted),
        JAH_FIELD(CacheWork, selfStaging),
        JAH_FIELD(CacheWork, bubbleGpuMs),
        JAH_FIELD(CacheWork, selfBubbleGpuMs),
        JAH_FIELD(CacheWork, insideSubmitted),
    });
    return t;
}

const Table<MonitorEvent> &Record<MonitorEvent>::table()
{
    static const Table<MonitorEvent> t({
        JAH_FIELD(MonitorEvent, kind),
        JAH_FIELD(MonitorEvent, frame),
        JAH_FIELD(MonitorEvent, startMs),
        JAH_FIELD(MonitorEvent, ms),
        JAH_FIELD(MonitorEvent, reason),
        JAH_FIELD(MonitorEvent, label),
        JAH_FIELD(MonitorEvent, detail),
        JAH_FIELD(MonitorEvent, value),
    });
    return t;
}

const Table<FrameSubmitStatus> &Record<FrameSubmitStatus>::table()
{
    static const Table<FrameSubmitStatus> t({
        JAH_FIELD(FrameSubmitStatus, submitted),
        JAH_FIELD(FrameSubmitStatus, messagesPosted),
        JAH_FIELD(FrameSubmitStatus, requests),
        JAH_FIELD(FrameSubmitStatus, scenePartsMerged),
        JAH_FIELD(FrameSubmitStatus, nextSerial),
        JAH_FIELD(FrameSubmitStatus, handOvers),
    });
    return t;
}

const Table<ShaderCacheStats> &Record<ShaderCacheStats>::table()
{
    static const Table<ShaderCacheStats> t({
        JAH_FIELD(ShaderCacheStats, enabled),
        JAH_FIELD(ShaderCacheStats, dir),
        JAH_FIELD(ShaderCacheStats, fingerprint),
        JAH_FIELD(ShaderCacheStats, sizeBytes),
        JAH_FIELD(ShaderCacheStats, files),
        JAH_FIELD(ShaderCacheStats, maxBytes),
        JAH_FIELD(ShaderCacheStats, pipelineCacheLoaded),
        JAH_FIELD(ShaderCacheStats, pipelineCacheReason),
        JAH_FIELD(ShaderCacheStats, microcodeLoaded),
        JAH_FIELD(ShaderCacheStats, microcodeEntries),
        JAH_FIELD(ShaderCacheStats, seedDir),
        JAH_FIELD(ShaderCacheStats, seedState),
        JAH_FIELD(ShaderCacheStats, seedEntries),
        JAH_FIELD(ShaderCacheStats, hlmsCachesLoaded),
        JAH_FIELD(ShaderCacheStats, computeRecordsLoaded),
        JAH_FIELD(ShaderCacheStats, computeRecordsRejected),
        JAH_FIELD(ShaderCacheStats, computeRecordsStale),
        JAH_FIELD(ShaderCacheStats, layersNotLoaded),
        JAH_FIELD(ShaderCacheStats, compiledThisRun),
        JAH_FIELD(ShaderCacheStats, loadedThisRun),
        JAH_FIELD(ShaderCacheStats, expectedShaders),
        JAH_FIELD(ShaderCacheStats, lastSavedUnixMs),
        JAH_FIELD(ShaderCacheStats, savesCopied),
        JAH_FIELD(ShaderCacheStats, copyMessages),
        JAH_FIELD(ShaderCacheStats, copyWorstMs),
        JAH_FIELD(ShaderCacheStats, copyChunkWorstMs),
        JAH_FIELD(ShaderCacheStats, copyChunkMaxEntries),
        JAH_FIELD(ShaderCacheStats, copyChunkEntries),
        JAH_FIELD(ShaderCacheStats, copyFrames),
        JAH_FIELD(ShaderCacheStats, copyFrameOverruns),
        JAH_FIELD(ShaderCacheStats, savesWritten),
        JAH_FIELD(ShaderCacheStats, saveInFlight),
        JAH_FIELD(ShaderCacheStats, enginePipelinesThisRun),
        JAH_FIELD(ShaderCacheStats, enginePipelineMs),
        JAH_FIELD(ShaderCacheStats, enginePipelineNames),
        JAH_FIELD(ShaderCacheStats, passCacheEntries),
        JAH_FIELD(ShaderCacheStats, passCacheCapacity),
        JAH_FIELD(ShaderCacheStats, renderableCacheEntries),
        JAH_FIELD(ShaderCacheStats, renderableCacheCapacity),
    });
    return t;
}

const Table<AtomDrawStatus> &Record<AtomDrawStatus>::table()
{
    static const Table<AtomDrawStatus> t({
        JAH_FIELD(AtomDrawStatus, live),
        JAH_FIELD(AtomDrawStatus, on),
        JAH_FIELD(AtomDrawStatus, atomItems),
        JAH_FIELD(AtomDrawStatus, atomTwoSided),
        JAH_FIELD(AtomDrawStatus, atomMasked),
        JAH_FIELD(AtomDrawStatus, pbsItems),
        JAH_FIELD(AtomDrawStatus, notWorld),
        JAH_FIELD(AtomDrawStatus, notPbs),
        JAH_FIELD(AtomDrawStatus, customPiece),
        JAH_FIELD(AtomDrawStatus, blended),
        JAH_FIELD(AtomDrawStatus, cullFront),
        JAH_FIELD(AtomDrawStatus, planar),
        JAH_FIELD(AtomDrawStatus, pending),
        JAH_FIELD(AtomDrawStatus, alphaTested),
        JAH_FIELD(AtomDrawStatus, skinned),
        JAH_FIELD(AtomDrawStatus, noRow),
        JAH_FIELD(AtomDrawStatus, atomCaster),
        JAH_FIELD(AtomDrawStatus, stockItems),
        JAH_FIELD(AtomDrawStatus, viewRoutesValid),
        JAH_FIELD(AtomDrawStatus, viewRouteItems),
        JAH_FIELD(AtomDrawStatus, viewRouteTriangles),
        JAH_FIELD(AtomDrawStatus, materials),
        JAH_FIELD(AtomDrawStatus, buckets),
        JAH_FIELD(AtomDrawStatus, twins),
        JAH_FIELD(AtomDrawStatus, decodeDraws),
        JAH_FIELD(AtomDrawStatus, screenDraws),
        JAH_FIELD(AtomDrawStatus, stereoViews),
        JAH_FIELD(AtomDrawStatus, passthroughViews),
        JAH_FIELD(AtomDrawStatus, viewPaintable),
        JAH_FIELD(AtomDrawStatus, wordSlotVisits),
        JAH_FIELD(AtomDrawStatus, pbsNotes),
        JAH_FIELD(AtomDrawStatus, pbsDatablocks),
        JAH_FIELD(AtomDrawStatus, cutValid),
        JAH_FIELD(AtomDrawStatus, cutClusters),
        JAH_FIELD(AtomDrawStatus, cutIndices),
        JAH_FIELD(AtomDrawStatus, cutEvaluated),
        JAH_FIELD(AtomDrawStatus, cutOverflow),
        JAH_FIELD(AtomDrawStatus, cutMissing),
        JAH_FIELD(AtomDrawStatus, cutOverflowIndices),
        JAH_FIELD(AtomDrawStatus, cutIndexBudget),
        JAH_FIELD(AtomDrawStatus, cutTriangles),
        JAH_FIELD(AtomDrawStatus, cutRejected),
        JAH_FIELD(AtomDrawStatus, cutRejectedOverflow),
        JAH_FIELD(AtomDrawStatus, cutPixelCulled),
        JAH_FIELD(AtomDrawStatus, cutLateClusters),
        JAH_FIELD(AtomDrawStatus, cutLateDropped),
        JAH_FIELD(AtomDrawStatus, cutSlackClusters),
        JAH_FIELD(AtomDrawStatus, cutSkyClusters),
        JAH_FIELD(AtomDrawStatus, cutReserveIndices),
        JAH_FIELD(AtomDrawStatus, cutSlackTriangles),
        JAH_FIELD(AtomDrawStatus, cutSkyTriangles),
        JAH_FIELD(AtomDrawStatus, occlusion),
        JAH_FIELD(AtomDrawStatus, occluded),
        JAH_FIELD(AtomDrawStatus, disoccluded),
        JAH_FIELD(AtomDrawStatus, casterValid),
        JAH_FIELD(AtomDrawStatus, casterMaps),
        JAH_FIELD(AtomDrawStatus, casterClusters),
        JAH_FIELD(AtomDrawStatus, casterInstances),
        JAH_FIELD(AtomDrawStatus, casterOverflow),
        JAH_FIELD(AtomDrawStatus, casterMissing),
        JAH_FIELD(AtomDrawStatus, casterIndexBudget),
        JAH_FIELD(AtomDrawStatus, casterTriangles),
        JAH_FIELD(AtomDrawStatus, casterMapsPeak),
        JAH_FIELD(AtomDrawStatus, casterUnrecorded),
        JAH_FIELD(AtomDrawStatus, casterOccluded),
        JAH_FIELD(AtomDrawStatus, casterLateInstances),
        JAH_FIELD(AtomDrawStatus, casterLateOccluded),
        JAH_FIELD(AtomDrawStatus, casterLateTriangles),
        JAH_FIELD(AtomDrawStatus, casterPerMap),
        JAH_FIELD(AtomDrawStatus, densityValid),
        JAH_FIELD(AtomDrawStatus, densityPixels),
        JAH_FIELD(AtomDrawStatus, densityObjects),
        JAH_FIELD(AtomDrawStatus, densityClassPixels),
        JAH_FIELD(AtomDrawStatus, densityClassObjects),
        JAH_FIELD(AtomDrawStatus, densityDepthPixels),
        JAH_FIELD(AtomDrawStatus, hiddenValid),
        JAH_FIELD(AtomDrawStatus, hiddenDrawnTriangles),
        JAH_FIELD(AtomDrawStatus, drawnHiddenTriangles),
        JAH_FIELD(AtomDrawStatus, frame),
    });
    return t;
}

const Table<AtomCasterMapStats> &Record<AtomCasterMapStats>::table()
{
    static const Table<AtomCasterMapStats> t({
        JAH_FIELD(AtomCasterMapStats, kind),
        JAH_FIELD(AtomCasterMapStats, map),
        JAH_FIELD(AtomCasterMapStats, face),
        JAH_FIELD(AtomCasterMapStats, lightTested),
        JAH_FIELD(AtomCasterMapStats, instances),
        JAH_FIELD(AtomCasterMapStats, occluded),
        JAH_FIELD(AtomCasterMapStats, lateInstances),
        JAH_FIELD(AtomCasterMapStats, lateOccluded),
        JAH_FIELD(AtomCasterMapStats, triangles),
        JAH_FIELD(AtomCasterMapStats, lateTriangles),
        JAH_FIELD(AtomCasterMapStats, pbsInstances),
        JAH_FIELD(AtomCasterMapStats, pbsVisibleInstances),
        JAH_FIELD(AtomCasterMapStats, pbsTriangles),
        JAH_FIELD(AtomCasterMapStats, pbsVisibleTriangles),
        JAH_FIELD(AtomCasterMapStats, receiverTested),
        JAH_FIELD(AtomCasterMapStats, receiverDilation),
    });
    return t;
}

}}}  // namespace jahshaka::engine::script
