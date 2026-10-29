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
// number, and it is NO SAMPLE, never sky; OUTSIDE, no cascade held its start - the chain knows nothing there, and the
// caller says what that is (the diffuse: GI off's answer, the sky over the ray's bin; the field's probes: no sample). (The DDA visits at most res.x + res.y + res.z + 3 cells of a box, so a
// finite ray never exhausts: the reason exists so that no caller can read a broken ray as an escape.)
//
// THE AT-SIGN RULE: this file becomes Hlms input (the piece JahVoxelRay), so no character of it may be the Hlms
// directive mark, comments included. It includes nothing; the caller inserts JahVoxelSample first (the finite test
// and the sample file's macros) and binds, besides the chain's (JAH_VOX_COUNT, JAH_VOX_MAX_CASCADES, JAH_VOX_INVRES,
// JAH_VOX_FROM_PREV_SCALE / _OFFSET), the level-0 reads JAH_VOX_SAMPLE_ISO, _BACK, _NRM, _COVP, _COVN, _POSP, _POSN.
// Every texture array is indexed by the loop's own counter, the same for every invocation still marching.

#ifndef JAH_VOXEL_RAY_GLSL
#define JAH_VOXEL_RAY_GLSL

#if !( JAH_VOX_MAX_CASCADES >= 1 )
#error "jah_voxel_ray.glsl: JAH_VOX_MAX_CASCADES must be a preprocessor integer of at least 1 - an identifier (a const int) reads 0 in #if and skips every cascade's hand-over"
#endif
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
const uint kJahRayOutside = 3u;		///< no cascade held its start: the chain says nothing of its world (the caller's)

struct JahVoxelRayResult
{
	vec3 radiance;		///< a HIT: the side of the voxel facing the ray, un-premultiplied, x the throughput - plus the panes'
						///< own light; an ESCAPE: the panes' own light alone; else 0
	uint exitReason;	///< kJahRayHit / kJahRayEscaped / kJahRayExhausted
	int cascade;		///< the cascade it ended in
	float tC0;			///< from the start to the hit cell's entry (or to the exit), cascade 0's normalised units
	int steps;			///< the cells it read - its price
	float throughput;	///< what the PANE CELLS it crossed passed (1 with none): an escape's sky is the caller's x this
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
					   bool crossing, bool planar, vec3 pJ, vec3 dS, float tIn, float tOut, uint skip, out int bothAxis,
					   out vec2 bothPlanes )
{
	bothAxis = -1;
	bothPlanes = vec2( 0.0, 0.0 );
	const vec3 an = abs( nLS );
	const int dom = an.x >= an.y ? ( an.x >= an.z ? 0 : 2 ) : ( an.y >= an.z ? 1 : 2 );
	vec4 cov[2] = vec4[2]( JAH_VOX_SAMPLE_COVP( c, uvw, 0.0 ), JAH_VOX_SAMPLE_COVN( c, uvw, 0.0 ) );
	// THE PANE FACES are not the occluder's (jahVoxelPaneMask; the caller crosses them): their coverage out of every test
	for( int h = 0; h < 2; ++h )
		for( int b = 0; b < 3; ++b )
			if( ( skip & ( 1u << uint( 3 * h + b ) ) ) != 0u )
				cov[h][b] = 0.0;
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
	// (PHOTON-7 10-4 R7, F2: every face is read, so the caller learns of A CELL WHOSE FACES IN FRONT HOLD BOTH
	// ORIENTATIONS ON ONE AXIS - a closed object under the cell, a wall's two faces: it has no plane to face, and the hit
	// is read by jahVoxelRayHitSide's rule; bothAxis that axis, bothPlanes its +a and -a faces' planes)
	bool hit = false;
	bool liveP[3] = bool[3]( false, false, false ), liveN[3] = bool[3]( false, false, false );
	float qP[3] = float[3]( 0.0, 0.0, 0.0 ), qN[3] = float[3]( 0.0, 0.0, 0.0 );
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
			bool live = !surface;
			if( surface )
			{
				vec3 q = b == dom ? centre : front;
				q[b] = qb - sJ[b];
				live = dot( q, nLS ) > epsJ;
			}
			if( live )
			{
				hit = true;
				if( h == 0 ) { liveP[b] = true; qP[b] = qb; }
				else { liveN[b] = true; qN[b] = qb; }
			}
		}
	}
	if( hit && !crossing )
	{
		for( int b = 0; b < 3 && bothAxis < 0; ++b )
		{
			if( liveP[b] && liveN[b] )
			{
				bothAxis = b;
				bothPlanes = vec2( qP[b], qN[b] );
			}
		}
	}
	return hit;
}

