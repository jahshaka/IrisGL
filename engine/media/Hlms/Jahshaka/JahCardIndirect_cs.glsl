// Jahshaka/CardAccumulate and Jahshaka/CardSensitivity (PHOTON-7 stage 5, 5-1) - A CARD'S INDIRECT IS STALE WHEN THE
// LIGHT IT GATHERS CHANGED BY A CODE SINCE ITS LAST MARCH, the change accumulated per card in absolute terms (the
// voxel feedback's resume rule, PhotonBrickFeedback_cs.glsl's ACCUM, with the card as the receiver).
//
// THE STATE, 4 words per atlas SLOT (the card's corner in 16-texel cells of the atlas, y x 128 + x: a card keeps its
// slot while it is resident, and a new card in a freed slot is captured, so marched, so its state is reset):
//   [0] the card's SENSITIVITY: codes of its radiance per unit of gathered radiance, float bits - 1 / its darkest lit
//       texel's code (a code is 1 % of the texel's radiance over the store's floor, Types.h kGiBrickFloor), the
//       re-emission bounded by one (a surface re-emits at most what it gathers);
//   [1] the would-move ACCUMULATED since its last march, fixed point (65536 units a code, saturating);
//   [2] the fraction of a unit the last accumulation's rounding left (float bits; carried: unbiased);
//   [3] REPORTED: the card is listed stale and waits for its march (no second listing).
// SENSITIVITY (jah_card_sens): one workgroup a card of the relight list, after the relight - its lit texels' lowest
// radiance, and on a MARCHED relight (mode bit 1) the accumulator restarts.
// REDUCE (mode 2): per CLUSTER of 4^3 bricks of a cascade that moved this frame, its bricks' largest move - a brick's
// injection's absolute move (state word 7) PLUS its pass's (word 4) where it was passed this frame: the pass gathers
// from the total the delta already wrote, so a voxel's total moved by at most the sum (PHOTON-7 stage 5's read A2
// (ii); the larger of the two under-bounded it, the feedback's ACCUM the same) - the box of the bricks that moved, and
// THE MOVED FLUX: the sum of the bricks' moved flux, the sum over their voxels of |move| x a^2 (PhotonBrickFeedback_cs.glsl's
// words 8 and 9, fixed point; a brick's bit 31 - a non-finite move - makes the cluster's flux unbounded), in TWO RECORDS:
// the bricks not wholly inside the next finer cascade's box, and those inside it (stage 5's read W1: since the world
// shells a reader reads THIS cascade inside the finer box too - a probe's ray past the finer cascade's shell end, a
// diffuse cone past its footprint's hand-over - so an inner brick is skipped only for a receiver every point of whose
// box lies within the finer cascade's shell end of the whole record: caInnerHi.w, the host's per reader; 0 for the
// cards, whose diffuse cones hand over within ~1.7 finer cells). No atomics: deterministic. THE BOUND per cluster
// is the smaller of the largest move over the moved box's share, m min( 1, S / ( pi d^2 ) ) (and, for a probe, over the
// depth moments' P), and THE FLUX's, flux sqrt(3) / ( pi d^2 ): a voxel (a cube of side a) seen from any point subtends
// at most sqrt(3) a^2 / d_v^2 (its largest silhouette; the limit is reached only far off along a diagonal) and d_v >= d -
// the physics written as the bound (PHOTON-7 stage 5: the largest move over the whole box was the field's slack (b), 22x
// for a small lamp - the max against the mean of the moves, not the box's geometry: a sub-box changed no restart).
// ACCUMULATE (mode 0): one thread a resident card - each cluster's largest move weighted by a BOUND of the share of the
// card's gather the moved bricks can fill, min( 1, S / ( pi d^2 ) ): S the moved box's largest silhouette (a box of sides
// x, y, z shows at most xy + yz + zx), d the card box's nearest distance to it (1 for boxes that touch; occlusion
// ignored) - the moved bricks' shares are solid angles inside the box's, so their sum is bounded by it and the largest
// move times it bounds the change - times the card's sensitivity. A card at a code or more is LISTED (the report, read
// back by the host, which relights its indirect).
// CLEAR (mode 1): the report's count, once a frame before the first accumulation.
// CARD TO CARD (mode 3; PHOTON-7 stage 10, 10-2c - at the ray tier a card's indirect is gathered by TLAS rays that read
// OTHER CARDS, rq_card_gather.comp): one thread a resident card - each card its last gather READ (its block of the
// read lists: the read card's slot and the largest share of one texel's rays it answered) times that card's MOVE this
// frame (the relight's largest change of its radiance, JahCardLight_cs.glsl) bounds the change of every texel of the
// reader (a texel's gather is the mean of its rays: a read card moved by m moves it by at most m times its share), times
// the reader's sensitivity, into its would-move - the same accumulator and the same listing as the voxels' moves, the
// list's terms summed IN THE KEYS' ORDER (a key's place in the block is its insertion's, an atomic's order). The Jacobi
// iteration of the cards' bounces, stopped by the code: a card relists while what it read moved a code of it. A list
// that overflowed (more cards read than it holds: WHICH cards it holds is an atomic's order) takes EVERY moved card
// through the geometric bound (below), its held ones too.
// CLEAR THE MOVES (mode 4): every slot's move, after mode 3 read them (a move is one frame's).
// THE FIELD'S VARIANT (jah_field_mark, Jahshaka/FieldAccumulate; PHOTON-7 stage 5, 5-4): the receivers are the irradiance
// field's probes (points: zero half extents; the state is the field's marks, its sensitivity the generation job's), and
// the bound takes VISIBILITY from the probe's own depth moments (DDGI's own test, the atlas the reader's Chebyshev reads):
// the cosine-weighted share of the probe's rays that pass the moved box's distance d is at most the one-sided Chebyshev
// bound sigma^2 / ( sigma^2 + ( d - mu )^2 ) of the depth texel facing the box (d past mu; capped at that TEXEL'S own
// stored bound - a ray past it may reach anything), over the cosine at the box's edge from that texel (every irradiance texel's
// share of the box's visible rays is at most that) - so the weight is min( 1, S / ( pi d^2 ), that bound): a sealed
// room's probes, every ray ending at its walls, are not reached by a neighbour's moves. AND IT COUNTS PER TEXEL: each of
// the probe's irradiance texels t changes by at most the move x the weight x the largest cosine between t's direction and
// the box's directions (t's value is the cosine-weighted mean of what its lobe sees: a texel facing away is not moved),
// over t's OWN code (1 % of its value, read from the atlas) - and the frame adds the largest over t to the probe's one
// accumulator (a sum of each frame's largest bounds the largest of the per-texel sums: conservative, no state per texel).
// A dark texel is moved only by what it faces.
// No at-sign in any comment of this file (the Hlms parser reads them).
@insertpiece( SetCrossPlatformSettings )

