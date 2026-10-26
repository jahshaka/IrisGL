@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

#define ushort2 uvec2

@insertpiece( PreBindingsHeaderCS )

vulkan_layout( ogre_T0 ) uniform samplerBuffer directionsBuffer;

vulkan( layout( ogre_s1 ) uniform sampler probeSampler );

// Jahshaka (PHOTON-READER-1): EVERY cascade's volumes, the probe rays walk the
// whole chain. The same declaration as the bounce-injection job's: one array per
// volume kind, unit order iso, then X, Y, Z (IrradianceField::bindChainToGenerationJob).
// uses_array_bindings = hlms_num_vct_cascades - 1 (arrays of more than one binding)
@pset( vctTexUnit, 1 )
@psub( uses_array_bindings, hlms_num_vct_cascades, 1 )

vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbes[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )

vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeX[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeY[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeZ[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
// Jahshaka (PHOTON-VOXEL-3): every cascade's PER-HALF-AXIS COVERAGE (+a faces, then -a).
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeCovP[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeCovN[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
// Jahshaka (PHOTON-VOXEL-4): every cascade's SURFACE POSITION per half-axis (+a, then -a), the last kinds.
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbePosP[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbePosN[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
// Jahshaka (PHOTON-VOXEL-5): the anisotropic tiers' level-0 back side and the voxelizer's normal.
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeBack[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeNrm[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )

// Jahshaka (PHOTON-ENV-1): the environment cube, after every volume, while the
// lighting has one (IrradianceField::bindChainToGenerationJob).
@property( jah_env )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform textureCube envCube;
	@add( vctTexUnit, 1 )
@end

// Jahshaka (PHOTON-FIELD-ROTATE-1): READ and written - every texel blends with its
// own history (the probe's mean over its integrations).
layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
uniform restrict image2D irradianceField;

layout( vulkan( ogre_u1 ) vk_comma @insertpiece(uav1_pf_type) )
uniform restrict image2D irradianceFieldDepth;

// Jahshaka (ENGINE-UPGRADE-2B stage 4): THE VARIANCE STATE - the counters, the per-probe
// stopped flags and the per-texel second moments (the piece's JAH_IFD_ layout).
layout( std430, ogre_U2 ) restrict buffer ifdVarianceLayout { uint ifdVariance[]; };
// Jahshaka (PHOTON-7 stage 5, 5-4): THE WORK AS A LIST of probe slots (the host's restarts and refinements), read when
// the work says so (boxSize[0].w).
layout( std430, ogre_U3 ) restrict readonly buffer ifdWorkLayout { uint ifdWork[]; };

// Jahshaka (PHOTON-7 stage 10-3, THE RADIANCE CACHE): the probes' radiance maps and their GGX chains - a probe's block
// is written whole by its own group (level 0, its border, levels 1-4 and theirs); level 0 READ too (its history).
@property( rad_resolution )
layout( vulkan( ogre_u4 ) vk_comma @insertpiece(uav4_pf_type) )
uniform restrict image2D radianceField;
// level 0's blended radiance (rgb) and its texel's solid angle (w, relative): the chain's source; then the chain's
// interiors (levels 1-4, in order)
shared float4 g_rad[@value( rad_resolution ) * @value( rad_resolution )];
shared float4 g_radChain[340];
@end

// One work group per probe: every ray's radiance and depth, and its direction.
shared float4 g_rayColourDepth[@value( num_rays_per_probe )];
shared float4 g_rayDir[@value( num_rays_per_probe )];
// The probe's history: x = the samples the blend keeps, y = 1 when the probe is skipped,
// z = the samples it held, w unused. And 1 when a texel of it is not at the precision.
shared float4 g_probeHistory;
shared uint g_probeUnconverged;
// (5-4) the darkest channel of the probe's irradiance tile after this integration (float bits): its sensitivity.
shared uint g_probeMinBits;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

@insertpiece( HeaderCS )

//in uvec3 gl_NumWorkGroups;
//in uvec3 gl_WorkGroupID;
//in uvec3 gl_LocalInvocationID;
//in uvec3 gl_GlobalInvocationID;
//in uint  gl_LocalInvocationIndex;

void main()
{
	@insertpiece( BodyCS )
}
