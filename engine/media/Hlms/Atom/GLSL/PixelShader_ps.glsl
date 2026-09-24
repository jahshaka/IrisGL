// HLMS ATOM's data-folder pixel template. It is Pbs/GLSL/PixelShader_ps.glsl's
// DECLARATION section - the outputs, the pass textures, the material textures and
// samplers, the lighting pieces' declarations - with the vertex-to-pixel block
// replaced by the decode (Hlms/Atom/Any/800.Atom_piece_ps.any). The BODY is
// upstream's own DefaultBodyPS, inserted whole, after the decode prologue has filled
// the local inPs. A decode draw is never a shadow caster.
@piece( CustomGlslExtensions )
	#extension GL_EXT_buffer_reference: require
	#extension GL_EXT_buffer_reference_uvec2: require
@end
@insertpiece( SetCrossPlatformSettings )
@insertpiece( SetCompatibilityLayer )
@insertpiece( DeclareUvModifierMacros )

layout(std140) uniform;

@property( !hlms_render_depth_only )
	@property( !hlms_shadowcaster )
		@property( !hlms_prepass )
			layout(location = @counter(rtv_target), index = 0) out midf4 outColour;
		@end
		@property( hlms_gen_normals_gbuffer )
			#define outPs_normals outNormals
			layout(location = @counter(rtv_target)) out midf4 outNormals;
		@end
		@property( hlms_prepass )
			#define outPs_shadowRoughness outShadowRoughness
			layout(location = @counter(rtv_target)) out midf2 outShadowRoughness;
		@end
	@else
		layout(location = @counter(rtv_target), index = 0) out float outColour;
	@end
@end

@insertpiece( custom_ps_output_types )

@property( hlms_use_prepass )
	@property( !hlms_use_prepass_msaa )
		vulkan_layout( ogre_t@value(gBuf_normals) )			uniform texture2D gBuf_normals;
		vulkan_layout( ogre_t@value(gBuf_shadowRoughness) )	uniform texture2D gBuf_shadowRoughness;
	@else
		uniform sampler2DMS gBuf_normals;
		uniform sampler2DMS gBuf_shadowRoughness;
		uniform sampler2DMS gBuf_depthTexture;
	@end

	@property( hlms_use_ssr )
		vulkan_layout( ogre_t@value(ssrTexture) ) uniform texture2D ssrTexture;
	@end
@end

@property( hlms_ss_refractions_available )
	@property( !hlms_use_prepass || !hlms_use_prepass_msaa )
		@property( !hlms_use_prepass_msaa )
			vulkan_layout( ogre_t@value(gBuf_depthTexture) ) uniform texture2D gBuf_depthTexture;
			#define depthTextureNoMsaa gBuf_depthTexture
		@else
			vulkan_layout( ogre_t@value(depthTextureNoMsaa) ) uniform texture2D depthTextureNoMsaa;
		@end
	@end
	vulkan_layout( ogre_t@value(refractionMap) )	midf_tex uniform texture2D	refractionMap;
	vulkan( layout( ogre_s@value(refractionMap) )	uniform sampler				refractionMapSampler );
@end

@insertpiece( DeclPlanarReflTextures )
@insertpiece( DeclAreaApproxTextures )
@insertpiece( DeclLightProfilesTexture )
@insertpiece( DeclBlueNoiseTexture )

@property( hlms_vpos )
in vec4 gl_FragCoord;
@end

@insertpiece( DefaultHeaderPS )
@insertpiece( custom_ps_uniformDeclaration )

@insertpiece( PccManualProbeDecl )

// The full-screen triangle hands over its draw id, which names the decode twin this
// draw serves (the bucket test in the prologue), and in hit mode the hit list's
// record count this frame (the vertex stage read it to cover only those rows).
vulkan_layout( location = 0 ) in block
{
	flat uint drawId;
	@property( atom_hit_mode )flat uint hitCount;@end
} inAtom;

@pset( currSampler, samplerStateStart )

@property( !hlms_shadowcaster )

@property( hlms_forwardplus )
	vulkan_layout( ogre_T@value(f3dGrid) ) uniform usamplerBuffer f3dGrid;
	ReadOnlyBufferF( @value(f3dLightList), float4, f3dLightList );
@end
@property( irradiance_volumes )
	vulkan_layout( ogre_t@value(irradianceVolume) )	midf_tex uniform texture3D	irradianceVolume;
	vulkan( layout( ogre_s@value(irradianceVolume) )uniform sampler				irradianceVolumeSampler );
@end

@foreach( num_textures, n )
	vulkan_layout( ogre_t@value(textureMaps@n) ) midf_tex uniform texture2DArray textureMaps@n;@end

@property( use_envprobe_map )
	@property( !hlms_enable_cubemaps_auto )
		vulkan_layout( ogre_t@value(texEnvProbeMap) ) midf_tex uniform textureCube texEnvProbeMap;
	@else
		@property( !hlms_cubemaps_use_dpm )
			vulkan_layout( ogre_t@value(texEnvProbeMap) ) midf_tex uniform textureCubeArray texEnvProbeMap;
		@else
			vulkan_layout( ogre_t@value(texEnvProbeMap) ) midf_tex uniform texture2DArray texEnvProbeMap;
			@insertpiece( DeclDualParaboloidFunc )
		@end
	@end
	@property( envMapRegSampler < samplerStateStart && syntax == glslvk )
		layout( ogre_s@value(envMapRegSampler) ) uniform sampler samplerState@value(envMapRegSampler);
	@end
@end

@property( syntax == glslvk )
	@foreach( num_samplers, n )
		layout( ogre_s@value(currSampler) ) uniform sampler samplerState@counter(currSampler);@end
@end

@property( use_parallax_correct_cubemaps )
	@insertpiece( DeclParallaxLocalCorrect )
@end

@insertpiece( DeclDecalsSamplers )

@insertpiece( DeclShadowMapMacros )
@insertpiece( DeclShadowSamplers )
@insertpiece( DeclShadowSamplingFuncs )

@insertpiece( DeclAreaLtcTextures )
@insertpiece( DeclAreaLtcLightFuncs )

@insertpiece( DeclVctTextures )
@insertpiece( DeclIrradianceFieldTextures )

@insertpiece( AtomDeclDecode )

@insertpiece( custom_ps_functions )

void main()
{
	@insertpiece( AtomDecodePrologue )
	@insertpiece( custom_ps_preExecution )
	@insertpiece( DefaultBodyPS )
	@insertpiece( custom_ps_posExecution )
}
@else ///!hlms_shadowcaster
void main()
{
	discard;
}
@end ///hlms_shadowcaster
