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
///       centre of the first texel plane the march reads from c0, at the cone's mip
///   [3] THE MARCH AT ZERO LENGTH: the march at that centre, one plane read whole
///       (JAH_MARCH_ONE_STEP) - what the march itself reads there, through its loop
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
		// THE FIRST PLANE the march reads from c0 (jahConeMarchCascade's own arithmetic,
		// spelled the same way so the two reads land on the same bits of position): the
		// texel plane of the point read's level holding c0, at its centre.
		const vec3 invRes = JAH_VOX_INVRES( cascade );
		const int axis = jahVoxelAxis( c1.xyz, invRes );
		const float da = c1.xyz[axis];
		const float sgn = da > 0.0 ? 1.0 : -1.0;
		const float invAbsDa = 1.0 / abs( da );
#if JAH_VOX_HAS_ANISO
		const bool directional = JAH_VOX_ANISO && c3.x > 0.5;
		const bool anisoTier = JAH_VOX_ANISO;
#else
		const bool directional = false;
		const bool anisoTier = false;
#endif
		const float texelCells = directional ? 2.0 : 1.0;
		const float mip = ( anisoTier && !directional ) ? 0.0
														: floor( jahVoxelKernelMip( c3.x, texelCells ) );
		const float texelLS = texelCells * exp2( mip ) * invRes[axis];
		const vec2 faces = jahMarchPlane( c0.xyz[axis], texelLS, sgn );
		const float tCentre = ( 0.5 * ( faces.x + faces.y ) - c0.xyz[axis] ) * sgn * invAbsDa;
		const vec3 at = c0.xyz + tCentre * c1.xyz;
		if( part == 2 )
			return jahVoxelSample( cascade, at, c1.xyz, c3.x );
		// The march from that centre, one plane, read whole.
		JahConeResult one = jahConeMarchCascade( cascade, at, c1.xyz, 0.0, c3.x, 0.0, 0.0,
												 kJahVoxelNoOrigin, JAH_MARCH_ONE_STEP );
		return vec4( one.colour, one.alpha );
	}
	JahConeResult r = jahConeMarch( c0.xyz, c1.xyz, c0.w, jahConeOrigin( c0.xyz, c2.xyz ), flags );
	if( part == 0 )
		return vec4( r.colour, r.alpha );
	return vec4( r.alpha, r.travelledC0, float( r.lastCascade ), r.travelled );
}

#endif   // JAH_VOXEL_PARITY_GLSL
