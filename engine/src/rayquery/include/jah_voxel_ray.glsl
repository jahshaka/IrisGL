// THE OCCUPANCY RAY OVER THE CASCADE CHAIN — the ray the world caches gather by (PHOTON-7 stage 10, 10-1; L9: NO
// LIGHT OF ANY TYPE PASSES A WALL; the preflight's option f1, spikes/voxel-leak-1/PREFLIGHT.md §4).
//
// WHY A RAY AND NOT A CONE OF APERTURE 0. The plane march (jah_voxel_march.glsl) reads, at level 0, the COVERAGE of
// the plane's own dominant axis and the lateral axes at the ray's crossing rate - a transparency by the fraction of a
// cell's face a surface covers, and by direction: a 1 m long, 0.4 m wall in the outer cascade's 1.875 m cells passed
// 0.44 of a ray aimed at its middle at normal incidence and 0.19-0.59 of one at 0-85 degrees (gi.voxel_ray_wall, red on
// the base; 10-0's P5 found the store marks every such cell). THIS RAY READS OCCUPANCY (the lead's ruling on P5,
// 2026-10-25): a cell the store marks occupied (the light volume's opacity c > 0, any geometry in it) is a hit FROM
// EVERY DIRECTION - but a PLANAR cell (its faces on one axis: the plane test, below), which a ray crossing its plane hits. What that costs is over-occlusion where a coarse cell is partly empty (a 1.875 m cell is as solid as
// its smallest object); what it buys is that no ray passes a wall at any angle in any cascade.
//
// THE HIT READS THE SIDE FACING THE RAY (the two-sided store, PHOTON-7 stage 7): the side of the voxel's surfaces the
// ray's entry point lies on (the front where it is on the side the canonical normal points to, the back behind it; by
// the travel only in a cell with no face on that axis) - each un-premultiplied by the opacity (what the surface emits,
// not how much of the cell it fills). A one-sided voxel's back stores 0 (stage 10's
// one-sided write): a ray reaching a solid's face from behind is inside the solid, and reads nothing.
//
// THE SURFACE THE RAY LEFT IS NEVER ITS OCCLUDER - THE TANGENT PLANE (the march's origin plane, made exact for a ray):
// a ray that leaves a surface meets that surface's own coarse cells as it climbs (an open floor's ray at 15 degrees is
// still inside the floor's 1.875 m cells at the third hop), so an occupied cell is a hit only when one of its faces
// lies IN FRONT of the surface's tangent plane - the plane through the surface point with the surface's normal - by
// more than the ray's start bias `eps` (the start is that far off the surface: a face nearer the plane is the surface's
// own). A face BEHIND the plane is unreachable by a ray that climbs away from it (every ray of a hemisphere does), so it
// is never one either: a slab's underside in the floor's own coarse cell. A face's point is its stored position along
// its own axis; across the cell, a face on the surface's own dominant axis takes the cell's centre (the surface and
// its coplanar continuation), a face on another axis the cell's corner MOST IN FRONT of the plane (it may reach
// anywhere across the cell: a wall standing on the floor is in front wherever the cell rises above the floor). (The
// first form of this gate was the cone march's own - the faces along the dominant axis within ONE CELL of the
// surface's coordinate, or behind it: a corner's diagonal normal made a whole wall's inner plane "the origin", and a
// 0.4 m slab within a 0.47 m cell hid its far face - gi.sealed_sky, the sky through the room's walls.)
// THE OWN CELL (`ownCell`: a voxel side's gather, the bounce): never read - it holds the side's own light and its
// other side's - and a face in front inside it that the ray CROSSES blocks the ray there (radiance 0: a surface nearer
// than a cell, a contact or a cavity this cascade cannot resolve, is dark, never the voxel's other side read through
// itself). THE START CELL - the cell the ray starts in, a cell off its surface - takes a face only where the ray crosses
// its stored plane inside the cell too: a floor texel within a cell of a wall standing on it starts INSIDE the wall's
// cell, and occupancy alone stopped every ray there, the ones leaving the wall included, each reading the wall's side
// facing it - its back, 0 (rtreflect.contact: the crease's floor row read 9 of the rows' 73-78, a dark line along it).
// THE PLANE TEST (the lead's ruling on 10-1's crease): A CELL WHOSE FACES ALL LIE ON ONE AXIS - a floor, a wall, a slab's
// two faces - is planar, and a planar surface blocks a ray exactly where the ray crosses it: the cell is a hit only
// where the ray crosses one of its STORED planes (the surface position of each half) inside the cell. A ray grazing
// above a floor inside the floor's own cell row is not occluded (rtreflect.contact: the block's bottom texel row's
// rays stayed in the floor's 7.8 cm row and hit it, the crease's first floor row 2.07 codes dark), and nothing that
// passes through a solid can avoid crossing its plane, so it cannot leak; a face that covers part of the cell is
// taken as covering it all (over-occlusion, never a leak). Every other cell - a corner, an edge, clutter, faces on two
// or three axes, a wall's end - is occupancy, a hit from every direction. THE FINEST CASCADE ONLY (`planeTest0`: the
// chain's first cascade is cascade 0): a half's stored plane is the coverage-weighted MEAN of its faces, and a coarse
// cell's half holds several - the 0.47 m cell under a room's wall holds the room's floor and the sky-lit ground outside,
// one plane between them, and a grazing ray slipped between the real faces under the mean (measured with the test in
// every cascade: gi.sealed_sky 3-4 % of the roof through the walls). At 7.8 cm one face is the case, and the crease lies
// there.
// THE SHELLS (the cone march's and LightInjection's jahInjectVisibility's, PHOTON-7 stages 1 and 5): cascade j reads
// the ray over the distances [D_(j-1), D_j) from the start, D_j the sum of eight cells of every cascade up to j, and
// hands over ON THE FIRST CELL PLANE OF CASCADE j + 1 PAST D_j (the next cascade enters its first cell at a face; a
// coarse cell entered mid-way would re-read what the fine cells read); a box that ends first hands over at its face
// (THE BAND: that ray is the placement's). A cascade whose box does not hold the point the ray has reached is passed
// over. A CHAIN THAT STARTS PAST THE FINEST CASCADE (a coarse cascade's bounce reads its own cascade and the ones
// outside it) keeps THE WORLD'S shells: its first cascade reads to that cascade's own D, `firstShellC0` (the job's
// table: a c2 voxel's ray reads c2 to 5.6 m, never c3 from 3.75 m - the 4 m room's far corners in the outer cascade's
// cells, where an inside floor and the sky-lit ground outside are one cell's one side). THE LAST CASCADE READS TO A WORLD SHELL, `lastShellC0` (the caller's: a distance every placement's last box
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

