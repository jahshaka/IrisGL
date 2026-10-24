// THE PIXEL'S AREA LIGHT VISIBILITY, ITS LAW - ONE TEXT FOR BOTH PRODUCERS (PHOTON-7 stage 7: 7-A wrote it for the ray
// tier, 7-6 moved it here for the no-ray route): the ray tier's two jobs (rq_area_vis.comp, rq_area_vis_resolve.comp,
// through glslang) and the no-ray route's (Jahshaka/ScreenAreaVis and Jahshaka/ScreenAreaVisResolve, the Hlms piece
// JahAreaVisLaw) call the two functions below; what differs is only HOW A STRATUM IS SEEN - one ray against the TLAS,
// or the voxel chain's own shadow march (LightInjection's jahInjectVisibility, on the world shells) - and how the
// images are bound, both through the includer's macros.
//
// THE TRACE (jahAreaVisTrace, one thread a trace texel): ONE STRATUM A FRAME a light a texel (the trace grid is the
// target over p.knobs.w, rounded up; a texel's ray leaves the top-left pixel of its block - a real surface the prepass
// drew, the sun contact's rule), the stratum advancing on the view's sequence (p.strata.y: the frame, the rest frame's
// own index at rest) offset by the receiver's own hash so neighbouring texels sample different strata; the jitter
// inside a stratum is the receiver's (the cards' key). The two sums are averaged OVER TIME SEPARATELY - the ratio of
// the means, never the mean of the ratios - per light in the channels (light i in channel i: the pass's forward LTC
// set, at most four, in HlmsPbs's own order - the host replicates its walk), each sample scaled by the squared
// distance to the light's centre (the ratio is scale-free; the scale keeps a far light's sums inside fp16's normal
// range).
// THE HISTORY: the previous frame's texel at the point's reprojection through the previous camera, accepted on THE
// SAME SURFACE only (its own point, rebuilt through the previous camera from its pixel and stored distance, on this
// point's plane with an agreeing normal: jahAreaSameSurface, the gather's plane test); a count-in-history blend (1/n while
// young) floored at 1 / n^2 (the mean of about one full set of strata). Off the previous picture, behind its camera,
// on another surface, a restart: this frame's sample whole.
// THE REST LAW (the gather's, word for word - R2): at rest (p.rest.x = the rest frame k >= 1) a TRUE MEAN of the rest
// frames' samples, restarted at the first (never seeded from the history), handed over from the moving history
// linearly over N = n^2 frames (p.rest.y), at k = N exactly the mean of every stratum once - after which the host holds:
// nothing is dispatched and the visibility stays what it is.
// TWO RATIOS (the spec's glossy-lobe rule, measured: gi.card_lamp_shadow (8) read 4.6 codes of a 0.3-rough metal's
// unoccluded specular between the diffuse-weighted visible fraction and the specular-weighted one - over a code): the
// DIFFUSE set's weight is the stratum's form factor (what the LTC diffuse integrates), the SPECULAR set's the GGX lobe
// at the pixel's own view and roughness, D G2 / ( 4 N.V ) times the emitter's cosine over r^2 (what the LTC specular
// integrates; F leaves the ratio), scaled by alpha^2 to keep a sharp lobe's peak inside fp16, its roughness floored at
// perceptual 0.1. Both over the same strata and the same visibility; the resolve hands both to the pass.
// THE RESOLVE (jahAreaVisResolve, one thread a full-resolution pixel): the trace's two temporal sums S and U of the
// 3 x 3 trace texels around it, weighed by a tent over the trace grid (one texel's reach) times THE SAME SURFACE (the
// texel's own point and normal - rebuilt from its pixel and stored distance - against the pixel's plane:
// jahAreaSameSurface; a joint-bilateral upsample - a penumbra never bleeds across a silhouette), filtered SEPARATELY and
// only then divided (Heitz et al. 2018: the ratio of the filtered sums, never the filter of the ratio) - for both sets:
// `jahAreaVis` (RG32_UINT) holds in x the diffuse visibility of light i in byte i (unorm8, packUnorm4x8) and in y the
// specular's. A light with no weight here (out of its range, no texel on this surface, no surface) reads 1 - the LTC
// term unchanged, which is then exactly zero or upstream's.
//
// Needs of the includer, first: jah_area_vis_params.glsl (the block `p` and its helpers), jah_area_strata.glsl and
// jah_reproject.glsl, then - for the trace - the macros
//   JAH_AREA_DEPTH( pix ) (the prepass's raw depth), JAH_AREA_NORMAL( pix ) (its view-space normal, encoded 0..1),
//   JAH_AREA_ALPHA( pix ) (the GGX alpha's packed value: the shadow-roughness target's y),
//   JAH_AREA_PREV_S / _U / _S2 / _U2 ( pc ) (vec4) and JAH_AREA_PREV_G( pc ) (uvec4) - the previous frame's history,
//   JAH_AREA_STORE_CUR( tc, S, U, S2, U2, G ) - this frame's,
//   JAH_AREA_REST_LOAD( tc, S, U, S2, U2 ) and JAH_AREA_REST_STORE( tc, S, U, S2, U2 ) - the rest means,
//   JAH_AREA_VISIBLE( originQ, dirQ, reachQ ) - a stratum's visibility in [0, 1] from originQ along dirQ for reachQ;
// and - for the resolve - JAH_AREA_DEPTH, JAH_AREA_NORMAL, JAH_AREA_CUR_S / _U / _S2 / _U2 ( t ) (vec4),
// JAH_AREA_CUR_G( t ) (uvec4) - this frame's history - and JAH_AREA_STORE_VIS( pix, words ) (uvec2).
#ifndef JAH_AREA_VIS_LAW_GLSL
#define JAH_AREA_VIS_LAW_GLSL

