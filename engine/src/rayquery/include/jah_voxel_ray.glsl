// THE OCCUPANCY RAY OVER THE CASCADE CHAIN — the ray the world caches gather by (PHOTON-7 stage 10, 10-1; L9: NO
// LIGHT OF ANY TYPE PASSES A WALL; the preflight's option f1, spikes/voxel-leak-1/PREFLIGHT.md §4).
//
// WHY A RAY AND NOT A CONE OF APERTURE 0. The plane march (jah_voxel_march.glsl) reads, at level 0, the COVERAGE of
// the plane's own dominant axis and the lateral axes at the ray's crossing rate - a transparency by the fraction of a
// cell's face a surface covers, and by direction: a 1 m long, 0.4 m wall in the outer cascade's 1.875 m cells passed
// 0.44 of a ray aimed at its middle at normal incidence and 0.19-0.59 of one at 0-85 degrees (gi.voxel_ray_wall, red on
// the base; 10-0's P5 found the store marks every such cell). THIS RAY READS OCCUPANCY (the lead's ruling on P5,
// 2026-10-25): a cell the store marks occupied (the light volume's opacity c > 0, any geometry in it) is a hit FROM
// EVERY DIRECTION. What that costs is over-occlusion where a coarse cell is partly empty (a 1.875 m cell is as solid as
// its smallest object); what it buys is that no ray passes a wall at any angle in any cascade.
//
// THE HIT READS THE SIDE FACING THE RAY (the two-sided store, PHOTON-7 stage 7): the front where the voxel's canonical
// normal faces against the ray, the back where it faces along it, their mean edge-on - each un-premultiplied by the
// opacity (what the surface emits, not how much of the cell it fills). A one-sided voxel's back stores 0 (stage 10's
// one-sided write): a ray reaching a solid's face from behind is inside the solid, and reads nothing.
//
// THE ORIGIN PLANE IS THE HEMISPHERE'S BOUNDARY, NEVER AN OCCLUDER (the march's rule, jah_voxel_sample.glsl): a ray
// that leaves a surface meets that surface's own coarse cells as it climbs - an open floor's ray at 15 degrees is
// still inside the floor's 1.875 m cells at the third hop - so a cell whose ONLY surfaces are the origin's (along the
// axis n the surface's normal is dominant on, every face within one of this cascade's cells of the origin's depth or
// behind it, and no face on another axis) is not a hit. Anything else in the cell - a wall standing on the floor, a
// chair's leg - is. A surface that is not axis-aligned keeps its own cells on the other axes: the stated residual.
//
// THE SHELLS (the cone march's and LightInjection's jahInjectVisibility's, PHOTON-7 stages 1 and 5): cascade j reads
// the ray over the distances [D_(j-1), D_j) from the start, D_j the sum of eight cells of every cascade up to j, and
// hands over ON THE FIRST CELL PLANE OF CASCADE j + 1 PAST D_j (the next cascade enters its first cell at a face; a
// coarse cell entered mid-way would re-read what the fine cells read); a box that ends first hands over at its face
// (THE BAND: that ray is the placement's). A cascade whose box does not hold the point the ray has reached is passed
// over. THE LAST CASCADE READS TO A WORLD SHELL, `lastShellC0` (the caller's: a distance every placement's last box
// contains from every point the caches gather at - the chain's table says it), or to its box if that comes first.
//
// THE EXIT IS EARNED (the spec's "THE MISS IS EARNED"; Lumen's closed-box leak is a ray that ran out of distance and
// read the sky): the result says HOW the ray ended - a HIT; ESCAPED, the ray left the last cascade (its world shell,
// or its box in the band) and only then is it the sky's; EXHAUSTED, it ran out of steps or met a value that is not a
// number, and it is NO SAMPLE, never sky. (The DDA visits at most res.x + res.y + res.z + 3 cells of a box, so a
// finite ray never exhausts: the reason exists so that no caller can read a broken ray as an escape.)
//
// THE AT-SIGN RULE: this file becomes Hlms input (the piece JahVoxelRay), so no character of it may be the Hlms
// directive mark, comments included. It includes nothing; the caller inserts JahVoxelSample first (the finite test
// and the sample file's macros) and binds, besides the chain's (JAH_VOX_COUNT, JAH_VOX_MAX_CASCADES, JAH_VOX_INVRES,
// JAH_VOX_FROM_PREV_SCALE / _OFFSET), the level-0 reads JAH_VOX_SAMPLE_ISO, _BACK, _NRM, _COVP, _COVN, _POSP, _POSN.
// Every texture array is indexed by the loop's own counter, the same for every invocation still marching.