/// THE PANE FACES of a cell (PHOTON-7 10-4 R7; VoxelMerge_piece_cs.any THE PANE FACES): bit h x 3 + a set where half h
/// (0 the faces looking +a, 1 looking -a) of axis a holds a pane's faces and no occluder's - axes x and y in the
/// coverage's alpha (two bits: the value x 3), axis z in the surface position's sixteenth bit.
uint jahVoxelPaneMask( int c, vec3 uvw )
{
	const uint p = uint( JAH_VOX_SAMPLE_COVP( c, uvw, 0.0 ).w * 3.0 + 0.5 ) |
				   ( ( uint( JAH_VOX_SAMPLE_POSP( c, uvw, 0.0 ).w * 65535.0 + 0.5 ) >> 15u ) << 2u );
	const uint n = uint( JAH_VOX_SAMPLE_COVN( c, uvw, 0.0 ).w * 3.0 + 0.5 ) |
				   ( ( uint( JAH_VOX_SAMPLE_POSN( c, uvw, 0.0 ).w * 65535.0 + 0.5 ) >> 15u ) << 2u );
	return ( p & 7u ) | ( ( n & 7u ) << 3u );
}

/// THE PANE FACES A RAY CROSSES IN A CELL (PHOTON-7 10-4 R7; the lead's cell rule, 2026-10-27; VoxelMerge_piece_cs.any
/// THE PANE FACES): a pane's face - a surface its material blends with what is behind it - TRANSMITS the ray, by the one
/// face model the light injection's march takes (jah_voxel_faces.glsl): each pane face (`mask`) the segment crosses (its
/// stored plane per half and axis, inside [tIn, tOut]), a front face x (1 - c), the inside fraction for a back one - c the face's
/// coverage, its projected area x its alpha, so a pane crossed whole passes 1 - alpha. What it stops returns the side of
/// the cell facing the ray (un-premultiplied: the pane's own light, the row's kD x alpha for a glass - its pixel's alpha^2
/// kD over the coverage's alpha). The faces the ray LEFT are not crossed: a face behind the tangent plane (the surface's
/// own, the header's gate). The OCCLUDERS' faces of the cell are the caller's (occupancy or the plane test, as every face
/// was: a wall's end beside a window still stops the ray). `T` / `fIn` the ray's throughput and inside fraction,
/// carried; returns the light stopped here (none where `light` is false: the side's own cell, never read).
vec3 jahVoxelPaneCross( int c, vec3 uvw, ivec3 iv, vec4 iso, uint mask, bool light, bool surface, vec3 sJ, vec3 nLS,
						float epsJ, vec3 invRes, vec3 pJ, vec3 dS, vec3 dirLS, float tIn, float tOut, inout float T,
						inout float fIn )
{
	const vec4 cov[2] = vec4[2]( JAH_VOX_SAMPLE_COVP( c, uvw, 0.0 ), JAH_VOX_SAMPLE_COVN( c, uvw, 0.0 ) );
	const vec4 pos[2] = vec4[2]( JAH_VOX_SAMPLE_POSP( c, uvw, 0.0 ), JAH_VOX_SAMPLE_POSN( c, uvw, 0.0 ) );
	const vec3 an = abs( nLS );
	const int dom = an.x >= an.y ? ( an.x >= an.z ? 0 : 2 ) : ( an.y >= an.z ? 1 : 2 );
	const vec3 centre = uvw - sJ;
	const vec3 front = centre + 0.5 * invRes * sign( nLS );
	float evT[6], evC[6];
	bool evFront[6];
	int evN = 0;
	for( int b = 0; b < 3; ++b )
	{
		if( dS[b] == 0.0 )
			continue;
		const float slack = 1e-3 * invRes[b] / abs( dS[b] );
		for( int h = 0; h < 2; ++h )
		{
			const float o = cov[h][b];
			if( !( o > 0.0 ) || ( mask & ( 1u << uint( 3 * h + b ) ) ) == 0u )
				continue;
			const float qb = ( float( iv[b] ) + clamp( pos[h][b] / o, 0.0, 1.0 ) ) * invRes[b];
			const float tx = ( qb - pJ[b] ) / dS[b];
			if( !( tx >= tIn - slack && tx <= tOut + slack ) )
				continue;
			if( surface )
			{
				vec3 q = b == dom ? centre : front;
				q[b] = qb - sJ[b];
				if( !( dot( q, nLS ) > epsJ ) )
					continue;	// the surface's own face
			}
			evT[evN] = tx;
			evC[evN] = min( o, 1.0 );
			// the faces looking +b (the P half) face a ray travelling -b: it enters through them
			evFront[evN] = h == 0 ? dS[b] < 0.0 : dS[b] > 0.0;
			++evN;
		}
	}
	if( evN == 0 )
		return vec3( 0.0, 0.0, 0.0 );
	const float before = T;
	jahVoxelFacesCross( evT, evC, evFront, evN, T, fIn );
	if( !light )
		return vec3( 0.0, 0.0, 0.0 );
	// THE SIDE FACING THE RAY (the hit's rule, by the travel: a pane cell's faces are one sheet's)
	const vec4 bk = JAH_VOX_SAMPLE_BACK( c, uvw, 0.0 );
	const vec3 nv = JAH_VOX_SAMPLE_NRM( c, uvw, 0.0 ).xyz * 2.0 - 1.0;
	const float along = dot( nv, -dirLS );
	const vec3 side = along < 0.0 ? bk.xyz : ( along > 0.0 ? iso.xyz : 0.5 * ( iso.xyz + bk.xyz ) );
	const vec3 L = side * ( 1.0 / iso.w );
	return finite3( L ) ? ( before - T ) * L : vec3( 0.0, 0.0, 0.0 );
}

