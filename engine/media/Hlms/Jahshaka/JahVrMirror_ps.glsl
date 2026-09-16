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

vulkan_layout( ogre_t0 ) uniform texture2D sourceTexture;

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
	const vec2 uv = inPs.uv0 * mirrorUv.xy + mirrorUv.zw;
	fragColour = vec4( texture( vkSampler2D( sourceTexture, bilinearSampler ), uv ).xyz, 1.0 );
}