@property( jah_card_sens )
	vulkan_layout( ogre_t0 ) uniform texture2D cardRadiance;
	vulkan_layout( ogre_t1 ) uniform texture2D cardDepth;
	// THE RELIGHT LIST (OgreSurfaceCache.cpp relightCards): nine vec4 a card, the first (atlasX, atlasY, size, mode)
	layout( std430, ogre_U0 ) restrict readonly buffer csRelightLayout { vec4 csRelights[]; };
	layout( std430, ogre_U1 ) restrict coherent buffer csSlotLayout { uint csSlot[]; };
@else
	@property( jah_field_mark )
		// the field's depth moments (RG32F: the mean distance and its square, in grid units), one bordered tile a probe,
		// and its irradiance (RGBA16F: each texel's value, its own code)
		vulkan_layout( ogre_t0 ) uniform texture2D fieldDepth;
		vulkan_layout( ogre_t1 ) uniform texture2D fieldIrrad;
	@end
	layout( std430, ogre_U0 ) restrict readonly buffer caStateLayout { uint caState[]; };
	// THE RESIDENT CARDS: two vec4 a card - ( centre.xyz, the slot as uint bits ), ( half extents.xyz, 0 )
	// (MODE 3 binds THE READ LISTS at U0 - rq_card_gather.comp's blocks - and THE MOVES at U4)
	layout( std430, ogre_U1 ) restrict readonly buffer caCardLayout { vec4 caCards[]; };
	layout( std430, ogre_U2 ) restrict coherent buffer caSlotLayout { uint caSlot[]; };
	layout( std430, ogre_U3 ) restrict coherent buffer caReportLayout { uint caReport[]; };
	// THE CLUSTERS, the reduce's output: two records of three words each (outside the next finer cascade's box, then
	// inside it) - the largest move (float bits), the moved bricks' box inside the cluster (lo xyz, hi xyz: 3 bits each,
	// bricks, hi exclusive) and the moved flux (float bits, radiance x m^2; +inf: unbounded)
	layout( std430, ogre_U4 ) restrict coherent buffer caClusterLayout { uint caCluster[]; };
