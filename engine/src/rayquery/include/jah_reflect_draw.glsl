// THE REFLECTION'S DRAW (PHOTON-7 stage 6, commit 6) - one text for the trace that draws a texel's direction, the
// resolve that re-derives a neighbour's (from its per-pixel seed: no direction is stored) and the passes that weigh a
// texel by its chance of the centre ray. Pure functions; nothing here reads a binding.
//
// THE MIRROR - ONE AVERAGED ESTIMATOR, A RAMP ON THE FOOTPRINT (PREFLIGHT_R section 4). f = the lobe's footprint at its
// hit in trace texels (jahReflectFootprint, the throw from the state); p( f ) = 1 - smoothstep( kCentreFull,
// kCentreNone, f ); the draw is the lobe's CENTRE RAY with probability p (the coin, same seed), else the GGX
// visible-normal sample. The temporal pass' memory follows it: MaxFrames( f ) = 1 + ( 12 - 1 ) ( 1 - p ) - a lobe under a
// texel takes its sample whole (the centre ray IS its integral: nothing to average, nothing to lag), a wide one Lumen's
// 12, between a continuous mixture. No flag, no hysteresis, no restart: p is continuous in f and f moves with the
// throw, never by a flip. (It replaces the fixed/stochastic switch with its hysteresis band and its restart, AB E6.)
// The G-buffer's floor (alpha 0.001, the packing's: a roughness-0 mirror) is always the centre ray.
#ifndef JAH_REFLECT_DRAW_GLSL
#define JAH_REFLECT_DRAW_GLSL

/// THE G-BUFFER'S FLOOR (fork 16d8e29d4 packs alpha over [0.001, 1]): an alpha this low IS a roughness-0 mirror - the
/// renderer's sharpest surface - and its draw is the lobe's centre at every throw.
const float kMirrorPackAlpha = 0.0011;
/// The ramp's two ends in footprints (the fixed estimator's hysteresis band before: its two edges, now a ramp).
const float kCentreFull = 0.8, kCentreNone = 1.25;

/// PCG's mixing step: the sequences' one hash.
uint jahReflectPcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
/// THE LOBE SAMPLE'S PAIR: the pixel and the frame (the rest's sequence index at rest) - the sequence's ONLY inputs, so
/// a still camera on a still scene averages a different ray every frame.
vec2 jahReflectSample2( ivec2 pix, uint frame )
{
	const uint h = jahReflectPcg( uint( pix.x ) * 1973u + uint( pix.y ) * 9277u + frame * 26699u );
	return vec2( float( h & 0xFFFFu ) / 65536.0, float( h >> 16u ) / 65536.0 );
}
/// THE CENTRE RAY'S COIN: a third draw of the same seed, its own stream.
float jahReflectCoin( ivec2 pix, uint frame )
{
	return float( jahReflectPcg( uint( pix.x ) * 6113u + uint( pix.y ) * 49157u + frame * 92821u + 0x6A09E667u ) &
				  0xFFFFFFu ) * ( 1.0 / 16777216.0 );
}

/// A TANGENT FRAME AROUND N (Duff et al., branchless).
void jahReflectFrame( vec3 N, out vec3 T, out vec3 B )
{
	const float sg = N.z >= 0.0 ? 1.0 : -1.0;
	const float a0 = -1.0 / ( sg + N.z );
	const float b0 = N.x * N.y * a0;
	T = vec3( 1.0 + sg * N.x * N.x * a0, sg * b0, -sg * N.x );
	B = vec3( b0, sg + N.y * N.y * a0, -N.y );
}

/// THE GGX VISIBLE-NORMAL SAMPLE (Heitz, "Sampling the GGX Distribution of Visible Normals", JCGT 2018): a half vector
/// in the surface's tangent frame given the view direction in that frame. Sampling the VNDF cancels D and the masking
/// term, leaving the estimator weight F G2 / G1 - NEITHER applied: this pass writes a radiance HlmsPbs multiplies by the
/// surface's own BRDF (F here would apply it twice); G2 / G1 is 1 at normal incidence and falls at grazing, the
/// approximation the probe path makes too. At alpha 0 it returns the normal exactly.
vec3 jahGgxVndf( vec3 Ve, float alpha, vec2 u )
{
	const vec3 Vh = normalize( vec3( alpha * Ve.x, alpha * Ve.y, Ve.z ) );
	const float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
	const vec3 T1 = lensq > 0.0 ? vec3( -Vh.y, Vh.x, 0.0 ) * inversesqrt( lensq ) : vec3( 1.0, 0.0, 0.0 );
	const vec3 T2 = cross( Vh, T1 );
	const float r = sqrt( u.x );
	const float phi = 6.2831853 * u.y;
	const float t1 = r * cos( phi );
	float t2 = r * sin( phi );
	const float s = 0.5 * ( 1.0 + Vh.z );
	t2 = ( 1.0 - s ) * sqrt( 1.0 - t1 * t1 ) + s * t2;
	const vec3 Nh = t1 * T1 + t2 * T2 + sqrt( max( 0.0, 1.0 - t1 * t1 - t2 * t2 ) ) * Vh;
	return normalize( vec3( alpha * Nh.x, alpha * Nh.y, max( 0.0, Nh.z ) ) );
}