/// A light's visibility stops this far short of its rectangle: its own emitter is not its shadow (the cards' kLampEnd).
const float kAreaLampEnd = 0.05;
/// The count the history saturates at (the blend reads it only up to its floor's 1 / n^2).
const uint kAreaCountMax = 4096u;

#ifdef JAH_AREA_VISIBLE
/// THE STORE, ROUNDED STOCHASTICALLY TO HALF PRECISION (PHOTON_2_SPEC R1: EVERY fp16 running statistic is,
/// jah_half_stochastic.glsl): the four sums' history and their four rest means live in halves (the ray tier's rgba16f,
/// the no-ray route's packed pairs), and an EMA at its floor 1 / n^2 rounded to nearest stalls within n^2 / 2 half-ulps
/// of its mean (8 ulps at n 4, 18 at Medium's n 6: ~0.4 - 1.8 %; the stage 7 read's F5). The draw a hash of the texel,
/// the frame's stratum index and the statistic.
vec4 jahAreaStoreHalf( vec4 v, ivec2 tc, uint which )
{
	uint s = jahRestHash( uint( tc.x ) * 73856093u ^ uint( tc.y ) * 19349663u ^ uint( p.strata.y ) * 2654435761u ^
						  which * 83492791u );
	vec4 r;
	for( int c = 0; c < 4; ++c )
	{
		s = jahRestHash( s + uint( c ) );
		r[c] = jahHalfStochastic( v[c], float( s >> 8u ) * ( 1.0 / 16777216.0 ) );
	}
	return r;
}

