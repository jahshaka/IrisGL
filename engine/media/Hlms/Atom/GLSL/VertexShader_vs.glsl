// HLMS ATOM's vertex template: the full-screen triangle. The POSITION stream holds
// its CLIP-SPACE corners; the other three streams exist only so this Hlms derives the
// same vertex properties the decoded geometry has (hlms_normal, hlms_tangent4,
// hlms_uv_count), and every one of them is consumed below so the pipeline declares no
// attribute the shader ignores. The one thing handed to the pixel shader is the draw
// id, which names the decode twin.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( SetCompatibilityLayer )

out gl_PerVertex
{
	vec4 gl_Position;
};

layout(std140) uniform;

vulkan_layout( OGRE_POSITION ) in vec4 vertex;
@property( hlms_normal )vulkan_layout( OGRE_NORMAL ) in float3 normal;@end
@property( normal_map )
	@property( hlms_tangent4 )vulkan_layout( OGRE_TANGENT ) in float4 tangent;@end
	@property( !hlms_tangent4 )vulkan_layout( OGRE_TANGENT ) in float3 tangent;@end
@end
@foreach( hlms_uv_count, n )
	vulkan_layout( OGRE_TEXCOORD@n ) in vec@value( hlms_uv_count@n ) uv@n;@end

vulkan_layout( OGRE_DRAWID ) in uint drawId;

vulkan_layout( location = 0 ) out block
{
	flat uint drawId;
	@property( atom_hit_mode )flat uint hitCount;@end
	@property( !atom_hit_mode )noperspective float2 ndc;@end
} outAtom;

@property( atom_hit_mode )
	// HIT MODE (PHOTON-HIT-SHADE-1): the triangle covers only the ROWS of the hit
	// list that hold this frame's records, so the decode costs what the records
	// cost and nothing per unused texel. The count is the ray jobs' atomic counter
	// (word 0, which may pass the capacity: the list's own size caps it).
	vulkan_layout( ogre_t@value(atomIdTex) ) uniform utexture2D atomIdTex;
	ReadOnlyBufferU( @value(atomHitBuf), uint, atomHitBuf );
@end

void main()
{
	// The corners are clip-space already; the other streams hold zeros (and a unit w
	// for the tangent), added so the compiler keeps them.
	gl_Position = float4( vertex.xy, 0.0, 1.0 );
	@property( hlms_uv_count )gl_Position.xy += uv0.xy;@end
	@property( hlms_normal )gl_Position.z += normal.x;@end
	@property( normal_map && hlms_tangent4 )gl_Position.w = tangent.w;@end
	outAtom.drawId = drawId;
	// THE PIXEL'S NDC INSIDE THE PASS'S VIEWPORT (screen mode): the pixel stage takes
	// the viewport's rectangle from it (a letterboxed view's inset included).
	@property( !atom_hit_mode )outAtom.ndc = gl_Position.xy;@end
	@property( atom_hit_mode )
		// Framebuffer row 0 is clip y = -1 (a positive-height viewport); the
		// triangle's corners are y = -1 and y = 3, so scaling their distance from
		// -1 by rows / height covers exactly the first `rows` rows.
		ivec2 atomListSize = textureSize( atomIdTex, 0 );
		uint atomW = uint( max( atomListSize.x, 1 ) ), atomH = uint( max( atomListSize.y, 1 ) );
		uint atomCount = min( atomHitBuf[0], atomW * atomH );
		uint atomRows = ( atomCount + atomW - 1u ) / atomW;
		gl_Position.y = -1.0 + ( gl_Position.y + 1.0 ) * ( float( atomRows ) / float( atomH ) );
		outAtom.hitCount = atomCount;
	@end
}