/// THE SIDE A HIT READS (the header's rule), un-premultiplied by the cell's opacity: BY WHERE THE RAY IS - the side of
/// the voxel's surfaces its entry point lies on, along the canonical normal's dominant axis (their coverage-weighted
/// plane: a slab's mid-plane, a single face itself) - and by its travel only where the cell holds no face on that axis.
/// (By its travel alone, a ray grazing a wall's cell column on its unlit side and leaving the wall read the wall's lit
/// side through it: gi.voxel_ray_wall (3).)
/// A CELL WHOSE FACES IN FRONT HOLD BOTH ORIENTATIONS ON ONE AXIS (PHOTON-7 10-4 R7, F2, the lead's ruling (c) of
/// 2026-10-27; `bothAxis`, its two faces' planes `bothPlanes`) has no plane to face: the store keeps two sides along the
/// canonical normal, and a closed object's six faces are not two. Its hit reads the side the ray is on only where that
/// is decided - the canonical normal along that axis and the entry outside the two faces (a wall's two faces, the ray
/// on one side of the wall: gi.voxel_ray_wall's W) - and otherwise THE DIMMER OF ITS TWO SIDES: never more light than
/// one side holds, so never light through a wall. It errs DARK, by up to the two sides' difference, where no finer
/// cascade holds the point (the chain march asks the finer cascade first: jahVoxelRay).
vec3 jahVoxelRayHitSide( int c, vec3 uvw, ivec3 iv, vec4 iso, vec3 pJ, vec3 dirLS, float s0j, float tIn, vec3 invRes,
						 int bothAxis, vec2 bothPlanes )
{
	const vec4 bk = JAH_VOX_SAMPLE_BACK( c, uvw, 0.0 );
	const vec3 nv = JAH_VOX_SAMPLE_NRM( c, uvw, 0.0 ).xyz * 2.0 - 1.0;
	const vec3 anv = abs( nv );
	const int na = anv.x >= anv.y ? ( anv.x >= anv.z ? 0 : 2 ) : ( anv.y >= anv.z ? 1 : 2 );
	if( bothAxis >= 0 )
	{
		const float e = pJ[bothAxis] + dirLS[bothAxis] * s0j * tIn;
		const bool outside = e < min( bothPlanes.x, bothPlanes.y ) || e > max( bothPlanes.x, bothPlanes.y );
		if( !( outside && na == bothAxis ) )
		{
			const vec3 luma = vec3( 0.2126, 0.7152, 0.0722 );
			return ( dot( iso.xyz, luma ) <= dot( bk.xyz, luma ) ? iso.xyz : bk.xyz ) * ( 1.0 / iso.w );
		}
	}
	float along = dot( nv, -dirLS );
	{
		// THE AXIS THE SIDE IS DECIDED ON (PHOTON-7 11-2, gi.small_emitter): the face the ray MEETS - of the axes the cell
		// holds faces LOOKING AT THE RAY on, the one whose coverage-weighted plane the ray reaches first from its entry (a
		// face it travels parallel to, has already passed, or that looks away from it is not met); none met: the canonical
		// normal's axis, as before. A wall's
		// column, faces on one axis, is decided on it as before (gi.voxel_ray_wall (3)); a cube's EDGE cell is decided on
		// the face toward the ray, never on the side face it meets edge-on (that read its x face's BACK for a ray
		// hitting the +z face: three store steps dark at the far emitter's peak).
		const vec4 cP = JAH_VOX_SAMPLE_COVP( c, uvw, 0.0 ), cN = JAH_VOX_SAMPLE_COVN( c, uvw, 0.0 );
		int a = na;
		float tMeet = 1e30;
		for( int k = 0; k < 3; ++k )
		{
			const float okP = cP[k], okN = cN[k];
			// (REDS-1, gi.screen_integrate's turned boxes: a face the ray MEETS looks AT it - only the half-axis facing the
			// ray counts, N for a ray travelling +k, P for -k, the coverage's own rule. Any coverage on the axis let a
			// TILTED face's minor axis be chosen, where the face looks AWAY from the ray, and nv's minor component decided
			// the BACK - a one-sided wall's back stores 0: the turned no-ray picture read 10-20 % dark, 10.86 -> 18.85 %)
			if( !( ( dirLS[k] > 1e-6 && okN > 0.0 ) || ( dirLS[k] < -1e-6 && okP > 0.0 ) ) )
				continue;
			const float qP = okP > 0.0 ? JAH_VOX_SAMPLE_POSP( c, uvw, 0.0 )[k] / okP : 0.0;
			const float qN = okN > 0.0 ? JAH_VOX_SAMPLE_POSN( c, uvw, 0.0 )[k] / okN : 0.0;
			const float mk = ( float( iv[k] ) + ( okP * clamp( qP, 0.0, 1.0 ) + okN * clamp( qN, 0.0, 1.0 ) ) /
											( okP + okN ) ) * invRes[k];
			const float ek = pJ[k] + dirLS[k] * s0j * tIn;
			const float tk = ( mk - ek ) / ( dirLS[k] * s0j );
			if( tk >= 0.0 && tk < tMeet )
			{
				tMeet = tk;
				a = k;
			}
		}
		// MEASURING ARM (BUGS-8 (1), to be reverted): the face rule only where the met axis carries at least half the
		// canonical normal's dominant component - elsewhere the dominant axis decides, as before 50e93463
		if( a != na && abs( nv[a] ) < 0.5 * abs( nv[na] ) )
			a = na;
		const float oP = cP[a];
		const float oN = cN[a];
		if( oP + oN > 0.0 && nv[a] != 0.0 )
		{
			const float pP = oP > 0.0 ? JAH_VOX_SAMPLE_POSP( c, uvw, 0.0 )[a] / oP : 0.0;
			const float pN = oN > 0.0 ? JAH_VOX_SAMPLE_POSN( c, uvw, 0.0 )[a] / oN : 0.0;
			const float m = ( float( iv[a] ) +
							  ( oP * clamp( pP, 0.0, 1.0 ) + oN * clamp( pN, 0.0, 1.0 ) ) / ( oP + oN ) ) *
							invRes[a];
			const float e = pJ[a] + dirLS[a] * s0j * tIn;
			const float side = ( e - m ) * ( nv[a] > 0.0 ? 1.0 : -1.0 );
			if( side != 0.0 )
				along = side;
		}
	}
	const vec3 side = along < 0.0 ? bk.xyz : ( along > 0.0 ? iso.xyz : 0.5 * ( iso.xyz + bk.xyz ) );
	return side * ( 1.0 / iso.w );
}

