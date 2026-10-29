// THE REFLECTION SET'S PARAMETERS (rq_reflect.comp, rq_reflect_filter.comp, rq_reflect_temporal.comp,
// rq_reflect_resolve.comp, rq_reflect_compose.comp - one block, OgreRayQuery.cpp's ReflectParams member for member;
// PHOTON-7 stage 10, 10-2: the five copies became this one text when the hit set's uniforms moved into JahHitParams).
//
// No Hlms directive mark anywhere in this file.

#ifndef JAH_REFLECT_PARAMS_GLSL
#define JAH_REFLECT_PARAMS_GLSL

const int kMaxCascades = 4;
#define JAH_HIT_PARAMS p.hit
#include "jah_rq_hit_params.glsl"

layout( set = 0, binding = 1 ) uniform Params
{
	/// xyz = the camera in world space, w = 1 for a perspective camera and 0
	/// for an orthographic one (the reconstruction differs — see worldAt()).
	vec4 camPos;
	/// The world-space ray basis: worldDir(u,v) = rayTL + rayRight*u + rayDown*v,
	/// scaled so its component along the camera's forward axis is exactly 1.
	/// Then `camPos + worldDir * viewDistance` is the surface. Under an
	/// orthographic camera rayTL/rayRight/rayDown carry the image PLANE instead
	/// (an offset, not a ray) and `fwd` is the one view direction.
	vec4 rayTL;
	vec4 rayRight;
	vec4 rayDown;
	vec4 fwd;
	/// x = A, y = B of Camera::getProjectionParamsAB() (reverse-Z safe),
	/// z = the far plane (the orthographic depth scale), w = the roughness
	/// gate's FEATHER half-width (kRayReflectFeather).
	vec4 projParams;
	/// xy = the trace resolution in pixels, zw = the full-resolution size of
	/// jahSsrReflection.
	vec4 resolution;
	/// x = the roughness gate — the PROJECT's dial, not a renderer constant:
	/// above it the probe/field photograph is a good enough integral of a wide
	/// lobe and a ray is spent for nothing. The transition is FEATHERED over
	/// projParams.w either side of it. y = the ray's maximum length in
	/// world units, z = the frame index (the sample sequence's only input),
	/// w unused (the cascades bound are the hit set's: JahHitParams).
	vec4 knobs;
	/// x = THE REST FRAME k, 1..N on a frame the view rests (PHOTON-2 1e, fix F1: the gather's rule — a rest mean handed
	/// over from the frozen moving history, rq_reflect_temporal.comp), 0 while it moves, y = the
	/// drawn surface's bound in sample footprints (Types.h rayOriginDrawnFootprints) - the RAY ORIGIN's
	/// (jah_ray_origin.glsl, at the point's Euclidean distance from its eye),
	/// z unused (the sky cube's flag is the hit set's: JahHitParams), w = the history's minimum blend
	/// weight (the floor on 1/n — how long the temporal mean remembers).
	vec4 knobs2;
	/// The VIEW space's three axes in world space (the camera's orientation as
	/// a rotation): what turns the G-buffer's view-space normal back into one.
	vec4 viewAxisX;
	vec4 viewAxisY;
	vec4 viewAxisZ;
	/// THE PREVIOUS FRAME'S CAMERA, in exactly the same five numbers as this
	/// frame's (camPos, rayTL, rayRight, rayDown, fwd). Not a view-projection
	/// matrix, deliberately: inverting the basis above is the same arithmetic
	/// forwards and backwards, so the reprojection cannot disagree with the
	/// reconstruction about a handedness, a y-flip or a depth convention — and
	/// each of those is a silent half-pixel-to-whole-image error that only shows
	/// up as a smear while the camera moves.
	vec4 prevCamPos;
	vec4 prevRayTL;
	vec4 prevRayRight;
	vec4 prevRayDown;
	vec4 prevFwd;
	/// ---- THE SECOND EYE (lane REFLECT-VR-1, LAYERED-STEREO-1) ------------
	/// x = 1 when this target carries TWO EYES, one per LAYER of every screen
	/// image here (the dispatch's z). Then the five basis vectors above are the
	/// LEFT eye's and the five below the right eye's, each over its own layer
	/// as a 0..1 image; with x = 0 the block below is unread and the basis
	/// above is the one camera's, exactly as before. One dispatch, one
	/// descriptor set, one history ring: the eye is the layer (gEye).
	///
	/// y = THE VIEW'S AGE: how many consecutive frames this history has been
	/// written (0 on a first frame, a resize, a scene bind): the filter's young-VIEW fade (a two-frame screenshot).
	/// z = 1 writes THE PHOTON VIEW'S Hits overlay (binding 37, PHOTON-VIEW-1).
	/// w = THE REST'S LENGTH N (PHOTON-2 fix F1): the frames the rest mean is handed over in, the gather's own
	/// settleFramesOf(the reflection's 32-frame history) — after which the view holds.
	vec4 stereo;
	vec4 camPos2;
	vec4 rayTL2;
	vec4 rayRight2;
	vec4 rayDown2;
	vec4 fwd2;
	vec4 prevCamPos2;
	vec4 prevRayTL2;
	vec4 prevRayRight2;
	vec4 prevRayDown2;
	vec4 prevFwd2;
	/// ---- A MOVING OBJECT (REFLECT-MOVERS-1) --------------------------------
	/// x = 1 when the VIEW'S ID IMAGE is bound (binding 38, the visibility
	/// buffer's `jahAtomIds`: which slot drew each pixel) — then a pixel on a
	/// moving slot reprojects through that slot's two poses (jah_reproject.glsl).
	/// 0 = no id pass on this chain (a stereo chain, the split off): every
	/// surface takes the camera path. yz = the id image's size in pixels.
	/// w = 1 with the lane's hit-motion rules on, 2 when a mover also moved in the
	/// last two frames (the departure rule's gate), 3 when a POSE moved in the last
	/// two frames (SKINNED-VELOCITY-1: the pixels no moving Atom item drew are
	/// identified through the TLAS; the arm "reflect.posed" off keeps it at 2 — the paired
	/// cost arm); 0 = the arm "reflect.motion" off: the camera-only
	/// history of before.
	vec4 motion;
	/// xy unused (the alpha table is the hit set's: JahHitParams), z = THE RAY ORIGIN'S POINT TERM: the rasteriser's
	/// sub-pixel snap in pixels (jah_ray_origin.glsl jahDepthPointError; 0 = the term off, rays.originPoint 0), w = THE
	/// HITS' AREA CYCLE (PHOTON-7 11-2 (2)): the rest samples one full cycle of the hit's time-stratified area draw takes
	/// (kHitAreaStrata^2) where the hit table holds an area light, 0 without - a rest texel's stop waits for it.
	vec4 alpha;
	/// THE MEDIA ALONG THE REFLECTION (PHOTON-I-1 fix 5): OgreScene::fogAlong's eight
	/// (jah_fog_along.glsl states them).
	vec4 fog[8];
	/// THE REST BLOCK (PHOTON-2 round 2): x = 1 = THE GI FREEZE (the arm "gi.freeze", M1's measuring arm: the temporal
	/// pass keeps every moving history as carried - no new sample, no clamp; a fixed estimator takes its ray;
	/// rq_reflect_temporal.comp).
	/// z = THE REST'S RAYS A FRAME a texel still converging traces (the trace's batch at rest): its share of the view's
	/// rest count (OgreRayQuery.cpp kReflectRestRaysCap's note - k at the rest's start, up to 64 in the tail);
	/// w = THE RECORD ALLOWANCE a batching texel's record run may take of the list this frame (rq_reflect.comp's record
	/// runs: min( k_active, capacity / batching ); 0 = a record ends the batch).
	vec4 rest;
	/// THE REST BREAKS PER TEXEL AGAINST THE CHANGED-REGION LIST (PHOTON-7 8B, B2; jah_rest_regions.glsl at the set's
	/// kReflectRegionsBinding): x = the list's regions this frame (0: none, or no list closed), y = 1 = every resting
	/// texel resumes (the list overflowed - the sun, the sky, a change with no box - or restarted), z = 1 = a brick mask
	/// is this frame's. A resting texel resumes when a GEOMETRY region (a mover's swept box, an edit) crosses one of its
	/// segments (its ray, its lobe's mean segment, its hit's sun leg, its NEE ray, C2's secondary rays), or a LIGHT
	/// region or a changed brick lies at one of its hits (rq_reflect.comp restSegmentMoved, restHitChanged).
	vec4 restLists;
	/// ...THE BRICK MASKS (the four bindings after the regions'), per cascade in the hit set's order: xyz the brick
	/// grid's corner, w a brick in metres; and xyz its bricks per axis, w 1 = its mask is this frame's.
	vec4 restBrickOrigin[4];
	vec4 restBrickDims[4];
	/// THE ONE ESTIMATOR'S SOURCES (PHOTON-7 stage 6): screen.x = 1 when the SCREEN TRACE runs (the chain's screen
	/// sources are bound: PostFxDesc::reflectSources Both or Screen, a mono view), y = 1 for THE SCREEN ALONE (Screen: a
	/// miss is no sample - no ray follows it), z = the march's step count, w = its phase rule (PostFxDesc::ssrMarchPhase);
	/// screen2.x = the march's length (ssrMaxDistance), y = its thickness (ssrThickness), z = where the FAR COPIES' query
	/// starts (the near length less the far copies' widest hand-over gap, the gather's rule), w = where it ends (the far
	/// plane; 0 = no far query).
	vec4 screen;
	vec4 screen2;
	/// x, y: the denoiser space's exposure (the resolve's, the temporal pass'); z = THE LOBE DRAW'S SEED OFFSET (the arm
	/// reflect.seedOffset, 0 shipped): added to the frame index the draw takes; w = 1 under the measuring arm
	/// reflect.flatHits (every hit answered by a constant of its instance).
	vec4 denoiser;
	/// (the filter's display table, beside: this pass reads none of it)
	vec4 display[16];
	/// THE REST'S COMPACT PATH (OgreRayQuery.cpp kReflectCompactTexels): x = the half of the list this frame fills (last
	/// frame's count, in the other half, decides the path), y = the tail's rays a frame (the rest's count / 8), z = L,
	/// w = 1 when the compact pass runs this frame.
	vec4 compact;
	// THE HIT SET'S UNIFORMS (PHOTON-7 stage 10, 10-2: the one text, jah_rq_hit_params.glsl - its cascades, environment,
	// cards' counts, hit list and sun, tables and surface)
	JahHitParams hit;
} p;

#endif   // JAH_REFLECT_PARAMS_GLSL
