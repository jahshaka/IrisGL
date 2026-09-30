// THE HEIGHT FOG OVER THE SKY'S PIXELS (SKY-DEFAULTS-1). A full-screen quad at
// the far plane, drawn after the sky, the cloud sheet and the sun disc and
// before any geometry (queue 5, subgroup 1 — JahAtmosphere::createFogQuad), so
// only the pixels that stay sky keep it: the camera ray is integrated through
// the height layer out to the sky distance (Types.h kHeightFogSkyDistance) and
// the fog's colour is blended in by what the layer lets through — a ray that
// climbs slowly takes the fog, a steep one stays the sky, a ray under the
// horizon is fogged completely. The integral is jah_height_fog.glsl's, the one
// the PBS pixel shader runs on every surface.
#version ogre_glsl_ver_330

vulkan( layout( ogre_P0 ) uniform Params { )
	// x = density (per m), y = falloff (per m), z = base height, w = start (m)
	uniform vec4 heightFog;
	// rgb = the in-scatter (linear), w = the sky distance (m)
	uniform vec4 heightFogColour;
	// the eye (Ogre fills it per camera: every view, a mirror, a thumbnail)
	uniform vec4 cameraPos;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec3 cameraDir;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

#include "JahHeightFog.glsl"

void main()
{
	const vec3 dir = normalize( inPs.cameraDir );
	const float t = jahHeightFogT( cameraPos.y, dir.y, heightFogColour.w, heightFog );
	// Alpha-blended (JahAtmosphere.material): sky x t + colour x (1 - t).
	fragColour = vec4( heightFogColour.rgb, 1.0 - t );
}