void jahAreaVisTrace( ivec2 tc )
{
	const ivec2 traceRes = ivec2( p.resolution.xy );
	if( any( greaterThanEqual( tc, traceRes ) ) )
		return;
	const ivec2 fullRes = ivec2( p.resolution.zw );
	const int divisor = max( int( p.knobs.w ), 1 );
	const ivec2 pix = min( tc * divisor, fullRes - 1 );
	const vec2 uv = ( vec2( pix ) + 0.5 ) / vec2( fullRes );

	const float rawDepth = JAH_AREA_DEPTH( pix );
	vec3 normalVS = JAH_AREA_NORMAL( pix ) * 2.0 - 1.0;
	if( !( rawDepth > 0.0 && rawDepth < 1.0 && dot( normalVS, normalVS ) > 1e-6 ) )
	{
		// no surface: no sum, no history (a zero surface word fails every test)
		JAH_AREA_STORE_CUR( tc, vec4( 0.0 ), vec4( 0.0 ), vec4( 0.0 ), vec4( 0.0 ), uvec4( 0u ) );
		if( p.rest.x >= 1.0 )
			JAH_AREA_REST_STORE( tc, vec4( 0.0 ), vec4( 0.0 ), vec4( 0.0 ), vec4( 0.0 ) );
		return;
	}
	normalVS = normalize( normalVS );
	const vec3 N = normalize( p.viewAxisX.xyz * normalVS.x + p.viewAxisY.xyz * normalVS.y + p.viewAxisZ.xyz * normalVS.z );
	const float dist = jahAreaViewDistance( rawDepth );
	const vec3 P = jahAreaWorldAt( uv, dist );
	// THE CLEARANCE (the producer's: the ray tier's two full-resolution footprints at the point's depth, the floor a
	// millimetre, taken along each ray - jahAreaRayStart; the no-ray route's the voxel march's own start bias, a cell
	// of cascade 0, taken along the NORMAL - JAH_AREA_LIFT_ALONG_NORMAL, below)
	const float lift = max( p.camPos.w < 0.5 ? p.knobs.x : p.knobs.x * dist, p.knobs.y );

	// ---- THIS FRAME'S SAMPLE: one stratum of each light ------------------------------------------------------------
	const uint n = clamp( uint( p.strata.x + 0.5 ), 1u, kAreaStrataMax );
	const uint key = jahAreaStrataKey( P );
	const uint k = ( uint( p.strata.y ) + jahAreaPcg( key ^ 0x9E3779B9u ) ) % ( n * n );
	vec4 sampleS = vec4( 0.0 ), sampleU = vec4( 0.0 ), sampleS2 = vec4( 0.0 ), sampleU2 = vec4( 0.0 );
	// the specular set's lobe: the pixel's view and roughness (floored at perceptual 0.1: alpha 0.01)
	const float alpha = max( JAH_AREA_ALPHA( pix ) * 0.999 + 0.001, 0.01 );
	const float a2 = alpha * alpha;
	const vec3 Vw = p.camPos.w < 0.5 ? -p.fwd.xyz : normalize( p.camPos.xyz - P );
	const float NoV = max( dot( N, Vw ), 1e-4 );
	const int count = clamp( int( p.lights.x + 0.5 ), 0, kAreaVisLights );
	for( int i = 0; i < kAreaVisLights; ++i )
	{
		if( i >= count )
			break;
		const vec3 c0 = p.light[i].c0.xyz, eu = p.light[i].eu.xyz, ev = p.light[i].ev.xyz;
		const vec3 centre = c0 + 0.5 * ( eu + ev );
		const float dc2 = dot( centre - P, centre - P );
		if( dc2 > p.light[i].eu.w * p.light[i].eu.w )
			continue;   // past the light's range: it lights nothing here (the pass's window is zero)
		vec3 wq, originQ;
		float clr2, reachQ;
		const float f = jahAreaStratumAt( P, N, N, lift, c0, eu, ev, p.light[i].emit.xyz, p.light[i].c0.w != 0.0, n, k,
										  key, kAreaLampEnd, wq, clr2, originQ, reachQ );
		vec3 wv = wq;
#ifdef JAH_AREA_LIFT_ALONG_NORMAL
		// THE NO-RAY ROUTE'S START, the cones' convention (jahConeStart): a cell along the NORMAL, the march from there
		// to the stratum's own point. Along the ray (a cell over the cosine: 2 cells at 60 degrees, ~6 at 80) a grazing
		// ray started past a wall standing at the point's foot, and an area light behind the wall reached the floor
		// along its base (PHOTON-7 stage 7's read, F6; gi.area_contact). The stratum's weights stay the receiver's.
		if( f > 0.0 )
		{
			const vec3 Q = P + wq * ( reachQ + jahAreaRayStart( N, wq, lift ) + kAreaLampEnd );
			originQ = P + N * lift;
			const vec3 dq = Q - originQ;
			const float dl = length( dq );
			wv = dq / max( dl, 1e-6 );
			reachQ = dl - kAreaLampEnd;
		}
#endif
		const float V = f > 0.0 && reachQ > 0.0 ? JAH_AREA_VISIBLE( originQ, wv, reachQ ) : 1.0;
		sampleS[i] = f * V * dc2;
		sampleU[i] = f * dc2;
		if( f > 0.0 )
		{
			// the GGX lobe's weight of the stratum (the head): D G2 / ( 4 N.V ) x cl / r^2, times alpha^2
			const float NoL = max( dot( N, wq ), 1e-4 );
			const vec3 hv = normalize( Vw + wq );
			const float NoH = max( dot( N, hv ), 0.0 );
			const float dd = NoH * NoH * ( a2 - 1.0 ) + 1.0;
			const float Da2 = a2 * a2 / ( 3.14159265 * dd * dd );
			const float G2 = 2.0 * NoL * NoV /
							 ( NoV * sqrt( a2 + ( 1.0 - a2 ) * NoL * NoL ) + NoL * sqrt( a2 + ( 1.0 - a2 ) * NoV * NoV ) );
			const float w2 = Da2 * G2 / ( 4.0 * NoV ) * clr2 * dc2;
			sampleS2[i] = w2 * V;
			sampleU2[i] = w2;
		}
	}

	// ---- THE HISTORY ------------------------------------------------------------------------------------------------
	vec4 prevS = vec4( 0.0 ), prevU = vec4( 0.0 ), prevS2 = vec4( 0.0 ), prevU2 = vec4( 0.0 );
	uint prevCount = 0u;
	if( p.strata.w >= 1.0 )
	{
		vec2 was;
		float z;
		if( jahReproject( P, p.prevCamPos.xyz, p.prevCamPos.w, p.prevRayTL.xyz, p.prevRayRight.xyz, p.prevRayDown.xyz,
						  p.prevFwd.xyz, was, z ) == 0 )
		{
			const ivec2 pc = clamp( ivec2( was * vec2( traceRes ) ), ivec2( 0 ), traceRes - 1 );
			const uvec4 g = JAH_AREA_PREV_G( pc );
			const float wasDist = uintBitsToFloat( g.x );
			// the texel's own point: its pixel (the block's top-left) through the previous camera at its distance
			const vec2 wasUv = ( vec2( min( pc * divisor, fullRes - 1 ) ) + 0.5 ) / vec2( fullRes );
			if( g.y > 0u && wasDist > 0.0 &&
				jahAreaSameSurface( jahAreaPrevWorldAt( wasUv, wasDist ), jahAreaUnpackNormal( g.z ), P, N, dist ) > 0.5 )
			{
				prevS = JAH_AREA_PREV_S( pc );
				prevU = JAH_AREA_PREV_U( pc );
				prevS2 = JAH_AREA_PREV_S2( pc );
				prevU2 = JAH_AREA_PREV_U2( pc );
				prevCount = g.y;
			}
		}
	}
	const float blend = prevCount > 0u ? max( 1.0 / ( float( prevCount ) + 1.0 ), p.strata.z ) : 1.0;
	vec4 meanS = mix( prevS, sampleS, blend ), meanU = mix( prevU, sampleU, blend );
	vec4 meanS2 = mix( prevS2, sampleS2, blend ), meanU2 = mix( prevU2, sampleU2, blend );
	// ---- AT REST, THE REST MEAN (R2) ---------------------------------------------------------------------------------
	const float restK = p.rest.x;
	if( restK >= 1.0 )
	{
		// (the first rest frame takes its sample whole, never the stored mean: R2)
		const bool first = restK < 1.5;
		vec4 rS = sampleS, rU = sampleU, rS2 = sampleS2, rU2 = sampleU2;
		if( !first )
		{
			vec4 oS, oU, oS2, oU2;
			JAH_AREA_REST_LOAD( tc, oS, oU, oS2, oU2 );
			rS = mix( oS, sampleS, 1.0 / restK );
			rU = mix( oU, sampleU, 1.0 / restK );
			rS2 = mix( oS2, sampleS2, 1.0 / restK );
			rU2 = mix( oU2, sampleU2, 1.0 / restK );
		}
		rS = jahAreaStoreHalf( rS, tc, 4u );
		rU = jahAreaStoreHalf( rU, tc, 5u );
		rS2 = jahAreaStoreHalf( rS2, tc, 6u );
		rU2 = jahAreaStoreHalf( rU2, tc, 7u );
		JAH_AREA_REST_STORE( tc, rS, rU, rS2, rU2 );
		const float h = clamp( ( restK - 1.0 ) / max( p.rest.y - 1.0, 1.0 ), 0.0, 1.0 );
		meanS = mix( mix( prevS, rS, blend ), rS, h );
		meanU = mix( mix( prevU, rU, blend ), rU, h );
		meanS2 = mix( mix( prevS2, rS2, blend ), rS2, h );
		meanU2 = mix( mix( prevU2, rU2, blend ), rU2, h );
	}
	meanS = jahAreaStoreHalf( meanS, tc, 0u );
	meanU = jahAreaStoreHalf( meanU, tc, 1u );
	meanS2 = jahAreaStoreHalf( meanS2, tc, 2u );
	meanU2 = jahAreaStoreHalf( meanU2, tc, 3u );
	JAH_AREA_STORE_CUR( tc, meanS, meanU, meanS2, meanU2,
						uvec4( floatBitsToUint( dist ), min( prevCount + 1u, kAreaCountMax ), jahAreaPackNormal( N ), 0u ) );
}
#endif

