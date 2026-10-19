// ATOM-SHADOWS-1 — THE CASTER CUT'S POINT-MAP FRAGMENT STAGE (OgreAtomCasterPass.cpp).
// A point light's cube face stores the biased LINEAR DISTANCE in its colour, the stock
// caster's own (ShadowCaster_piece_ps.any, hlms_shadowcaster_point under reverse-Z):
// (|toCamera| - near) / (far - near) + bias, then 1 - that. A directional or spot map
// is depth only and has no fragment stage (but for its MASKED ranges:
// atom_caster_masked.frag). JAH_MASKED: a cut-out's fragment is discarded where its
// mask is clear first (ENGINE-POLISH-ATOM-1 stage 3).
#version 460
#ifdef JAH_MASKED
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_GOOGLE_include_directive : require
#include "jah_alpha_mask.glsl"
#endif

layout( push_constant ) uniform AtomCasterPc
{
	vec4  viewProjRow[4];
	vec4  eyeBias;
	vec4  depthRange;
	uvec2 instances;
	uvec2 meshes;
	uvec2 rows;
	uvec2 maskTable;
} pc;

layout( location = 0 ) in vec3 inToCamera;
layout( location = 1 ) flat in float inConstBias;
#ifdef JAH_MASKED
layout( location = 2 ) in vec2 inUv;
layout( location = 3 ) flat in uint inSlot;
#endif

layout( location = 0 ) out float outDistance;

void main()
{
#ifdef JAH_MASKED
	// the footprint in uniform control flow (ATOM-3 stage 2: the pyramid level it asks for)
	const vec2 duvdx = dFdx( inUv ), duvdy = dFdy( inUv );
	const uint rec = jahMaskRecord( pc.maskTable, inSlot, 0u );
	if( rec != kJahMaskNone && !jahMaskSolidAtFootprint( pc.maskTable, rec, inUv, duvdx, duvdy ) )
		discard;
#endif
	float d = ( length( inToCamera ) - pc.depthRange.x ) * pc.depthRange.y + inConstBias;
	outDistance = 1.0 - d;
}