#ifndef JAH_VOXEL_RAY_GLSL
#define JAH_VOXEL_RAY_GLSL

#ifndef JAH_VOX_SAMPLE_NRM
#error "jah_voxel_ray.glsl: the hit reads the side facing the ray - bind JAH_VOX_SAMPLE_NRM (the voxeliser's normal)"
#endif
#ifndef JAH_VOX_SAMPLE_BACK
#error "jah_voxel_ray.glsl: the hit reads the side facing the ray - bind JAH_VOX_SAMPLE_BACK (level 0's back side)"
#endif

/// HOW A RAY ENDED.
const uint kJahRayExhausted = 0u;	///< out of steps, or not a number: NO SAMPLE
const uint kJahRayHit = 1u;			///< an occupied cell (not the origin's own)
const uint kJahRayEscaped = 2u;		///< it left the last cascade: the sky's

struct JahVoxelRayResult
{
	vec3 radiance;		///< a HIT: the side of the voxel facing the ray, un-premultiplied; else 0
	uint exitReason;	///< kJahRayHit / kJahRayEscaped / kJahRayExhausted
	int cascade;		///< the cascade it ended in
	float tC0;			///< from the start to the hit cell's entry (or to the exit), cascade 0's normalised units
	int steps;			///< the cells it read - its price
};

/// IS AN OCCUPIED CELL THE ORIGIN'S OWN SURFACE (the header)? Cell `iv` of cascade `c` at its centre `uvw`; `org` the
/// origin plane in this cascade's normalised space (jahConeOrigin's form: x the axis n, -1 none; y the surface's
/// coordinate along n; z the normal's sign along n). True only when the cell's every face is the origin's: no coverage
/// on another axis, some on n, and every half's surface on n within one cell of the origin's depth or behind it.
bool jahVoxelRayOriginOnly( int c, vec3 uvw, ivec3 iv, vec4 org, vec3 invRes )
{
	if( !( org.x >= 0.0 ) )
		return false;
	const int n = clamp( int( org.x ), 0, 2 );	// (clamped: a constant negative index is rejected by SPIR-V)
	const vec3 cP = JAH_VOX_SAMPLE_COVP( c, uvw, 0.0 ).xyz;
	const vec3 cN = JAH_VOX_SAMPLE_COVN( c, uvw, 0.0 ).xyz;
	float other = 0.0;
	for( int b = 0; b < 3; ++b )
	{
		if( b != n )
			other += cP[b] + cN[b];
	}
	if( other > 0.0 || !( cP[n] + cN[n] > 0.0 ) )
		return false;
	// level 0 stores O x f, f the faces' place IN THE CELL (jah_voxel_sample.glsl, jahVoxelPos0Centred)
	const float pP = JAH_VOX_SAMPLE_POSP( c, uvw, 0.0 )[n];
	const float pN = JAH_VOX_SAMPLE_POSN( c, uvw, 0.0 )[n];
	const float cell = invRes[n];
	if( cP[n] > 0.0 && ( ( pP / cP[n] + float( iv[n] ) ) * cell - org.y ) * org.z > cell )
		return false;
	if( cN[n] > 0.0 && ( ( pN / cN[n] + float( iv[n] ) ) * cell - org.y ) * org.z > cell )
		return false;
	return true;
}

