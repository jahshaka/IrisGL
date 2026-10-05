// THE HIDDEN-AREA MASK UNDER MULTIVIEW STEREO (LAYERED-STEREO-1).
//
// The runtime's per-eye visibility mesh, both eyes in ONE vertex buffer with
// the eye's index in z (OgreVrSession.cpp, fetchHiddenAreaData). Ogre's own
// `Ogre/VR/HiddenAreaMeshVr_vs` sends a vertex to its eye's VIEWPORT; the stereo
// chain is multiview — both eyes at the same origin of their own layer, every
// draw broadcast to both — so here a vertex belongs to the view it is drawn in
// or to nobody: the other eye's triangles collapse onto one point outside the
// clip volume and rasterise nothing.
#version ogre_glsl_ver_330

#extension GL_EXT_multiview : require

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform mat4 projectionMatrix;
	uniform vec2 rsDepthRange;
vulkan( }; )

vulkan_layout( OGRE_POSITION ) in vec4 vertex;

out gl_PerVertex
{
	vec4 gl_Position;
};

void main()
{
	if( int( vertex.z ) != int( gl_ViewIndex ) )
	{
		gl_Position = vec4( 2.0, 2.0, 2.0, 1.0 );
		return;
	}
	gl_Position.xy = ( projectionMatrix * vec4( vertex.xy, 0.0, 1.0 ) ).xy;
	gl_Position.z = rsDepthRange.x;
	gl_Position.w = 1.0;
}
