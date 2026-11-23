// THE GI FAMILY's FIELD TABLES (SCRIPT-IN-ENGINE-1 stage 2): one per record records_gi.h declares, every field named
// once in declaration order (records.h). THE DOMAINS are the engine's own: what it would clamp FAILS by name (the PCC
// probes 1..8 an axis: clampProbes, PhotonGi.cpp:7885-7887; the probe overlap >= 0.01 and the snaps >= 0: :8089-8092;
// a capture size 0 or 64..1024, :8026-8027 - its power of two is engine.gi.set's own check; the update budget, the
// instance cap and the card budget >= 0, 0 the documented "paused" / "none" / "the tier's": :4365, :5319); the cascade
// table as the record holds it (a used entry's 0 is "the tier's": GiParams.h; engine.gi.set judges the used ones by the
// GI's whole law); a pin frees a cascade or not (0 / 1); the sun contact's range 0.05..50 m - kSunContactMinRange ..
// kSunContactMaxRange (GiGather.h, the band the engine clamps it to: OgreRayQuery.cpp:14399-14400) WRITTEN AS DOUBLES: the
// registry compares in double, and the float constant 0.05f widens to 0.05000000074505806, which would refuse 0.05 itself.
#include "script/records_gi.h"

namespace jahshaka { namespace engine { namespace script {

const Table<GiParams::GiCascadeDesc> &Record<GiParams::GiCascadeDesc>::table()
{
    using C = GiParams::GiCascadeDesc;
    static const Table<C> t({JAH_FIELD(C, halfSize).atLeast(0).in("m"), JAH_FIELD(C, resolution).range(0, 256).in("cells"),
                             JAH_FIELD(C, stepCells).range(0, 128).in("cells")});
    return t;
}

const Table<GiParams> &Record<GiParams>::table()
{
    using G = GiParams;
    static const Table<G> t({JAH_FIELD(G, mode), JAH_FIELD(G, quality),
                             JAH_FIELD(G, testProbeRegionMin).in("m"),
                             JAH_FIELD(G, testProbeRegionMax).in("m"),
                             JAH_FIELD(G, testProbeGridFitMax).in("m"), JAH_FIELD(G, bounces),
                             JAH_FIELD(G, pccProbesX).range(1, 8), JAH_FIELD(G, pccProbesY).range(1, 8),
                             JAH_FIELD(G, pccProbesZ).range(1, 8), JAH_FIELD(G, probeHdr),
                             JAH_FIELD(G, probeShadows), JAH_FIELD(G, probeCaptureSize).range(0, 1024).in("px"),
                             JAH_FIELD(G, probeOverlap).atLeast(0.01), JAH_FIELD(G, probeSnapDeviation).atLeast(0),
                             JAH_FIELD(G, probeSnapSidesMin).atLeast(0),
                             JAH_FIELD(G, probeSnapSidesMax).atLeast(0),
                             JAH_FIELD(G, updateBudget).atLeast(0).in("probes/frame"), JAH_FIELD(G, ddgi),
                             JAH_FIELD(G, cascadeCount).range(0, kGiTierMaxCascades), JAH_FIELD(G, cascadeSet),
                             JAH_FIELD(G, cascadeInstanceCap).atLeast(0),
                             JAH_FIELD(G, cascadeVoxelLod), JAH_FIELD(G, dragMoverChannel),
                             JAH_FIELD(G, cards), JAH_FIELD(G, cardBudgetTexels).atLeast(0).in("texels/frame")});
    return t;
}

const Table<GiPlacementPin> &Record<GiPlacementPin>::table()
{
    using P = GiPlacementPin;
    static const Table<P> t({JAH_FIELD(P, on), JAH_FIELD(P, cascadeBuiltCam).upTo(3 * kGiTierMaxCascades).in("m"),
                             JAH_FIELD(P, cascadeFree).upTo(kGiTierMaxCascades).range(0, 1), JAH_FIELD(P, field),
                             JAH_FIELD(P, fieldOrigin).in("m")});
    return t;
}

const Table<SunContactDesc> &Record<SunContactDesc>::table()
{
    using S = SunContactDesc;
    static const Table<S> t({JAH_FIELD(S, enabled), JAH_FIELD(S, range).range(0.05, 50).in("m"),
                             JAH_FIELD(S, resolution)});
    return t;
}

}}}  // namespace jahshaka::engine::script