/// THE RAY from `posLS0` (cascade 0's normalised space; a cache's caller starts it off its surface: jahConeStart) along
/// the unit `dirLS` (the same space; the chain's cascades are cubes, so it is every cascade's direction), with the
/// origin plane `origin` (jahConeOrigin; kJahVoxelNoOrigin for a ray in free space) and the last cascade's world shell
/// `lastShellC0` (cascade 0's normalised units from the start; 0 or less: its box alone).
JahVoxelRayResult jahVoxelRay( vec3 posLS0, vec3 dirLS, vec4 origin, float lastShellC0 )
{
	JahVoxelRayResult r;
	r.radiance = vec3( 0.0, 0.0, 0.0 );
	r.exitReason = kJahRayExhausted;
	r.cascade = 0;
	r.tC0 = 0.0;
	r.steps = 0;
	if( !finite3( posLS0 ) || !finite3( dirLS ) || !finite1( lastShellC0 ) || !finite1( origin.y ) )
		return r;
	vec3 posJ = posLS0;		// the start, in cascade j's normalised space
	float s0j = 1.0;		// cascade 0's normalised units to cascade j's (one scale: cubes)
	float shellC0 = 0.0;	// D_j, cascade 0's units from the start
	float tCur = 0.0;		// how far the ray has been read, cascade 0's units
	bool entered = false;	// a cascade has read: the next one enters past the hand-over's plane
	for( int j = 0; j < JAH_VOX_COUNT; ++j )
	{
		const vec3 invRes = JAH_VOX_INVRES( j );
		const vec3 res = vec3( 1.0, 1.0, 1.0 ) / invRes;
		shellC0 += 8.0 * invRes.x / s0j;
		const bool last = j + 1 >= JAH_VOX_COUNT;
		// THE NEXT CASCADE's map of the start and of the origin plane (computed here, indexed j + 1 >= 1)
		vec3 posN = posJ;
		float s0n = s0j;
		vec4 orgN = origin;
		vec3 resN = res;
#if JAH_VOX_MAX_CASCADES > 1
		if( !last )
		{
			const vec4 sc = JAH_VOX_FROM_PREV_SCALE( j + 1 );
			const vec4 of = JAH_VOX_FROM_PREV_OFFSET( j + 1 );
			posN = posJ * sc.xyz + of.xyz;
			s0n = s0j * sc.x;
			if( origin.x >= 0.0 )
			{
				const int n = clamp( int( origin.x ), 0, 2 );
				orgN.y = origin.y * sc[n] + of[n];
			}
			resN = vec3( 1.0, 1.0, 1.0 ) / JAH_VOX_INVRES( j + 1 );
		}
#endif
		// THIS SHELL'S END: the first cell plane of cascade j + 1 the ray crosses past D_j; the last's, its world shell
		float tShell = 1e30;
		if( !last )
		{
			const vec3 qN = ( posN + dirLS * ( s0n * shellC0 ) ) * resN;
			const vec3 dqN = dirLS * s0n * resN;
			for( int a = 0; a < 3; ++a )
			{
				if( dqN[a] == 0.0 )
					continue;
				const float plane = dqN[a] > 0.0 ? floor( qN[a] ) + 1.0 : ceil( qN[a] ) - 1.0;
				tShell = min( tShell, shellC0 + ( plane - qN[a] ) / dqN[a] );
			}
		}
		else if( lastShellC0 > 0.0 )
			tShell = lastShellC0;
		// THE DDA ON CASCADE j's LEVEL-0 CELLS: q = posJ x res + dq t, t in cascade 0's units
		const vec3 qS = posJ * res;
		const vec3 dq = dirLS * s0j * res;
		const vec3 nudge = entered ? sign( dq ) * 1e-4 : vec3( 0.0, 0.0, 0.0 );
		ivec3 iv = ivec3( floor( qS + dq * tCur + nudge ) );
		const ivec3 resI = ivec3( res + 0.5 );
		if( all( greaterThanEqual( iv, ivec3( 0, 0, 0 ) ) ) && all( lessThan( iv, resI ) ) )
		{
			ivec3 stp;
			vec3 tMax, tDel;
			float tStop = tShell;
			for( int a = 0; a < 3; ++a )
			{
				stp[a] = dq[a] > 0.0 ? 1 : -1;
				if( dq[a] == 0.0 )
				{
					tMax[a] = 1e30;
					tDel[a] = 1e30;
					continue;
				}
				tMax[a] = ( float( iv[a] ) + ( dq[a] > 0.0 ? 1.0 : 0.0 ) - qS[a] ) / dq[a];
				tDel[a] = 1.0 / abs( dq[a] );
				tStop = min( tStop, ( ( dq[a] > 0.0 ? res[a] : 0.0 ) - qS[a] ) / dq[a] );
			}
			entered = true;
			float tIn = tCur;
			bool through = false;
			const int maxSteps = resI.x + resI.y + resI.z + 3;
			for( int it = 0; it < maxSteps; ++it )
			{
				++r.steps;
				const vec3 uvw = ( vec3( iv ) + 0.5 ) * invRes;
				const vec4 iso = JAH_VOX_SAMPLE_ISO( j, uvw, 0.0 );
				if( iso.w > 0.0 && !jahVoxelRayOriginOnly( j, uvw, iv, origin, invRes ) )
				{
					// THE SIDE FACING THE RAY (the header), un-premultiplied
					const vec4 bk = JAH_VOX_SAMPLE_BACK( j, uvw, 0.0 );
					const float along = dot( JAH_VOX_SAMPLE_NRM( j, uvw, 0.0 ).xyz * 2.0 - 1.0, -dirLS );
					const vec3 side = along < 0.0 ? bk.xyz : ( along > 0.0 ? iso.xyz : 0.5 * ( iso.xyz + bk.xyz ) );
					r.radiance = side * ( 1.0 / iso.w );
					r.exitReason = finite3( r.radiance ) ? kJahRayHit : kJahRayExhausted;
					if( r.exitReason != kJahRayHit )
						r.radiance = vec3( 0.0, 0.0, 0.0 );
					r.cascade = j;
					r.tC0 = tIn;
					return r;
				}
				const float tOut = min( min( tMax.x, tMax.y ), tMax.z );
				if( tOut >= tStop )
				{
					through = true;
					break;
				}
				const int a = tMax.x <= tMax.y ? ( tMax.x <= tMax.z ? 0 : 2 ) : ( tMax.y <= tMax.z ? 1 : 2 );
				iv[a] += stp[a];
				tIn = tMax[a];
				tMax[a] += tDel[a];
			}
			r.cascade = j;
			if( !through )
				return r;	// EXHAUSTED: no sample
			tCur = tStop;
			r.tC0 = tCur;
		}
		if( last )
		{
			// out of the last cascade (its world shell, or its box in the band): the sky's - if any cascade held the ray
			r.exitReason = entered ? kJahRayEscaped : kJahRayExhausted;
			r.cascade = j;
			return r;
		}
		posJ = posN;
		s0j = s0n;
		origin = orgN;
	}
	return r;
}

