// THE STEREO SCREEN QUAD (SPECS/VR_SPEC.md §4.3; lane VR-2's F2).
//
// THE DEFECT IT EXISTS FOR. Under instanced stereo the pass draws EVERYTHING
// twice (OgreRenderQueue.cpp:697-699 — two instances, the base instance shifted
// by one) and the second instance reaches the second eye only if the vertex
// shader sends it there with `gl_ViewportIndex`. Every Hlms vertex shader does
// (Pbs/Unlit/Terra's 800.VertexShader_piece_vs.any). The three SCREEN QUADS in
// this engine — Ogre's sky, Ogre's atmosphere and our sun disc — do not: they
// are low-level materials whose vertex programs were written for one viewport,
// so both copies land in viewport 0 and THE RIGHT EYE HAS NO SKY.
//
// And the ray is wrong in the left eye too, which is the half that is easy to
// miss. Upstream's `Ogre/Compositor/QuadCameraDirNoUV_vs` does not compute the
// camera ray at all: it reads it out of the quad's NORMALS, which
// SceneManager writes once per camera from that camera's own frustum corners
// (OgreSceneManager.cpp:1487-1499). In a VR session the rendering camera is the
// HEAD — its own projection is the View's CameraDesc, not either eye's — so the
// sky is drawn through a frustum nobody is looking through.
//
// THIS SHADER FIXES BOTH, and it fixes them the way our sun disc already
// worked: the eye index comes from the instance, and the ray is UNPROJECTED
// from that eye's own inverse view-projection instead of read out of a normal.
// The session writes the pair every frame (the same two matrices VrData holds).
//
// IT SERVES ALL THREE QUADS. Every consumer normalizes the interpolated ray
// (SkyCubemap_ps, SkyEquirectangular_ps, AtmosphereNprSky_ps.any:41, our own
// JahSunDisc_ps), so the ray's LENGTH is free and one program can feed them
// all; and all three quads are full-screen rectangles whose vertices are
// already normalised device coordinates, so there is no transform to apply.
//
// VULKAN ONLY, deliberately: `gl_InstanceIndex` is the Vulkan spelling and
// instanced stereo is a Vulkan feature of this engine. A GL3Plus build would
// need `gl_InstanceID` and a second delegate; it would also need instanced
// stereo, which it does not have.
#version ogre_glsl_ver_330

#extension GL_ARB_shader_viewport_layer_array : require

vulkan_layout( OGRE_POSITION ) in vec2 vertex;

vulkan( layout( ogre_P0 ) uniform Params { )
	// x = the near NDC depth, y = the FAR one (the backend's convention).
	uniform vec2 rsDepthRange;
	// THE FIRST EYE IS OGRE'S OWN AUTO-PARAMS — the RENDERING camera's inverse
	// view-projection and position, which Ogre fills for every camera without
	// being asked. That is what makes this ONE program correct in every pass:
	// in a VR pass the rendering camera carries the LEFT eye's projection (the
	// session sets it), and in any other pass — the desktop mirror view, a
	// probe capture, a thumbnail, the mono control a suite renders — it is that
	// pass's own camera and the quad is drawn exactly as it always was.
	uniform mat4 autoInvViewProj;
	uniform vec4 autoCameraPos;
	// THE SECOND EYE is the only thing the session has to write: no ordinary
	// camera has a second eye, so no auto-param can carry it.
	//
	// AS FOUR CORNER RAYS, not as a matrix, and that is a finding rather than a
	// preference: a ray unprojected from an inverse view-projection has to
	// agree with Ogre's auto-param on THREE conventions at once — the reverse-Z
	// depth range, the render-pass texture flip, and the handedness of the
	// projection — and a mismatch in any of them is a sky that looks plausible
	// and is wrong. The corner rays have no conventions in them at all: they
	// are four directions in world space, computed from the eye's own pose and
	// its own field of view, bilinearly interpolated exactly as the normals
	// upstream's own sky quad carries are. (The calibration that fixes which
	// corner is which is a test, not a comment: the suite renders the FIRST eye
	// through this same path and compares it with a mono render of that eye.)
	//
	// Order: 0 = bottom-left, 1 = bottom-right, 2 = top-left, 3 = top-right,
	// in the quad's own normalised device coordinates.
	uniform vec4 jahEyeCorner[4];
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
	// THE EYE IS THE INSTANCE'S LOW BIT, which is the same convention every
	// Hlms shader uses (`inVs_stereoDrawId & 0x01u`): the queue shifts the base
	// instance left by one and draws two instances, so the low bit is the eye
	// whether or not the device supports a base instance at all.
	const int eye = gl_InstanceIndex & 1;
	gl_ViewportIndex = eye;

	gl_Position = vec4( vertex.xy, rsDepthRange.y, 1.0 );

	if( eye == 0 )
	{
		// Ogre's own auto-params, i.e. this pass's own camera.
		const vec4 farPoint = autoInvViewProj * vec4( vertex.xy, rsDepthRange.y, 1.0 );
		outVs.cameraDir = farPoint.xyz / farPoint.w - autoCameraPos.xyz;
	}
	else
	{
		// The second eye's own four corners, picked by this vertex's own
		// normalised device coordinates (the quad spans -1..1 in both).
		const float u = vertex.x * 0.5 + 0.5;
		const float v = vertex.y * 0.5 + 0.5;
		outVs.cameraDir = mix( mix( jahEyeCorner[0].xyz, jahEyeCorner[1].xyz, u ),
							   mix( jahEyeCorner[2].xyz, jahEyeCorner[3].xyz, u ), v );
	}
}
