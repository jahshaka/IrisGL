// THE STATUS FAMILY's FIELD TABLES (SCRIPT-IN-ENGINE-1 2d): one per record records_status.h declares, every field
// named once in declaration order (records.h), by the member's own name. ANSWERS ONLY (no command takes these
// records): no domain is declared on them. A field the engine adds to a tabled record is added HERE in the same commit
// (the guard breaks the build until it is).
#include "script/records_status.h"

namespace jahshaka { namespace engine { namespace script {

const Table<PublishedFrame::RiderTag> &Record<PublishedFrame::RiderTag>::table()
{
    static const Table<PublishedFrame::RiderTag> t({
        field("rider", &PublishedFrame::RiderTag::rider),
        field("position", &PublishedFrame::RiderTag::position),
        field("rotation", &PublishedFrame::RiderTag::rotation),
        field("scale", &PublishedFrame::RiderTag::scale),
        field("owner", &PublishedFrame::RiderTag::owner),
        field("bone", &PublishedFrame::RiderTag::bone),
    });
    return t;
}

const Table<PublishedFrame::Facts> &Record<PublishedFrame::Facts>::table()
{
    static const Table<PublishedFrame::Facts> t({
        field("shares", &PublishedFrame::Facts::shares),
        field("reflectors", &PublishedFrame::Facts::reflectors),
        field("reflectorArms", &PublishedFrame::Facts::reflectorArms),
        field("clips", &PublishedFrame::Facts::clips),
    });
    return t;
}

const Table<PublishedFrame::Facts::Share> &Record<PublishedFrame::Facts::Share>::table()
{
    static const Table<PublishedFrame::Facts::Share> t({
        field("follower", &PublishedFrame::Facts::Share::follower),
        field("source", &PublishedFrame::Facts::Share::source),
    });
    return t;
}

const Table<PublishedFrame::Facts::NodeClips> &Record<PublishedFrame::Facts::NodeClips>::table()
{
    static const Table<PublishedFrame::Facts::NodeClips> t({
        field("node", &PublishedFrame::Facts::NodeClips::node),
        field("clips", &PublishedFrame::Facts::NodeClips::clips),
    });
    return t;
}

const Table<PublishedFrame::Facts::Clip> &Record<PublishedFrame::Facts::Clip>::table()
{
    static const Table<PublishedFrame::Facts::Clip> t({
        field("id", &PublishedFrame::Facts::Clip::id),
        field("name", &PublishedFrame::Facts::Clip::name),
    });
    return t;
}

const Table<PublishedFrame::Completion> &Record<PublishedFrame::Completion>::table()
{
    static const Table<PublishedFrame::Completion> t({
        field("seq", &PublishedFrame::Completion::seq),
        field("token", &PublishedFrame::Completion::token),
        field("ok", &PublishedFrame::Completion::ok),
        field("error", &PublishedFrame::Completion::error),
        field("frame", &PublishedFrame::Completion::frame),
    });
    return t;
}

const Table<GiStatus> &Record<GiStatus>::table()
{
    static const Table<GiStatus> t({
        field("mode", &GiStatus::mode),
        field("updateBudget", &GiStatus::updateBudget),
        field("probeCount", &GiStatus::probeCount),
        field("probeGridBudgetBytes", &GiStatus::probeGridBudgetBytes),
        field("probeGridBudgetProbes", &GiStatus::probeGridBudgetProbes),
        field("probeGridBytes", &GiStatus::probeGridBytes),
        field("probesOverBudget", &GiStatus::probesOverBudget),
        field("pccBound", &GiStatus::pccBound),
        field("vctBound", &GiStatus::vctBound),
        field("boundsMin", &GiStatus::boundsMin),
        field("boundsMax", &GiStatus::boundsMax),
        field("voxelMetres", &GiStatus::voxelMetres),
        field("probeRegionMin", &GiStatus::probeRegionMin),
        field("probeRegionMax", &GiStatus::probeRegionMax),
        field("probeFitMin", &GiStatus::probeFitMin),
        field("probeFitMax", &GiStatus::probeFitMax),
        field("probeHdr", &GiStatus::probeHdr),
        field("probeShadows", &GiStatus::probeShadows),
        field("probeCaptureSize", &GiStatus::probeCaptureSize),
        field("probesDropped", &GiStatus::probesDropped),
        field("probeGridByRays", &GiStatus::probeGridByRays),
        field("probePlacements", &GiStatus::probePlacements),
        field("probeReplacements", &GiStatus::probeReplacements),
        field("probeCapturesTotal", &GiStatus::probeCapturesTotal),
        field("probeUpdatesPerFrame", &GiStatus::probeUpdatesPerFrame),
        field("probeShapeMin", &GiStatus::probeShapeMin),
        field("probeShapeMax", &GiStatus::probeShapeMax),
        field("worstProbeShapeCellRatio", &GiStatus::worstProbeShapeCellRatio),
        field("probesExceedingCell", &GiStatus::probesExceedingCell),
        field("probesClampedToRegion", &GiStatus::probesClampedToRegion),
        field("probeGateCrossings", &GiStatus::probeGateCrossings),
        field("cubemapProbeSlotsPerCell", &GiStatus::cubemapProbeSlotsPerCell),
        field("reusedLastRefresh", &GiStatus::reusedLastRefresh),
        field("ifdBound", &GiStatus::ifdBound),
        field("ifdProbes", &GiStatus::ifdProbes),
        field("ifdProbesPerFrame", &GiStatus::ifdProbesPerFrame),
        field("ifdMin", &GiStatus::ifdMin),
        field("ifdMax", &GiStatus::ifdMax),
        field("ifdFollows", &GiStatus::ifdFollows),
        field("ifdScrollProbes", &GiStatus::ifdScrollProbes),
        field("ifdScrolls", &GiStatus::ifdScrolls),
        field("ifdReplacements", &GiStatus::ifdReplacements),
        field("ifdTargetSamples", &GiStatus::ifdTargetSamples),
        field("ifdRestartDepth", &GiStatus::ifdRestartDepth),
        field("ifdMarkLag", &GiStatus::ifdMarkLag),
        field("ifdOwedRestarts", &GiStatus::ifdOwedRestarts),
        field("ifdOwedRefines", &GiStatus::ifdOwedRefines),
        field("ifdOwedIntegrations", &GiStatus::ifdOwedIntegrations),
        field("ifdWorking", &GiStatus::ifdWorking),
        field("ifdMarkedByMoves", &GiStatus::ifdMarkedByMoves),
        field("ifdMarkedByRegion", &GiStatus::ifdMarkedByRegion),
        field("ifdMarkAlls", &GiStatus::ifdMarkAlls),
        field("ifdRestarts", &GiStatus::ifdRestarts),
        field("ifdReportsKept", &GiStatus::ifdReportsKept),
        field("ifdDispatchedLastFrame", &GiStatus::ifdDispatchedLastFrame),
        field("ifdGrantLastFrame", &GiStatus::ifdGrantLastFrame),
        field("ifdReadUnheld", &GiStatus::ifdReadUnheld),
        field("ifdIntegrations", &GiStatus::ifdIntegrations),
        field("giAtRest", &GiStatus::giAtRest),
        field("chainOwed", &GiStatus::chainOwed),
        field("regionFrame", &GiStatus::regionFrame),
        field("regionCount", &GiStatus::regionCount),
        field("regionsByKind", &GiStatus::regionsByKind),
        field("regionOverflow", &GiStatus::regionOverflow),
        field("regionRestart", &GiStatus::regionRestart),
        field("regionMerges", &GiStatus::regionMerges),
        field("regionCloses", &GiStatus::regionCloses),
        field("regionClosesListing", &GiStatus::regionClosesListing),
        field("ssaoSuppressed", &GiStatus::ssaoSuppressed),
        field("ssaoSuppressedReason", &GiStatus::ssaoSuppressedReason),
        field("probeCapturesLastFrame", &GiStatus::probeCapturesLastFrame),
        field("probeCapturesDeferred", &GiStatus::probeCapturesDeferred),
        field("staleProbes", &GiStatus::staleProbes),
        field("lastStaleReason", &GiStatus::lastStaleReason),
        field("staleSerial", &GiStatus::staleSerial),
        field("rebuilds", &GiStatus::rebuilds),
        field("giScans", &GiStatus::giScans),
        field("giScansFull", &GiStatus::giScansFull),
        field("giScanMicros", &GiStatus::giScanMicros),
        field("giAabbReads", &GiStatus::giAabbReads),
        field("cascades", &GiStatus::cascades),
        field("cascadeVoxelLod", &GiStatus::cascadeVoxelLod),
        field("cascadesAwaitingCamera", &GiStatus::cascadesAwaitingCamera),
        field("awaitingVoxelTextures", &GiStatus::awaitingVoxelTextures),
        field("cascadeProfileVr", &GiStatus::cascadeProfileVr),
        field("cascadeFullRebuilds", &GiStatus::cascadeFullRebuilds),
        field("cascadeDeferrals", &GiStatus::cascadeDeferrals),
        field("feedbackRests", &GiStatus::feedbackRests),
        field("feedbackOwed", &GiStatus::feedbackOwed),
        field("chainInjectionRefusals", &GiStatus::chainInjectionRefusals),
        field("chainInjectionsPeakFrame", &GiStatus::chainInjectionsPeakFrame),
        field("dragMovers", &GiStatus::dragMovers),
        field("dragMoverGestures", &GiStatus::dragMoverGestures),
        field("schedulerPushes", &GiStatus::schedulerPushes),
        field("schedulerResolves", &GiStatus::schedulerResolves),
        field("settlePending", &GiStatus::settlePending),
        field("settleStableFrames", &GiStatus::settleStableFrames),
        field("gestureActive", &GiStatus::gestureActive),
        field("cascadeDirtyMajority", &GiStatus::cascadeDirtyMajority),
        field("cascadeScrollAborts", &GiStatus::cascadeScrollAborts),
        field("cascadeCommitShaderWaits", &GiStatus::cascadeCommitShaderWaits),
        field("relightCount", &GiStatus::relightCount),
        field("cascadeTornCleared", &GiStatus::cascadeTornCleared),
        field("gather", &GiStatus::gather),
        field("cards", &GiStatus::cards),
        field("voxelSides", &GiStatus::voxelSides),
    });
    return t;
}

const Table<GiStatus::CascadeStatus> &Record<GiStatus::CascadeStatus>::table()
{
    static const Table<GiStatus::CascadeStatus> t({
        field("halfSize", &GiStatus::CascadeStatus::halfSize),
        field("resolution", &GiStatus::CascadeStatus::resolution),
        field("cell", &GiStatus::CascadeStatus::cell),
        field("step", &GiStatus::CascadeStatus::step),
        field("guaranteedRadius", &GiStatus::CascadeStatus::guaranteedRadius),
        field("hitLevelRadius", &GiStatus::CascadeStatus::hitLevelRadius),
        field("centre", &GiStatus::CascadeStatus::centre),
        field("builtCam", &GiStatus::CascadeStatus::builtCam),
        field("rebuilds", &GiStatus::CascadeStatus::rebuilds),
        field("pending", &GiStatus::CascadeStatus::pending),
        field("scrolls", &GiStatus::CascadeStatus::scrolls),
        field("scrollFrames", &GiStatus::CascadeStatus::scrollFrames),
        field("scrolling", &GiStatus::CascadeStatus::scrolling),
        field("tornCleared", &GiStatus::CascadeStatus::tornCleared),
        field("shellEnds", &GiStatus::CascadeStatus::shellEnds),
        field("commitLitSlab", &GiStatus::CascadeStatus::commitLitSlab),
        field("commitLitBand", &GiStatus::CascadeStatus::commitLitBand),
        field("commitSlabCells", &GiStatus::CascadeStatus::commitSlabCells),
        field("commitInjectCells", &GiStatus::CascadeStatus::commitInjectCells),
        field("commitCounted", &GiStatus::CascadeStatus::commitCounted),
        field("deltaMarched", &GiStatus::CascadeStatus::deltaMarched),
        field("feedbackActive", &GiStatus::CascadeStatus::feedbackActive),
        field("feedbackOnPrior", &GiStatus::CascadeStatus::feedbackOnPrior),
        field("feedbackOwed", &GiStatus::CascadeStatus::feedbackOwed),
        field("feedbackPasses", &GiStatus::CascadeStatus::feedbackPasses),
        field("feedbackBricksRan", &GiStatus::CascadeStatus::feedbackBricksRan),
        field("feedbackStops", &GiStatus::CascadeStatus::feedbackStops),
        field("feedbackResumes", &GiStatus::CascadeStatus::feedbackResumes),
        field("feedbackSaturated", &GiStatus::CascadeStatus::feedbackSaturated),
        field("feedbackReadings", &GiStatus::CascadeStatus::feedbackReadings),
        field("feedbackCarried", &GiStatus::CascadeStatus::feedbackCarried),
        field("relightCarried", &GiStatus::CascadeStatus::relightCarried),
        field("relightRounds", &GiStatus::CascadeStatus::relightRounds),
        field("relightVoxels", &GiStatus::CascadeStatus::relightVoxels),
        field("relightSlab", &GiStatus::CascadeStatus::relightSlab),
        field("regionRebuilds", &GiStatus::CascadeStatus::regionRebuilds),
        field("moverRegionRebuilds", &GiStatus::CascadeStatus::moverRegionRebuilds),
        field("regionCells", &GiStatus::CascadeStatus::regionCells),
        field("regionOwed", &GiStatus::CascadeStatus::regionOwed),
        field("marchOwed", &GiStatus::CascadeStatus::marchOwed),
        field("regionBouncePasses", &GiStatus::CascadeStatus::regionBouncePasses),
        field("regionBounceLast", &GiStatus::CascadeStatus::regionBounceLast),
        field("lagLast", &GiStatus::CascadeStatus::lagLast),
        field("lagMax", &GiStatus::CascadeStatus::lagMax),
        field("items", &GiStatus::CascadeStatus::items),
        field("attached", &GiStatus::CascadeStatus::attached),
        field("lastCpuMs", &GiStatus::CascadeStatus::lastCpuMs),
        field("voxelLevels", &GiStatus::CascadeStatus::voxelLevels),
        field("voxelTriangles", &GiStatus::CascadeStatus::voxelTriangles),
        field("voxelDispatches", &GiStatus::CascadeStatus::voxelDispatches),
        field("voxelRecords", &GiStatus::CascadeStatus::voxelRecords),
        field("voxelOverflow", &GiStatus::CascadeStatus::voxelOverflow),
        field("voxelGathers", &GiStatus::CascadeStatus::voxelGathers),
        field("voxelReadingGather", &GiStatus::CascadeStatus::voxelReadingGather),
        field("voxelReadoutFlushes", &GiStatus::CascadeStatus::voxelReadoutFlushes),
        field("lightsInRange", &GiStatus::CascadeStatus::lightsInRange),
        field("lightsInjected", &GiStatus::CascadeStatus::lightsInjected),
        field("lightCapacity", &GiStatus::CascadeStatus::lightCapacity),
        field("lightsOverBudget", &GiStatus::CascadeStatus::lightsOverBudget),
        field("lightsInRangeMax", &GiStatus::CascadeStatus::lightsInRangeMax),
        field("injectionsOverSlots", &GiStatus::CascadeStatus::injectionsOverSlots),
        field("bricks", &GiStatus::CascadeStatus::bricks),
        field("bricksDirectLast", &GiStatus::CascadeStatus::bricksDirectLast),
        field("bricksBounceLast", &GiStatus::CascadeStatus::bricksBounceLast),
        field("bricksActiveLast", &GiStatus::CascadeStatus::bricksActiveLast),
        field("brickUpdates", &GiStatus::CascadeStatus::brickUpdates),
        field("brickReadings", &GiStatus::CascadeStatus::brickReadings),
        field("brickReadingsSkipped", &GiStatus::CascadeStatus::brickReadingsSkipped),
        field("foreignDirectMax", &GiStatus::CascadeStatus::foreignDirectMax),
        field("foreignBounceMax", &GiStatus::CascadeStatus::foreignBounceMax),
        field("foreignDirectMaxBrick", &GiStatus::CascadeStatus::foreignDirectMaxBrick),
        field("foreignBounceMaxBrick", &GiStatus::CascadeStatus::foreignBounceMaxBrick),
        field("foreignReadings", &GiStatus::CascadeStatus::foreignReadings),
        field("foreignFirstFrame", &GiStatus::CascadeStatus::foreignFirstFrame),
        field("foreignFirstSkipped", &GiStatus::CascadeStatus::foreignFirstSkipped),
        field("foreignFirstBrick", &GiStatus::CascadeStatus::foreignFirstBrick),
        field("foreignFirstWord", &GiStatus::CascadeStatus::foreignFirstWord),
        field("foreignFirstCount", &GiStatus::CascadeStatus::foreignFirstCount),
        field("foreignFirstCodes", &GiStatus::CascadeStatus::foreignFirstCodes),
        field("signReadings", &GiStatus::CascadeStatus::signReadings),
        field("signFirstFrame", &GiStatus::CascadeStatus::signFirstFrame),
        field("signFirstBrick", &GiStatus::CascadeStatus::signFirstBrick),
        field("signFirstCount", &GiStatus::CascadeStatus::signFirstCount),
        field("signFirstCodes", &GiStatus::CascadeStatus::signFirstCodes),
    });
    return t;
}

const Table<GatherStatus> &Record<GatherStatus>::table()
{
    static const Table<GatherStatus> t({
        field("on", &GatherStatus::on),
        field("running", &GatherStatus::running),
        field("stride", &GatherStatus::stride),
        field("octRes", &GatherStatus::octRes),
        field("raysPerProbe", &GatherStatus::raysPerProbe),
        field("probesX", &GatherStatus::probesX),
        field("probesY", &GatherStatus::probesY),
        field("probes", &GatherStatus::probes),
        field("adaptive", &GatherStatus::adaptive),
        field("adaptiveCap", &GatherStatus::adaptiveCap),
        field("adaptiveRequested", &GatherStatus::adaptiveRequested),
        field("raysPerFrame", &GatherStatus::raysPerFrame),
        field("targetW", &GatherStatus::targetW),
        field("targetH", &GatherStatus::targetH),
        field("stereo", &GatherStatus::stereo),
        field("eyeProbesX", &GatherStatus::eyeProbesX),
        field("vramBytes", &GatherStatus::vramBytes),
        field("historyBytes", &GatherStatus::historyBytes),
        field("placeMs", &GatherStatus::placeMs),
        field("traceMs", &GatherStatus::traceMs),
        field("integrateMs", &GatherStatus::integrateMs),
        field("cpuMs", &GatherStatus::cpuMs),
        field("filterMs", &GatherStatus::filterMs),
        field("temporal", &GatherStatus::temporal),
        field("historyAge", &GatherStatus::historyAge),
        field("restFrames", &GatherStatus::restFrames),
        field("sinceRestart", &GatherStatus::sinceRestart),
        field("settleFrames", &GatherStatus::settleFrames),
        field("historyFrames", &GatherStatus::historyFrames),
        field("settled", &GatherStatus::settled),
        field("held", &GatherStatus::held),
        field("cellsReached", &GatherStatus::cellsReached),
        field("cellsHeld", &GatherStatus::cellsHeld),
        field("cellsActive", &GatherStatus::cellsActive),
        field("cellsRing", &GatherStatus::cellsRing),
        field("resumes", &GatherStatus::resumes),
        field("restLog", &GatherStatus::restLog),
        field("cameraCuts", &GatherStatus::cameraCuts),
        field("foliageRuleAccepts", &GatherStatus::foliageRuleAccepts),
        field("starvedDiffuse", &GatherStatus::starvedDiffuse),
        field("starvedRough", &GatherStatus::starvedRough),
        field("shotValid", &GatherStatus::shotValid),
        field("shotStarvedDiffuse", &GatherStatus::shotStarvedDiffuse),
        field("shotStarvedRough", &GatherStatus::shotStarvedRough),
        field("irradiance", &GatherStatus::irradiance),
        field("irradianceW", &GatherStatus::irradianceW),
        field("irradianceH", &GatherStatus::irradianceH),
        field("irradianceFrame", &GatherStatus::irradianceFrame),
        field("frame", &GatherStatus::frame),
        field("historyCount", &GatherStatus::historyCount),
        field("historyClass", &GatherStatus::historyClass),
        field("historyDist", &GatherStatus::historyDist),
        field("historyStreak", &GatherStatus::historyStreak),
        field("historyCountW", &GatherStatus::historyCountW),
        field("historyCountH", &GatherStatus::historyCountH),
        field("historyCountFrame", &GatherStatus::historyCountFrame),
        field("rays", &GatherStatus::rays),
        field("raysFrame", &GatherStatus::raysFrame),
        field("adaptiveCells", &GatherStatus::adaptiveCells),
        field("error", &GatherStatus::error),
    });
    return t;
}

const Table<GatherRestRecord> &Record<GatherRestRecord>::table()
{
    static const Table<GatherRestRecord> t({
        field("frame", &GatherRestRecord::frame),
        field("restK", &GatherRestRecord::restK),
        field("reached", &GatherRestRecord::reached),
        field("held", &GatherRestRecord::held),
        field("active", &GatherRestRecord::active),
        field("everyHeld", &GatherRestRecord::everyHeld),
        field("movingProbes", &GatherRestRecord::movingProbes),
        field("fastProbes", &GatherRestRecord::fastProbes),
        field("handoverM", &GatherRestRecord::handoverM),
        field("refined", &GatherRestRecord::refined),
    });
    return t;
}

const Table<GatherRay> &Record<GatherRay>::table()
{
    static const Table<GatherRay> t({
        field("source", &GatherRay::source),
        field("cascade", &GatherRay::cascade),
        field("cellX", &GatherRay::cellX),
        field("cellY", &GatherRay::cellY),
        field("ray", &GatherRay::ray),
        field("radiance", &GatherRay::radiance),
        field("distance", &GatherRay::distance),
        field("hitPos", &GatherRay::hitPos),
        field("hitNormal", &GatherRay::hitNormal),
        field("sunVisibility", &GatherRay::sunVisibility),
        field("weight", &GatherRay::weight),
    });
    return t;
}

const Table<CardCacheStatus> &Record<CardCacheStatus>::table()
{
    static const Table<CardCacheStatus> t({
        field("built", &CardCacheStatus::built),
        field("pageSize", &CardCacheStatus::pageSize),
        field("pages", &CardCacheStatus::pages),
        field("pagesUsed", &CardCacheStatus::pagesUsed),
        field("bytesPerTexel", &CardCacheStatus::bytesPerTexel),
        field("bytes", &CardCacheStatus::bytes),
        field("emissiveFormat", &CardCacheStatus::emissiveFormat),
        field("instancesResident", &CardCacheStatus::instancesResident),
        field("cardsResident", &CardCacheStatus::cardsResident),
        field("residentHash", &CardCacheStatus::residentHash),
        field("admitted", &CardCacheStatus::admitted),
        field("evicted", &CardCacheStatus::evicted),
        field("compactionMoves", &CardCacheStatus::compactionMoves),
        field("requestOnlyPasses", &CardCacheStatus::requestOnlyPasses),
        field("requestOnlyKeysBuilt", &CardCacheStatus::requestOnlyKeysBuilt),
        field("cacheFrame", &CardCacheStatus::cacheFrame),
        field("arrivalMarkNewest", &CardCacheStatus::arrivalMarkNewest),
        field("demandReadEvery", &CardCacheStatus::demandReadEvery),
        field("demandReadLag", &CardCacheStatus::demandReadLag),
        field("demandChangeFrame", &CardCacheStatus::demandChangeFrame),
        field("demandFirst", &CardCacheStatus::demandFirst),
        field("demandNewest", &CardCacheStatus::demandNewest),
        field("demandOpenLag", &CardCacheStatus::demandOpenLag),
        field("lampTraces", &CardCacheStatus::lampTraces),
        field("lampTracedLastFrame", &CardCacheStatus::lampTracedLastFrame),
        field("cardLampsMax", &CardCacheStatus::cardLampsMax),
        field("lampOverflowShareMax", &CardCacheStatus::lampOverflowShareMax),
        field("cardAreaLampsMax", &CardCacheStatus::cardAreaLampsMax),
        field("cardsMultiArea", &CardCacheStatus::cardsMultiArea),
        field("cardsSharedFractional", &CardCacheStatus::cardsSharedFractional),
        field("lampRaysLastTrace", &CardCacheStatus::lampRaysLastTrace),
        field("areaStrata", &CardCacheStatus::areaStrata),
        field("gather", &CardCacheStatus::gather),
        field("gatherWaits", &CardCacheStatus::gatherWaits),
        field("gatherByRays", &CardCacheStatus::gatherByRays),
        field("indirectByCards", &CardCacheStatus::indirectByCards),
        field("indirectByBound", &CardCacheStatus::indirectByBound),
        field("visibleRelights", &CardCacheStatus::visibleRelights),
        field("subQuantumRelights", &CardCacheStatus::subQuantumRelights),
        field("unmeasuredRelights", &CardCacheStatus::unmeasuredRelights),
        field("readersHoldOwed", &CardCacheStatus::readersHoldOwed),
        field("moveCodesLast", &CardCacheStatus::moveCodesLast),
        field("moveCodesFrame", &CardCacheStatus::moveCodesFrame),
        field("restDrained", &CardCacheStatus::restDrained),
        field("restDecisions", &CardCacheStatus::restDecisions),
        field("decisionUsedTexels", &CardCacheStatus::decisionUsedTexels),
        field("decisionFreeTexels", &CardCacheStatus::decisionFreeTexels),
        field("decisionAdmitted", &CardCacheStatus::decisionAdmitted),
        field("decisionRefused", &CardCacheStatus::decisionRefused),
        field("decisionReaders", &CardCacheStatus::decisionReaders),
        field("decisionRefusedCards", &CardCacheStatus::decisionRefusedCards),
        field("windowDrains", &CardCacheStatus::windowDrains),
        field("workRoots", &CardCacheStatus::workRoots),
        field("workRound", &CardCacheStatus::workRound),
        field("demandHits", &CardCacheStatus::demandHits),
        field("queueLength", &CardCacheStatus::queueLength),
        field("budgetTexels", &CardCacheStatus::budgetTexels),
        field("capturesLastFrame", &CardCacheStatus::capturesLastFrame),
        field("texelsLastFrame", &CardCacheStatus::texelsLastFrame),
        field("captures", &CardCacheStatus::captures),
        field("invalidTransform", &CardCacheStatus::invalidTransform),
        field("invalidMaterial", &CardCacheStatus::invalidMaterial),
        field("captureMs", &CardCacheStatus::captureMs),
        field("captureWorkspaceMs", &CardCacheStatus::captureWorkspaceMs),
        field("captureCopyMs", &CardCacheStatus::captureCopyMs),
        field("captureTraceMs", &CardCacheStatus::captureTraceMs),
        field("cardRecords", &CardCacheStatus::cardRecords),
        field("instanceSlots", &CardCacheStatus::instanceSlots),
        field("radianceFormat", &CardCacheStatus::radianceFormat),
        field("lightBudgetTexels", &CardCacheStatus::lightBudgetTexels),
        field("relitLastFrame", &CardCacheStatus::relitLastFrame),
        field("relitTexelsLastFrame", &CardCacheStatus::relitTexelsLastFrame),
        field("relights", &CardCacheStatus::relights),
        field("invalidRadiance", &CardCacheStatus::invalidRadiance),
        field("lightMs", &CardCacheStatus::lightMs),
        field("indirectBudgetTexels", &CardCacheStatus::indirectBudgetTexels),
        field("indirectLastFrame", &CardCacheStatus::indirectLastFrame),
        field("indirectTexelsLastFrame", &CardCacheStatus::indirectTexelsLastFrame),
        field("indirectRelights", &CardCacheStatus::indirectRelights),
        field("invalidIndirect", &CardCacheStatus::invalidIndirect),
        field("indirectReported", &CardCacheStatus::indirectReported),
        field("indirectByRegion", &CardCacheStatus::indirectByRegion),
        field("indirectOverflows", &CardCacheStatus::indirectOverflows),
        field("indirectAnswered", &CardCacheStatus::indirectAnswered),
        field("reportsDropped", &CardCacheStatus::reportsDropped),
        field("indirectOn", &CardCacheStatus::indirectOn),
        field("lightsInRange", &CardCacheStatus::lightsInRange),
        field("lightsDropped", &CardCacheStatus::lightsDropped),
        field("invalidSun", &CardCacheStatus::invalidSun),
        field("stillTraces", &CardCacheStatus::stillTraces),
        field("stillTracedLastFrame", &CardCacheStatus::stillTracedLastFrame),
        field("stillTexelsLastFrame", &CardCacheStatus::stillTexelsLastFrame),
        field("stillPending", &CardCacheStatus::stillPending),
        field("stillGpuMs", &CardCacheStatus::stillGpuMs),
        field("casterRetraces", &CardCacheStatus::casterRetraces),
        field("moverCasters", &CardCacheStatus::moverCasters),
        field("moverTracedLastFrame", &CardCacheStatus::moverTracedLastFrame),
        field("moverTexelsLastFrame", &CardCacheStatus::moverTexelsLastFrame),
        field("moverTraces", &CardCacheStatus::moverTraces),
        field("moverRetired", &CardCacheStatus::moverRetired),
        field("moverPending", &CardCacheStatus::moverPending),
        field("moverPendingAge", &CardCacheStatus::moverPendingAge),
        field("moverGpuMs", &CardCacheStatus::moverGpuMs),
        field("relightGpuMs", &CardCacheStatus::relightGpuMs),
    });
    return t;
}

const Table<CardCacheStatus::GatherCounts> &Record<CardCacheStatus::GatherCounts>::table()
{
    static const Table<CardCacheStatus::GatherCounts> t({
        field("dispatches", &CardCacheStatus::GatherCounts::dispatches),
        field("rays", &CardCacheStatus::GatherCounts::rays),
        field("card", &CardCacheStatus::GatherCounts::card),
        field("voxel", &CardCacheStatus::GatherCounts::voxel),
        field("backFaces", &CardCacheStatus::GatherCounts::backFaces),
        field("noSample", &CardCacheStatus::GatherCounts::noSample),
        field("sky", &CardCacheStatus::GatherCounts::sky),
        field("texels", &CardCacheStatus::GatherCounts::texels),
        field("emptyTexels", &CardCacheStatus::GatherCounts::emptyTexels),
        field("readsOverflowed", &CardCacheStatus::GatherCounts::readsOverflowed),
        field("buried", &CardCacheStatus::GatherCounts::buried),
    });
    return t;
}

const Table<CardCacheStatus::Refused> &Record<CardCacheStatus::Refused>::table()
{
    static const Table<CardCacheStatus::Refused> t({
        field("node", &CardCacheStatus::Refused::node),
        field("index", &CardCacheStatus::Refused::index),
        field("size", &CardCacheStatus::Refused::size),
        field("mark", &CardCacheStatus::Refused::mark),
    });
    return t;
}

const Table<CardCacheStatus::WorkRoot> &Record<CardCacheStatus::WorkRoot>::table()
{
    static const Table<CardCacheStatus::WorkRoot> t({
        field("kind", &CardCacheStatus::WorkRoot::kind),
        field("frame", &CardCacheStatus::WorkRoot::frame),
    });
    return t;
}

const Table<GiStatus::VoxelSideCounts> &Record<GiStatus::VoxelSideCounts>::table()
{
    static const Table<GiStatus::VoxelSideCounts> t({
        field("dispatches", &GiStatus::VoxelSideCounts::dispatches),
        field("rays", &GiStatus::VoxelSideCounts::rays),
        field("card", &GiStatus::VoxelSideCounts::card),
        field("voxel", &GiStatus::VoxelSideCounts::voxel),
        field("backFaces", &GiStatus::VoxelSideCounts::backFaces),
        field("noSample", &GiStatus::VoxelSideCounts::noSample),
        field("sky", &GiStatus::VoxelSideCounts::sky),
        field("sides", &GiStatus::VoxelSideCounts::sides),
        field("emptySides", &GiStatus::VoxelSideCounts::emptySides),
        field("waits", &GiStatus::VoxelSideCounts::waits),
        field("routeChanges", &GiStatus::VoxelSideCounts::routeChanges),
        field("buried", &GiStatus::VoxelSideCounts::buried),
    });
    return t;
}

const Table<PublishedFrame::Atmosphere> &Record<PublishedFrame::Atmosphere>::table()
{
    static const Table<PublishedFrame::Atmosphere> t({
        field("on", &PublishedFrame::Atmosphere::on),
        field("rayleighScale", &PublishedFrame::Atmosphere::rayleighScale),
        field("mieScale", &PublishedFrame::Atmosphere::mieScale),
        field("ozone", &PublishedFrame::Atmosphere::ozone),
        field("planetRadiusKm", &PublishedFrame::Atmosphere::planetRadiusKm),
        field("atmosphereHeightKm", &PublishedFrame::Atmosphere::atmosphereHeightKm),
        field("observerKm", &PublishedFrame::Atmosphere::observerKm),
    });
    return t;
}

const Table<HeightFogStatus> &Record<HeightFogStatus>::table()
{
    static const Table<HeightFogStatus> t({
        field("on", &HeightFogStatus::on),
        field("colour", &HeightFogStatus::colour),
        field("colourFromSky", &HeightFogStatus::colourFromSky),
    });
    return t;
}

const Table<PublishedFrame::Completeness> &Record<PublishedFrame::Completeness>::table()
{
    static const Table<PublishedFrame::Completeness> t({
        field("incomplete", &PublishedFrame::Completeness::incomplete),
        field("terms", &PublishedFrame::Completeness::terms),
        field("incompleteFrames", &PublishedFrame::Completeness::incompleteFrames),
        field("completionSerial", &PublishedFrame::Completeness::completionSerial),
    });
    return t;
}

const Table<CloudStatus> &Record<CloudStatus>::table()
{
    static const Table<CloudStatus> t({
        field("drawn", &CloudStatus::drawn),
        field("reason", &CloudStatus::reason),
        field("changeCaptures", &CloudStatus::changeCaptures),
        field("scrollCaptures", &CloudStatus::scrollCaptures),
        field("clockTicks", &CloudStatus::clockTicks),
        field("capturePeriodFrames", &CloudStatus::capturePeriodFrames),
        field("fieldBakes", &CloudStatus::fieldBakes),
        field("scroll", &CloudStatus::scroll),
    });
    return t;
}

const Table<AtmosphereStatus> &Record<AtmosphereStatus>::table()
{
    static const Table<AtmosphereStatus> t({
        field("on", &AtmosphereStatus::on),
        field("transmittanceBuilds", &AtmosphereStatus::transmittanceBuilds),
        field("multiScatterBuilds", &AtmosphereStatus::multiScatterBuilds),
        field("skyViewBuilds", &AtmosphereStatus::skyViewBuilds),
        field("aerialBuilds", &AtmosphereStatus::aerialBuilds),
        field("deferredUpdates", &AtmosphereStatus::deferredUpdates),
        field("topIlluminance", &AtmosphereStatus::topIlluminance),
        field("observerAltitudeM", &AtmosphereStatus::observerAltitudeM),
        field("observerRebuilds", &AtmosphereStatus::observerRebuilds),
        field("environmentObserverM", &AtmosphereStatus::environmentObserverM),
        field("aerialBound", &AtmosphereStatus::aerialBound),
    });
    return t;
}

const Table<RayQueryStatus> &Record<RayQueryStatus>::table()
{
    static const Table<RayQueryStatus> t({
        field("available", &RayQueryStatus::available),
        field("enabled", &RayQueryStatus::enabled),
        field("blasCount", &RayQueryStatus::blasCount),
        field("instances", &RayQueryStatus::instances),
        field("skinnedInstances", &RayQueryStatus::skinnedInstances),
        field("skinCaches", &RayQueryStatus::skinCaches),
        field("skinCacheBytes", &RayQueryStatus::skinCacheBytes),
        field("skinBlasBytes", &RayQueryStatus::skinBlasBytes),
        field("skinPasses", &RayQueryStatus::skinPasses),
        field("skinDispatches", &RayQueryStatus::skinDispatches),
        field("skinBlasBuilds", &RayQueryStatus::skinBlasBuilds),
        field("skinRefits", &RayQueryStatus::skinRefits),
        field("skinLastItems", &RayQueryStatus::skinLastItems),
        field("skinLastVertices", &RayQueryStatus::skinLastVertices),
        field("skinMs", &RayQueryStatus::skinMs),
        field("skinRefitMs", &RayQueryStatus::skinRefitMs),
        field("skinCpuMs", &RayQueryStatus::skinCpuMs),
        field("skinReason", &RayQueryStatus::skinReason),
        field("farInstances", &RayQueryStatus::farInstances),
        field("levelBlasCount", &RayQueryStatus::levelBlasCount),
        field("levelBlasBytes", &RayQueryStatus::levelBlasBytes),
        field("triangles", &RayQueryStatus::triangles),
        field("blasBytes", &RayQueryStatus::blasBytes),
        field("tlasBytes", &RayQueryStatus::tlasBytes),
        field("tlasMs", &RayQueryStatus::tlasMs),
        field("blasMs", &RayQueryStatus::blasMs),
        field("gatherMs", &RayQueryStatus::gatherMs),
        field("tlasJobDispatches", &RayQueryStatus::tlasJobDispatches),
        field("feedSlotVisits", &RayQueryStatus::feedSlotVisits),
        field("updateLookups", &RayQueryStatus::updateLookups),
        field("updateRows", &RayQueryStatus::updateRows),
        field("updateVisits", &RayQueryStatus::updateVisits),
        field("alphaMasksMade", &RayQueryStatus::alphaMasksMade),
        field("alphaStaleServed", &RayQueryStatus::alphaStaleServed),
        field("hitLights", &RayQueryStatus::hitLights),
        field("hitMaterialRows", &RayQueryStatus::hitMaterialRows),
        field("hitTableBuffers", &RayQueryStatus::hitTableBuffers),
        field("reflectEmitters", &RayQueryStatus::reflectEmitters),
        field("reflectEmittersOver", &RayQueryStatus::reflectEmittersOver),
        field("reflectEmitterSetSerial", &RayQueryStatus::reflectEmitterSetSerial),
        field("reflectEmitterTable", &RayQueryStatus::reflectEmitterTable),
        field("reflectPortals", &RayQueryStatus::reflectPortals),
        field("reflectPick", &RayQueryStatus::reflectPick),
        field("reflectPickFrames", &RayQueryStatus::reflectPickFrames),
        field("colourPick", &RayQueryStatus::colourPick),
        field("colourPickFrames", &RayQueryStatus::colourPickFrames),
        field("reflectPortalTable", &RayQueryStatus::reflectPortalTable),
        field("reflectNeeRays", &RayQueryStatus::reflectNeeRays),
        field("reflectNeeHits", &RayQueryStatus::reflectNeeHits),
        field("reflectRestNeedMax", &RayQueryStatus::reflectRestNeedMax),
        field("reflectRestStopVMax", &RayQueryStatus::reflectRestStopVMax),
        field("lastWasRefit", &RayQueryStatus::lastWasRefit),
        field("hitRecords", &RayQueryStatus::hitRecords),
        field("hitDropped", &RayQueryStatus::hitDropped),
        field("hitCapacity", &RayQueryStatus::hitCapacity),
        field("escapeRecords", &RayQueryStatus::escapeRecords),
        field("escapeDropped", &RayQueryStatus::escapeDropped),
        field("escapeCapacity", &RayQueryStatus::escapeCapacity),
        field("escapeTraced", &RayQueryStatus::escapeTraced),
        field("escapeCard", &RayQueryStatus::escapeCard),
        field("escapeVoxel", &RayQueryStatus::escapeVoxel),
        field("escapeDirect", &RayQueryStatus::escapeDirect),
        field("escapeFar", &RayQueryStatus::escapeFar),
        field("escapeSky", &RayQueryStatus::escapeSky),
        field("escapeBack", &RayQueryStatus::escapeBack),
        field("escapeNoSample", &RayQueryStatus::escapeNoSample),
        field("escapeSaturated", &RayQueryStatus::escapeSaturated),
        field("panesOverflow", &RayQueryStatus::panesOverflow),
        field("proceedStepsMax", &RayQueryStatus::proceedStepsMax),
        field("raysRefused", &RayQueryStatus::raysRefused),
        field("hitViews", &RayQueryStatus::hitViews),
        field("hitDecodeDraws", &RayQueryStatus::hitDecodeDraws),
        field("tlasBuilds", &RayQueryStatus::tlasBuilds),
        field("tlasRefits", &RayQueryStatus::tlasRefits),
        field("blasBuilds", &RayQueryStatus::blasBuilds),
        field("reflect", &RayQueryStatus::reflect),
        field("reflectRays", &RayQueryStatus::reflectRays),
        field("reflectMs", &RayQueryStatus::reflectMs),
        field("reflectMotionMs", &RayQueryStatus::reflectMotionMs),
        field("reflectHistoryCount", &RayQueryStatus::reflectHistoryCount),
        field("reflectHistorySe", &RayQueryStatus::reflectHistorySe),
        field("reflectHistoryW", &RayQueryStatus::reflectHistoryW),
        field("reflectHistoryH", &RayQueryStatus::reflectHistoryH),
        field("reflectHistoryFrame", &RayQueryStatus::reflectHistoryFrame),
        field("reflectFrame", &RayQueryStatus::reflectFrame),
        field("reflectRestFrames", &RayQueryStatus::reflectRestFrames),
        field("reflectHeld", &RayQueryStatus::reflectHeld),
        field("reflectRestActive", &RayQueryStatus::reflectRestActive),
        field("reflectRestCapped", &RayQueryStatus::reflectRestCapped),
        field("reflectRests", &RayQueryStatus::reflectRests),
        field("reflectShotValid", &RayQueryStatus::reflectShotValid),
        field("reflectShotRestFrames", &RayQueryStatus::reflectShotRestFrames),
        field("reflectShotHeld", &RayQueryStatus::reflectShotHeld),
        field("reflectShotRests", &RayQueryStatus::reflectShotRests),
        field("reflectRestMoverFrames", &RayQueryStatus::reflectRestMoverFrames),
        field("reflectCameraCuts", &RayQueryStatus::reflectCameraCuts),
        field("reflectShapeRefusals", &RayQueryStatus::reflectShapeRefusals),
    });
    return t;
}

const Table<RayQueryStatus::ReflectEmitter> &Record<RayQueryStatus::ReflectEmitter>::table()
{
    static const Table<RayQueryStatus::ReflectEmitter> t({
        field("slot", &RayQueryStatus::ReflectEmitter::slot),
        field("centre", &RayQueryStatus::ReflectEmitter::centre),
        field("radius", &RayQueryStatus::ReflectEmitter::radius),
        field("radiance", &RayQueryStatus::ReflectEmitter::radiance),
    });
    return t;
}

const Table<RayQueryStatus::HitListView> &Record<RayQueryStatus::HitListView>::table()
{
    static const Table<RayQueryStatus::HitListView> t({
        field("eyes", &RayQueryStatus::HitListView::eyes),
        field("photonTerm", &RayQueryStatus::HitListView::photonTerm),
        field("eyeWidth", &RayQueryStatus::HitListView::eyeWidth),
        field("eyeHeight", &RayQueryStatus::HitListView::eyeHeight),
        field("firstCapacity", &RayQueryStatus::HitListView::firstCapacity),
        field("capacity", &RayQueryStatus::HitListView::capacity),
        field("records", &RayQueryStatus::HitListView::records),
        field("dropped", &RayQueryStatus::HitListView::dropped),
    });
    return t;
}

const Table<SunContactStatus> &Record<SunContactStatus>::table()
{
    static const Table<SunContactStatus> t({
        field("on", &SunContactStatus::on),
        field("running", &SunContactStatus::running),
        field("width", &SunContactStatus::width),
        field("height", &SunContactStatus::height),
        field("targetW", &SunContactStatus::targetW),
        field("targetH", &SunContactStatus::targetH),
        field("divisor", &SunContactStatus::divisor),
        field("rays", &SunContactStatus::rays),
        field("range", &SunContactStatus::range),
        field("toSun", &SunContactStatus::toSun),
        field("gpuMs", &SunContactStatus::gpuMs),
        field("cpuMs", &SunContactStatus::cpuMs),
        field("reason", &SunContactStatus::reason),
    });
    return t;
}

const Table<AreaVisStatus> &Record<AreaVisStatus>::table()
{
    static const Table<AreaVisStatus> t({
        field("running", &AreaVisStatus::running),
        field("lights", &AreaVisStatus::lights),
        field("strata", &AreaVisStatus::strata),
        field("traceW", &AreaVisStatus::traceW),
        field("traceH", &AreaVisStatus::traceH),
        field("targetW", &AreaVisStatus::targetW),
        field("targetH", &AreaVisStatus::targetH),
        field("restFrames", &AreaVisStatus::restFrames),
        field("held", &AreaVisStatus::held),
        field("rays", &AreaVisStatus::rays),
        field("traceMs", &AreaVisStatus::traceMs),
        field("resolveMs", &AreaVisStatus::resolveMs),
        field("cpuMs", &AreaVisStatus::cpuMs),
        field("vramBytes", &AreaVisStatus::vramBytes),
        field("reason", &AreaVisStatus::reason),
        field("vis", &AreaVisStatus::vis),
        field("visW", &AreaVisStatus::visW),
        field("visH", &AreaVisStatus::visH),
    });
    return t;
}

const Table<PublishedFrame::AtomHidden> &Record<PublishedFrame::AtomHidden>::table()
{
    static const Table<PublishedFrame::AtomHidden> t({
        field("valid", &PublishedFrame::AtomHidden::valid),
        field("drawn", &PublishedFrame::AtomHidden::drawn),
        field("hidden", &PublishedFrame::AtomHidden::hidden),
    });
    return t;
}

const Table<MobilityStatus> &Record<MobilityStatus>::table()
{
    static const Table<MobilityStatus> t({
        field("movableItems", &MobilityStatus::movableItems),
        field("movableLights", &MobilityStatus::movableLights),
        field("movableNodes", &MobilityStatus::movableNodes),
    });
    return t;
}

const Table<RigStats> &Record<RigStats>::table()
{
    static const Table<RigStats> t({
        field("rigged", &RigStats::rigged),
        field("instances", &RigStats::instances),
        field("shared", &RigStats::shared),
        field("streamedBones", &RigStats::streamedBones),
    });
    return t;
}

const Table<SceneChangeStats> &Record<SceneChangeStats>::table()
{
    static const Table<SceneChangeStats> t({
        field("itemsCreated", &SceneChangeStats::itemsCreated),
        field("itemsDestroyed", &SceneChangeStats::itemsDestroyed),
        field("giInvalidations", &SceneChangeStats::giInvalidations),
        field("probeBuildRestarts", &SceneChangeStats::probeBuildRestarts),
        field("engineAssignedIds", &SceneChangeStats::engineAssignedIds),
    });
    return t;
}

const Table<GpuSceneStatus> &Record<GpuSceneStatus>::table()
{
    static const Table<GpuSceneStatus> t({
        field("live", &GpuSceneStatus::live),
        field("slotCount", &GpuSceneStatus::slotCount),
        field("capacity", &GpuSceneStatus::capacity),
        field("meshEntries", &GpuSceneStatus::meshEntries),
        field("lastDirty", &GpuSceneStatus::lastDirty),
        field("writes", &GpuSceneStatus::writes),
        field("copyRuns", &GpuSceneStatus::copyRuns),
        field("updates", &GpuSceneStatus::updates),
        field("grows", &GpuSceneStatus::grows),
        field("scans", &GpuSceneStatus::scans),
        field("aabbReads", &GpuSceneStatus::aabbReads),
        field("lastCopyMs", &GpuSceneStatus::lastCopyMs),
        field("lastScanMicros", &GpuSceneStatus::lastScanMicros),
        field("feedNotifies", &GpuSceneStatus::feedNotifies),
    });
    return t;
}

const Table<PublishedFrame::NodeCount> &Record<PublishedFrame::NodeCount>::table()
{
    static const Table<PublishedFrame::NodeCount> t({
        field("node", &PublishedFrame::NodeCount::node),
        field("count", &PublishedFrame::NodeCount::count),
    });
    return t;
}

const Table<PublishedFrame::RigPoses> &Record<PublishedFrame::RigPoses>::table()
{
    static const Table<PublishedFrame::RigPoses> t({
        field("node", &PublishedFrame::RigPoses::node),
        field("poses", &PublishedFrame::RigPoses::poses),
    });
    return t;
}

const Table<BonePose> &Record<BonePose>::table()
{
    static const Table<BonePose> t({
        field("position", &BonePose::position),
        field("rotation", &BonePose::rotation),
        field("scale", &BonePose::scale),
    });
    return t;
}

const Table<PublishedFrame::NodePose> &Record<PublishedFrame::NodePose>::table()
{
    static const Table<PublishedFrame::NodePose> t({
        field("node", &PublishedFrame::NodePose::node),
        field("position", &PublishedFrame::NodePose::position),
        field("rotation", &PublishedFrame::NodePose::rotation),
    });
    return t;
}

const Table<ViewFrames> &Record<ViewFrames>::table()
{
    static const Table<ViewFrames> t({
        field("id", &ViewFrames::id),
        field("name", &ViewFrames::name),
        field("presented", &ViewFrames::presented),
        field("blank", &ViewFrames::blank),
        field("width", &ViewFrames::width),
        field("height", &ViewFrames::height),
        field("sampleCount", &ViewFrames::sampleCount),
        field("pipGeneration", &ViewFrames::pipGeneration),
        field("workspaceGeneration", &ViewFrames::workspaceGeneration),
        field("globalsPushes", &ViewFrames::globalsPushes),
        field("postFx", &ViewFrames::postFx),
        field("routeGather", &ViewFrames::routeGather),
        field("routeIntegrate", &ViewFrames::routeIntegrate),
        field("routeReflect", &ViewFrames::routeReflect),
    });
    return t;
}

const Table<PublishedEngineFrame> &Record<PublishedEngineFrame>::table()
{
    static const Table<PublishedEngineFrame> t({
        field("frame", &PublishedEngineFrame::frame),
        field("serial", &PublishedEngineFrame::serial),
        field("recordApplied", &PublishedEngineFrame::recordApplied),
        field("handOver", &PublishedEngineFrame::handOver),
        field("stepSerialDrawn", &PublishedEngineFrame::stepSerialDrawn),
        field("poseSerial", &PublishedEngineFrame::poseSerial),
        field("displayTimeNs", &PublishedEngineFrame::displayTimeNs),
        field("presentAlpha", &PublishedEngineFrame::presentAlpha),
        field("presentFree", &PublishedEngineFrame::presentFree),
        field("input", &PublishedEngineFrame::input),
        field("vr", &PublishedEngineFrame::vr),
        field("vrState", &PublishedEngineFrame::vrState),
        field("vrInfo", &PublishedEngineFrame::vrInfo),
        field("vrJoints", &PublishedEngineFrame::vrJoints),
        field("vrBindingBlocks", &PublishedEngineFrame::vrBindingBlocks),
        field("streaming", &PublishedEngineFrame::streaming),
        field("shaders", &PublishedEngineFrame::shaders),
        field("hasEnabledViews", &PublishedEngineFrame::hasEnabledViews),
        field("paceOwesWork", &PublishedEngineFrame::paceOwesWork),
        field("streamingFrames", &PublishedEngineFrame::streamingFrames),
        field("views", &PublishedEngineFrame::views),
        field("frameError", &PublishedEngineFrame::frameError),
        field("frameErrors", &PublishedEngineFrame::frameErrors),
        field("framesWithError", &PublishedEngineFrame::framesWithError),
        field("refusals", &PublishedEngineFrame::refusals),
        field("vrSessionLive", &PublishedEngineFrame::vrSessionLive),
        field("completions", &PublishedEngineFrame::completions),
        field("deviceLost", &PublishedEngineFrame::deviceLost),
        field("gpuFault", &PublishedEngineFrame::gpuFault),
        field("monitor", &PublishedEngineFrame::monitor),
        field("shadow", &PublishedEngineFrame::shadow),
        field("render", &PublishedEngineFrame::render),
        field("publishMicros", &PublishedEngineFrame::publishMicros),
    });
    return t;
}

const Table<FrameInputStatus> &Record<FrameInputStatus>::table()
{
    static const Table<FrameInputStatus> t({
        field("drained", &FrameInputStatus::drained),
        field("drainedEmpty", &FrameInputStatus::drainedEmpty),
        field("fieldsApplied", &FrameInputStatus::fieldsApplied),
        field("viewFieldsApplied", &FrameInputStatus::viewFieldsApplied),
        field("gestureActive", &FrameInputStatus::gestureActive),
        field("changesMoved", &FrameInputStatus::changesMoved),
        field("changesLights", &FrameInputStatus::changesLights),
        field("changesIncomplete", &FrameInputStatus::changesIncomplete),
        field("nodeRecords", &FrameInputStatus::nodeRecords),
        field("nodeTransforms", &FrameInputStatus::nodeTransforms),
        field("messages", &FrameInputStatus::messages),
        field("messagesTotal", &FrameInputStatus::messagesTotal),
        field("messagesRefused", &FrameInputStatus::messagesRefused),
        field("nodeRecordsTotal", &FrameInputStatus::nodeRecordsTotal),
        field("messagesCarried", &FrameInputStatus::messagesCarried),
        field("drainsCut", &FrameInputStatus::drainsCut),
        field("drainMicros", &FrameInputStatus::drainMicros),
        field("drainNameMicros", &FrameInputStatus::drainNameMicros),
        field("drainRecordsMicros", &FrameInputStatus::drainRecordsMicros),
        field("drainInterpolateMicros", &FrameInputStatus::drainInterpolateMicros),
        field("drainRecordLookups", &FrameInputStatus::drainRecordLookups),
        field("drainNameVisits", &FrameInputStatus::drainNameVisits),
        field("drainNameLookups", &FrameInputStatus::drainNameLookups),
    });
    return t;
}

const Table<VrStatus> &Record<VrStatus>::table()
{
    static const Table<VrStatus> t({
        field("state", &VrStatus::state),
        field("active", &VrStatus::active),
        field("frames", &VrStatus::frames),
        field("rendered", &VrStatus::rendered),
        field("framesOffRenderThread", &VrStatus::framesOffRenderThread),
        field("displayTimeNs", &VrStatus::displayTimeNs),
        field("refreshHz", &VrStatus::refreshHz),
        field("missedDisplayPeriods", &VrStatus::missedDisplayPeriods),
        field("ipd", &VrStatus::ipd),
        field("eyeWidth", &VrStatus::eyeWidth),
        field("eyeHeight", &VrStatus::eyeHeight),
        field("swapchainFormat", &VrStatus::swapchainFormat),
        field("colourEncodedOnce", &VrStatus::colourEncodedOnce),
        field("warmUpFrames", &VrStatus::warmUpFrames),
        field("warmUpMs", &VrStatus::warmUpMs),
        field("mirror", &VrStatus::mirror),
        field("mirrorShowing", &VrStatus::mirrorShowing),
        field("worldScale", &VrStatus::worldScale),
        field("asymmetricFov", &VrStatus::asymmetricFov),
        field("headPosition", &VrStatus::headPosition),
        field("headRotation", &VrStatus::headRotation),
        field("posesValid", &VrStatus::posesValid),
        field("origin", &VrStatus::origin),
        field("originYaw", &VrStatus::originYaw),
        field("originSeq", &VrStatus::originSeq),
        field("spaceChanges", &VrStatus::spaceChanges),
        field("hiddenAreaSource", &VrStatus::hiddenAreaSource),
        field("hiddenAreaFraction", &VrStatus::hiddenAreaFraction),
        field("hiddenAreaTriangles", &VrStatus::hiddenAreaTriangles),
        field("hands", &VrStatus::hands),
        field("input", &VrStatus::input),
        field("inputFocused", &VrStatus::inputFocused),
        field("profile", &VrStatus::profile),
        field("bindingProfiles", &VrStatus::bindingProfiles),
        field("bindingProfilesAccepted", &VrStatus::bindingProfilesAccepted),
        field("handActions", &VrStatus::handActions),
        field("handJoints", &VrStatus::handJoints),
        field("handsEnabled", &VrStatus::handsEnabled),
    });
    return t;
}

const Table<VrPose> &Record<VrPose>::table()
{
    static const Table<VrPose> t({
        field("valid", &VrPose::valid),
        field("position", &VrPose::position),
        field("rotation", &VrPose::rotation),
    });
    return t;
}

const Table<VrHandState> &Record<VrHandState>::table()
{
    static const Table<VrHandState> t({
        field("valid", &VrHandState::valid),
        field("aim", &VrHandState::aim),
        field("grip", &VrHandState::grip),
        field("manipPose", &VrHandState::manipPose),
        field("select", &VrHandState::select),
        field("selectPressed", &VrHandState::selectPressed),
        field("grab", &VrHandState::grab),
        field("grabPressed", &VrHandState::grabPressed),
        field("menuPressed", &VrHandState::menuPressed),
        field("stickX", &VrHandState::stickX),
        field("stickY", &VrHandState::stickY),
        field("stickPressed", &VrHandState::stickPressed),
        field("profile", &VrHandState::profile),
        field("jointsTracked", &VrHandState::jointsTracked),
        field("fromInjection", &VrHandState::fromInjection),
    });
    return t;
}

const Table<VrProfileName> &Record<VrProfileName>::table()
{
    static const Table<VrProfileName> t({
        field("text", &VrProfileName::text).bytes(),
    });
    return t;
}

const Table<VrInfo> &Record<VrInfo>::table()
{
    static const Table<VrInfo> t({
        field("available", &VrInfo::available),
        field("reason", &VrInfo::reason),
        field("failure", &VrInfo::failure),
        field("manifest", &VrInfo::manifest),
        field("manifestRuntime", &VrInfo::manifestRuntime),
        field("openxrCalls", &VrInfo::openxrCalls),
        field("runtime", &VrInfo::runtime),
        field("runtimeVersion", &VrInfo::runtimeVersion),
        field("system", &VrInfo::system),
        field("apiMajor", &VrInfo::apiMajor),
        field("apiMinor", &VrInfo::apiMinor),
        field("eyeWidth", &VrInfo::eyeWidth),
        field("eyeHeight", &VrInfo::eyeHeight),
        field("refreshHz", &VrInfo::refreshHz),
        field("space", &VrInfo::space),
        field("visibilityMask", &VrInfo::visibilityMask),
        field("depthLayer", &VrInfo::depthLayer),
    });
    return t;
}

const Table<VrBindingBlock> &Record<VrBindingBlock>::table()
{
    static const Table<VrBindingBlock> t({
        field("profile", &VrBindingBlock::profile),
        field("bindings", &VrBindingBlock::bindings),
        field("accepted", &VrBindingBlock::accepted),
    });
    return t;
}

const Table<StreamingWork> &Record<StreamingWork>::table()
{
    static const Table<StreamingWork> t({
        field("giStagesLeft", &StreamingWork::giStagesLeft),
        field("giStage", &StreamingWork::giStage),
        field("materialsAwaitingTexture", &StreamingWork::materialsAwaitingTexture),
        field("shadersCompiled", &StreamingWork::shadersCompiled),
        field("giRebuilds", &StreamingWork::giRebuilds),
        field("textureDrains", &StreamingWork::textureDrains),
        field("textureDrainLanded", &StreamingWork::textureDrainLanded),
    });
    return t;
}

const Table<AsyncShaderStats> &Record<AsyncShaderStats>::table()
{
    static const Table<AsyncShaderStats> t({
        field("running", &AsyncShaderStats::running),
        field("threads", &AsyncShaderStats::threads),
        field("pending", &AsyncShaderStats::pending),
        field("completed", &AsyncShaderStats::completed),
        field("failed", &AsyncShaderStats::failed),
        field("placeholderDraws", &AsyncShaderStats::placeholderDraws),
        field("pendingSkips", &AsyncShaderStats::pendingSkips),
        field("captureDeferredDraws", &AsyncShaderStats::captureDeferredDraws),
        field("compiledInBackground", &AsyncShaderStats::compiledInBackground),
        field("compiledInForeground", &AsyncShaderStats::compiledInForeground),
        field("paramsMisfits", &AsyncShaderStats::paramsMisfits),
        field("compileErrors", &AsyncShaderStats::compileErrors),
        field("computeParsedInFrame", &AsyncShaderStats::computeParsedInFrame),
        field("computePsoInFrame", &AsyncShaderStats::computePsoInFrame),
        field("computeDestroyWaits", &AsyncShaderStats::computeDestroyWaits),
        field("computeDestroyWaitMs", &AsyncShaderStats::computeDestroyWaitMs),
        field("computeJoins", &AsyncShaderStats::computeJoins),
        field("computeJoinMs", &AsyncShaderStats::computeJoinMs),
        field("glslangCompiles", &AsyncShaderStats::glslangCompiles),
        field("glslangMs", &AsyncShaderStats::glslangMs),
        field("graphicsPsos", &AsyncShaderStats::graphicsPsos),
        field("graphicsPsoMs", &AsyncShaderStats::graphicsPsoMs),
        field("computePsos", &AsyncShaderStats::computePsos),
        field("computePsoMs", &AsyncShaderStats::computePsoMs),
        field("graphicsBuiltInFrame", &AsyncShaderStats::graphicsBuiltInFrame),
        field("graphicsCompiledInFrame", &AsyncShaderStats::graphicsCompiledInFrame),
        field("lowLevelBuiltInFrame", &AsyncShaderStats::lowLevelBuiltInFrame),
        field("rayBuildsJoinedWaiting", &AsyncShaderStats::rayBuildsJoinedWaiting),
        field("rayBuildsInFlight", &AsyncShaderStats::rayBuildsInFlight),
        field("computeDeferred", &AsyncShaderStats::computeDeferred),
        field("rayBuildsLanded", &AsyncShaderStats::rayBuildsLanded),
        field("submitted", &AsyncShaderStats::submitted),
        field("aheadPasses", &AsyncShaderStats::aheadPasses),
        field("aheadRun", &AsyncShaderStats::aheadRun),
        field("aheadPromoted", &AsyncShaderStats::aheadPromoted),
        field("aheadOverOwn", &AsyncShaderStats::aheadOverOwn),
        field("submittedGraphics", &AsyncShaderStats::submittedGraphics),
        field("shaderBurst", &AsyncShaderStats::shaderBurst),
        field("rayBurst", &AsyncShaderStats::rayBurst),
    });
    return t;
}

const Table<AsyncShaderStats::Burst> &Record<AsyncShaderStats::Burst>::table()
{
    static const Table<AsyncShaderStats::Burst> t({
        field("landed", &AsyncShaderStats::Burst::landed),
        field("total", &AsyncShaderStats::Burst::total),
    });
    return t;
}

const Table<PublishedEngineFrame::FrameError> &Record<PublishedEngineFrame::FrameError>::table()
{
    static const Table<PublishedEngineFrame::FrameError> t({
        field("serial", &PublishedEngineFrame::FrameError::serial),
        field("frame", &PublishedEngineFrame::FrameError::frame),
        field("text", &PublishedEngineFrame::FrameError::text),
    });
    return t;
}

const Table<PublishedEngineFrame::Refusal> &Record<PublishedEngineFrame::Refusal>::table()
{
    static const Table<PublishedEngineFrame::Refusal> t({
        field("frame", &PublishedEngineFrame::Refusal::frame),
        field("text", &PublishedEngineFrame::Refusal::text),
    });
    return t;
}

const Table<MonitorStatus> &Record<MonitorStatus>::table()
{
    static const Table<MonitorStatus> t({
        field("level", &MonitorStatus::level),
        field("attachedListeners", &MonitorStatus::attachedListeners),
        field("ringCapacity", &MonitorStatus::ringCapacity),
        field("ringFrames", &MonitorStatus::ringFrames),
        field("pendingEvents", &MonitorStatus::pendingEvents),
        field("framesRecorded", &MonitorStatus::framesRecorded),
        field("framesDropped", &MonitorStatus::framesDropped),
        field("eventsDropped", &MonitorStatus::eventsDropped),
        field("gpuCompiled", &MonitorStatus::gpuCompiled),
        field("gpuSupported", &MonitorStatus::gpuSupported),
        field("gpuActive", &MonitorStatus::gpuActive),
        field("gpuQueryPools", &MonitorStatus::gpuQueryPools),
        field("gpuMarksDropped", &MonitorStatus::gpuMarksDropped),
        field("gpuFramesAgedOut", &MonitorStatus::gpuFramesAgedOut),
        field("gpuReason", &MonitorStatus::gpuReason),
        field("overheadMs", &MonitorStatus::overheadMs),
    });
    return t;
}

const Table<ShadowStatus> &Record<ShadowStatus>::table()
{
    static const Table<ShadowStatus> t({
        field("live", &ShadowStatus::live),
        field("resolution", &ShadowStatus::resolution),
        field("maps", &ShadowStatus::maps),
        field("pssmSplits", &ShadowStatus::pssmSplits),
        field("focusedMaps", &ShadowStatus::focusedMaps),
        field("lightSlots", &ShadowStatus::lightSlots),
        field("casters", &ShadowStatus::casters),
        field("budget", &ShadowStatus::budget),
        field("requestedBudget", &ShadowStatus::requestedBudget),
        field("atlasWidth", &ShadowStatus::atlasWidth),
        field("atlasHeight", &ShadowStatus::atlasHeight),
        field("atlasBytes", &ShadowStatus::atlasBytes),
        field("reflectAtlasBytes", &ShadowStatus::reflectAtlasBytes),
        field("probeAtlasBytes", &ShadowStatus::probeAtlasBytes),
        field("mapped", &ShadowStatus::mapped),
        field("unmapped", &ShadowStatus::unmapped),
        field("countersMeasured", &ShadowStatus::countersMeasured),
        field("shadowPassesLastFrame", &ShadowStatus::shadowPassesLastFrame),
        field("cachedMapRendersLastFrame", &ShadowStatus::cachedMapRendersLastFrame),
        field("shaderLightMismatches", &ShadowStatus::shaderLightMismatches),
        field("reflectPassesLastFrame", &ShadowStatus::reflectPassesLastFrame),
        field("probePassesLastFrame", &ShadowStatus::probePassesLastFrame),
        field("reflectLampPassesLastFrame", &ShadowStatus::reflectLampPassesLastFrame),
        field("probeLampPassesLastFrame", &ShadowStatus::probeLampPassesLastFrame),
        field("cachedInstances", &ShadowStatus::cachedInstances),
        field("uncachedInstances", &ShadowStatus::uncachedInstances),
        field("viewCached", &ShadowStatus::viewCached),
        field("mapsDirtiedLastFrame", &ShadowStatus::mapsDirtiedLastFrame),
        field("atlasRebuilds", &ShadowStatus::atlasRebuilds),
        field("casterWalkItems", &ShadowStatus::casterWalkItems),
    });
    return t;
}

const Table<ShadowMapInfo> &Record<ShadowMapInfo>::table()
{
    static const Table<ShadowMapInfo> t({
        field("slot", &ShadowMapInfo::slot),
        field("node", &ShadowMapInfo::node),
        field("isCached", &ShadowMapInfo::isCached),
        field("dirty", &ShadowMapInfo::dirty),
        field("pssm", &ShadowMapInfo::pssm),
        field("passesLastFrame", &ShadowMapInfo::passesLastFrame),
    });
    return t;
}

const Table<RenderStats> &Record<RenderStats>::table()
{
    static const Table<RenderStats> t({
        field("metricsRecording", &RenderStats::metricsRecording),
        field("fps", &RenderStats::fps),
        field("frameMs", &RenderStats::frameMs),
        field("lastMs", &RenderStats::lastMs),
        field("p95Ms", &RenderStats::p95Ms),
        field("p99Ms", &RenderStats::p99Ms),
        field("bestMs", &RenderStats::bestMs),
        field("worstMs", &RenderStats::worstMs),
        field("draws", &RenderStats::draws),
        field("batches", &RenderStats::batches),
        field("triangles", &RenderStats::triangles),
        field("vertices", &RenderStats::vertices),
        field("instances", &RenderStats::instances),
        field("gpuCountLagFrames", &RenderStats::gpuCountLagFrames),
        field("incompletePsoRequests", &RenderStats::incompletePsoRequests),
        field("forwardPlusLights", &RenderStats::forwardPlusLights),
        field("forwardPlusBudget", &RenderStats::forwardPlusBudget),
        field("forwardPlusOverBudget", &RenderStats::forwardPlusOverBudget),
        field("resourceAdvances", &RenderStats::resourceAdvances),
    });
    return t;
}

const Table<MonitorRecord> &Record<MonitorRecord>::table()
{
    static const Table<MonitorRecord> t({
        field("frame", &MonitorRecord::frame),
        field("serial", &MonitorRecord::serial),
        field("startMs", &MonitorRecord::startMs),
        field("totalMs", &MonitorRecord::totalMs),
        field("cause", &MonitorRecord::cause),
        field("onscreen", &MonitorRecord::onscreen),
        field("scenesUpdated", &MonitorRecord::scenesUpdated),
        field("inputMoved", &MonitorRecord::inputMoved),
        field("inputGesture", &MonitorRecord::inputGesture),
        field("inputFieldsApplied", &MonitorRecord::inputFieldsApplied),
        field("stages", &MonitorRecord::stages),
        field("passes", &MonitorRecord::passes),
        field("cacheWork", &MonitorRecord::cacheWork),
        field("draws", &MonitorRecord::draws),
        field("batches", &MonitorRecord::batches),
        field("instances", &MonitorRecord::instances),
        field("triangles", &MonitorRecord::triangles),
        field("probeCaptures", &MonitorRecord::probeCaptures),
        field("cascadeRebuilds", &MonitorRecord::cascadeRebuilds),
        field("shadowPasses", &MonitorRecord::shadowPasses),
        field("shadowPassesReflect", &MonitorRecord::shadowPassesReflect),
        field("shadowPassesProbe", &MonitorRecord::shadowPassesProbe),
        field("planarRenders", &MonitorRecord::planarRenders),
        field("shaderCompiles", &MonitorRecord::shaderCompiles),
        field("shaderCompileErrors", &MonitorRecord::shaderCompileErrors),
        field("incompleteScenes", &MonitorRecord::incompleteScenes),
        field("incompleteFramesMax", &MonitorRecord::incompleteFramesMax),
        field("metricsRecording", &MonitorRecord::metricsRecording),
        field("orphanedPasses", &MonitorRecord::orphanedPasses),
        field("textureWaitMs", &MonitorRecord::textureWaitMs),
        field("gpuMs", &MonitorRecord::gpuMs),
        field("frameGpuMs", &MonitorRecord::frameGpuMs),
        field("gpuIdleMs", &MonitorRecord::gpuIdleMs),
        field("unattributedGpuMs", &MonitorRecord::unattributedGpuMs),
        field("rowBubbleGpuMs", &MonitorRecord::rowBubbleGpuMs),
        field("widestGapMs", &MonitorRecord::widestGapMs),
        field("widestGap", &MonitorRecord::widestGap),
        field("gpuMarksDropped", &MonitorRecord::gpuMarksDropped),
        field("stagingFlushes", &MonitorRecord::stagingFlushes),
        field("stagingWraps", &MonitorRecord::stagingWraps),
        field("stagingWaits", &MonitorRecord::stagingWaits),
        field("stagingWaitSubmits", &MonitorRecord::stagingWaitSubmits),
        field("stagingWaitMs", &MonitorRecord::stagingWaitMs),
        field("stagingBytes", &MonitorRecord::stagingBytes),
        field("queueFlushes", &MonitorRecord::queueFlushes),
        field("queueFencedFlushes", &MonitorRecord::queueFencedFlushes),
        field("queueSelfDrains", &MonitorRecord::queueSelfDrains),
        field("queueSelfDrainMs", &MonitorRecord::queueSelfDrainMs),
        field("overheadMs", &MonitorRecord::overheadMs),
    });
    return t;
}

const Table<FrameStage> &Record<FrameStage>::table()
{
    static const Table<FrameStage> t({
        field("name", &FrameStage::name),
        field("ms", &FrameStage::ms),
    });
    return t;
}

const Table<FramePass> &Record<FramePass>::table()
{
    static const Table<FramePass> t({
        field("workspace", &FramePass::workspace),
        field("node", &FramePass::node),
        field("pass", &FramePass::pass),
        field("bucket", &FramePass::bucket),
        field("shadowMapIdx", &FramePass::shadowMapIdx),
        field("draws", &FramePass::draws),
        field("batches", &FramePass::batches),
        field("instances", &FramePass::instances),
        field("triangles", &FramePass::triangles),
        field("orphaned", &FramePass::orphaned),
        field("cpuMs", &FramePass::cpuMs),
        field("shadowMs", &FramePass::shadowMs),
        field("gpuMs", &FramePass::gpuMs),
        field("topLevel", &FramePass::topLevel),
        field("selfGpuMs", &FramePass::selfGpuMs),
        field("selfMeasured", &FramePass::selfMeasured),
        field("selfSubmitted", &FramePass::selfSubmitted),
        field("selfStaging", &FramePass::selfStaging),
        field("bubbleGpuMs", &FramePass::bubbleGpuMs),
        field("selfBubbleGpuMs", &FramePass::selfBubbleGpuMs),
        field("insideSubmitted", &FramePass::insideSubmitted),
        field("readsGi", &FramePass::readsGi),
    });
    return t;
}

const Table<CacheWork> &Record<CacheWork>::table()
{
    static const Table<CacheWork> t({
        field("cache", &CacheWork::cache),
        field("reason", &CacheWork::reason),
        field("id", &CacheWork::id),
        field("detail", &CacheWork::detail),
        field("units", &CacheWork::units),
        field("ms", &CacheWork::ms),
        field("gpuMs", &CacheWork::gpuMs),
        field("topLevel", &CacheWork::topLevel),
        field("selfGpuMs", &CacheWork::selfGpuMs),
        field("selfMeasured", &CacheWork::selfMeasured),
        field("selfSubmitted", &CacheWork::selfSubmitted),
        field("selfStaging", &CacheWork::selfStaging),
        field("bubbleGpuMs", &CacheWork::bubbleGpuMs),
        field("selfBubbleGpuMs", &CacheWork::selfBubbleGpuMs),
        field("insideSubmitted", &CacheWork::insideSubmitted),
    });
    return t;
}

const Table<MonitorEvent> &Record<MonitorEvent>::table()
{
    static const Table<MonitorEvent> t({
        field("kind", &MonitorEvent::kind),
        field("frame", &MonitorEvent::frame),
        field("startMs", &MonitorEvent::startMs),
        field("ms", &MonitorEvent::ms),
        field("reason", &MonitorEvent::reason),
        field("label", &MonitorEvent::label),
        field("detail", &MonitorEvent::detail),
        field("value", &MonitorEvent::value),
    });
    return t;
}

const Table<FrameSubmitStatus> &Record<FrameSubmitStatus>::table()
{
    static const Table<FrameSubmitStatus> t({
        field("submitted", &FrameSubmitStatus::submitted),
        field("messagesPosted", &FrameSubmitStatus::messagesPosted),
        field("requests", &FrameSubmitStatus::requests),
        field("scenePartsMerged", &FrameSubmitStatus::scenePartsMerged),
        field("nextSerial", &FrameSubmitStatus::nextSerial),
        field("handOvers", &FrameSubmitStatus::handOvers),
    });
    return t;
}

const Table<ShaderCacheStats> &Record<ShaderCacheStats>::table()
{
    static const Table<ShaderCacheStats> t({
        field("enabled", &ShaderCacheStats::enabled),
        field("dir", &ShaderCacheStats::dir),
        field("fingerprint", &ShaderCacheStats::fingerprint),
        field("sizeBytes", &ShaderCacheStats::sizeBytes),
        field("files", &ShaderCacheStats::files),
        field("maxBytes", &ShaderCacheStats::maxBytes),
        field("pipelineCacheLoaded", &ShaderCacheStats::pipelineCacheLoaded),
        field("pipelineCacheReason", &ShaderCacheStats::pipelineCacheReason),
        field("microcodeLoaded", &ShaderCacheStats::microcodeLoaded),
        field("microcodeEntries", &ShaderCacheStats::microcodeEntries),
        field("seedDir", &ShaderCacheStats::seedDir),
        field("seedState", &ShaderCacheStats::seedState),
        field("seedEntries", &ShaderCacheStats::seedEntries),
        field("hlmsCachesLoaded", &ShaderCacheStats::hlmsCachesLoaded),
        field("computeRecordsLoaded", &ShaderCacheStats::computeRecordsLoaded),
        field("computeRecordsRejected", &ShaderCacheStats::computeRecordsRejected),
        field("computeRecordsStale", &ShaderCacheStats::computeRecordsStale),
        field("layersNotLoaded", &ShaderCacheStats::layersNotLoaded),
        field("compiledThisRun", &ShaderCacheStats::compiledThisRun),
        field("loadedThisRun", &ShaderCacheStats::loadedThisRun),
        field("expectedShaders", &ShaderCacheStats::expectedShaders),
        field("lastSavedUnixMs", &ShaderCacheStats::lastSavedUnixMs),
        field("savesCopied", &ShaderCacheStats::savesCopied),
        field("copyMessages", &ShaderCacheStats::copyMessages),
        field("copyWorstMs", &ShaderCacheStats::copyWorstMs),
        field("copyChunkWorstMs", &ShaderCacheStats::copyChunkWorstMs),
        field("copyChunkMaxEntries", &ShaderCacheStats::copyChunkMaxEntries),
        field("copyChunkEntries", &ShaderCacheStats::copyChunkEntries),
        field("copyFrames", &ShaderCacheStats::copyFrames),
        field("copyFrameOverruns", &ShaderCacheStats::copyFrameOverruns),
        field("savesWritten", &ShaderCacheStats::savesWritten),
        field("saveInFlight", &ShaderCacheStats::saveInFlight),
        field("enginePipelinesThisRun", &ShaderCacheStats::enginePipelinesThisRun),
        field("enginePipelineMs", &ShaderCacheStats::enginePipelineMs),
        field("enginePipelineNames", &ShaderCacheStats::enginePipelineNames),
        field("passCacheEntries", &ShaderCacheStats::passCacheEntries),
        field("passCacheCapacity", &ShaderCacheStats::passCacheCapacity),
        field("renderableCacheEntries", &ShaderCacheStats::renderableCacheEntries),
        field("renderableCacheCapacity", &ShaderCacheStats::renderableCacheCapacity),
    });
    return t;
}

const Table<AtomDrawStatus> &Record<AtomDrawStatus>::table()
{
    static const Table<AtomDrawStatus> t({
        field("live", &AtomDrawStatus::live),
        field("on", &AtomDrawStatus::on),
        field("atomItems", &AtomDrawStatus::atomItems),
        field("atomTwoSided", &AtomDrawStatus::atomTwoSided),
        field("atomMasked", &AtomDrawStatus::atomMasked),
        field("pbsItems", &AtomDrawStatus::pbsItems),
        field("notWorld", &AtomDrawStatus::notWorld),
        field("notPbs", &AtomDrawStatus::notPbs),
        field("customPiece", &AtomDrawStatus::customPiece),
        field("blended", &AtomDrawStatus::blended),
        field("cullFront", &AtomDrawStatus::cullFront),
        field("planar", &AtomDrawStatus::planar),
        field("pending", &AtomDrawStatus::pending),
        field("alphaTested", &AtomDrawStatus::alphaTested),
        field("skinned", &AtomDrawStatus::skinned),
        field("noRow", &AtomDrawStatus::noRow),
        field("atomCaster", &AtomDrawStatus::atomCaster),
        field("stockItems", &AtomDrawStatus::stockItems),
        field("viewRoutesValid", &AtomDrawStatus::viewRoutesValid),
        field("viewRouteItems", &AtomDrawStatus::viewRouteItems),
        field("viewRouteTriangles", &AtomDrawStatus::viewRouteTriangles),
        field("materials", &AtomDrawStatus::materials),
        field("buckets", &AtomDrawStatus::buckets),
        field("twins", &AtomDrawStatus::twins),
        field("decodeDraws", &AtomDrawStatus::decodeDraws),
        field("screenDraws", &AtomDrawStatus::screenDraws),
        field("stereoViews", &AtomDrawStatus::stereoViews),
        field("passthroughViews", &AtomDrawStatus::passthroughViews),
        field("viewPaintable", &AtomDrawStatus::viewPaintable),
        field("wordSlotVisits", &AtomDrawStatus::wordSlotVisits),
        field("pbsNotes", &AtomDrawStatus::pbsNotes),
        field("pbsDatablocks", &AtomDrawStatus::pbsDatablocks),
        field("cutValid", &AtomDrawStatus::cutValid),
        field("cutClusters", &AtomDrawStatus::cutClusters),
        field("cutIndices", &AtomDrawStatus::cutIndices),
        field("cutEvaluated", &AtomDrawStatus::cutEvaluated),
        field("cutOverflow", &AtomDrawStatus::cutOverflow),
        field("cutMissing", &AtomDrawStatus::cutMissing),
        field("cutOverflowIndices", &AtomDrawStatus::cutOverflowIndices),
        field("cutIndexBudget", &AtomDrawStatus::cutIndexBudget),
        field("cutTriangles", &AtomDrawStatus::cutTriangles),
        field("cutRejected", &AtomDrawStatus::cutRejected),
        field("cutRejectedOverflow", &AtomDrawStatus::cutRejectedOverflow),
        field("cutPixelCulled", &AtomDrawStatus::cutPixelCulled),
        field("cutLateClusters", &AtomDrawStatus::cutLateClusters),
        field("cutLateDropped", &AtomDrawStatus::cutLateDropped),
        field("cutSlackClusters", &AtomDrawStatus::cutSlackClusters),
        field("cutSkyClusters", &AtomDrawStatus::cutSkyClusters),
        field("cutReserveIndices", &AtomDrawStatus::cutReserveIndices),
        field("cutSlackTriangles", &AtomDrawStatus::cutSlackTriangles),
        field("cutSkyTriangles", &AtomDrawStatus::cutSkyTriangles),
        field("occlusion", &AtomDrawStatus::occlusion),
        field("occluded", &AtomDrawStatus::occluded),
        field("disoccluded", &AtomDrawStatus::disoccluded),
        field("casterValid", &AtomDrawStatus::casterValid),
        field("casterMaps", &AtomDrawStatus::casterMaps),
        field("casterClusters", &AtomDrawStatus::casterClusters),
        field("casterInstances", &AtomDrawStatus::casterInstances),
        field("casterOverflow", &AtomDrawStatus::casterOverflow),
        field("casterMissing", &AtomDrawStatus::casterMissing),
        field("casterIndexBudget", &AtomDrawStatus::casterIndexBudget),
        field("casterTriangles", &AtomDrawStatus::casterTriangles),
        field("casterMapsPeak", &AtomDrawStatus::casterMapsPeak),
        field("casterUnrecorded", &AtomDrawStatus::casterUnrecorded),
        field("casterOccluded", &AtomDrawStatus::casterOccluded),
        field("casterLateInstances", &AtomDrawStatus::casterLateInstances),
        field("casterLateOccluded", &AtomDrawStatus::casterLateOccluded),
        field("casterLateTriangles", &AtomDrawStatus::casterLateTriangles),
        field("casterPerMap", &AtomDrawStatus::casterPerMap),
        field("densityValid", &AtomDrawStatus::densityValid),
        field("densityPixels", &AtomDrawStatus::densityPixels),
        field("densityObjects", &AtomDrawStatus::densityObjects),
        field("densityClassPixels", &AtomDrawStatus::densityClassPixels),
        field("densityClassObjects", &AtomDrawStatus::densityClassObjects),
        field("densityDepthPixels", &AtomDrawStatus::densityDepthPixels),
        field("hiddenValid", &AtomDrawStatus::hiddenValid),
        field("hiddenDrawnTriangles", &AtomDrawStatus::hiddenDrawnTriangles),
        field("drawnHiddenTriangles", &AtomDrawStatus::drawnHiddenTriangles),
        field("frame", &AtomDrawStatus::frame),
    });
    return t;
}

const Table<AtomCasterMapStats> &Record<AtomCasterMapStats>::table()
{
    static const Table<AtomCasterMapStats> t({
        field("kind", &AtomCasterMapStats::kind),
        field("map", &AtomCasterMapStats::map),
        field("face", &AtomCasterMapStats::face),
        field("lightTested", &AtomCasterMapStats::lightTested),
        field("instances", &AtomCasterMapStats::instances),
        field("occluded", &AtomCasterMapStats::occluded),
        field("lateInstances", &AtomCasterMapStats::lateInstances),
        field("lateOccluded", &AtomCasterMapStats::lateOccluded),
        field("triangles", &AtomCasterMapStats::triangles),
        field("lateTriangles", &AtomCasterMapStats::lateTriangles),
        field("pbsInstances", &AtomCasterMapStats::pbsInstances),
        field("pbsVisibleInstances", &AtomCasterMapStats::pbsVisibleInstances),
        field("pbsTriangles", &AtomCasterMapStats::pbsTriangles),
        field("pbsVisibleTriangles", &AtomCasterMapStats::pbsVisibleTriangles),
        field("receiverTested", &AtomCasterMapStats::receiverTested),
        field("receiverDilation", &AtomCasterMapStats::receiverDilation),
    });
    return t;
}

}}}  // namespace jahshaka::engine::script
