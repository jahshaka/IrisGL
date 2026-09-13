// THE SUN DISC (SPECS/SKY_LIGHT_SPEC.md §3).
//
// A disc of radiance around one world DIRECTION, drawn over the sky quad. The
// whole shader is one dot product and one smoothstep, because that is all a
// disc at infinity is: every camera ray whose angle to the sun is inside the
// sun's angular RADIUS is on the disc.
//
// THE EDGE. `sunDirection.w` is cos(radius) and the falloff runs from there to
// cos(radius * kEdge) — a limb a few percent of the radius wide, which is what
// stops a 0.53-degree disc from being a stair-stepped pentagon at 1080p (it is
// about eight pixels across on a 60-degree-vfov 1080p frame, so the edge has to
// be sub-pixel-smooth rather than merely anti-aliased).
//
// NO TEXTURE, NO PARAMETERS BEYOND THESE TWO. The sun's colour, its intensity
// and the overdrive that makes it clip white all arrive pre-multiplied in
// `sunColour` — the host owns what a sun is worth, the shader owns where it is.
#version ogre_glsl_ver_330

vulkan( layout( ogre_P0 ) uniform Params { )
	// xyz = unit vector from the scene TOWARDS the sun; w = cos(angular radius).
	uniform vec4 sunDirection;
	// rgb = the disc's radiance, already scaled by the host. a is unused.
	uniform vec4 sunColour;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec3 cameraDir;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

void main()
{
	const vec3 dir = normalize( inPs.cameraDir );
	const float cosAngle = dot( dir, sunDirection.xyz );
	const float cosOuter = sunDirection.w;
	// The inner edge: cos of 88% of the radius. cos is monotone decreasing, so
	// "inside" is the LARGER cosine — hence smoothstep(outer, inner, x).
	const float cosInner = cos( acos( clamp( cosOuter, -1.0, 1.0 ) ) * 0.88 );
	const float disc = smoothstep( cosOuter, cosInner, cosAngle );
	// Additive blend: alpha is ignored by the pass, and writing the coverage
	// into rgb is what makes the limb fade instead of hard-clipping.
	fragColour = vec4( sunColour.rgb * disc, 1.0 );
}