/// ONE CELL OF THE MARCH (the chain's and a finer cascade's segment alike): cell `iv` of cascade `c` over the ray's
/// segment [tIn, tCell] (cascade 0's units; `pJ`, `sJ` the start and the surface point in this cascade's space, `s0j`
/// its scale). Returns 0 the ray PASSES the cell; 1 a HIT (`radiance` the panes' light + the throughput x the side it
/// reads, `bothAxis` >= 0 where the cell is F2's); 2 a hit in the voxel side's OWN cell (dark: the panes' light alone);
/// 3 the PANES stopped it whole (their light alone). The pane faces cross by the one face model (jahVoxelPaneCross).
int jahVoxelRayCell( int c, ivec3 iv, vec3 invRes, bool own, bool crossing, bool planar, bool surface, vec3 pJ, vec3 sJ,
					 vec3 nLS, float epsJ, float s0j, vec3 dirLS, float tIn, float tCell, inout float paneT,
					 inout float paneIn, inout vec3 paneLight, out vec3 radiance, out int bothAxis )
{
	radiance = vec3( 0.0, 0.0, 0.0 );
	bothAxis = -1;
	const vec3 uvw = ( vec3( iv ) + 0.5 ) * invRes;
	const vec4 iso = JAH_VOX_SAMPLE_ISO( c, uvw, 0.0 );
	if( !( iso.w > 0.0 ) )
		return 0;
	// THE PANE FACES (jahVoxelPaneMask): out of the occluders' test, crossed after it (jahVoxelPaneCross)
	const uint paneMask = jahVoxelPaneMask( c, uvw );
	bool occluders = true;
	if( paneMask != 0u )
	{
		const vec4 cP = JAH_VOX_SAMPLE_COVP( c, uvw, 0.0 ), cN = JAH_VOX_SAMPLE_COVN( c, uvw, 0.0 );
		occluders = false;
		for( int b = 0; b < 3; ++b )
			occluders = occluders || ( cP[b] > 0.0 && ( paneMask & ( 1u << uint( b ) ) ) == 0u ) ||
						( cN[b] > 0.0 && ( paneMask & ( 1u << uint( 3 + b ) ) ) == 0u );
	}
	vec2 bothPlanes = vec2( 0.0, 0.0 );
	const bool front = occluders && jahVoxelRayFront( c, uvw, iv, surface, sJ, nLS, epsJ, invRes, crossing, planar, pJ,
													  dirLS * s0j, tIn, tCell, paneMask, bothAxis, bothPlanes );
	if( !front )
	{
		bothAxis = -1;
		if( paneMask != 0u )
		{
			paneLight += jahVoxelPaneCross( c, uvw, iv, iso, paneMask, !own, surface, sJ, nLS, epsJ, invRes, pJ,
											dirLS * s0j, dirLS, tIn, tCell, paneT, paneIn );
			if( !( paneT > 0.0 ) )
			{
				// nothing passes: the panes' light is the whole answer, a hit where it stopped
				radiance = finite3( paneLight ) ? paneLight : vec3( 0.0, 0.0, 0.0 );
				return 3;
			}
		}
		return 0;
	}
	if( own )
	{
		// a face in front inside the side's own cell: blocked there, dark (the header) - but the panes before it
		radiance = finite3( paneLight ) ? paneLight : vec3( 0.0, 0.0, 0.0 );
		return 2;
	}
	// ...through the panes crossed before it, and their own light beside it (PHOTON-7 10-4 R7)
	radiance = paneLight + paneT * jahVoxelRayHitSide( c, uvw, iv, iso, pJ, dirLS, s0j, tIn, invRes, bothAxis, bothPlanes );
	return 1;
}

