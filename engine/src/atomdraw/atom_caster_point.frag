// ATOM-SHADOWS-1 — THE CASTER CUT'S POINT-MAP FRAGMENT STAGE (OgreAtomCasterPass.cpp).
// A point light's cube face stores the biased LINEAR DISTANCE in its colour, the stock
// caster's own (ShadowCaster_piece_ps.any, hlms_shadowcaster_point under reverse-Z):
// (|toCamera| - near) / (far - near) + bias, then 1 - that. A directional or spot map
// is depth only and has no fragment stage.
#version 460

layout( push_constant ) uniform AtomCasterPc
{
	vec4  viewProjRow[4];
	vec4  eyeBias;
	vec4  depthRange;
	uvec2 instances;
	uvec2 meshes;
	uvec2 rows;
	uvec2 pad;
} pc;

layout( location = 0 ) in vec3 inToCamera;
layout( location = 1 ) flat in float inConstBias;

layout( location = 0 ) out float outDistance;

void main()
{
	float d = ( length( inToCamera ) - pc.depthRange.x ) * pc.depthRange.y + inConstBias;
	outDistance = 1.0 - d;
}