/// DOES AN OCCUPIED CELL STOP THE RAY (the header)? Cell `iv` of cascade `c` at its centre `uvw`; `surface` whether the
/// ray left one - `sJ` the surface point and `epsJ` the start bias in this cascade's normalised space, `nLS` the unit
/// normal (`invRes` this cascade's cell, per axis); the ray's segment in the cell from `pJ` along `dS` (this cascade's
/// normalised units per cascade 0's) between `tIn` and `tOut`. Only the faces IN FRONT of the tangent plane count.
///   A PLANAR CELL (its faces on one axis, and `planar` - the finest cascade - or `crossing`): it stops the ray where the
///   segment reaches THE SOLID its stored planes bound - behind a single face (its half's coverage-weighted plane; the
///   side it does not look to), between a slab's two faces, outside two faces looking at each other - so a ray grazing in
///   front of a floor in the floor's own cell row passes, and a ray that met a surface's relief above its mean plane is
///   still caught before it leaves the solid.
///   THE START CELL and THE OWN CELL holding faces on several axes (`crossing`): a face where the segment CROSSES its plane.
///   ANY OTHER CELL: occupancy - stopped from every direction.
bool jahVoxelRayFront( int c, vec3 uvw, ivec3 iv, bool surface, vec3 sJ, vec3 nLS, float epsJ, vec3 invRes,
					   bool crossing, bool planar, vec3 pJ, vec3 dS, float tIn, float tOut )
{
	const vec3 an = abs( nLS );
	const int dom = an.x >= an.y ? ( an.x >= an.z ? 0 : 2 ) : ( an.y >= an.z ? 1 : 2 );
	const vec4 cov[2] = vec4[2]( JAH_VOX_SAMPLE_COVP( c, uvw, 0.0 ), JAH_VOX_SAMPLE_COVN( c, uvw, 0.0 ) );
	const vec4 pos[2] = vec4[2]( JAH_VOX_SAMPLE_POSP( c, uvw, 0.0 ), JAH_VOX_SAMPLE_POSN( c, uvw, 0.0 ) );
	// the cell's centre, and its corner most in front of the tangent plane, relative to the surface point
	const vec3 centre = uvw - sJ;
	const vec3 front = centre + 0.5 * invRes * sign( nLS );
	int axes = 0, axis = 0;
	for( int b = 0; b < 3; ++b )
	{
		if( cov[0][b] + cov[1][b] > 0.0 )
		{
			++axes;
			axis = b;
		}
	}
	if( axes == 0 )
		return !crossing;	// (occupied with no coverage read: a hit - never a leak on a quantum)
	// each half's plane on an axis, and whether that face is in front of the tangent plane
	const bool planeCell = axes == 1 && ( planar || crossing );
	float qh[2];
	bool live[2];
	for( int h = 0; h < 2; ++h )
	{
		const int b = planeCell ? axis : 0;
		const float o = cov[h][b];
		qh[h] = o > 0.0 ? ( float( iv[b] ) + clamp( pos[h][b] / o, 0.0, 1.0 ) ) * invRes[b] : 0.0;
		bool inFront = o > 0.0;
		if( inFront && surface )
		{
			vec3 q = b == dom ? centre : front;
			q[b] = qh[h] - sJ[b];
			inFront = dot( q, nLS ) > epsJ;
		}
		live[h] = inFront;
	}
	if( planeCell )
	{
		// THE SOLID THE PLANES BOUND, against the segment's extent along the axis (a thousandth of a cell of slack: a
		// face ON a cell plane - a floor at y = 0 on a lattice of 7.8 cm - is reached where the ray enters or leaves)
		const int b = axis;
		const float slack = 1e-3 * invRes[b];
		const float a0 = pJ[b] + dS[b] * tIn, a1 = pJ[b] + dS[b] * tOut;
		const float lo = min( a0, a1 ) - slack, hi = max( a0, a1 ) + slack;
		// (the P half looks +axis: its solid is below its plane; the N half's above)
		if( live[0] && live[1] )
		{
			if( qh[1] <= qh[0] )
				return hi >= qh[1] && lo <= qh[0];	// a slab between them
			return lo <= qh[0] || hi >= qh[1];		// two faces looking at each other: the solid outside
		}
		if( live[0] )
			return lo <= qh[0];
		if( live[1] )
			return hi >= qh[1];
		return false;	// every face the surface's own
	}
	// several axes: occupancy, or (the start and own cells) the faces the segment crosses - in front of the plane
	for( int h = 0; h < 2; ++h )
	{
		for( int b = 0; b < 3; ++b )
		{
			const float o = cov[h][b];
			if( !( o > 0.0 ) )
				continue;
			const float qb = ( float( iv[b] ) + clamp( pos[h][b] / o, 0.0, 1.0 ) ) * invRes[b];
			if( crossing )
			{
				if( dS[b] == 0.0 )
					continue;
				const float tx = ( qb - pJ[b] ) / dS[b];
				const float slack = 1e-3 * invRes[b] / abs( dS[b] );
				if( !( tx >= tIn - slack && tx <= tOut + slack ) )
					continue;
			}
			if( !surface )
				return true;
			vec3 q = b == dom ? centre : front;
			q[b] = qb - sJ[b];
			if( dot( q, nLS ) > epsJ )
				return true;
		}
	}
	return false;
}