/// THE DENSITY OF A REFLECTED DIRECTION L UNDER THAT SAMPLE, in solid angle: D( H ) G1( V ) / ( 4 N.V ) (the VNDF's
/// D G1 max( V.H, 0 ) / N.V times the reflection's Jacobian 1 / ( 4 V.H )); 0 where L leaves the surface's hemisphere.
float jahGgxVndfPdf( vec3 N, vec3 V, vec3 L, float alpha )
{
	const float NdotV = dot( N, V ), NdotL = dot( N, L );
	if( !( NdotV > 0.0 ) || !( NdotL > 0.0 ) )
		return 0.0;
	const vec3 H = normalize( V + L );
	const float NdotH = max( dot( N, H ), 0.0 );
	const float a2 = alpha * alpha;
	const float d = NdotH * NdotH * ( a2 - 1.0 ) + 1.0;
	const float D = a2 / max( 3.14159265 * d * d, 1e-30 );
	const float G1 = 2.0 * NdotV / ( NdotV + sqrt( a2 + ( 1.0 - a2 ) * NdotV * NdotV ) );
	return D * G1 / ( 4.0 * NdotV );
}

/// THE FOLDED DRAW'S DENSITY q( L ) (the trace folds a draw below the geometric plane g about it, READ_A W4): pv( L ) +
/// pv( R_g L ) above g, 0 below - the density the resolve reweights by and the NEE's balance weight takes (one text).
float jahReflectFoldedPdf( vec3 N, vec3 g, vec3 V, vec3 L, float alpha )
{
	const float a = dot( L, g );
	if( !( a > 0.0 ) )
		return 0.0;
	const float al = max( alpha, 1e-4 );
	return jahGgxVndfPdf( N, V, L, al ) + jahGgxVndfPdf( N, V, L - 2.0 * a * g, al );
}

/// A UNIFORM POINT OF THE UNIT DISK (Shirley-Chiu's concentric map): the resolve's and the spatial filter's taps.
vec2 jahConcentricDisk( vec2 u )
{
	const vec2 o = 2.0 * u - 1.0;
	if( o.x == 0.0 && o.y == 0.0 )
		return vec2( 0.0 );
	float r, th;
	if( abs( o.x ) > abs( o.y ) ) { r = o.x; th = 0.78539816 * ( o.y / o.x ); }
	else { r = o.y; th = 1.57079633 - 0.78539816 * ( o.x / o.y ); }
	return r * vec2( cos( th ), sin( th ) );
}

/// THE LOBE'S FOOTPRINT AT ITS HIT, IN TRACE TEXELS - the lobe's angular width at the hit, ~4 alpha, over the throw t,
/// against one trace texel's width at the reflected image (the camera's pixel angle over (eye distance + t),
/// perspective; the image plane's texel, orthographic). 0 = no throw known.
float jahReflectFootprint( float lobeAlpha, float throwT, float eyeDist, float texelAngle, bool perspective )
{
	if( !( throwT > 0.0 ) )
		return 0.0;
	const float texelAtImage = perspective ? texelAngle * ( eyeDist + throwT ) : texelAngle;
	return 4.0 * lobeAlpha * throwT / max( texelAtImage, 1e-9 );
}
/// THE CENTRE RAY'S CHANCE p( f ) for a texel of GGX alpha `lobeAlpha` whose state's throw is `throwT`: 1 at the
/// packing floor, 0 with no throw known (the lobe's width is unknown: the sample stays stochastic).
float jahReflectCentreChance( float lobeAlpha, float throwT, float eyeDist, float texelAngle, bool perspective )
{
	if( lobeAlpha <= kMirrorPackAlpha )
		return 1.0;
	if( !( throwT > 0.0 ) )
		return 0.0;
	return 1.0 - smoothstep( kCentreFull, kCentreNone,
							 jahReflectFootprint( lobeAlpha, throwT, eyeDist, texelAngle, perspective ) );
}
/// THE MEMORY'S LENGTH at that chance: 1 + ( maxFrames - 1 ) ( 1 - p ) - one frame for the centre ray, Lumen's
/// MaxFrames for a stochastic lobe.
float jahReflectMaxFrames( float centreChance, float maxFrames )
{
	return 1.0 + ( maxFrames - 1.0 ) * ( 1.0 - clamp( centreChance, 0.0, 1.0 ) );
}

/// ONE TEXEL'S DRAW: the direction L for a surface of shading normal N seen along V (both unit), GGX alpha `alpha`,
/// at the seed's pixel and frame, with the centre ray's chance `centreChance`. `centre` = the draw was the lobe's centre
/// (the mirror direction); `vndf` = L is a VNDF draw whose density is jahGgxVndfPdf (false for the centre ray and for
/// the mirror fallback - a lobe sample that left the surface takes the mirror direction).
vec3 jahReflectDraw( vec3 N, vec3 V, float alpha, ivec2 seedPix, uint frame, float centreChance, out bool centre,
					 out bool vndf )
{
	centre = jahReflectCoin( seedPix, frame ) < centreChance;
	vndf = !centre;
	if( centre )
		return reflect( -V, N );
	vec3 T, B;
	jahReflectFrame( N, T, B );
	const vec3 Ve = vec3( dot( V, T ), dot( V, B ), dot( V, N ) );
	const vec3 Ht = jahGgxVndf( Ve, max( alpha, 1e-4 ), jahReflectSample2( seedPix, frame ) );
	const vec3 H = normalize( Ht.x * T + Ht.y * B + Ht.z * N );
	vec3 L = reflect( -V, H );
	if( dot( L, N ) <= 0.0 )
	{
		vndf = false;
		L = reflect( -V, N );		// the lobe left the surface: take the mirror
	}
	return L;
}

#endif