#ifdef JAH_AREA_STORE_VIS
void jahAreaVisResolve( ivec2 pix )
{
	const ivec2 fullRes = ivec2( p.resolution.zw );
	if( any( greaterThanEqual( pix, fullRes ) ) )
		return;
	const float rawDepth = JAH_AREA_DEPTH( pix );
	vec3 normalVS = JAH_AREA_NORMAL( pix ) * 2.0 - 1.0;
	if( !( rawDepth > 0.0 && rawDepth < 1.0 && dot( normalVS, normalVS ) > 1e-6 ) )
	{
		JAH_AREA_STORE_VIS( pix, uvec2( packUnorm4x8( vec4( 1.0 ) ), packUnorm4x8( vec4( 1.0 ) ) ) );
		return;
	}
	normalVS = normalize( normalVS );
	const vec3 Np = normalize( p.viewAxisX.xyz * normalVS.x + p.viewAxisY.xyz * normalVS.y + p.viewAxisZ.xyz * normalVS.z );
	const float dist = jahAreaViewDistance( rawDepth );
	const vec3 Pp = jahAreaWorldAt( ( vec2( pix ) + 0.5 ) / vec2( fullRes ), dist );
	const ivec2 traceRes = ivec2( p.resolution.xy );
	const float divisor = max( p.knobs.w, 1.0 );
	// the pixel's position on the trace grid (a texel's ray left its block's top-left pixel)
	const vec2 g = ( vec2( pix ) + 0.5 ) / divisor - 0.5 / divisor;
	const ivec2 c = ivec2( floor( g + 0.5 ) );
	vec4 S = vec4( 0.0 ), U = vec4( 0.0 ), S2 = vec4( 0.0 ), U2 = vec4( 0.0 );
	for( int dy = -1; dy <= 1; ++dy )
		for( int dx = -1; dx <= 1; ++dx )
		{
			const ivec2 t = c + ivec2( dx, dy );
			if( any( lessThan( t, ivec2( 0 ) ) ) || any( greaterThanEqual( t, traceRes ) ) )
				continue;
			const uvec4 gw = JAH_AREA_CUR_G( t );
			const float td = uintBitsToFloat( gw.x );
			if( gw.y == 0u || !( td > 0.0 ) )
				continue;
			const vec2 tUv = ( vec2( min( t * int( divisor ), fullRes - 1 ) ) + 0.5 ) / vec2( fullRes );
			const float same = jahAreaSameSurface( jahAreaWorldAt( tUv, td ), jahAreaUnpackNormal( gw.z ), Pp, Np, dist );
			const vec2 o = abs( vec2( t ) - g );
			const float w = max( 1.5 - o.x, 0.0 ) * max( 1.5 - o.y, 0.0 ) * same;
			if( !( w > 0.0 ) )
				continue;
			S += JAH_AREA_CUR_S( t ) * w;
			U += JAH_AREA_CUR_U( t ) * w;
			S2 += JAH_AREA_CUR_S2( t ) * w;
			U2 += JAH_AREA_CUR_U2( t ) * w;
		}
	vec4 vis = vec4( 1.0 ), visS = vec4( 1.0 );
	for( int i = 0; i < kAreaVisLights; ++i )
	{
		if( U[i] > 0.0 )
			vis[i] = clamp( S[i] / U[i], 0.0, 1.0 );
		if( U2[i] > 0.0 )
			visS[i] = clamp( S2[i] / U2[i], 0.0, 1.0 );
	}
	JAH_AREA_STORE_VIS( pix, uvec2( packUnorm4x8( vis ), packUnorm4x8( visS ) ) );
}
#endif

#endif