/// THE RAY from `posLS0` (cascade 0's normalised space; a cache's caller starts it `epsC0` off its surface) along the
/// unit `dirLS` (the same space; the chain's cascades are cubes, so it is every cascade's direction); the surface it left
/// - its point `surfLS0` and unit normal `nLS` (zero: a ray in free space, no surface) - and the voxel side's own cell
/// `ownCell` (xyz in cascade 0's cells, w 1; w 0: none); the first cascade's world shell end `firstShellC0` (0 or
/// less: its eight cells - the finest cascade's own D) and the last cascade's world shell `lastShellC0` (cascade 0's
/// normalised units from the start; 0 or less: its box alone).
JahVoxelRayResult jahVoxelRay( vec3 posLS0, vec3 dirLS, vec3 surfLS0, vec3 nLS, float epsC0, ivec4 ownCell,
							   float firstShellC0, float lastShellC0, bool planeTest0 )
{
	JahVoxelRayResult r;
	r.radiance = vec3( 0.0, 0.0, 0.0 );
	r.exitReason = kJahRayExhausted;
	r.cascade = 0;
	r.tC0 = 0.0;
	r.steps = 0;
	if( !finite3( posLS0 ) || !finite3( dirLS ) || !finite1( lastShellC0 ) || !finite3( surfLS0 ) || !finite3( nLS ) ||
		!finite1( epsC0 ) || !finite1( firstShellC0 ) )
		return r;
	const bool surface = dot( nLS, nLS ) > 0.0;
	vec3 posJ = posLS0;		// the start, in cascade j's normalised space
	vec3 surfJ = surfLS0;	// ...and the surface point
	float s0j = 1.0;		// cascade 0's normalised units to cascade j's (one scale: cubes)
	float shellC0 = 0.0;	// D_j, cascade 0's units from the start
	float tCur = 0.0;		// how far the ray has been read, cascade 0's units
	bool entered = false;	// a cascade has read: the next one enters past the hand-over's plane
	for( int j = 0; j < JAH_VOX_COUNT; ++j )
	{
		const vec3 invRes = JAH_VOX_INVRES( j );
		const vec3 res = vec3( 1.0, 1.0, 1.0 ) / invRes;
		shellC0 += j == 0 && firstShellC0 > 0.0 ? firstShellC0 : 8.0 * invRes.x / s0j;
		const bool last = j + 1 >= JAH_VOX_COUNT;
		// THE NEXT CASCADE's map of the start and of the surface point (computed here, indexed j + 1 >= 1)
		vec3 posN = posJ;
		vec3 surfN = surfJ;
		float s0n = s0j;
		vec3 resN = res;
#if JAH_VOX_MAX_CASCADES > 1
		if( !last )
		{
			const vec4 sc = JAH_VOX_FROM_PREV_SCALE( j + 1 );
			const vec4 of = JAH_VOX_FROM_PREV_OFFSET( j + 1 );
			posN = posJ * sc.xyz + of.xyz;
			surfN = surfJ * sc.xyz + of.xyz;
			s0n = s0j * sc.x;
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
			const bool firstDda = !entered;
			entered = true;
			float tIn = tCur;
			bool through = false;
			const int maxSteps = resI.x + resI.y + resI.z + 3;
			for( int it = 0; it < maxSteps; ++it )
			{
				++r.steps;
				const vec3 uvw = ( vec3( iv ) + 0.5 ) * invRes;
				const vec4 iso = JAH_VOX_SAMPLE_ISO( j, uvw, 0.0 );
				const bool own = j == 0 && ownCell.w > 0 && all( equal( iv, ownCell.xyz ) );
				// THE START CELL AND THE OWN CELL take a face only where the ray crosses it (the header)
				const bool crossing = own || ( firstDda && it == 0 );
				const float tCell = min( min( min( tMax.x, tMax.y ), tMax.z ), tStop );
				const bool front = iso.w > 0.0 && jahVoxelRayFront( j, uvw, iv, surface, surfJ, nLS, epsC0 * s0j, invRes,
																	   crossing, planeTest0 && j == 0, posJ, dirLS * s0j,
																	   tIn, tCell );
				if( front && own )
				{
					// a face in front inside the side's own cell: blocked there, dark (the header)
					r.exitReason = kJahRayHit;
					r.cascade = j;
					r.tC0 = tIn;
					return r;
				}
				if( front )
				{
					// THE SIDE FACING THE RAY (the header), un-premultiplied: BY WHERE THE RAY IS - the side of the voxel's
					// surfaces its entry point lies on, along the canonical normal's dominant axis (their coverage-weighted
					// plane: a slab's mid-plane, a single face itself) - and by its travel only where the cell holds no
					// face on that axis. (By its travel alone, a ray grazing a wall's cell column on its unlit side and
					// leaving the wall read the wall's lit side through it: gi.voxel_ray_wall (3).)
					const vec4 bk = JAH_VOX_SAMPLE_BACK( j, uvw, 0.0 );
					const vec3 nv = JAH_VOX_SAMPLE_NRM( j, uvw, 0.0 ).xyz * 2.0 - 1.0;
					float along = dot( nv, -dirLS );
					{
						const vec3 anv = abs( nv );
						const int na = anv.x >= anv.y ? ( anv.x >= anv.z ? 0 : 2 ) : ( anv.y >= anv.z ? 1 : 2 );
						const float oP = JAH_VOX_SAMPLE_COVP( j, uvw, 0.0 )[na];
						const float oN = JAH_VOX_SAMPLE_COVN( j, uvw, 0.0 )[na];
						if( oP + oN > 0.0 )
						{
							const float pP = oP > 0.0 ? JAH_VOX_SAMPLE_POSP( j, uvw, 0.0 )[na] / oP : 0.0;
							const float pN = oN > 0.0 ? JAH_VOX_SAMPLE_POSN( j, uvw, 0.0 )[na] / oN : 0.0;
							const float m = ( float( iv[na] ) +
											  ( oP * clamp( pP, 0.0, 1.0 ) + oN * clamp( pN, 0.0, 1.0 ) ) / ( oP + oN ) ) *
											invRes[na];
							const float e = posJ[na] + dirLS[na] * s0j * tIn;
							const float side = ( e - m ) * ( nv[na] >= 0.0 ? 1.0 : -1.0 );
							if( side != 0.0 )
								along = side;
						}
					}
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
		surfJ = surfN;
		s0j = s0n;
	}
	return r;
}

/// THE WORLD CACHES' DIRECTIONS (the spec's "N fixed, world-keyed, cosine-distributed rays"): direction `i` of `n` in
/// the frame whose third axis is the normal (jah_voxel_cones.glsl's jahConeBasis: a function of the WORLD normal alone) -
/// a Vogel spiral on the unit disc (radius sqrt((i + 1/2) / n), the golden angle between neighbours: equal areas, so
/// equal projected solid angles once lifted onto the hemisphere, Malley's method). ONE SET FOR EVERY SURFACE POINT,
/// never turned per texel or voxel: a cache is READ - by a reflection, by the gather's hits - texel by texel, and a set
/// turned by a world key per texel stored the set's own sampling noise into the cache, a speckle of codes the readers
/// show (rtreflect.contact: the crease's floor rows 91-98 a row apart, 36 columns over its law); the one set is smooth
/// across a surface, deterministic, and a function of the world (a cache's rest, "replace at once", walk = teleport).
vec3 jahRayCosineDir( int i, int n )
{
	const float u = ( float( i ) + 0.5 ) / float( n );
	const float rr = sqrt( u );
	const float phi = 6.2831853 * fract( float( i ) * 0.38196601 );
	return vec3( rr * cos( phi ), rr * sin( phi ), sqrt( max( 1.0 - u, 0.0 ) ) );
}

#endif   // JAH_VOXEL_RAY_GLSL