@end

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

vulkan( layout( ogre_P0 ) uniform Params { )
	// x: the mode (0 accumulate, 1 clear, 2 reduce); y: the cards (accumulate) or the relight entries (sensitivity); z:
	// the report's capacity
	uniform uint4 caMode;
	// the cascade's box corner (world, xyz) and brick side (metres, w)
	uniform float4 caSelf;
	// its bricks per axis (xyz) and their count (w)
	uniform uint4 caDims;
	// the next finer cascade's box (world, xyz; caInnerLo.w = 1 when there is one) and caInnerHi.w the distance from a
	// receiver past which its reads use THIS cascade inside that box (metres; the finer cascade's shell end)
	uniform float4 caInnerLo;
	uniform float4 caInnerHi;
@property( jah_field_mark )
	// the field (jah_field_mark): x the probes per row of slots (numProbes.x), y numProbes.x x numProbes.y, z the depth
	// tile's interior resolution, w the depth atlas's width; and the probe spacing per axis (metres, xyz) and the depth's
	// own bound in cascade 0's normalised units (w, 1.414: a texel's bound in grid units is w x |d| . numProbes)
	uniform uint4 caField;
	uniform float4 caSpacing;
	// x the irradiance tile's interior resolution, y the irradiance atlas's width, z numProbes.z
	uniform uint4 caField2;
@end
vulkan( }; )

#define CA_PI 3.14159265358979
#define CA_ONE 65536.0
#define CA_RAN 512u

@property( jah_card_sens )
shared uint csMinBits;
@end

@property( jah_field_mark )
// A probe texel's direction (the generation job's jahIfdTexelDir, IrradianceFieldGenFromVct_piece_cs.any: the same
// octahedral layout)
vec3 jahFieldTexelDir( uvec2 xy, uint res )
{
	vec2 f = ( vec2( xy ) + 0.5 ) * ( 2.0 / float( res ) ) - 1.0;
	vec3 n = vec3( f.x, f.y, 1.0 - abs( f.x ) - abs( f.y ) );
	float t = clamp( -n.z, 0.0, 1.0 );
	n.x += n.x >= 0.0 ? -t : t;
	n.y += n.y >= 0.0 ? -t : t;
	return normalize( n );
}
@end

