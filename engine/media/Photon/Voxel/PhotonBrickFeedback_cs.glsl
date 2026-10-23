@insertpiece( SetCrossPlatformSettings )

// PHOTON-7 stage 4: THE BOUNCES BY FEEDBACK - the per-brick state of the voxel light's one solver
// (PhotonVoxelLighting::feedback*). A cascade's total is never re-solved from its direct term: each frame ONE bounce
// pass (LightVctBounceInject, from the CURRENT total) runs over the ACTIVE 8^3 bricks only, through an indirect
// dispatch these modes size, and a brick STOPS once its last pass moved it by less than (1 - r) / r codes, r its own
// measured contraction (the ratio of its last two moves): the tail of its series is then under one code. A stopped
// brick RESUMES when its direct term changes, when a listed region meets it (the host's boxes), or when the changes
// its gather can see, ACCUMULATED since its stop, would move it by (1 - r) codes - its error with its own series'
// gain 1 / (1 - r) is then a code. A code is 1 % of a voxel's own radiance per channel over the float store's
// absolute floor (Types.h kGiBrickFloor), never the display's: this is a world cache (L1).
//
// THE BUDGET (PHOTON-7 stage 4's hitch fix; Types.h GiQualityFacts::feedbackBrickPasses): the passes a frame are a COUNT
// per tier, spent over the chain innermost cascade first; an active brick the count does not reach is CARRIED (active, no
// pass, its series untouched) and taken first by its cascade's next selection, which starts where the last one stopped
// (a rotating cursor over the bricks' index): every active brick is passed once in ceil(active / its share) frames, and
// the choice is a function of the state alone (a scan in index order, never an atomic's order).
// THE STATE, 8 words a brick (x fastest over the cascade's bricks):
//   [0] bit 0 active; bits 1..8 the passes since it was (re)activated (saturating at 255); bit 9 PASSED THIS FRAME (the
//       selection's pick, cleared by POST: an active brick without it was carried)
//   [1] its last pass's move (relative: the brick mask's word 1, float bits)
//   [2] r, its measured contraction (float bits; kept over a stop for the resume test)
//   [3] the would-move ACCUMULATED since its stop, in FIXED POINT: FB_ACC_ONE units a code (uint, saturating). It
//       decides the resume, so it is an integer sum across the frames' accumulations - exact, and each accumulation's
//       float sum runs over a list in the selection's brick-index order (SELECT), never an atomic's order (a float sum
//       over an atomic's order put a borderline brick's resume on another frame in another process: gi.walk L1/L2).
//   [4] this frame's pass: the most a voxel's total moved, absolute radiance (float bits; atomicMax by the pass)
//   [5] the brick's SENSITIVITY: the most of its voxels' re-emission over their own code, codes per unit of radiance
//       an input moves (float bits; atomicMax by the pass, kept over a stop)
//   [6] the FRACTION of a unit the last accumulation's rounding left (float bits, in units: carried into the next, so
//       the fixed point rounds once a brick and never biases - PHOTON-7 stage 4's read R6)
//   [7] THIS FRAME'S INJECTIONS' move: the most a voxel's total changed by a direct term's delta, absolute radiance (float
//       bits; atomicMax by LightInjection, cleared by CLEAR) - the change a pass's move [4] cannot see (a pass gathers
//       from the total the delta already wrote), read by the cards' accumulation (PHOTON-7 stage 5, JahCardIndirect)
// THE ARGUMENTS, 16 words: [0..2] the bounce's and the copy's groups (8 x the bricks: group y is the brick), [4..6]
// the directional levels' (1 x the bricks), [8] the bricks listed (passed) this frame, [9] the bricks active at the
// selection (passed or carried), [12] the bricks active after the frame, [13] stops, [14] resumes, [15] the bricks whose
// pass count saturated (a door, never a condition).
// THE CHAIN'S WORDS (U8, the innermost cascade's buffer, bound by every cascade of the chain): [0] the brick-passes left
// this frame, [1 + k] cascade k's selection cursor (the brick index its next selection starts from).
// THE MODES (fbDims.w): 0 CLEAR (the frame's mask and arguments), 1 PRE (activation), 5 SELECT (the frame's list under
// the budget: ONE workgroup), 2 POST (the stop and resume rules), 3 ACCUM (one source cascade's moves into the stopped
// bricks' would-move), 4 SHIFT (a scroll commit's state moved with the store). The copy job (jah_brick_copy) copies the
// pass's target back into the total over the listed bricks.

