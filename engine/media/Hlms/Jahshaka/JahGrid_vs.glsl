// THE GRID QUAD'S VERTEX PROGRAM (GRID-2) — JahSunDisc_vs's, verbatim in shape:
// a screen-filling quad at the far plane whose camera ray is derived from the
// inverse view-projection (nothing writes a second Rectangle2D's normals, so the
// camera-direction normals Ogre's own sky quad reads are not available here).
#version ogre_glsl_ver_330

vulkan_layout( OGRE_POSITION ) in vec2 vertex;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec2 rsDepthRange;
	uniform mat4 invViewProj;
	uniform vec4 cameraPos;
vulkan( }; )

out gl_PerVertex
{
	vec4 gl_Position;
};

vulkan_layout( location = 0 )
out block
{
	vec3 cameraDir;
} outVs;

void main()
{
	gl_Position = vec4( vertex.xy, rsDepthRange.y, 1.0 );
	const vec4 farPoint = invViewProj * vec4( vertex.xy, rsDepthRange.y, 1.0 );
	outVs.cameraDir = farPoint.xyz / farPoint.w - cameraPos.xyz;
}