void main()
{
@property( jah_card_sens )
	// SENSITIVITY: the workgroup is the relight entry
	const uint e = gl_WorkGroupID.x;
	if( e >= caMode.y )
		return;
	const vec4 rect = csRelights[9u * e];
	const uint ax = uint( rect.x ), ay = uint( rect.y ), size = uint( rect.z ), mode = uint( rect.w );
	const uint lid = gl_LocalInvocationIndex;
	if( lid == 0u )
		csMinBits = 0x7F7FFFFFu;   // the largest finite float: no lit texel yet
	memoryBarrierShared();
	barrier();
	uint lowest = 0x7F7FFFFFu;
	for( uint k = lid; k < size * size; k += 64u )
	{
		const ivec2 at = ivec2( int( ax + k % size ), int( ay + k / size ) );
		if( !( texelFetch( cardDepth, at, 0 ).x > 0.0 ) )
			continue;
		const vec3 L = texelFetch( cardRadiance, at, 0 ).xyz;
		const float l = max( max( max( L.x, L.y ), L.z ), 0.0 );
		lowest = min( lowest, floatBitsToUint( l ) );   // (a non-negative float's bits order as the float does)
	}
	atomicMin( csMinBits, lowest );
	memoryBarrierShared();
	barrier();
	if( lid == 0u )
	{
		const uint s = 4u * ( ( ay >> 4u ) * 128u + ( ax >> 4u ) );
		const float code = 0.01 * max( uintBitsToFloat( csMinBits ), 5.9604645e-6 );
		csSlot[s] = csMinBits == 0x7F7FFFFFu ? 0u : floatBitsToUint( 1.0 / code );
		if( ( mode & 1u ) != 0u )
		{
			csSlot[s + 1u] = 0u;
			csSlot[s + 2u] = 0u;
			csSlot[s + 3u] = 0u;
		}
	}
@else
	if( caMode.x == 1u )
	{
		if( gl_GlobalInvocationID.x == 0u )
			caReport[0] = 0u;
		return;
	}
	if( caMode.x == 4u )
	{
		// THE MOVES CLEARED (mode 4): every slot (caMode.y = the slots)
		if( gl_GlobalInvocationID.x < caMode.y )
			caCluster[gl_GlobalInvocationID.x] = 0u;
		return;
	}
	// THE CLUSTERS: 4^3 bricks each, per axis ceil( bricks / 4 )
	const uvec3 cd = ( caDims.xyz + 3u ) / 4u;
	const uint clusters = cd.x * cd.y * cd.z;
	const float side = caSelf.w;
	if( caMode.x == 2u )
	{
		const uint q = gl_GlobalInvocationID.x;
		if( q >= clusters )
			return;
		const uvec3 cq = uvec3( q % cd.x, ( q / cd.x ) % cd.y, q / ( cd.x * cd.y ) );
		// TWO RECORDS a cluster (stage 5's read W1): [0] the bricks NOT wholly inside the next finer cascade's box, [1]
		// those inside it - read by a receiver only from the inner cascade's shell on (ACCUMULATE's distance test)
		float most[2] = { 0.0, 0.0 }, fluxUnits[2] = { 0.0, 0.0 };
		bool fluxOpen[2] = { false, false };
		uvec3 mlo[2] = { uvec3( 4u, 4u, 4u ), uvec3( 4u, 4u, 4u ) }, mhi[2] = { uvec3( 0u, 0u, 0u ), uvec3( 0u, 0u, 0u ) };
		for( uint z = 0u; z < 4u; ++z )
			for( uint y = 0u; y < 4u; ++y )
				for( uint x = 0u; x < 4u; ++x )
				{
					const uvec3 bb = cq * 4u + uvec3( x, y, z );
					if( any( greaterThanEqual( bb, caDims.xyz ) ) )
						continue;
					const uint b = bb.x + caDims.x * ( bb.y + caDims.y * bb.z );
					const uint f = caState[10u * b];
					const bool ran = ( f & CA_RAN ) != 0u;
					const float m = ( ran ? uintBitsToFloat( caState[10u * b + 4u] ) : 0.0 ) +
									uintBitsToFloat( caState[10u * b + 7u] );
					if( !( m > 0.0 ) )
						continue;
					uint r = 0u;
					if( caInnerLo.w > 0.5 )
					{
						const vec3 lo = caSelf.xyz + vec3( bb ) * side;
						if( all( greaterThanEqual( lo, caInnerLo.xyz ) ) && all( lessThanEqual( lo + side, caInnerHi.xyz ) ) )
							r = 1u;
					}
					most[r] = max( most[r], m );
					mlo[r] = min( mlo[r], uvec3( x, y, z ) );
					mhi[r] = max( mhi[r], uvec3( x, y, z ) + 1u );
					const uint fp = ran ? caState[10u * b + 8u] : 0u, fi = caState[10u * b + 9u];
					fluxOpen[r] = fluxOpen[r] || ( ( fp | fi ) & 0x80000000u ) != 0u;
					fluxUnits[r] += float( fp & 0x7FFFFFFFu ) + float( fi & 0x7FFFFFFFu );
				}
		// the flux in radiance x m^2: the words' unit (1/64 of radiance) times a voxel's face (an eighth of a brick, squared),
		// rounded up by 2^-16 (the float sum of at most 128 words and the products round to nearest: under 2^-16 together);
		// unbounded where a brick marked a non-finite move
		const float vox = side * 0.125;
		for( uint r = 0u; r < 2u; ++r )
		{
			caCluster[6u * q + 3u * r] = floatBitsToUint( most[r] );
			caCluster[6u * q + 3u * r + 1u] =
				most[r] > 0.0 ? ( mlo[r].x | ( mlo[r].y << 3u ) | ( mlo[r].z << 6u ) | ( mhi[r].x << 9u ) |
								  ( mhi[r].y << 12u ) | ( mhi[r].z << 15u ) )
							  : 0u;
			caCluster[6u * q + 3u * r + 2u] =
				fluxOpen[r] ? 0x7F800000u
							: floatBitsToUint( fluxUnits[r] * ( 1.0 / 64.0 ) * vox * vox * ( 1.0 + 1.0 / 65536.0 ) );
		}
		return;
	}
	const uint k = gl_GlobalInvocationID.x;
	if( k >= caMode.y )
		return;
	const vec4 c0 = caCards[2u * k], c1 = caCards[2u * k + 1u];
	const vec3 centre = c0.xyz, halfExt = c1.xyz;
	const uint s = 4u * floatBitsToUint( c0.w );
	const float sens = uintBitsToFloat( caSlot[s] );
	const bool want = sens > 0.0 && caSlot[s + 3u] == 0u;
	if( !want )
		return;
	float sum = 0.0;   // codes
	uint reportBits = 0u;   // a listing's origin in its report word (bit 30: card to card; bit 31: the bound took part)
@property( jah_field_mark )
@else
	if( caMode.x == 3u )
	{
		// CARD TO CARD (mode 3, the head of this file): the reader's block (1 + 2 x 32 words) of the read lists at U0 -
		// each card it read, its exact share times its move
		reportBits = 0x40000000u;
		const uint slotB = floatBitsToUint( c0.w );
		const uint base = slotB * 65u;
		const bool overflowed = ( caState[base] & 1u ) != 0u;
		// THE LIST IN ITS KEYS' ORDER, never the block's: a key's place in the block is where its insertion probed (an
		// atomic's order under a collision), and a float sum over an atomic's order put a borderline listing on another
		// frame in another process (gi.walk's glass, L2: the arms 1585 px apart at the hold) - the voxels' own rule
		// (PhotonBrickFeedback_cs.glsl ACCUM)
		uint last = 0u;
		for( uint n = 0u; n < 32u && !overflowed; ++n )
		{
			uint key = 0xFFFFFFFFu, at = 0u;
			for( uint e = 0u; e < 32u; ++e )
			{
				const uint k = caState[base + 1u + 2u * e];
				if( k > last && k < key )
				{
					key = k;
					at = e;
				}
			}
			if( key == 0xFFFFFFFFu )
				break;
			last = key;
			if( key > 16384u )
				continue;
			const float m = uintBitsToFloat( caCluster[key - 1u] );
			if( !( m > 0.0 ) )
				continue;
			sum += sens * m * ( float( caState[base + 2u + 2u * at] ) * ( 1.0 / 65535.0 ) );
		}
		// AN OVERFLOWED LIST (the lead's ruling: the GEOMETRIC BOUND, the voxels' own): EVERY moved card - the list's held
		// ones too: WHICH cards it holds is an atomic's order - its move times min( 1, S / ( pi d^2 ) ), S its box's
		// largest silhouette, d the gap between the two boxes (occlusion ignored): a cosine-weighted mean's share of a box
		// is at most its solid angle over pi, so the sum bounds the change of every texel's integral, never misses a move
		// and is the same in every process (the cards in the host's order)
		if( overflowed )
		{
			float bound = 0.0;
			for( uint j = 0u; j < caMode.y; ++j )
			{
				const vec4 a0 = caCards[2u * j], a1 = caCards[2u * j + 1u];
				const uint slotA = floatBitsToUint( a0.w );
				if( slotA >= 16384u )
					continue;
				const float m = uintBitsToFloat( caCluster[slotA] );
				if( !( m > 0.0 ) )
					continue;
				const vec3 ext = 2.0 * a1.xyz;
				const float area = ext.x * ext.y + ext.y * ext.z + ext.z * ext.x;
				const vec3 gap = max( abs( centre - a0.xyz ) - ( halfExt + a1.xyz ), vec3( 0.0, 0.0, 0.0 ) );
				const float d2 = dot( gap, gap );
				bound += m * ( d2 > 0.0 ? min( 1.0, area / ( CA_PI * d2 ) ) : 1.0 );
			}
			if( bound > 0.0 )
				reportBits |= 0x80000000u;
			sum += sens * bound;
		}
		else
		{
			// THE ARRIVALS: a card captured this frame (up to the batch's eight, their records in caDims and caSelf's
			// bits) - what read its instance before read the voxels there, and no list holds it - its brightest reported
			// radiance through the same geometric bound, for every reader whose list does not hold it
			float bound = 0.0;
			for( uint f = 0u; f < 8u; ++f )
			{
				const uint j = f < 4u ? caDims[f] : floatBitsToUint( caSelf[f - 4u] );
				if( j >= caMode.y )
					continue;
				const vec4 a0 = caCards[2u * j], a1 = caCards[2u * j + 1u];
				const uint slotA = floatBitsToUint( a0.w );
				if( slotA >= 16384u )
					continue;
				const float m = uintBitsToFloat( caCluster[slotA] );
				if( !( m > 0.0 ) )
					continue;
				bool listed = false;
				for( uint e = 0u; e < 32u && !listed; ++e )
					listed = caState[base + 1u + 2u * e] == slotA + 1u;
				if( listed )
					continue;
				const vec3 ext = 2.0 * a1.xyz;
				const float area = ext.x * ext.y + ext.y * ext.z + ext.z * ext.x;
				const vec3 gap = max( abs( centre - a0.xyz ) - ( halfExt + a1.xyz ), vec3( 0.0, 0.0, 0.0 ) );
				const float d2 = dot( gap, gap );
				bound += m * ( d2 > 0.0 ? min( 1.0, area / ( CA_PI * d2 ) ) : 1.0 );
			}
			if( bound > 0.0 )
				reportBits |= 0x80000000u;
			sum += sens * bound;
		}
	}
	else
	{
@end
@property( jah_field_mark )
	// THE PROBE'S TEXELS (the head of this file): each irradiance texel's direction and its own code
	const uint irrRes = min( caField2.x, 8u );
	const uint nTex = irrRes * irrRes;
	vec3 texDir[64];
	float texCode[64], texSum[64];
	{
		const uint slotT = floatBitsToUint( c0.w );
		const uint borderedT = irrRes + 2u;
		const uvec2 tileT = uvec2( ( slotT * borderedT ) % caField2.y, ( ( slotT * borderedT ) / caField2.y ) * borderedT ) +
							uvec2( 1u, 1u );
		for( uint t = 0u; t < nTex; ++t )
		{
			const uvec2 xy = uvec2( t % irrRes, t / irrRes );
			const vec3 v = texelFetch( fieldIrrad, ivec2( tileT + xy ), 0 ).xyz;
			texCode[t] = 0.01 * max( max( max( v.x, v.y ), v.z ), 5.9604645e-6 );
			texDir[t] = jahFieldTexelDir( xy, irrRes );
			texSum[t] = 0.0;
		}
	}
@end
	for( uint qr = 0u; qr < 2u * clusters; ++qr )
	{
		const uint q = qr >> 1u, rec = qr & 1u;
		const float m = uintBitsToFloat( caCluster[6u * q + 3u * rec] );
		if( !( m > 0.0 ) )
			continue;
		const uint bx = caCluster[6u * q + 3u * rec + 1u];
		const float flux = uintBitsToFloat( caCluster[6u * q + 3u * rec + 2u] );
		const uvec3 mlo = uvec3( bx & 7u, ( bx >> 3u ) & 7u, ( bx >> 6u ) & 7u );
		const uvec3 mhi = uvec3( ( bx >> 9u ) & 7u, ( bx >> 12u ) & 7u, ( bx >> 15u ) & 7u );
		const uvec3 cq = uvec3( q % cd.x, ( q / cd.x ) % cd.y, q / ( cd.x * cd.y ) );
		const vec3 lo = caSelf.xyz + vec3( cq * 4u + mlo ) * side;
		const vec3 ext = vec3( mhi - mlo ) * side;
		// THE INNER RECORD (the bricks inside the next finer cascade's box) reaches the receiver only where its reads
		// use THIS cascade there: from the finer cascade's shell end on (caInnerHi.w, metres from the receiver - the
		// narrow rays' world shell D_(j-1), jah_voxel_march.glsl). Skipped only for a receiver whose box lies inside the
		// finer box (a march starting outside it starts in a coarser cascade and reads this one at any distance; the
		// boxes are convex, so the segment to the record stays inside) and when the record's box lies wholly within
		// that distance of every point of the receiver's box.
		if( rec == 1u && all( greaterThanEqual( centre - halfExt, caInnerLo.xyz ) ) &&
			all( lessThanEqual( centre + halfExt, caInnerHi.xyz ) ) &&
			length( abs( centre - ( lo + 0.5 * ext ) ) + halfExt + 0.5 * ext ) < caInnerHi.w )
			continue;
		const float area = ext.x * ext.y + ext.y * ext.z + ext.z * ext.x;   // the moved box's largest silhouette
		const vec3 gap = max( abs( centre - ( lo + 0.5 * ext ) ) - ( halfExt + 0.5 * ext ), vec3( 0.0, 0.0, 0.0 ) );
		const float d2 = dot( gap, gap );
		float weight = d2 > 0.0 ? min( 1.0, area / ( CA_PI * d2 ) ) : 1.0;
@property( jah_field_mark )
		if( d2 > 0.0 && weight > 0.0 )
		{
			// THE VISIBILITY (the head of this file): the depth texel facing the moved box's centre
			const vec3 toBox = ( lo + 0.5 * ext ) - centre;
			const float toLen = length( toBox );
			const vec3 dirB = toBox / max( toLen, 1e-6 );
			// the depth texel (the generation job's octahedral layout, jahIfdTexelDir's inverse)
			const uint res = caField.z;
			vec2 pOct = dirB.xy / ( abs( dirB.x ) + abs( dirB.y ) + abs( dirB.z ) );
			if( dirB.z < 0.0 )
				pOct = ( 1.0 - abs( pOct.yx ) ) * vec2( pOct.x >= 0.0 ? 1.0 : -1.0, pOct.y >= 0.0 ? 1.0 : -1.0 );
			const uvec2 txy = uvec2( min( floor( ( pOct * 0.5 + 0.5 ) * float( res ) ), vec2( float( res - 1u ) ) ) );
			// ...its own direction (jahIfdTexelDir), and THE LEAST COSINE between it and the box's directions: the box's
			// angular radius from the probe plus the texel direction's own offset from the box's centre
			vec2 fT = ( vec2( txy ) + 0.5 ) * ( 2.0 / float( res ) ) - 1.0;
			vec3 nT = vec3( fT.x, fT.y, 1.0 - abs( fT.x ) - abs( fT.y ) );
			const float tT = clamp( -nT.z, 0.0, 1.0 );
			nT.x += nT.x >= 0.0 ? -tT : tT;
			nT.y += nT.y >= 0.0 ? -tT : tT;
			nT = normalize( nT );
			const float rad = 0.5 * length( ext );
			const float boxAngle = toLen > rad ? asin( clamp( rad / toLen, 0.0, 1.0 ) ) : CA_PI;
			const float offAngle = acos( clamp( dot( nT, dirB ), -1.0, 1.0 ) );
			const float edgeCos = cos( min( boxAngle + offAngle, 0.5 * CA_PI ) );
			if( edgeCos > 1e-3 )
			{
				const uint slot = floatBitsToUint( c0.w );
				const uint bordered = res + 2u;
				const uvec2 tile = uvec2( ( slot * bordered ) % caField.w, ( ( slot * bordered ) / caField.w ) * bordered ) +
								   uvec2( 1u, 1u );
				const vec2 mom = texelFetch( fieldDepth, ivec2( tile + txy ), 0 ).xy;
				// the nearest point of the box in grid units (the depth's), capped at THIS TEXEL'S stored bound: the
				// generation job bounds a ray's depth at cascade 0's diagonal along ITS direction, 1.414 x |d| . numProbes
				// (IrradianceFieldGenFromVct), so on a 32 x 16 x 16 field the bound along y or z is 22.6 grid units, not
				// the largest count's 45.3 - a box past the texel's own bound reads at the bound, never as hidden behind
				// it (stage 5's read B2)
				const vec3 numProbesF = vec3( float( caField.x ), float( caField.y / caField.x ), float( caField2.z ) );
				const float capT = caSpacing.w * dot( abs( nT ), numProbesF );
				const float dg = min( length( gap / caSpacing.xyz ), capT );
				const float mu = mom.x, var = max( mom.y - mom.x * mom.x, 0.0 ) + 1e-6;
				float pass = 1.0;
				if( dg > mu )
					pass = var / ( var + ( dg - mu ) * ( dg - mu ) );
				weight = min( weight, min( 1.0, pass / edgeCos ) );
			}
		}
		// THE CLUSTER'S AMOUNT: the smaller of its largest move over its share (the visibility in it, for a probe) and its
		// flux's bound (the head of this file)
		const float amount = d2 > 0.0 ? min( m * weight, flux * ( 1.7320509 / ( CA_PI * d2 ) ) ) : m * weight;
		// PER TEXEL: the box's nearest direction to each texel's (a probe inside the box: every direction)
		{
			const vec3 toBoxT = ( lo + 0.5 * ext ) - centre;
			const float lenT = length( toBoxT );
			const vec3 dirT = toBoxT / max( lenT, 1e-6 );
			const float radT = 0.5 * length( ext );
			const float angBox = d2 > 0.0 && lenT > radT ? asin( clamp( radT / lenT, 0.0, 1.0 ) ) : CA_PI;
			for( uint t = 0u; t < nTex; ++t )
			{
				const float a = max( acos( clamp( dot( texDir[t], dirT ), -1.0, 1.0 ) ) - angBox, 0.0 );
				if( a < 0.5 * CA_PI )
					texSum[t] += amount * cos( a ) / texCode[t];
			}
		}
@else
		// (the cluster's amount, as the field's: the smaller of the largest move over its share and its flux's bound)
		sum += sens * ( d2 > 0.0 ? min( m * weight, flux * ( 1.7320509 / ( CA_PI * d2 ) ) ) : m * weight );
@end
	}
@property( jah_field_mark )
@else
	}   // (mode 0's clusters)
@end
@property( jah_field_mark )
	for( uint t = 0u; t < nTex; ++t )
		sum = max( sum, texSum[t] );
@end
	if( !( sum > 0.0 ) )
		return;
	// THE FIXED POINT, ROUNDED ONCE (PhotonBrickFeedback_cs.glsl's rule): the sum in units plus the fraction carried,
	// floored, the new fraction kept; saturating.
	uint acc = caSlot[s + 1u];
	const float units = sum * CA_ONE + uintBitsToFloat( caSlot[s + 2u] );
	if( !( units < 4294967040.0 ) )
	{
		acc = 0xFFFFFFFFu;
		caSlot[s + 2u] = 0u;
	}
	else
	{
		const float whole = floor( units );
		caSlot[s + 2u] = floatBitsToUint( units - whole );
		const uint q = uint( whole );
		acc = q > 0xFFFFFFFFu - acc ? 0xFFFFFFFFu : acc + q;
	}
	caSlot[s + 1u] = acc;
	// A CODE OR MORE: listed once
	if( acc >= uint( CA_ONE ) )
	{
		const uint at = atomicAdd( caReport[0], 1u );
		if( at < caMode.z )
		{
			caReport[1u + at] = ( s >> 2u ) | reportBits;
			caSlot[s + 3u] = 1u;
		}
	}
@end
}
