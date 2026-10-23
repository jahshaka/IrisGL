// THE RAY ORIGIN - ONE LAW FOR EVERY RAY THAT LEAVES A SURFACE (PHOTON-7 stage 6, READ_B finding 3). The C++ half is
// jahshaka/engine/Types.h (rayOriginDrawnFootprints, rayOriginLevelLift, kLevelDriftOverBound): the host turns the
// surface's LOD rule into the numbers this file spends, so no shader carries a constant of the law.
//
// Every ray traces each mesh's LEVEL 0 (Types.h, ONE RAY LEVEL PER MESH) while the surface it starts on is the one the
// VIEW DREW (Atom's cut, a stock item's level - the reflection, the gather's probes, the sun contact) or the one a CARD
// CAPTURED (the bake's level for its texel - the card sun). With tMin 0 and no back-face cull, a start inside level 0
// hits it at t ~ 0, so the origin is lifted along the drawn surface's GEOMETRIC normal (jah_geometric_normal.glsl:
// never a normal map's tilt, which shrinks the lift's geometric component) by
//     the POINT'S OWN ERROR   - its float bound (jahRayFloatBound) plus what its reconstruction adds (a card texel's
//                               average and its depth layer's quantum; nothing for a depth-buffer pixel)
//   + the SURFACE'S BOUND     - how far the drawn surface may lie from level 0, measured as the LOD rule measures it.
// THE VIEW'S BOUND (jahViewDrawnBound) is the rule's own arithmetic: the host's footprint count (the tolerance x the
// LOD bias over one minus the view's switch band, times the bake's drift ratio) x one sample's footprint per metre of
// THIS EYE (|rayDown| / the full target's height: 2 / (proj11 x H), the strategy's sampleFootprintPerspective) x the
// point's EUCLIDEAN distance from the eye - the stock strategy and Atom's cut both judge an object at its bounding
// sphere's nearest distance, and no point of the object is nearer the eye than that. The FORWARD depth the old lift
// used is that distance x cos(off-axis angle): 1.31x short at a 16:9 45-degree frame's corner. Orthographic: no
// distance term (the rule's own ortho case); |rayDown| is the ortho window's height.
// No at-sign anywhere in this file (the house rule for every shared include).
#ifndef JAH_RAY_ORIGIN_GLSL
#define JAH_RAY_ORIGIN_GLSL

#include "jah_rq_finite.glsl"

/// The drawn surface's bound against level 0 at a VIEW's point: `drawnFootprints` (the host's
/// rayOriginDrawnFootprints, 0 under the rays.originCut control) at the point `world` the eye at `eyePos` drew, through
/// that eye's `rayDown` over a full target `height` pixels tall; `perspective` false = orthographic.
float jahViewDrawnBound( float drawnFootprints, vec3 world, vec3 eyePos, vec3 rayDown, float height, bool perspective )
{
	const float footprint = length( rayDown ) / max( height, 1.0 );
	return drawnFootprints * footprint * ( perspective ? length( world - eyePos ) : 1.0 );
}

/// THE ORIGIN: the point `surface` lifted along `geomN` (its surface's geometric normal, on the side the ray leaves
/// into) by its float bound (`carried` = the distance it was reconstructed along) plus `surfaceBound` (the point's
/// reconstruction beyond float + the surface's bound against level 0, metres). The caller traces from it with tMin 0.
vec3 jahRayOrigin( vec3 surface, vec3 geomN, float carried, float surfaceBound )
{
	return surface + geomN * ( jahRayFloatBound( surface, carried ) + surfaceBound );
}

#endif   // JAH_RAY_ORIGIN_GLSL
