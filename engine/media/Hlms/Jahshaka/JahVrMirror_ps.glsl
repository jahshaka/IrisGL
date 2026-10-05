// THE VR MIRROR'S ONE SHADER (SPECS/VR_SPEC.md §4.3).
//
// A rectangle of the both-eyes VR target, stretched over the destination. The
// sample is BILINEAR and not a texelFetch (the looks' rule) precisely because
// this is a resize: the eye target is 2 x 1007 px tall on the simulated runtime
// and 2 x 2376 on a Quest Pro, and the window it lands in is neither.
//
// There is no colour maths here at all, deliberately: whatever the chain wrote
// for the headset is what the desktop shows, so "the mirror disagrees with the
// headset" can never be this file's doing.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2DArray sourceTexture;

vulkan( layout( ogre_s0 ) uniform sampler bilinearSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 mirrorUv;	// xy = uv scale, zw = uv offset
vulkan( }; )

vulkan_layout( location = 0 )
out vec4 fragColour;

void main()
{
	// THE SOURCE IS LAYERED (LAYERED-STEREO-1): one eye per layer of a two-layer
	// array. `uv` is in the PAIR's space (the eyes side by side, as mirrorUv
	// has always addressed them), so its whole part along x is the layer and
	// its fraction the eye's own u.
	const vec2 uv = inPs.uv0 * mirrorUv.xy + mirrorUv.zw;
	const float pairU = uv.x * 2.0;
	const float layer = clamp( floor( pairU ), 0.0, 1.0 );
	const vec3 eyeUv = vec3( pairU - layer, uv.y, layer );
	fragColour = vec4( texture( vkSampler2DArray( sourceTexture, bilinearSampler ), eyeUv ).xyz, 1.0 );
}
