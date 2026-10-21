// Jahshaka — ATOM-8: THE ONE DEPTH TEST of a box against the farthest-depth pyramid, shared by the instance test
// (JahCullTest_cs.glsl) and the cluster cut (JahCullCut_cs.glsl, JahCullCutLate_cs.glsl). ONE implementation, so the
// instance's box and a cluster's box are judged by the same arithmetic and the same mip rule.
//
// THE BOX: eight corners through the cull matrix (rows); any corner at or behind w = 0 = NOT TESTABLE (the box
// crosses the near plane: its rectangle is not a rectangle - kept, as Nanite keeps it). The rectangle in NDC, the
// box's NEAREST device depth (reverse-Z aware).
// THE TEXELS: NDC to the viewport's rectangle in mip 0 (y inverted: the pyramid's row 0 is the top - the caller's
// cull matrix makes that true, OgreAtomIdPass.cpp cullMatrixOf).
// THE MIP (ATOM-8 correction 2, the owner's rule): the level where the rectangle spans AT MOST 4 texels per axis -
// at most 5 texel indices a side when misaligned, every one fetched (<= 25 reads) - the receiver test's shape and
// Nanite's 4 x 4 footprint. The FARTHEST depth under the rectangle (each texel holds its footprint's farthest).
// OCCLUDED when even the nearest point of the box is farther than that, beyond a relative margin of 2 to the minus 16
// of its depth (a surface ON the occluder's depth stays drawn: the test may keep what is hidden, never drop what is
// seen). Sky (the far plane) under the rectangle never occludes.
// THE PIXEL CENTRE (ATOM-8; Nanite's bOverlapsPixelCenter): a rectangle, in the viewport's pixels through THIS
// frame's matrix, that covers no pixel centre rasterises no fragment (one sample a pixel, no conservative
// rasterisation) - culled outright, exactly. The rectangle is widened by 1/256 of a pixel against rounding.
//
// The includer declares hzbTexture under the property cull_hzb (both jobs name their permutation so). No Hlms
// directive mark appears in any comment of this file.
@piece( JahHzbTest )
	vec4 jahHzbClip( vec4 r0, vec4 r1, vec4 r2, vec4 r3, vec3 p )
	{
		vec4 h = vec4( p, 1.0 );
		return vec4( dot( r0, h ), dot( r1, h ), dot( r2, h ), dot( r3, h ) );
	}

	// The box's NDC rectangle and nearest depth; false = not testable (a corner at or behind the eye plane).
	bool jahHzbBoxRect( vec3 bMin, vec3 bMax, vec4 r0, vec4 r1, vec4 r2, vec4 r3, bool reverseZ,
						out vec2 lo, out vec2 hi, out float nearest )
	{
		lo = vec2( 1.0e30 );
		hi = vec2( -1.0e30 );
		nearest = reverseZ ? -1.0e30 : 1.0e30;
		for( int c = 0; c < 8; ++c )
		{
			vec3 p = vec3( ( c & 1 ) != 0 ? bMax.x : bMin.x,
						   ( c & 2 ) != 0 ? bMax.y : bMin.y,
						   ( c & 4 ) != 0 ? bMax.z : bMin.z );
			vec4 clip = jahHzbClip( r0, r1, r2, r3, p );
			if( clip.w <= 0.0 )
				return false;
			vec3 ndc = clip.xyz / clip.w;
			lo = min( lo, ndc.xy );
			hi = max( hi, ndc.xy );
			nearest = reverseZ ? max( nearest, ndc.z ) : min( nearest, ndc.z );
		}
		return true;
	}

	// NDC to the texels (pixels) of the viewport's rectangle in a target of `size`: rect = (x0, y0, w, h), w 0 = the
	// whole target. Clamped to the rectangle.
	void jahHzbTexels( vec2 lo, vec2 hi, uvec4 rect, vec2 size, out vec2 t0, out vec2 t1 )
	{
		vec2 org = vec2( 0.0 );
		vec2 ext = size;
		if( rect.z != 0u )
		{
			org = vec2( rect.xy );
			ext = vec2( rect.zw );
		}
		t0 = org + vec2( ( lo.x * 0.5 + 0.5 ) * ext.x, ( 0.5 - hi.y * 0.5 ) * ext.y );
		t1 = org + vec2( ( hi.x * 0.5 + 0.5 ) * ext.x, ( 0.5 - lo.y * 0.5 ) * ext.y );
		t0 = max( t0, org );
		t1 = min( t1, org + ext );
	}

	// A PIXEL CENTRE (i + 0.5, j + 0.5) inside the closed rectangle [t0, t1], widened by 1/256 of a pixel.
	bool jahHzbCoversPixelCentre( vec2 t0, vec2 t1 )
	{
		vec2 a = ceil( t0 - vec2( 0.5 + 1.0 / 256.0 ) );
		vec2 b = floor( t1 - vec2( 0.5 - 1.0 / 256.0 ) );
		return all( greaterThanEqual( b, a ) );
	}

	bool jahHzbOccluded( float nearest, float farthestUnder, bool reverseZ )
	{
		float margin = abs( nearest ) * 1.52587890625e-5;
		return reverseZ ? ( nearest + margin < farthestUnder ) : ( nearest - margin > farthestUnder );
	}
@property( cull_hzb )
	// The farthest depth under [t0, t1] (mip-0 texels) at the 4-texel mip; `hzb` = (levels, width, height, reverseZ).
	float jahHzbFarthestUnder( vec2 t0, vec2 t1, uvec4 hzb, out int mip )
	{
		vec2 rect = max( t1 - t0, vec2( 0.0 ) );
		float widest = max( rect.x, rect.y );
		mip = clamp( int( ceil( log2( max( widest, 1.0 ) / 4.0 ) ) ), 0, int( hzb.x ) - 1 );
		ivec2 mipSize = ivec2( max( ivec2( hzb.yz ) >> mip, ivec2( 1 ) ) );
		ivec2 a = clamp( ivec2( floor( t0 / float( 1 << mip ) ) ), ivec2( 0 ), mipSize - 1 );
		ivec2 b = clamp( ivec2( floor( t1 / float( 1 << mip ) ) ), ivec2( 0 ), mipSize - 1 );
		float farthestUnder = hzb.w != 0u ? 1.0e30 : -1.0e30;
		for( int yy = a.y; yy <= b.y; ++yy )
			for( int xx = a.x; xx <= b.x; ++xx )
			{
				float d = texelFetch( hzbTexture, ivec2( xx, yy ), mip ).x;
				farthestUnder = hzb.w != 0u ? min( farthestUnder, d ) : max( farthestUnder, d );
			}
		return farthestUnder;
	}
@end
@end