@property( jah_brick_copy )
	layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
	uniform restrict readonly image3D fbFrom;
	layout( vulkan( ogre_u1 ) vk_comma @insertpiece(uav1_pf_type) )
	uniform restrict writeonly image3D fbTo;
	layout( std430, ogre_U2 ) restrict readonly buffer fbListLayout { uint fbList[]; };
@else
	layout( std430, ogre_U0 ) restrict coherent buffer fbStateLayout { uint fbState[]; };
	layout( std430, ogre_U1 ) restrict coherent buffer fbMaskLayout { uint fbMask[]; };
	layout( std430, ogre_U2 ) restrict coherent buffer fbListLayout { uint fbList[]; };
	layout( std430, ogre_U3 ) restrict coherent buffer fbArgsLayout { uint fbArgs[]; };
	layout( std430, ogre_U4 ) restrict readonly buffer fbSrcListLayout { uint fbSrcList[]; };
	layout( std430, ogre_U5 ) restrict readonly buffer fbSrcStateLayout { uint fbSrcState[]; };
	layout( std430, ogre_U6 ) restrict readonly buffer fbSrcArgsLayout { uint fbSrcArgs[]; };
	layout( std430, ogre_U7 ) restrict readonly buffer fbOldStateLayout { uint fbOldState[]; };
	layout( std430, ogre_U8 ) restrict coherent buffer fbChainLayout { uint fbChain[]; };
@end

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

@property( !jah_brick_copy )
vulkan( layout( ogre_P0 ) uniform Params { )
	// xyz: this cascade's bricks per axis; w: the mode
	uniform uint4 fbDims;
	// x: PRE activates every brick; y: the activation boxes in fbBoxLo / fbBoxHi; z: ACCUM's source is an OUTER cascade
	// (its bricks inside this cascade's box are skipped: this cascade's cones read their own volume there); w: ACCUM's
	// source is this cascade (its list, state and arguments are the own buffers, not the source's).
	// SELECT: x the frame's first selection (the chain's budget is set to y first), y the budget, z this cascade's index
	// in the chain (its cursor's word)
	uniform uint4 fbFlags;
	// PRE: the boxes of bricks [lo, hi) a listed region meets
	uniform uint4 fbBoxLo[16];
	uniform uint4 fbBoxHi[16];
	// ACCUM: this cascade's box corner (world, xyz) and brick (metres, w); the source's; the source's bricks per axis
	uniform float4 fbSelf;
	uniform float4 fbSrc;
	uniform uint4 fbSrcDims;
	// SHIFT: the new brick's cells read the old store's cells + fbShift (cells; the commit's shift)
	uniform int4 fbShift;
vulkan( }; )
@end

#define FB_PI 3.14159265358979
// THE RESUME'S FIXED POINT: 2^16 units a code. A code (1 % of the voxel's own radiance over the store's floor) is the
// rule's own unit and the threshold (1 - r) codes is at least 0.001 of one (r <= 0.999), so a unit is 1/65 of the
// finest threshold; an accumulation is FLOORED once a brick with the fraction carried (word 6: unbiased - a term
// rounded up per source brick added a unit a frame per active brick, a resume by age after ~165 frames of activity
// anywhere, PHOTON-7 stage 4's read R6) and the 32-bit sum saturates at 65,536 codes, far past every threshold.
#define FB_ACC_ONE 65536.0

// THE SELECTION'S SCRATCH (SELECT, one workgroup of 64): the chunk's prefix sums, the bricks taken so far, the budget,
// the cursor and the brick after the last one taken.
#define FB_RAN 512u
@property( !jah_brick_copy )
shared uint fbSScan[64];
shared uint fbSTaken;
shared uint fbSBudget;
shared uint fbSStart;
shared uint fbSNext;
@end

uint fbPack( uint3 b ) { return b.x | ( b.y << 10u ) | ( b.z << 20u ); }
uint3 fbUnpack( uint p ) { return uint3( p & 1023u, ( p >> 10u ) & 1023u, p >> 20u ); }
float fbF( uint u ) { return uintBitsToFloat( u ); }
uint fbU( float f ) { return floatBitsToUint( f ); }

