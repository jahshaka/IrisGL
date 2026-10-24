// THE RAY ORIGIN - ONE LAW FOR EVERY RAY THAT LEAVES A SURFACE (PHOTON-7 stage 6, READ_B finding 3). The C++ half is
// jahshaka/engine/Types.h (rayOriginDrawnFootprints, rayOriginLevelLift, kLevelDriftOverBound): the host turns the
// surface's LOD rule into the numbers this file spends, so no shader carries a constant of the law.
//
// Every ray traces each mesh's LEVEL 0 (Types.h, ONE RAY LEVEL PER MESH) while the surface it starts on is the one the
// VIEW DREW (Atom's cut, a stock item's level - the reflection, the gather's probes, the sun contact) or the one a CARD
// CAPTURED (the bake's level for its texel - the card sun). With tMin 0 and no back-face cull, a start inside level 0
// hits it at t ~ 0, so the origin is lifted along the drawn surface's GEOMETRIC normal (jah_geometric_normal.glsl:
// never a normal map's tilt, which shrinks the lift's geometric component) by
//     the POINT'S OWN ERROR   - its float bound (jahRayFloatBound: the unprojection's, or a ray-query hit's own
//                               position with the distance it was carried) plus what its reconstruction adds: a card
//                               texel's average and its depth layer's quantum; for a DEPTH-BUFFER point the depth's
//                               quantum through the inverse projection and the rasteriser's sub-pixel snap
//                               (jahDepthPointError, below)
//   + the SURFACE'S BOUND     - how far the drawn surface may lie from level 0, measured as the LOD rule measures it
//                               (0 for a ray leaving a hit ON level 0: it starts on the surface it traces).
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

/// THE DEPTH-REBUILT POINT'S OWN ERROR along its surface's normal (READ_B F5 finding (b); the lead's round-7 derivation):
/// a point rebuilt at pixel p from its stored depth `raw` (a 32-bit float, reverse or forward) at view distance `dist`
/// is off its surface by
///   (1) THE DEPTH'S QUANTUM. The stored value lies within kRayDepthUlps of the rasterised plane's (the quantum
///       jah_geometric_normal.glsl accepts as one plane's rounding); through the inverse projection that is
///       e = q x |d dist / d raw| of FORWARD distance - perspective dist = B / (raw - A), so dist^2 / |B|; orthographic
///       dist = |(raw - A) / B|, so 1 / |B| - which moves the point along its view ray `ray` (forward component 1,
///       perspective; the eye's forward, orthographic): e x |N . ray| off the surface.
///   (2) THE RASTERISER'S SUB-PIXEL SNAP. A vertex's framebuffer position is rounded to the device's
///       subPixelPrecisionBits, so the drawn triangle is the true one with each corner moved by up to `snapPx` pixels
///       (sqrt 2 x 2^-(bits + 1)); the depth a covered pixel stores is the true surface's at a screen point within snapPx
///       of the pixel (a convex combination of the corners' moves), and the rebuild puts it on the pixel's own ray at
///       that depth: a move IN THE IMAGE PLANE of snapPx x one pixel's world size there (`footprint`), whose normal
///       component is at most that x sqrt(1 - (N . fwd)^2).
/// The unprojection's own float bound is the caller's jahRayFloatBound. `snapPx` <= 0 = the term off (the arm
/// rays.originPoint 0: the float-only control).
const float kRayDepthUlps = 8.0;
float jahDepthPointError( float raw, float dist, float projB, bool perspective, vec3 ray, vec3 fwd, vec3 geomN,
						  float footprint, float snapPx )
{
	if( !( snapPx > 0.0 ) )
		return 0.0;
	const float ulp = exp2( floor( log2( max( abs( raw ), 1e-30 ) ) ) - 23.0 );
	const float b = max( abs( projB ), 1e-30 );
	const float e = kRayDepthUlps * ulp * ( perspective ? dist * dist / b : 1.0 / b );
	const float nf = dot( geomN, fwd );
	return e * abs( dot( geomN, ray ) ) + snapPx * footprint * sqrt( max( 1.0 - nf * nf, 0.0 ) );
}

/// THE ORIGIN: the point `surface` lifted along `geomN` (its surface's geometric normal, on the side the ray leaves
/// into) by its float bound (`carried` = the distance it was reconstructed along) plus `surfaceBound` (the point's
/// reconstruction beyond float + the surface's bound against level 0, metres). The caller traces from it with tMin 0.
/// ONE FORM, AT EVERY RAY (decided at the photon-7r merge on rtreflect.contact, gi.sun_contact_acne and
/// gi.card_lamp_shadow under both forms; spikes/photon-7/merge): the lift is ALONG THE NORMAL, never along the ray's own
/// segment (lift / cos: unbounded at grazing - a start past an occluder, an L9 path; measured worse on the contact's
/// edges and crease and the area light's card penumbra, and red on gi.area_contact). Its cases:
///   a DIRECTION (the sun, a reflection, a gather or a hit's ray): traced from the origin along it;
///   a POINT (a lamp, an area light's stratum - a finite distance): the ray AIMED FROM THE ORIGIN at the point, so the
///     segment tested ends on the light and the lift's parallax vanishes there (jah_area_strata.glsl's
///     jahAreaRayOrigin writes this case once more: that include is also an Hlms piece, which cannot include this file);
///   (stage 10 adds a VOXEL SIDE's rays as a third case of this function, never a second law).
vec3 jahRayOrigin( vec3 surface, vec3 geomN, float carried, float surfaceBound )
{
	return surface + geomN * ( jahRayFloatBound( surface, carried ) + surfaceBound );
}

#endif   // JAH_RAY_ORIGIN_GLSL