/// THE WORLD CACHES' DIRECTIONS (the spec's "N fixed, world-keyed, cosine-distributed rays"): direction `i` of `n` in
/// the frame whose third axis is the normal - a Vogel spiral on the unit disc (radius sqrt((i + 1/2) / n), the golden
/// angle between neighbours: equal areas, so equal projected solid angles once lifted onto the hemisphere, Malley's
/// method) turned about the normal by `turn` in [0, 1) of a revolution (the caller's world key: deterministic per
/// texel or voxel side, so a cache's rest and "replace at once" hold, and neighbours decorrelate).
vec3 jahRayCosineDir( int i, int n, float turn )
{
	const float u = ( float( i ) + 0.5 ) / float( n );
	const float rr = sqrt( u );
	const float phi = 6.2831853 * fract( float( i ) * 0.38196601 + turn );
	return vec3( rr * cos( phi ), rr * sin( phi ), sqrt( max( 1.0 - u, 0.0 ) ) );
}

/// THE WORLD KEY's turn from a world point: its bits at a fixed quantum (1/1024 of a metre-scale unit the caller picks)
/// hashed (PCG) to [0, 1) - the same point, the same turn, at any placement of any box.
float jahRayWorldTurn( vec3 keyPoint, uint salt )
{
	const ivec3 q = ivec3( floor( keyPoint * 1024.0 ) );
	uint h = uint( q.x ) * 73856093u ^ uint( q.y ) * 19349663u ^ uint( q.z ) * 83492791u ^ salt * 2654435761u;
	h = h * 747796405u + 2891336453u;
	h = ( ( h >> ( ( h >> 28u ) + 4u ) ) ^ h ) * 277803737u;
	h = ( h >> 22u ) ^ h;
	return float( h >> 8u ) * ( 1.0 / 16777216.0 );
}

#endif   // JAH_VOXEL_RAY_GLSL