void main()
{
@property( jah_brick_copy )
	// THE COPY-BACK: what the pass wrote into its target becomes the total over the listed bricks (8 groups of 4^3 a
	// brick, the brick group y; the texture identities never change).
	const uint3 brick = fbUnpack( fbList[gl_WorkGroupID.y] );
	const uint sub = gl_WorkGroupID.x;
	const uint3 cell = brick * 8u + uint3( sub & 1u, ( sub >> 1u ) & 1u, sub >> 2u ) * 4u + gl_LocalInvocationID.xyz;
	imageStore( fbTo, int3( cell ), imageLoad( fbFrom, int3( cell ) ) );
@else
	const uint mode = fbDims.w;
	const uint numBricks = fbDims.x * fbDims.y * fbDims.z;
	const uint i = gl_LocalInvocationIndex + gl_WorkGroupID.x * 64u;
	if( mode == 0u )
	{
		// CLEAR: the frame's brick mask (both words) and the arguments.
		if( i == 0u )
		{
			for( uint k = 0u; k < 16u; ++k )
				fbArgs[k] = k == 0u ? 8u : ( ( k == 2u || k == 4u || k == 6u ) ? 1u : 0u );
		}
		if( i < numBricks )
		{
			fbMask[2u * i] = 0u;
			fbMask[2u * i + 1u] = 0u;
			fbState[8u * i + 7u] = 0u;
		}
		return;
	}
	if( mode == 5u )
	{
		// SELECT (one workgroup): the active bricks in index order from the cursor, the first `budget` of them listed
		// and marked passed (their pass's two words cleared for its atomicMax), the rest carried; the budget spent.
		const uint lid = gl_LocalInvocationIndex;
		if( lid == 0u )
		{
			if( fbFlags.x != 0u )
				fbChain[0] = fbFlags.y;
			fbSBudget = fbChain[0];
			fbSStart = numBricks > 0u ? fbChain[1u + fbFlags.z] % numBricks : 0u;
			fbSNext = fbSStart;
			fbSTaken = 0u;
		}
		memoryBarrierShared();
		barrier();
		for( uint base = 0u; base < numBricks; base += 64u )
		{
			const uint taken0 = fbSTaken;
			if( taken0 >= fbSBudget )
				break;
			const uint k = base + lid;
			const uint idx = ( fbSStart + k ) % numBricks;
			const bool act = k < numBricks && ( fbState[8u * idx] & 1u ) != 0u;
			fbSScan[lid] = act ? 1u : 0u;
			memoryBarrierShared();
			barrier();
			for( uint off = 1u; off < 64u; off <<= 1u )
			{
				const uint v = lid >= off ? fbSScan[lid - off] : 0u;
				memoryBarrierShared();
				barrier();
				fbSScan[lid] += v;
				memoryBarrierShared();
				barrier();
			}
			const uint pos = taken0 + fbSScan[lid] - 1u;
			const uint end = min( fbSBudget, taken0 + fbSScan[63] );
			if( act && pos < end )
			{
				fbList[pos] = fbPack( uint3( idx % fbDims.x, ( idx / fbDims.x ) % fbDims.y, idx / ( fbDims.x * fbDims.y ) ) );
				fbState[8u * idx] |= FB_RAN;
				fbState[8u * idx + 4u] = 0u;
				fbState[8u * idx + 5u] = 0u;
				if( pos + 1u == end )
					fbSNext = ( idx + 1u ) % numBricks;
			}
			memoryBarrierShared();
			barrier();
			if( lid == 0u )
				fbSTaken = end;
			memoryBarrierShared();
			barrier();
		}
		if( lid == 0u )
		{
			const uint taken = fbSTaken;
			fbChain[0] = fbSBudget - taken;
			fbChain[1u + fbFlags.z] = fbSNext;
			fbArgs[1] = taken;
			fbArgs[5] = taken;
			fbArgs[8] = taken;
		}
		return;
	}
	if( i >= numBricks )
		return;
	const uint3 b = uint3( i % fbDims.x, ( i / fbDims.x ) % fbDims.y, i / ( fbDims.x * fbDims.y ) );
	const uint s = 8u * i;
	uint flags = fbState[s];
	if( mode == 1u )
	{
		// PRE: a brick whose direct term changed this frame (the mask's word 0, the store's own bits compared), or that a
		// listed region meets, or every brick, (re)starts: its contraction is measured again from its next two passes.
		// The active bricks are counted; SELECT lists the ones the budget passes.
		bool start = fbFlags.x != 0u || fbMask[2u * i] != 0u;
		for( uint k = 0u; k < fbFlags.y && !start; ++k )
			start = all( greaterThanEqual( b, fbBoxLo[k].xyz ) ) && all( lessThan( b, fbBoxHi[k].xyz ) );
		if( start )
		{
			if( ( flags & 1u ) == 0u )
			{
				fbState[s + 3u] = 0u;
				fbState[s + 6u] = 0u;
			}
			flags = 1u;
		}
		if( ( flags & 1u ) != 0u )
			atomicAdd( fbArgs[9], 1u );
		fbState[s] = flags & ~FB_RAN;
		return;
	}
	if( mode == 2u )
	{
		// POST: an active brick takes its pass's move (the mask's word 1) and stops when the tail of its series is under
		// a code; a stopped one resumes when what it accumulated would leave it a code off. An active brick the budget
		// carried (no pass this frame) keeps its series as it stands.
		if( ( flags & 1u ) != 0u && ( flags & FB_RAN ) == 0u )
		{
		}
		else if( ( flags & 1u ) != 0u )
		{
			const float d = fbF( fbMask[2u * i + 1u] );
			const float prev = fbF( fbState[s + 1u] );
			uint n = ( flags >> 1u ) & 255u;
			if( n == 254u )
				atomicAdd( fbArgs[15], 1u );
			n = min( n + 1u, 255u );
			float r = fbF( fbState[s + 2u] );
			if( n >= 2u )
				r = prev > 0.0 ? clamp( d / prev, 0.0, 0.999 ) : ( d > 0.0 ? 0.999 : 0.0 );
			fbState[s + 1u] = fbU( d );
			fbState[s + 2u] = fbU( r );
			// (a move is the store's own: the pass reads back what it wrote, so a voxel at the store's fixed point moves 0)
			const bool stop = !( d > 0.0 ) || ( n >= 2u && 100.0 * d * r < 1.0 - r );
			if( stop )
			{
				flags = n << 1u;
				fbState[s + 3u] = 0u;
				fbState[s + 6u] = 0u;
				atomicAdd( fbArgs[13], 1u );
			}
			else
			{
				flags = 1u | ( n << 1u );
			}
		}
		else
		{
			const float accum = float( fbState[s + 3u] ) / FB_ACC_ONE;
			const float r = fbF( fbState[s + 2u] );
			if( accum >= 1.0 - r )
			{
				flags = 1u;
				fbState[s + 3u] = 0u;
				fbState[s + 6u] = 0u;
				atomicAdd( fbArgs[14], 1u );
			}
		}
		if( ( flags & 1u ) != 0u )
			atomicAdd( fbArgs[12], 1u );
		fbState[s] = flags;
		return;
	}
	if( mode == 3u )
	{
		// ACCUM: a stopped brick adds what the source's listed bricks' moves this frame could move it by - each move
		// weighted by a BOUND of the share of the brick's gather it can fill, times its sensitivity (codes per unit of
		// radiance). THE BOUND (PHOTON-7 stage 4's read R3): a source brick of side a moving by m changes the irradiance a
		// receiver point gathers by at most m x its projected solid angle / pi; the brick's silhouette from any direction is
		// at most sqrt(3) a^2 (a cube's largest) and every point of this brick is at least the two boxes' NEAREST distance d
		// away, so the share is at most min( 1, sqrt(3) a^2 / ( pi d^2 ) ) - 1 for bricks that touch (the distance between
		// CENTRES with one face's area gave a neighbour 1 / pi: it resumed ~3x late and rested ~2-3 codes off, measured by
		// gi.bounce_series' resume arm). Occlusion ignored: a bound.
		if( ( flags & 1u ) != 0u )
			return;
		const float sens = fbF( fbState[s + 5u] );
		if( !( sens > 0.0 ) )
			return;
		const float3 selfLo = fbSelf.xyz;
		const float3 selfHi = fbSelf.xyz + float3( fbDims.xyz ) * fbSelf.w;
		const float3 centre = fbSelf.xyz + ( float3( b ) + 0.5 ) * fbSelf.w;
		const float area = 1.7320508 * fbSrc.w * fbSrc.w;   // sqrt(3) a^2: the source brick's largest silhouette
		const float halfSum = 0.5 * ( fbSelf.w + fbSrc.w );
		const bool own = fbFlags.w != 0u;
		const uint count = own ? fbArgs[8] : fbSrcArgs[8];
		float sum = 0.0;   // codes, over the list in its order
		for( uint k = 0u; k < count; ++k )
		{
			const uint3 o = fbUnpack( own ? fbList[k] : fbSrcList[k] );
			const uint oi = o.x + fbSrcDims.x * ( o.y + fbSrcDims.y * o.z );
			const float m = fbF( own ? fbState[8u * oi + 4u] : fbSrcState[8u * oi + 4u] );
			if( !( m > 0.0 ) )
				continue;
			const float3 oLo = fbSrc.xyz + float3( o ) * fbSrc.w;
			const float3 oHi = oLo + fbSrc.w;
			if( fbFlags.z != 0u && all( greaterThanEqual( oLo, selfLo ) ) && all( lessThanEqual( oHi, selfHi ) ) )
				continue;
			const float3 gap = max( abs( centre - ( oLo + 0.5 * fbSrc.w ) ) - halfSum, float3( 0.0, 0.0, 0.0 ) );
			const float d2 = dot( gap, gap );
			sum += sens * m * ( d2 > 0.0 ? min( 1.0, area / ( FB_PI * d2 ) ) : 1.0 );
		}
		// THE FIXED POINT, ROUNDED ONCE: the sum in units plus the fraction the last rounding left, floored; the new
		// fraction kept (word 6). Saturating (a sensitivity of 1e30 - a voxel with no radiance of its own - saturates).
		const float units = sum * FB_ACC_ONE + fbF( fbState[s + 6u] );
		if( !( units < 4294967040.0 ) )
		{
			fbState[s + 3u] = 0xFFFFFFFFu;
			fbState[s + 6u] = 0u;
			return;
		}
		const float whole = floor( units );
		fbState[s + 6u] = fbU( units - whole );
		const uint q = uint( whole );
		if( q > 0u )
		{
			const uint a = fbState[s + 3u];
			fbState[s + 3u] = q > 0xFFFFFFFFu - a ? 0xFFFFFFFFu : a + q;
		}
		return;
	}
	if( mode == 4u )
	{
		// SHIFT: the new brick's cells were the old store's cells + fbShift; the old bricks they span merge
		// conservatively (active if any is, the larger move, contraction, would-move and sensitivity, the fewer passes).
		// A brick the shift brought in (any of its cells outside the old store: the slab) starts fresh and active.
		const int3 lo = int3( b ) * 8 + fbShift.xyz;
		const int3 hi = lo + 7;
		const int3 bl = lo >> 3, bh = hi >> 3;
		if( any( lessThan( bl, int3( 0 ) ) ) || any( greaterThanEqual( bh, int3( fbDims.xyz ) ) ) )
		{
			fbState[s] = 1u;
			for( uint k = 1u; k < 8u; ++k )
				fbState[s + k] = 0u;
			return;
		}
		uint anyActive = 0u, nMin = 255u;
		uint acc = 0u;
		float mv = 0.0, rr = 0.0, sn = 0.0;
		for( int z = bl.z; z <= bh.z; ++z )
			for( int y = bl.y; y <= bh.y; ++y )
				for( int x = bl.x; x <= bh.x; ++x )
				{
					const uint oi = 8u * ( uint( x ) + fbDims.x * ( uint( y ) + fbDims.y * uint( z ) ) );
					const uint f = fbOldState[oi];
					anyActive |= f & 1u;
					nMin = min( nMin, ( f >> 1u ) & 255u );
					mv = max( mv, fbF( fbOldState[oi + 1u] ) );
					rr = max( rr, fbF( fbOldState[oi + 2u] ) );
					acc = max( acc, fbOldState[oi + 3u] );
					sn = max( sn, fbF( fbOldState[oi + 5u] ) );
				}
		fbState[s] = anyActive | ( nMin << 1u );
		fbState[s + 1u] = fbU( mv );
		fbState[s + 2u] = fbU( rr );
		fbState[s + 3u] = acc;
		fbState[s + 4u] = 0u;
		fbState[s + 5u] = fbU( sn );
		fbState[s + 6u] = 0u;
		fbState[s + 7u] = 0u;
		return;
	}
@end
}
