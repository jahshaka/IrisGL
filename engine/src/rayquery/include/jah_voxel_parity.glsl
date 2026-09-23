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
//     JAH_VOX_SDF_FACTOR, JAH_VOX_SDF_MAXMIP  (the specular empty-space skip)
//
// No character of this file may be the Hlms directive mark (the compute half
// inserts it as the piece JahVoxelParity).

#ifndef JAH_VOXEL_PARITY_GLSL
#define JAH_VOXEL_PARITY_GLSL

/// Four float4s per cone:
///   [0] the march's colour and alpha
///   [1] its escape opacity, its age in cascade 0's units, the cascade it
///       stopped in, its age in that cascade's units
///   [2] the RAY HIT's read (jahVoxelSample - what jah_rq_hit.glsl calls) at the
///       point the march's FIRST sample lands on, at the cone's mip
///   [3] THE MARCH AT ZERO LENGTH: one march step (JAH_MARCH_ONE_STEP) from a
///       start one march step behind that point - what the march itself reads
///       there, through its own loops
vec4 jahParityAnswer( int cone, int part )
{
	vec4 c0 = JAH_PARITY_CONE( 4 * cone + 0 );
	vec4 c1 = JAH_PARITY_CONE( 4 * cone + 1 );
	vec4 c2 = JAH_PARITY_CONE( 4 * cone + 2 );
	vec4 c3 = JAH_PARITY_CONE( 4 * cone + 3 );
	uint flags = uint( c1.w );
	int cascade = int( c2.w );
	if( part >= 2 )
	{
		// The march's own first-sample arithmetic (jahConeMarchCascade), spelled
		// the same way so the two reads land on the same bits of position.
		float step0 = dot( abs( c1.xyz ), JAH_VOX_INVRES( cascade ) );
		vec3 at = c0.xyz + step0 * c1.xyz;
		if( part == 2 )
			return jahVoxelSample( cascade, at, c1.xyz, c3.x );
		JahConeResult one = jahConeMarchCascade( cascade, c0.xyz, c1.xyz, 0.0, c3.x, 0.0, 0.0,
												 0.0, JAH_MARCH_ONE_STEP );
		return vec4( one.colour, one.alpha );
	}
	JahConeResult r = jahConeMarch( c0.xyz, c1.xyz, c0.w, c2.xyz, c2.xyz, flags );
	if( part == 0 )
		return vec4( r.colour, r.alpha );
	return vec4( r.escapeAlpha, r.travelledC0, float( r.lastCascade ), r.travelled );
}

#endif   // JAH_VOXEL_PARITY_GLSL
