// THE SUN DISC's vertex program (SPECS/SKY_LIGHT_SPEC.md §3).
//
// WHY OURS AND NOT UPSTREAM'S `Ogre/Compositor/QuadCameraDirNoUV_vs`, which is
// what Ogre's own sky quad uses: that shader does not COMPUTE the camera ray,
// it reads it out of the quad's NORMAL attribute — and the only thing that ever
// writes those normals is SceneManager::_renderPhase02, which does it for
// `mSky` and for nothing else (OgreSceneManager.cpp: mSky->setNormals(cameraDirs
// ...); mSky->update()). A second Rectangle2D using that program renders with
// four uninitialised rays, which is why the first version of this drew nothing
// at all. There is exactly one sky in a SceneManager and it is Ogre's.
//
// So this one derives the ray itself, from auto-params Ogre fills for EVERY
// camera without being asked: the inverse view-projection and the world-space
// camera position. One unprojection of the far plane per vertex, and the disc
// is then correct in every view of the scene — the editor, the player, a
// thumbnail, a planar reflection — with no per-camera work of ours and no
// upstream patch.
//
// THE IDENTITY-TRANSFORM TRAP that comes with it: Rectangle2D's constructor
// sets useIdentityView/useIdentityProjection, and AutoParamDataSource honours
// those flags for the VIEW and PROJECTION auto-params — `invViewProj` would
// arrive as the IDENTITY. OgreScene::applySunDisc therefore turns both flags
// OFF and this shader places the quad in clip space by hand instead of going
// through worldViewProj.
#version ogre_glsl_ver_330

vulkan_layout( OGRE_POSITION ) in vec2 vertex;

vulkan( layout( ogre_P0 ) uniform Params { )
	// x = the near NDC depth, y = the FAR one (the backend's convention, which
	// is not the same on every API — never a hard-coded 1.0).
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
	// The rectangle's own vertices ARE normalised device coordinates
	// (setGeometry(-1,-1 .. 2x2)), so the quad needs no transform at all.
	gl_Position = vec4( vertex.xy, rsDepthRange.y, 1.0 );
	// The ray through this corner: unproject the far plane and subtract the eye.
	const vec4 farPoint = invViewProj * vec4( vertex.xy, rsDepthRange.y, 1.0 );
	outVs.cameraDir = farPoint.xyz / farPoint.w - cameraPos.xyz;
}