/// THE RAY from `posLS0` (cascade 0's normalised space; a cache's caller starts it `epsC0` off its surface) along the
/// unit `dirLS` (the same space; the chain's cascades are cubes, so it is every cascade's direction); the surface it left
/// - its point `surfLS0` and unit normal `nLS` (zero: a ray in free space, no surface) - and the voxel side's own cell
/// `ownCell` (xyz in cascade 0's cells, w 1; w 0: none); the first cascade's world shell end `firstShellC0` (0 or
/// less: its eight cells - the finest cascade's own D) and the last cascade's world shell `lastShellC0` (cascade 0's
/// normalised units from the start; 0 or less: its box alone).
/// A FINER CASCADE HOLDING THE POINT WINS (F2, the lead's cell rule): a hit in an F2 cell of cascade j > 0 (both
/// orientations on one axis: jahVoxelRayHitSide) asks the finest cascade whose box holds the cell's segment - both its
/// ends - to march that segment on its own finer cells: its hit is the answer, and a segment it proves empty lets the
/// ray go on past the coarse cell. With no finer cascade holding it, the coarse cell's own rule answers.
JahVoxelRayResult jahVoxelRay( vec3 posLS0, vec3 dirLS, vec3 surfLS0, vec3 nLS, float epsC0, ivec4 ownCell,
							   float firstShellC0, float lastShellC0, bool planeTest0 )
{
	JahVoxelRayResult r;
	r.radiance = vec3( 0.0, 0.0, 0.0 );
	r.exitReason = kJahRayExhausted;
	r.cascade = 0;
	r.tC0 = 0.0;
	r.steps = 0;
	r.throughput = 1.0;
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
	float paneT = 1.0;		// THE THROUGHPUT past the pane faces crossed (PHOTON-7 10-4 R7), and the inside fraction
	float paneIn = 0.0;
	vec3 paneLight = vec3( 0.0, 0.0, 0.0 );	// ...and the light they stopped
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
			// ONE CALL OF THE CELL RULE (jahVoxelRayCell), for this cascade's cells AND for an F2 cell's finer segment:
			// the march alternates between them in one loop (`sub` the segment's), so the rule - the faces, the panes,
			// the hit's side - is compiled once, never once per march (the F2 segment's own call made every includer
			// compile the rule twice over: rq_voxel_sides' pipeline 4.0 s cold against 1.0 s)
			bool sub = false;
			int itMain = 0, itSub = 0, maxK = 0, subC = 0;
			ivec3 ivK = ivec3( 0 ), stpK = ivec3( 0 ), resIK = ivec3( 0 );
			vec3 tMaxK = vec3( 0.0 ), tDelK = vec3( 0.0 ), pK = posLS0, sK = surfLS0, invResK = invRes;
			float s0k = 1.0, tInK = 0.0, tCell = 0.0;
			vec3 coarseRad = vec3( 0.0 );
			float coarseT = 1.0;
			for( ;; )
			{
				if( !sub && itMain >= maxSteps )
					break;
				const float tCellK = sub ? min( min( min( tMaxK.x, tMaxK.y ), tMaxK.z ), tCell ) : 0.0;
				if( !sub )
					tCell = min( min( min( tMax.x, tMax.y ), tMax.z ), tStop );
				++r.steps;
				const bool own = !sub && j == 0 && ownCell.w > 0 && all( equal( iv, ownCell.xyz ) );
				// THE START CELL AND THE OWN CELL take a face only where the ray crosses it (the header)
				const bool crossing = !sub && ( own || ( firstDda && itMain == 0 ) );
				const int cc = sub ? subC : j;
				const float s0c = sub ? s0k : s0j;
				vec3 cellRad = vec3( 0.0 );
				int bothAxis = -1;
				int st = 0;
				// (the cascades' textures are indexed by a LOOP's own counter - dynamically uniform among the lanes that
				// run the body, wherever each lane's march stands - and the rule is still written once)
				for( int c = 0; c < JAH_VOX_COUNT; ++c )
				{
					if( c != cc )
						continue;
					st = jahVoxelRayCell( c, sub ? ivK : iv, sub ? invResK : invRes, own, crossing, planeTest0 && c == 0,
										  surface, sub ? pK : posJ, sub ? sK : surfJ, nLS, epsC0 * s0c, s0c, dirLS,
										  sub ? tInK : tIn, sub ? tCellK : tCell, paneT, paneIn, paneLight, cellRad,
										  bothAxis );
				}
				if( sub )
				{
					++itSub;
					if( st != 0 )
					{
						r.radiance = finite3( cellRad ) ? cellRad : vec3( 0.0, 0.0, 0.0 );
						r.throughput = st == 3 ? 0.0 : paneT;
						r.exitReason = st == 1 && !finite3( cellRad ) ? kJahRayExhausted : kJahRayHit;
						r.cascade = subC;
						r.tC0 = tInK;
						return r;
					}
					const bool covered = tCellK >= tCell;
					bool lost = false;
					if( !covered )
					{
						const int a = tMaxK.x <= tMaxK.y ? ( tMaxK.x <= tMaxK.z ? 0 : 2 ) : ( tMaxK.y <= tMaxK.z ? 1 : 2 );
						ivK[a] += stpK[a];
						tInK = tMaxK[a];
						tMaxK[a] += tDelK[a];
						lost = itSub >= maxK || any( lessThan( ivK, ivec3( 0, 0, 0 ) ) ) || any( greaterThanEqual( ivK, resIK ) );
					}
					if( lost )
					{
						// a segment the finer cells did not cover to its end proves nothing: the coarse cell's own answer
						r.radiance = finite3( coarseRad ) ? coarseRad : vec3( 0.0, 0.0, 0.0 );
						r.throughput = coarseT;
						r.exitReason = !finite3( coarseRad ) ? kJahRayExhausted : kJahRayHit;
						r.cascade = j;
						r.tC0 = tIn;
						return r;
					}
					if( !covered )
						continue;
					// the segment proved empty: the ray goes on past the coarse cell
					sub = false;
				}
				else
				{
					++itMain;
#if JAH_VOX_MAX_CASCADES > 1
					// F2: THE FINER CASCADE HOLDING THE POINT WINS (the header) - the finest whose box holds both ends of
					// this cell's segment marches it on its own cells (each texture array indexed by a loop's own counter)
					if( st == 1 && bothAxis >= 0 && j > 0 )
					{
						vec3 q = posLS0, qs = surfLS0;
						float s = 1.0;
						int held = -1;
						for( int k = 0; k < JAH_VOX_COUNT && k < j && held < 0; ++k )
						{
							if( k > 0 )
							{
								const vec4 sc = JAH_VOX_FROM_PREV_SCALE( k );
								const vec4 of = JAH_VOX_FROM_PREV_OFFSET( k );
								q = q * sc.xyz + of.xyz;
								qs = qs * sc.xyz + of.xyz;
								s *= sc.x;
							}
							const vec3 e0 = q + dirLS * ( s * tIn ), e1 = q + dirLS * ( s * tCell );
							if( any( lessThan( min( e0, e1 ), vec3( 0.0 ) ) ) || any( greaterThan( max( e0, e1 ), vec3( 1.0 ) ) ) )
								continue;
							held = k;
							pK = q;
							sK = qs;
							s0k = s;
							invResK = JAH_VOX_INVRES( k );
						}
						if( held >= 0 )
						{
							// THE SEGMENT on cascade held's cells, from tIn to tCell
							subC = held;
							const vec3 resK = vec3( 1.0, 1.0, 1.0 ) / invResK;
							const vec3 qSK = pK * resK;
							const vec3 dqK = dirLS * s0k * resK;
							ivK = ivec3( floor( qSK + dqK * tIn + sign( dqK ) * 1e-4 ) );
							resIK = ivec3( resK + 0.5 );
							for( int a = 0; a < 3; ++a )
							{
								stpK[a] = dqK[a] > 0.0 ? 1 : -1;
								if( dqK[a] == 0.0 )
								{
									tMaxK[a] = 1e30;
									tDelK[a] = 1e30;
									continue;
								}
								tMaxK[a] = ( float( ivK[a] ) + ( dqK[a] > 0.0 ? 1.0 : 0.0 ) - qSK[a] ) / dqK[a];
								tDelK[a] = 1.0 / abs( dqK[a] );
							}
							tInK = tIn;
							coarseRad = cellRad;
							coarseT = paneT;
							maxK = resIK.x + resIK.y + resIK.z + 3;
							itSub = 0;
							if( any( lessThan( ivK, ivec3( 0, 0, 0 ) ) ) || any( greaterThanEqual( ivK, resIK ) ) )
							{
								// (a segment that starts outside the finer cells proves nothing either)
								r.radiance = finite3( cellRad ) ? cellRad : vec3( 0.0, 0.0, 0.0 );
								r.throughput = paneT;
								r.exitReason = !finite3( cellRad ) ? kJahRayExhausted : kJahRayHit;
								r.cascade = j;
								r.tC0 = tIn;
								return r;
							}
							sub = true;
							continue;
						}
					}
#endif
					if( st != 0 )
					{
						r.radiance = finite3( cellRad ) ? cellRad : vec3( 0.0, 0.0, 0.0 );
						r.throughput = st == 3 ? 0.0 : paneT;
						r.exitReason = st == 1 && !finite3( cellRad ) ? kJahRayExhausted : kJahRayHit;
						r.cascade = j;
						r.tC0 = tIn;
						return r;
					}
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
			// out of the last cascade (its world shell, or its box in the band): the sky's - if any cascade held the ray;
			// a ray NO cascade held is OUTSIDE the chain's world (a pixel beyond a short chain, the eye in a teleport's
			// window before the chain is rebuilt around it): the caller's to answer, never a hit or an escape it earned
			r.exitReason = entered ? kJahRayEscaped : kJahRayOutside;
			r.cascade = j;
			// the panes crossed: their own light, and what they pass of the sky (the caller's, x r.throughput)
			r.radiance = finite3( paneLight ) ? paneLight : vec3( 0.0, 0.0, 0.0 );
			r.throughput = paneT;
			return r;
		}
		posJ = posN;
		surfJ = surfN;
		s0j = s0n;
	}
	return r;
}

