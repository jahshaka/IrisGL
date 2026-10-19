// ENGINE-POLISH-ATOM-1 stage 3 — THE CASTER CUT'S MASKED FRAGMENT STAGE for a directional or spot map
// (OgreAtomCasterPass.cpp's masked pipelines; compiled with JAH_MASKED). Depth only, as the opaque ranges are: the
// one thing this stage does is DISCARD where the cut-out's mask is clear (jah_alpha_mask.glsl, at the pyramid level
// the fragment's UV footprint in the map's texels asks for: ATOM-3 stage 2) — the stock caster's alpha test, so the
// map is holed where the card is (a point map's masked stage is atom_caster_point.frag's).
#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_GOOGLE_include_directive : require

#include "jah_alpha_mask.glsl"

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
layout( location = 2 ) in vec2 inUv;
layout( location = 3 ) flat in uint inSlot;

void main()
{
	// THE FOOTPRINT in the map's texels, in uniform control flow (ATOM-3 stage 2: the pyramid level it asks for).
	const vec2 duvdx = dFdx( inUv ), duvdy = dFdy( inUv );
	const uint rec = jahMaskRecord( pc.maskTable, inSlot, 0u );
	if( rec != kJahMaskNone && !jahMaskSolidAtFootprint( pc.maskTable, rec, inUv, duvdx, duvdy ) )
		discard;
}
