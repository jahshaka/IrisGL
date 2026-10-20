// Jahshaka — ATOM stage 2: THE CLUSTER CUT, GLSL half (lane ATOM-CLUSTER-1;
// SPECS/atom/B2_CLUSTER_DAG_DESIGN.md section 2).
//
// THE C++ HALF is jahshaka/engine/Types.h: clusterGroupAllowed,
// clusterGroupAffordable and clusterDrawn. There are two copies because a compute
// shader cannot include a C++ header; there is ONE RULE because
// engine.lod_rule_parity's fourth copy runs this piece on the device over the
// shipped meshes' DAGs and fails when the drawn set differs from the C++ one.
//
// THE RULE is clusterlod.h's render test with the MEASURED group error in the
// quality currency's units: a cluster is drawn when its own group is NOT
// affordable and (it is level 0, or its refined group IS affordable). A group is
// affordable when its measured error is STRICTLY below what the consumer affords
// at the group's own distance, which is the chain's level walk's comparison.
//
// THE PRODUCT'S CUT INCLUDES IT (ATOM-CLUSTER-CUT): the cull's cut job
// (JahCullCut_cs.glsl) evaluates every (surviving instance, cluster) through it. It
// spends the ONE currency — jahSampleFootprint / jahAllowedWorldError
// in JahLevelRule_piece_cs.any (piece JahLevelRuleCurrency) — so an includer
// inserts that piece first, and the instance's scale is the caller's
// jahWorldMaxAxisScale (the longest COLUMN, the same piece's JahLevelRuleScale).
//
// THE TABLES' LAYOUT IS A CONTRACT with the C++ that uploads them (a std430
// struct of vec4s, no padding surprises):
//   JahClusterGroup  sphere = xyz the simplified bounds' centre, w its radius (mesh space)
//                    error  = x the MEASURED error (mesh units; 3.4e38 = terminal),
//                             y the header's estimate, z the depth, w unused
//   JahCluster       range  = x first index, y index count, z the group it is IN,
//                             w the group that PRODUCED it (0xFFFFFFFF = level 0)
//                    sphere = xyz centre, w radius (culling only)
// and the instance is three ROWS of its 3x4 transform, as the GPU scene table
// carries it.
@piece( JahClusterCut )
struct JahClusterGroup
{
	vec4 sphere;
	vec4 error;
};

struct JahCluster
{
	uvec4 range;
	vec4  sphere;
};

// The distance the rule judges a group at: from the eye to the group's sphere,
// transformed by the instance, minus its radius at the instance's scale (never
// below zero), spelled operation for operation as clusterGroupAllowed spells it.
// ORTHOGRAPHIC: no distance term (the level walk's own ortho case,
// JahCullTest_cs.glsl) - one metre makes the footprint the window over the target.
float jahClusterGroupDistance( vec4 sphere, vec4 row0, vec4 row1, vec4 row2, float scale, vec3 eye,
							   bool orthographic )
{
	// PRECISE: no fused multiply-add the compiler chooses per call site — one group
	// judged at two places must give one answer (the cut job judges each group once
	// anyway; this keeps the inline fallback, the parity job and the cut probe to the
	// same rule).
	precise float cx = row0.x * sphere.x + row0.y * sphere.y + row0.z * sphere.z + row0.w;
	precise float cy = row1.x * sphere.x + row1.y * sphere.y + row1.z * sphere.z + row1.w;
	precise float cz = row2.x * sphere.x + row2.y * sphere.y + row2.z * sphere.z + row2.w;
	precise float dx = cx - eye.x;
	precise float dy = cy - eye.y;
	precise float dz = cz - eye.z;
	precise float d = orthographic ? 1.0 : max( 0.0, sqrt( dx * dx + dy * dy + dz * dz ) - sphere.w * scale );
	return d;
}

// One sample's world size at the group's distance (Types.h clusterGroupFootprint).
float jahClusterGroupFootprint( vec4 sphere, vec4 row0, vec4 row1, vec4 row2, float scale, vec3 eye,
								float projScaleY, float viewportHeight, bool orthographic )
{
	precise float d = jahClusterGroupDistance( sphere, row0, row1, row2, scale, eye, orthographic );
	return jahSampleFootprint( d, projScaleY, viewportHeight );
}

// What the consumer can afford AT ONE GROUP: one sample's footprint at the group's
// distance, in the mesh's units.
float jahClusterGroupAllowed( vec4 sphere, vec4 row0, vec4 row1, vec4 row2, float scale,
							  vec3 eye, float tolerance, float projScaleY, float viewportHeight,
							  bool orthographic )
{
	precise float d = jahClusterGroupDistance( sphere, row0, row1, row2, scale, eye, orthographic );
	return jahAllowedWorldError( tolerance, jahSampleFootprint( d, projScaleY, viewportHeight ), scale );
}

// THE COMPARISON, once: allowed of zero affords nothing (the cut is level 0).
bool jahClusterGroupAffordable( float error, float allowed )
{
	return allowed > 0.0 && error < allowed;
}

// ONE GROUP'S VERDICT IN ONE VIEW (Types.h clusterGroupVerdict): THE EXACT RULE, error < allowed —
// the cut job, the parity job and the probe judge a group by this.
bool jahClusterGroupVerdict( JahClusterGroup gr, vec4 row0, vec4 row1, vec4 row2, float scale, vec3 eye,
							 float tolerance, float projScaleY, float viewportHeight, bool orthographic )
{
	float footprint = jahClusterGroupFootprint( gr.sphere, row0, row1, row2, scale, eye, projScaleY,
												viewportHeight, orthographic );
	return jahClusterGroupAffordable( gr.error.x, jahAllowedWorldError( tolerance, footprint, scale ) );
}

// THE RENDER TEST over the two answers a cluster needs.
bool jahClusterDrawn( bool ownGroupAffordable, bool isLevel0, bool refinedAffordable )
{
	if( ownGroupAffordable )
		return false;
	return isLevel0 || refinedAffordable;
}
@end
