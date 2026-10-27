// THE PANES A RAY CROSSED, FOLDED (PHOTON-7 10-4 R7; L9: a transparent surface transmits a ray, attenuated by its
// material). The rays that read light (the caches' gathers, the screen gather, the reflection) list the panes they
// crossed (jah_rq_alpha.glsl's JAH_RQ_PROCEED_PANES); after the traversal the panes nearer than the committed opaque
// hit - or than the ray's end, on a miss - are taken in crossing order:
//   THE THROUGHPUT past them all is the product of their T (Ogre's blend passes 1 - alpha behind Fade, Transparent and
//   Refractive alike: OgreScene::gpuPaneWordFor);
//   EACH PANE's OWN LIGHT is its share (alpha) of the light its SURFACE CARD holds at the crossing - the store's own
//   first answer for a surface with a card (jahHitRadianceStore), the card's row carrying kD x alpha for Transparent /
//   Refractive, so a glass sends alpha^2 kD as its pixel does, a Fade alpha kD - weighted by the throughput of the
//   panes in front of it. A pane with no card here (a far copy, a pane the cards do not hold) adds none and still
//   transmits: the store's other answer (the voxels and the direct light at the hit) is the whole hit shading, and
//   inlined once more per caller it multiplied the callers' cold pipeline compiles (rq_reflect 14.8 s against 2.9 s on
//   a tree without the fold, rq_voxel_sides 4.0 s against 1.0 s; the card read alone: 3.8 s and 1.4 s).
// The caller adds own + throughput x (the committed hit's light, or the sky over a miss). Needs the hit-radiance
// includes (jahCardRadiance, jahHitWoundNormal, JAH_HIT_INSTANCE) and jah_rq_alpha.glsl.
// The includer enables GL_EXT_control_flow_attributes (the fold's loop is never unrolled).
// No Hlms directive mark anywhere in this file.

#ifndef JAH_RQ_PANES_GLSL
#define JAH_RQ_PANES_GLSL

/// The pane i's object-to-world transform: the GPU scene's instance rows (the TLAS copies them, rq_tlas_write.comp).
mat4x3 jahPaneToWorld( uint slot )
{
	const vec4 r0 = uintBitsToFloat( JAH_HIT_INSTANCE( slot, 0u ) );
	const vec4 r1 = uintBitsToFloat( JAH_HIT_INSTANCE( slot, 1u ) );
	const vec4 r2 = uintBitsToFloat( JAH_HIT_INSTANCE( slot, 2u ) );
	return transpose( mat3x4( r0, r1, r2 ) );
}

/// THE FOLD: `p` already reduced to the panes in front (jahPanesInFront); `origin` and unit `dir` the ray's,
/// Returns the panes' own light; `throughput` the product of their T.
vec3 jahPanesOwnLight( JahPanes p, vec3 origin, vec3 dir, out float throughput )
{
	vec3 own = vec3( 0.0 );
	float T = 1.0;
	// (never unrolled: the list's bound is a constant 8 and the body holds the card read)
	[[dont_unroll]] for( int i = 0; i < p.n; ++i )
	{
		const uint w = p.word[i];
		const float share = jahPaneOwn( w );
		if( share > 0.0 && T > 0.0 )
		{
			const uint slot = p.slotFar[i] & 0xFFFFFFu;
			const uint geom = ( p.slotFar[i] >> 24u ) & 127u;
			const bool far = ( p.slotFar[i] >> 31u ) != 0u;
			bool ok = false;
			vec3 L = vec3( 0.0 );
			// (a far copy's triangle is the coarsest level's: no near row, no card names it)
			if( !far )
			{
				vec3 wound = -dir;
				const bool known = jahHitWoundNormal( slot, geom, p.prim[i], jahPaneToWorld( slot ), wound );
				const vec3 hitN = known ? ( dot( wound, dir ) > 0.0 ? -wound : wound ) : -dir;
				L = jahCardRadiance( slot, origin + dir * p.t[i], hitN, -dir, ok );
			}
			if( ok && finite3( L ) )
				own += T * share * L;
		}
		T *= jahPaneT( w );
	}
	throughput = T;
	return own;
}

#endif   // JAH_RQ_PANES_GLSL
