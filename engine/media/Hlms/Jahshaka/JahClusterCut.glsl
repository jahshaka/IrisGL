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
// NOTHING IN THE PRODUCT INCLUDES THIS PIECE YET. Stage 3's GPU cut will, from
// the cull. It spends the ONE currency — jahSampleFootprint / jahAllowedWorldError
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

// What the consumer can afford AT ONE GROUP: the distance from the eye to the
// group's sphere, transformed by the instance, spelled operation for operation as
// clusterGroupAllowed spells it.
float jahClusterGroupAllowed( vec4 sphere, vec4 row0, vec4 row1, vec4 row2, float scale,
							  vec3 eye, float tolerance, float projScaleY, float viewportHeight )
{
	float cx = row0.x * sphere.x + row0.y * sphere.y + row0.z * sphere.z + row0.w;
	float cy = row1.x * sphere.x + row1.y * sphere.y + row1.z * sphere.z + row1.w;
	float cz = row2.x * sphere.x + row2.y * sphere.y + row2.z * sphere.z + row2.w;
	float dx = cx - eye.x;
	float dy = cy - eye.y;
	float dz = cz - eye.z;
	float d = max( 0.0, sqrt( dx * dx + dy * dy + dz * dz ) - sphere.w * scale );
	return jahAllowedWorldError( tolerance, jahSampleFootprint( d, projScaleY, viewportHeight ), scale );
}

// THE COMPARISON, once: allowed of zero affords nothing (the cut is level 0).
bool jahClusterGroupAffordable( float error, float allowed )
{
	return allowed > 0.0 && error < allowed;
}

// THE RENDER TEST over the two answers a cluster needs.
bool jahClusterDrawn( bool ownGroupAffordable, bool isLevel0, bool refinedAffordable )
{
	if( ownGroupAffordable )
		return false;
	return isLevel0 || refinedAffordable;
}
@end
