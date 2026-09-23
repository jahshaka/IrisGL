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
} outAtom;

void main()
{
	// The corners are clip-space already; the other streams hold zeros (and a unit w
	// for the tangent), added so the compiler keeps them.
	gl_Position = float4( vertex.xy, 0.0, 1.0 );
	@property( hlms_uv_count )gl_Position.xy += uv0.xy;@end
	@property( hlms_normal )gl_Position.z += normal.x;@end
	@property( normal_map && hlms_tangent4 )gl_Position.w = tangent.w;@end
	outAtom.drawId = drawId;
}