/// AN ESCAPE PAST THE WORLD's EXIT (PHOTON-7 10-3 R4 (iii)): a ray that left the chain (ESCAPED, `tEscape` along unit
/// `dirLS` from `startLS`, the start's normalised units) at or past the exit of the world's box - the traced instances'
/// union, which a caller binds in the start's normalised space as JAH_WORLD_LS_ON / _MIN / _MAX - has left the world:
/// the sky, with no trace. One that left the chain INSIDE the box is the CALLER's: a reader with the TLAS continues it
/// (rq_probe_integrate.comp); one without keeps the sky - the voxel route's limit (a loose box says nothing of what is
/// in it). Without the box every escape is an exit.
#ifndef JAH_WORLD_LS_ON
#define JAH_WORLD_LS_ON false
#define JAH_WORLD_LS_MIN vec3( 0.0, 0.0, 0.0 )
#define JAH_WORLD_LS_MAX vec3( 0.0, 0.0, 0.0 )
#endif
bool jahEscapeIsExit( vec3 startLS, vec3 dirLS, float tEscape )
{
	if( !( JAH_WORLD_LS_ON ) )
		return true;
	const vec3 lo = JAH_WORLD_LS_MIN, hi = JAH_WORLD_LS_MAX;
	float tExit = 1e30;
	for( int a = 0; a < 3; ++a )
	{
		if( dirLS[a] == 0.0 )
			continue;
		tExit = min( tExit, ( ( dirLS[a] > 0.0 ? hi[a] : lo[a] ) - startLS[a] ) / dirLS[a] );
	}
	return tEscape >= tExit;
}

// (THE WORLD CACHES' DIRECTIONS - jahRayCosineDir - are jah_ray_set.glsl's, with the frame they are lifted in.)

#endif   // JAH_VOXEL_RAY_GLSL
