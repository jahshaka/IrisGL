// THE ONE READER'S PARITY HARNESS — one cone, every answer the reader gives
// (PHOTON-READER-1; engine.voxel_reader_parity). Test machinery, not a
// renderer: the suite marches the same cones through the same bound cascade
// chain in a FRAGMENT shader and in a COMPUTE job, both built from this file and
// the two reader files, and asserts the answers are identical to the bit.
//
// THE CALLER BINDS the reader's macros (jah_voxel_sample.glsl,
// jah_voxel_march.glsl) and one more:
//
//     JAH_PARITY_CONE(i)   vec4: the i-th of four float4s per cone -
//                          (posLS, tanHalfAngle), (dirLS, flags),
//                          (biasDirLS, cascade), (lod, 0, 0, 0)
//
// No character of this file may be the Hlms directive mark (the compute half
// inserts it as the piece JahVoxelParity).

#ifndef JAH_VOXEL_PARITY_GLSL
#define JAH_VOXEL_PARITY_GLSL

/// Four float4s per cone:
///   [0] the march's colour and alpha
///   [1] its escape opacity, its age in cascade 0's units, the cascade it
///       stopped in, its age in that cascade's units
///   [2] the RAY HIT's read at the cone's start (jahVoxelSample - what
///       jah_rq_hit.glsl calls)
///   [3] the MARCH's read of the same point, spelled the way the march spells
///       it (the directional read on its folded coordinate, or the isotropic
///       slot)
vec4 jahParityAnswer( int cone, int part )
{
	vec4 c0 = JAH_PARITY_CONE( 4 * cone + 0 );
	vec4 c1 = JAH_PARITY_CONE( 4 * cone + 1 );
	vec4 c2 = JAH_PARITY_CONE( 4 * cone + 2 );
	vec4 c3 = JAH_PARITY_CONE( 4 * cone + 3 );
	uint flags = uint( c1.w );
	int cascade = int( c2.w );
	if( part == 2 )
		return jahVoxelSample( cascade, c0.xyz, c1.xyz, c3.x );
	if( part == 3 )
	{
#if JAH_VOX_HAS_ANISO
		if( JAH_VOX_ANISO )
			return jahVoxelSampleAniso( cascade, jahVoxelAnisoUvw( c0.xyz ), c1.xyz, c3.x );
#endif
		return JAH_VOX_SAMPLE_ISO( cascade, c0.xyz, c3.x );
	}
	JahConeResult r = jahConeMarch( c0.xyz, c1.xyz, c0.w, c2.xyz, c2.xyz, flags );
	if( part == 0 )
		return vec4( r.colour, r.alpha );
	return vec4( r.escapeAlpha, r.travelledC0, float( r.lastCascade ), r.travelled );
}

#endif   // JAH_VOXEL_PARITY_GLSL
