// THE GRID, ANALYTICALLY (GRID-2; Types.h GridDesc; OgreGrid.cpp).
//
// For every pixel: intersect the camera ray with the grid plane (through the
// world origin; `gridNormal`), take the plane point p, and read its two in-plane
// coordinates through `gridU` / `gridV`. A line is where a coordinate is within
// half a THICKNESS of a multiple of the spacing, measured in PIXELS through the
// coordinate's screen-space derivative (fwidth), so the line is `thicknessPx`
// wide wherever it stands and its edge is anti-aliased by the same footprint.
// Minor lines fade out where a cell is narrower than a few pixels (the moiré a
// grid makes at grazing angles is exactly the regime where fwidth exceeds the
// cell — an aliased grid's "solid fill" is that term saturating); major lines
// the same at their own period; everything fades to nothing at `fadeDistance`
// along the ray.
//
// THE DEPTH is the plane point's own, through the real view-projection, with a
// one-part-in-ten-thousand RELATIVE bias toward the camera (2 cm at 100 m, a
// tenth of a millimetre at 1 m) so a floor lying exactly on the plane draws
// UNDER the grid without z-fighting while a box standing on the floor covers
// it. Relative, because the chain runs reverse-Z and a far depth is a small
// number: an absolute bias would be a metre at 100 m. The bias's direction is
// read from rs_depth_range (near in .x), so it holds under either convention.
#version ogre_glsl_ver_330

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec2 rsDepthRange;
	uniform mat4 viewProj;
	uniform vec4 cameraPos;
	uniform vec4 gridNormal;      // xyz: the plane's normal (unit)
	uniform vec4 gridU;           // xyz: the first in-plane axis
	uniform vec4 gridV;           // xyz: the second in-plane axis
	uniform vec4 gridParams;      // x: spacing, y: majorEvery, z: thicknessPx, w: fadeDistance
	uniform vec4 minorColour;
	uniform vec4 majorColour;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec3 cameraDir;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

// The line term for one period: 1 on the line's centre, 0 a thickness away,
// anti-aliased by the coordinate's pixel footprint.
float lineTerm( vec2 coord, vec2 footprint, float thicknessPx )
{
	const vec2 d = abs( fract( coord - 0.5 ) - 0.5 ) / max( footprint, vec2( 1e-6 ) );
	const float px = min( d.x, d.y );   // distance to the nearest line, in pixels
	return 1.0 - smoothstep( 0.5 * thicknessPx - 0.5, 0.5 * thicknessPx + 0.5, px );
}

void main()
{
	const vec3 dir = normalize( inPs.cameraDir );
	const float denom = dot( gridNormal.xyz, dir );
	if( abs( denom ) < 1e-6 )
		discard;
	const float t = -dot( gridNormal.xyz, cameraPos.xyz ) / denom;
	if( t <= 0.0 )
		discard;
	const vec3 p = cameraPos.xyz + dir * t;

	const float spacing = gridParams.x;
	const float majorEvery = max( gridParams.y, 1.0 );
	const float thicknessPx = max( gridParams.z, 0.5 );
	const float fadeDistance = max( gridParams.w, 1.0 );

	const vec2 uv = vec2( dot( p, gridU.xyz ), dot( p, gridV.xyz ) );
	const vec2 minorCoord = uv / spacing;
	const vec2 majorCoord = minorCoord / majorEvery;
	const vec2 minorFw = fwidth( minorCoord );
	const vec2 majorFw = fwidth( majorCoord );

	// A cell narrower than ~4 px cannot show its lines: fade the period out
	// before its footprint saturates the line term.
	const float minorVis = 1.0 - smoothstep( 0.15, 0.35, max( minorFw.x, minorFw.y ) );
	const float majorVis = 1.0 - smoothstep( 0.15, 0.35, max( majorFw.x, majorFw.y ) );
	const float minor = lineTerm( minorCoord, minorFw, thicknessPx ) * minorVis;
	const float major = lineTerm( majorCoord, majorFw, thicknessPx ) * majorVis;

	const float fade = 1.0 - smoothstep( 0.6 * fadeDistance, fadeDistance, t );
	const float minorA = minorColour.a * minor;
	const float majorA = majorColour.a * major;
	const float a = max( minorA, majorA ) * fade;
	if( a <= 0.002 )
		discard;
	const vec3 rgb = mix( minorColour.rgb, majorColour.rgb, majorA / max( minorA, majorA ) );
	fragColour = vec4( rgb, a );

	const vec4 clip = viewProj * vec4( p, 1.0 );
	float depth = clip.z / clip.w;
	// Toward the camera by one part in ten thousand, whichever way "near" lies.
	depth *= ( rsDepthRange.x > rsDepthRange.y ) ? 1.0001 : 0.9999;
	gl_FragDepth = clamp( depth, min( rsDepthRange.x, rsDepthRange.y ), max( rsDepthRange.x, rsDepthRange.y ) );
}
