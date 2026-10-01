/*
-----------------------------------------------------------------------------
This source file is part of OGRE-Next
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-present Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/

#include "OgreStableHeaders.h"

#include "photon/voxel/PhotonVoxelizer.h"

#include "photon/voxel/PhotonVoxelMaterial.h"
#include "photon/voxel/PhotonVoxelVisualizer.h"

#include "Compute/OgreComputeTools.h"
#include "OgreHlmsCompute.h"
#include "OgreHlmsComputeJob.h"
#include "OgreItem.h"
#include "OgreLogManager.h"
#include "OgreLwString.h"
#include "OgreMaterial.h"
#include "OgreMaterialManager.h"
#include "OgreMesh2.h"
#include "OgrePixelFormatGpuUtils.h"
#include "OgreProfiler.h"
#include "OgreRenderSystem.h"
#include "OgreRoot.h"
#include "OgreSceneManager.h"
#include "OgreStringConverter.h"
#include "OgreSubMesh2.h"
#include "OgreTextureGpuManager.h"
#include "Vao/OgreIndexBufferPacked.h"
#include "Vao/OgreReadOnlyBufferPacked.h"
#include "Vao/OgreVertexBufferPacked.h"
#include "Vao/OgreStagingBuffer.h"
#include "Vao/OgreTexBufferPacked.h"
#include "Vao/OgreUavBufferPacked.h"
#include "Vao/OgreVaoManager.h"
#include "Vao/OgreVertexArrayObject.h"

#define TODO_deal_no_index_buffer

namespace Ogre
{
    static const uint32 c_numVctProperties = 2u;

    struct PhotonVoxelizerProp
    {
        static const IdString HasDiffuseTex;
        static const IdString HasEmissiveTex;

        static const IdString *AllProps[c_numVctProperties];
    };

    const IdString PhotonVoxelizerProp::HasDiffuseTex = IdString( "has_diffuse_tex" );
    const IdString PhotonVoxelizerProp::HasEmissiveTex = IdString( "has_emissive_tex" );

    const IdString *PhotonVoxelizerProp::AllProps[c_numVctProperties] = {
        &PhotonVoxelizerProp::HasDiffuseTex,
        &PhotonVoxelizerProp::HasEmissiveTex,
    };
    //-------------------------------------------------------------------------
    PhotonVoxelizer::PhotonVoxelizer( IdType id, RenderSystem *renderSystem, HlmsManager *hlmsManager,
                                bool correctAreaLightShadows, PhotonVoxelMaterial *materialStore ) :
        PhotonVoxelizerSourceBase( id, renderSystem, hlmsManager ),
        mMergeAccumTex( 0 ),
        mGeometryBuffer( 0 ),
        mRecords( 0 ),
        mRecordsAsTex( 0 ),
        mRanges( 0 ),
        mLastDispatchCount( 0u ),
        mNeedsAlbedoMipmaps( correctAreaLightShadows ),
        mNeedsAllMipmaps( false ),
        mComputeTools( new ComputeTools( hlmsManager->getComputeHlms() ) ),
        mVctMaterial( materialStore ),
        mNumOctantsX( 0u ),
        mNumOctantsY( 0u ),
        mNumOctantsZ( 0u )
    {
        memset( mComputeJobs, 0, sizeof( mComputeJobs ) );
        createComputeJobs();

        // A device with no buffer device addresses cannot voxelize at all any more:
        // the shader dereferences the raster's vertex and index pools. Say so ONCE,
        // loudly, instead of producing empty volumes - the cure is a driver with
        // VK_KHR_buffer_device_address, which every target this engine ships on has.
        if( !mVaoManager->supportsBufferDeviceAddress() )
        {
            LogManager::getSingleton().logMessage(
                "WARNING: PhotonVoxelizer needs buffer device addresses (the compute shader "
                "reads the raster's own vertex and index buffers) and this device has none. "
                "Voxel-cone GI will be empty.",
                LML_CRITICAL );
        }
    }
    //-------------------------------------------------------------------------
    PhotonVoxelizer::~PhotonVoxelizer()
    {
        setDebugVisualization( DebugVisualizationNone, 0 );
        destroyVoxelTextures();
        clearComputeJobResources();

        // NOTHING HERE IS OURS BUT THE VOLUMES: the store, the geometry rows, the
        // records and the ranges all belong to the owner and outlive us.
        mVctMaterial = 0;
        mGeometryBuffer = 0;
        mRecords = 0;
        mRecordsAsTex = 0;
        mRanges = 0;

        delete mComputeTools;
        mComputeTools = 0;
    }
    //-------------------------------------------------------------------------
    /// THE VAO OF ONE LOD LEVEL, and the ONE place this file resolves which one a
    /// (SubMesh, level) request means. The level arrives PER ITEM (ATOM P4 / AT-A10).
    ///
    /// `mVao[VpNormal]` is the mesh's LOD chain, finest first, and it always has
    /// at least one entry: a mesh with no chain has exactly one and every level
    /// clamps back onto it, which is why this is a drop-in for the
    /// `.front()` this class used everywhere.
    static VertexArrayObject *getLodVao( const SubMesh *subMesh, uint32 lodLevel )
    {
        const VertexArrayObjectArray &vaos = subMesh->mVao[VpNormal];
        if( vaos.empty() )
            return 0;
        const size_t idx = std::min<size_t>( lodLevel, vaos.size() - 1u );
        return vaos[idx];
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelizer::createComputeJobs()
    {
        HlmsCompute *hlmsCompute = mHlmsManager->getComputeHlms();

        HlmsComputeJob *voxelizerJob = hlmsCompute->findComputeJobNoThrow( "VCT/Voxelizer" );

#if OGRE_NO_JSON
        OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                     "To use PhotonVoxelizer, Ogre must be build with JSON support "
                     "and you must include the resources bundled at "
                     "media/Photon/Voxel",
                     "PhotonVoxelizer::createComputeJobs" );
#endif
        if( !voxelizerJob )
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                         "To use PhotonVoxelizer, you must include the resources bundled at "
                         "media/Photon/Voxel\n"
                         "Could not find VCT/Voxelizer",
                         "PhotonVoxelizer::createComputeJobs" );
        }

        uint32 numVariants = 1u << c_numVctProperties;

        char tmpBuffer[128];
        LwString jobName( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );

        for( uint32 variant = 0u; variant < numVariants; ++variant )
        {
            jobName.clear();
            jobName.a( "VCT/Voxelizer/", variant );

            mComputeJobs[variant] = hlmsCompute->findComputeJobNoThrow( jobName.c_str() );

            if( !mComputeJobs[variant] )
            {
                mComputeJobs[variant] = voxelizerJob->clone( jobName.c_str() );

                ShaderParams &glslShaderParams = mComputeJobs[variant]->getShaderParams( "glsl" );

                uint8 numTexUnits = 1u;
                if( variant &
                    ( PhotonVoxelizerJobSetting::HasDiffuseTex | PhotonVoxelizerJobSetting::HasEmissiveTex ) )
                {
                    ShaderParams::Param param;
                    param.name = "texturePool";
                    param.setManualValue( static_cast<int32>(
                        numTexUnits + mComputeJobs[variant]->_getRawGlTexSlotStart() ) );
                    glslShaderParams.mParams.push_back( param );
                    glslShaderParams.setDirty();
                    ++numTexUnits;
                }
                mComputeJobs[variant]->setNumTexUnits( numTexUnits );

                for( uint32 property = 0; property < c_numVctProperties; ++property )
                {
                    const int32 propValue = variant & ( 1u << property ) ? 1 : 0;
                    mComputeJobs[variant]->setProperty( *PhotonVoxelizerProp::AllProps[property],
                                                        propValue );
                }
            }
        }

    }
    //-------------------------------------------------------------------------
    void PhotonVoxelizer::clearComputeJobResources()
    {
        // Do not leave dangling pointers in the shared jobs: they live in HlmsCompute,
        // every voxelizer dispatches them, and every buffer bound here is the HOST'S -
        // it may be grown (destroyed and re-created) before the next build.
        for( size_t i = 0; i < sizeof( mComputeJobs ) / sizeof( mComputeJobs[0] ); ++i )
        {
            if( !mComputeJobs[i] )
                continue;
            mComputeJobs[i]->clearUavBuffers();
            mComputeJobs[i]->clearTexBuffers();
        }
    }
    //-------------------------------------------------------------------------
    /// The normal / uv formats a geometry row can name, mirrored in
    /// Voxelizer_piece_cs.any. A FORMAT IS DATA, NOT A SHADER PERMUTATION: the old
    /// `compressed_vertex_format` property existed only because this class chose the
    /// format itself when it repacked every mesh.
    namespace PhotonVoxelizerGeomFlag
    {
        enum PhotonVoxelizerGeomFlag
        {
            Index32bit = 1u << 0u,

            NormalShift = 4u,
            NormalNone = 0u << 4u,
            NormalFloat3 = 1u << 4u,
            NormalShort4Snorm = 2u << 4u,
            NormalHalf4 = 3u << 4u,

            UvShift = 8u,
            UvNone = 0u << 8u,
            UvFloat2 = 1u << 8u,
            UvHalf2 = 2u << 8u,
        };
    }
    //-------------------------------------------------------------------------
    /// THE ONE PLACE A (mesh, level, submesh)'s GEOMETRY IS DESCRIBED - and it is a
    /// STATIC, because a row is a fact about a MESH and not about a voxelisation.
    /// See the declaration for why the host owns the table.
    bool PhotonVoxelizer::describeGeometryRow( const MeshPtr &mesh, uint32 level, uint32 submesh,
                                           VaoManager *vaoManager, GeometryRow &out )
    {
        if( !mesh || !vaoManager || submesh >= mesh->getNumSubMeshes() )
            return false;

        SubMesh *subMesh = mesh->getSubMesh( (uint16)submesh );
        VertexArrayObject *vao = getLodVao( subMesh, level );
        if( !vao )
            return false;
        IndexBufferPacked *indexBuffer = vao->getIndexBuffer();
        if( !indexBuffer )
        {
            TODO_deal_no_index_buffer;
            return false;
        }

        size_t posSource = 0u, posOffset = 0u;
        const VertexElement2 *posElem = vao->findBySemantic( VES_POSITION, posSource, posOffset );
        if( !posElem || posElem->mType != VET_FLOAT3 ||
            posSource >= vao->getVertexBuffers().size() )
        {
            LogManager::getSingleton().logMessage(
                "WARNING: Mesh '" + mesh->getName() +
                    "' has no float3 VES_POSITION the voxelizer can read in place. It "
                    "will not contribute to GI.",
                LML_CRITICAL );
            return false;
        }

        VertexBufferPacked *vertexBuffer = vao->getVertexBuffers()[posSource];
        const uint64 posAddress = vaoManager->getBufferDeviceAddress( vertexBuffer );
        const uint64 rawIdxAddress = vaoManager->getBufferDeviceAddress( indexBuffer );
        if( !posAddress || !rawIdxAddress )
            return false;  // no addresses on this device

        GeometryRow row;
        memset( &row, 0, sizeof( row ) );
        row.vertexStride = vertexBuffer->getBytesPerElement();
        row.posOffset = uint32( posOffset );
        row.normalOffset = 0xFFFFFFFFu;
        row.uvOffset = 0xFFFFFFFFu;
        row.flags = indexBuffer->getIndexType() == IndexBufferPacked::IT_32BIT
                        ? PhotonVoxelizerGeomFlag::Index32bit
                        : 0u;

        // A SEMANTIC IN ANOTHER SOURCE BUFFER IS TREATED AS ABSENT, which is exactly
        // what the deleted download path did when its helper returned no data for one
        // (normal fell back to UNIT_Y, uv to zero). One row carries one vertex address;
        // a second source would need a second, and no mesh this engine bakes has one.
        size_t normSource = 0u, normOffset = 0u;
        const VertexElement2 *normElem = vao->findBySemantic( VES_NORMAL, normSource, normOffset );
        if( normElem && normSource == posSource )
        {
            uint32 fmt = PhotonVoxelizerGeomFlag::NormalNone;
            if( normElem->mType == VET_FLOAT3 || normElem->mType == VET_FLOAT4 )
                fmt = PhotonVoxelizerGeomFlag::NormalFloat3;
            else if( normElem->mType == VET_SHORT4_SNORM )
                fmt = PhotonVoxelizerGeomFlag::NormalShort4Snorm;
            else if( normElem->mType == VET_HALF4 )
                fmt = PhotonVoxelizerGeomFlag::NormalHalf4;
            if( fmt != PhotonVoxelizerGeomFlag::NormalNone )
            {
                row.flags |= fmt;
                row.normalOffset = uint32( normOffset );
            }
        }

        size_t uvSource = 0u, uvOffset = 0u;
        const VertexElement2 *uvElem =
            vao->findBySemantic( VES_TEXTURE_COORDINATES, uvSource, uvOffset );
        if( uvElem && uvSource == posSource )
        {
            uint32 fmt = PhotonVoxelizerGeomFlag::UvNone;
            if( uvElem->mType == VET_FLOAT2 || uvElem->mType == VET_FLOAT3 ||
                uvElem->mType == VET_FLOAT4 )
                fmt = PhotonVoxelizerGeomFlag::UvFloat2;
            else if( uvElem->mType == VET_HALF2 || uvElem->mType == VET_HALF4 )
                fmt = PhotonVoxelizerGeomFlag::UvHalf2;
            if( fmt != PhotonVoxelizerGeomFlag::UvNone )
            {
                row.flags |= fmt;
                row.uvOffset = uint32( uvOffset );
            }
        }

        // A buffer_reference of uints must sit on a 4-byte boundary and a 16-bit index
        // buffer can start on an odd uint16. Floor the address and carry the remainder
        // as an ELEMENT bias, so the shader's index arithmetic stays whole-element and
        // nothing is copied to make it align.
        const uint32 idxBytes = indexBuffer->getBytesPerElement();
        const uint64 flooredIdx = rawIdxAddress & ~uint64( 3u );
        row.idxBias = uint32( ( rawIdxAddress - flooredIdx ) / idxBytes );
        row.posAddress[0] = uint32( posAddress & 0xFFFFFFFFu );
        row.posAddress[1] = uint32( posAddress >> 32u );
        row.idxAddress[0] = uint32( flooredIdx & 0xFFFFFFFFu );
        row.idxAddress[1] = uint32( flooredIdx >> 32u );

        // THE SHADER'S ADDRESS ARITHMETIC IS IN 4-BYTE LANES (`GEOM_LANE` does a
        // `>> 2`), so an odd stride or element offset would not fail - it would read
        // the WRONG BYTES, silently, for that one mesh.
        const bool bAligned = ( row.vertexStride & 3u ) == 0u && ( row.posOffset & 3u ) == 0u &&
                              ( row.normalOffset == 0xFFFFFFFFu ||
                                ( row.normalOffset & 3u ) == 0u ) &&
                              ( row.uvOffset == 0xFFFFFFFFu || ( row.uvOffset & 3u ) == 0u );
        if( !bAligned )
        {
            LogManager::getSingleton().logMessage(
                "WARNING: Mesh '" + mesh->getName() +
                    "' has a vertex stride or element offset that is not a multiple of 4 "
                    "bytes; the voxelizer reads vertices in 4-byte lanes and will not "
                    "read this mesh. It will not contribute to GI.",
                LML_CRITICAL );
            return false;
        }

        out = row;
        return true;
    }
    //-------------------------------------------------------------------------
    /// Jahshaka fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065): the merge accumulator dies with the voxel textures.
    void PhotonVoxelizer::destroyVoxelTextures()
    {
        if( mMergeAccumTex )
        {
            mTextureGpuManager->destroyTexture( mMergeAccumTex );
            mMergeAccumTex = 0;
        }
        PhotonVoxelizerSourceBase::destroyVoxelTextures();
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelizer::createVoxelTextures()
    {
        if( mAlbedoVox && mAlbedoVox->getWidth() == mWidth && mAlbedoVox->getHeight() == mHeight &&
            mAlbedoVox->getDepth() == mDepth )
        {
            mAccumValVox->scheduleTransitionTo( GpuResidency::Resident );
            // Jahshaka fork ad452604a+0338ca7f2+c4c80b5f7 (was 0071): the merge accumulator is NOT transient. It is
            // created once with the voxel textures and stays Resident until they
            // are destroyed -- see the note at the end of build().
            return;
        }

        const bool hasTypedUavs = mRenderSystem->getCapabilities()->hasCapability( RSC_TYPED_UAV_LOADS );

        if( !mAlbedoVox )
        {
            uint32 texFlags = TextureFlags::Uav;
            if( !hasTypedUavs )
                texFlags |= TextureFlags::Reinterpretable;

            if( mNeedsAlbedoMipmaps || mNeedsAllMipmaps )
                texFlags |= TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps;

            mAlbedoVox = mTextureGpuManager->createTexture(
                "VctVoxelizer" + StringConverter::toString( getId() ) + "/Albedo",
                GpuPageOutStrategy::Discard, texFlags, TextureTypes::Type3D );

            if( !mNeedsAllMipmaps )
                texFlags &= ~uint32( TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps );

            mEmissiveVox = mTextureGpuManager->createTexture(
                "VctVoxelizer" + StringConverter::toString( getId() ) + "/Emissive",
                GpuPageOutStrategy::Discard, texFlags, TextureTypes::Type3D );
            mNormalVox = mTextureGpuManager->createTexture(
                "VctVoxelizer" + StringConverter::toString( getId() ) + "/Normal",
                GpuPageOutStrategy::Discard, texFlags, TextureTypes::Type3D );

            texFlags &= ~uint32( TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps );

            mAccumValVox = mTextureGpuManager->createTexture(
                "VctVoxelizer" + StringConverter::toString( getId() ) + "/AccumVal",
                GpuPageOutStrategy::Discard, TextureFlags::NotTexture | texFlags, TextureTypes::Type3D );

            // Jahshaka (PHOTON-VOXEL-3): the per-axis coverage. A full mip chain on
            // every tier, generated after each build like the albedo's: the
            // isotropic tier's cone march reads the light volume's own mips and the
            // coverage's beside them at the same lod, and the injection's shadow
            // march reads it at the lod an area light widens to.
            // (PHOTON-VOXEL-4) One per half-axis: the faces looking +a, then -a.
            // Jahshaka (PHOTON-VOXEL-4): the surface position per half, a full mip chain
            // beside the coverage's (autogenerated: O-premultiplied, the box filter is
            // the coverage-weighted mean exactly).
            const char *halfName[2] = { "P", "N" };
            for( size_t h = 0u; h < 2u; ++h )
            {
                mCoverageVox[h] = mTextureGpuManager->createTexture(
                    "VctVoxelizer" + StringConverter::toString( getId() ) + "/Coverage" + halfName[h],
                    GpuPageOutStrategy::Discard,
                    TextureFlags::Uav | TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps,
                    TextureTypes::Type3D );
                mPositionVox[h] = mTextureGpuManager->createTexture(
                    "VctVoxelizer" + StringConverter::toString( getId() ) + "/Position" + halfName[h],
                    GpuPageOutStrategy::Discard,
                    TextureFlags::Uav | TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps,
                    TextureTypes::Type3D );
            }

            // Jahshaka fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065) — the order-independent merge's accumulator.
            mMergeAccumTex = mTextureGpuManager->createTexture(
                "VctVoxelizer" + StringConverter::toString( getId() ) + "/MergeAccum",
                GpuPageOutStrategy::Discard,
                TextureFlags::NotTexture | TextureFlags::Uav, TextureTypes::Type3D );
        }

        TextureGpu *textures[4] = { mAlbedoVox, mEmissiveVox, mNormalVox, mAccumValVox };
        for( size_t i = 0; i < sizeof( textures ) / sizeof( textures[0] ); ++i )
            textures[i]->scheduleTransitionTo( GpuResidency::OnStorage );

        mAlbedoVox->setPixelFormat( PFG_RGBA8_UNORM );
        // JAHSHAKA fork ad452604a+155a56bf8 (was 0087): THE EMISSIVE VOXEL IS A FLOAT.
        //
        // Albedo is a RATIO and lives in [0, 1] by definition, so its UNORM store
        // above is exact. EMISSIVE is a RADIANCE -- W/(m^2 sr), no upper bound
        // worth naming -- and this store was the one place it was clipped: the
        // material store carries it as four honest floats (PhotonVoxelMaterial:
        // shaderMaterial.emissive[i] = emissiveCol[i]) and the merge accumulates
        // it on a fixed-point grid clamped at 16.0 per contribution (fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065)),
        // but the final imageWrite went into a UNORM8 texel, so an emitter
        // authored at 3.0 was stored as exactly 1.0 and the light injection seeded
        // the radiance volume from that (LightInjection_piece_cs.any:
        // blockColour = emissiveVal.xyz) -- a third of the energy, entering the
        // bounces, the irradiance field, the cones and the ray hits, with nothing
        // in any log and a picture that merely looks dimmer.
        //
        // RGBA16_FLOAT and not 32: half carries 65504 with 11 bits of mantissa,
        // which is finer than the merge's own 1/4096 grid everywhere in [0, 16] --
        // the grid's clamp, not the format, is now the documented ceiling.
        //
        // NOTHING ELSE MOVES. The GLSL image declaration carries no hard-coded
        // format: HlmsComputeJob generates the layout qualifier from the bound
        // texture (uav4_pf_type, OgreHlmsComputeJob.cpp), the typed-UAV write is a
        // float4 either way, ComputeTools::clearUavFloat clears any non-integer
        // format, and every reader loads through a sampled texture3D. The one
        // backend that cannot follow is D3D11 WITHOUT typed UAV loads, whose
        // branch packs the texel into a single uint -- see the note beside it in
        // Voxelizer_piece_cs.any.
        mEmissiveVox->setPixelFormat( PFG_RGBA16_FLOAT );
        mNormalVox->setPixelFormat( PFG_R10G10B10A2_UNORM );
        if( hasTypedUavs )
            mAccumValVox->setPixelFormat( PFG_R16_UINT );
        else
            mAccumValVox->setPixelFormat( PFG_R32_UINT );

        // Jahshaka fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065): thirteen texels per voxel, interleaved in Z — the
        // shader derives their coordinates from the voxel's own, so a dispatch
        // that covers one OCTANT needs to know nothing about the volume's depth.
        // R32_UINT rather than RGBA32_UINT because ComputeTools' clear of a
        // 128-bit 3D uav loses the device on this driver (VoxelMerge_piece_cs),
        // and thirteen rather than sixteen because only thirteen carry anything.
        mMergeAccumTex->scheduleTransitionTo( GpuResidency::OnStorage );
        mMergeAccumTex->setPixelFormat( PFG_R32_UINT );
        // Jahshaka (PHOTON-WRITER-1): FOURTEEN scalar sums per voxel - the thirteen of
        // fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065) and the material roughness; (PHOTON-VOXEL-3) FIFTEEN - the three
        // per-axis coverage sums packed into one (VoxelMerge_piece_cs.any, voxelMergeUvw);
        // (PHOTON-VOXEL-4) EIGHTEEN - the coverage split by the side a face looks to
        // (two packed sums) and the two position-weighted sums.
        mMergeAccumTex->setResolution( mWidth, mHeight, mDepth * 18u );
        mMergeAccumTex->setNumMipmaps( 1u );
        mMergeAccumTex->scheduleTransitionTo( GpuResidency::Resident );

        // Jahshaka (PHOTON-VOXEL-4): THE CHAIN STOPS WHERE THE SHORTEST AXIS REACHES ONE TEXEL.
        // A cell is a cube and every level halves every axis, so every texel stays a cube -
        // one level is one footprint in every direction (the reader's kernels). A volume of
        // 32 x 16 x 32 cells has five levels, not six: a sixth would be 1 x 1 x 1 over a
        // 2 x 1 x 2 block of the fifth, a texel twice as wide as tall. A cube's chain is the
        // same as before.
        const uint8 numMipmaps =
            PixelFormatGpuUtils::getMaxMipmapCount( std::min( mWidth, std::min( mHeight, mDepth ) ) );

        // Jahshaka (PHOTON-VOXEL-3/-4): the coverage per half-axis, 10 bits a component
        // (the merge resolves it on a 1/512 grid) - 2 x 4 bytes a voxel; the surface
        // position per half, 16 bits a component ABSOLUTE in the volume's normalised
        // coordinate (1/65535 of the box: 0.002 cells at 128^3) - 2 x 8 bytes. With the
        // mips (+1/7): 27.4 B a voxel, 57 MB at 128^3, 7.2 MB at 64^3, 0.9 MB at 32^3 per
        // cascade.
        for( size_t h = 0u; h < 2u; ++h )
        {
            mCoverageVox[h]->scheduleTransitionTo( GpuResidency::OnStorage );
            mCoverageVox[h]->setPixelFormat( PFG_R10G10B10A2_UNORM );
            mCoverageVox[h]->setResolution( mWidth, mHeight, mDepth );
            mCoverageVox[h]->setNumMipmaps( numMipmaps );
            mCoverageVox[h]->scheduleTransitionTo( GpuResidency::Resident );
            mPositionVox[h]->scheduleTransitionTo( GpuResidency::OnStorage );
            mPositionVox[h]->setPixelFormat( PFG_RGBA16_UNORM );
            mPositionVox[h]->setResolution( mWidth, mHeight, mDepth );
            mPositionVox[h]->setNumMipmaps( numMipmaps );
            mPositionVox[h]->scheduleTransitionTo( GpuResidency::Resident );
        }

        for( size_t i = 0; i < sizeof( textures ) / sizeof( textures[0] ); ++i )
        {
            if( textures[i] != mAccumValVox || hasTypedUavs )
                textures[i]->setResolution( mWidth, mHeight, mDepth );
            else
                textures[i]->setResolution( mWidth >> 1u, mHeight, mDepth );
            if( ( ( mNeedsAlbedoMipmaps && i == 0u ) || mNeedsAllMipmaps ) && i < 3u )
                textures[i]->setNumMipmaps( numMipmaps );
            else
                textures[i]->setNumMipmaps( 1u );
            textures[i]->scheduleTransitionTo( GpuResidency::Resident );
        }

        if( mDebugVoxelVisualizer )
        {
            setTextureToDebugVisualizer();
            mDebugVoxelVisualizer->setVisible( true );
        }
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelizer::setRegionToVoxelize( const Aabb &regionToVoxelize )
    {
        mRegionToVoxelize = regionToVoxelize;

        // THE OCTANTS DESCRIBE THE REGION, so a region that moves takes them with it.
        // dividideOctants() COPIES mRegionToVoxelize into every octant's own Aabb, and
        // the octant's box is then used twice: the host's gather culls records against
        // it (getOctantRegion), and build() passes its minimum as `voxelOrigin` - the
        // world point the voxelisation shader writes from. Leave them behind and the
        // next build() voxelises the geometry of the OLD box, at the OLD origin, into a
        // texture that PhotonVoxelLighting maps onto the NEW one (fillConstBufferData reads
        // getVoxelOrigin() live): a wrong bounce, permanently, with no log line, no
        // validation error and no cost difference. Re-deriving (rather than clearing)
        // keeps every caller working: a clear would leave build() with an empty octant
        // list, which is an assert in debug and a silently BLACK volume in release.
        if( mNumOctantsX )
            dividideOctants( mNumOctantsX, mNumOctantsY, mNumOctantsZ );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelizer::dividideOctants( uint32 numOctantsX, uint32 numOctantsY, uint32 numOctantsZ )
    {
        // Remembered so that setRegionToVoxelize can re-derive them when the region
        // moves (see the note there).
        mNumOctantsX = numOctantsX;
        mNumOctantsY = numOctantsY;
        mNumOctantsZ = numOctantsZ;

        mOctants.clear();
        mOctants.reserve( numOctantsX * numOctantsY * numOctantsZ );

        OGRE_ASSERT_LOW( mWidth % numOctantsX == 0 );
        OGRE_ASSERT_LOW( mHeight % numOctantsY == 0 );
        OGRE_ASSERT_LOW( mDepth % numOctantsZ == 0 );

        Octant octant;
        octant.width = mWidth / numOctantsX;
        octant.height = mHeight / numOctantsY;
        octant.depth = mDepth / numOctantsZ;

        const Vector3 voxelOrigin = mRegionToVoxelize.getMinimum();
        const Vector3 voxelCellSize = mRegionToVoxelize.getSize() /
                                      Vector3( (Real)numOctantsX, (Real)numOctantsY, (Real)numOctantsZ );

        for( uint32 x = 0u; x < numOctantsX; ++x )
        {
            octant.x = x * octant.width;
            for( uint32 y = 0u; y < numOctantsY; ++y )
            {
                octant.y = y * octant.height;
                for( uint32 z = 0u; z < numOctantsZ; ++z )
                {
                    octant.z = z * octant.depth;

                    Vector3 octantOrigin = Vector3( (Real)x, (Real)y, (Real)z ) * voxelCellSize;
                    octantOrigin += voxelOrigin;
                    octant.region.setExtents( octantOrigin, octantOrigin + voxelCellSize );
                    mOctants.push_back( octant );
                }
            }
        }
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelizer::clearVoxels()
    {
        OgreProfileGpuBegin( "VCT Voxelization Clear" );
        float fClearValue[4];
        uint32 uClearValue[4];
        float fClearNormals[4];
        memset( fClearValue, 0, sizeof( fClearValue ) );
        memset( uClearValue, 0, sizeof( uClearValue ) );
        fClearNormals[0] = 0.5f;
        fClearNormals[1] = 0.5f;
        fClearNormals[2] = 0.5f;
        fClearNormals[3] = 0.0f;

        mResourceTransitions.clear();
        mComputeTools->prepareForUavClear( mResourceTransitions, mAlbedoVox );
        mComputeTools->prepareForUavClear( mResourceTransitions, mEmissiveVox );
        mComputeTools->prepareForUavClear( mResourceTransitions, mNormalVox );
        mComputeTools->prepareForUavClear( mResourceTransitions, mAccumValVox );
        // Jahshaka fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065): the sums start at zero, every build.
        mComputeTools->prepareForUavClear( mResourceTransitions, mMergeAccumTex );
        for( size_t h = 0u; h < 2u; ++h )
        {
            mComputeTools->prepareForUavClear( mResourceTransitions, mCoverageVox[h] );
            mComputeTools->prepareForUavClear( mResourceTransitions, mPositionVox[h] );
        }
        mRenderSystem->executeResourceTransition( mResourceTransitions );

        mComputeTools->clearUavFloat( mAlbedoVox, fClearValue );
        mComputeTools->clearUavFloat( mEmissiveVox, fClearValue );
        mComputeTools->clearUavFloat( mNormalVox, fClearNormals );
        mComputeTools->clearUavUint( mAccumValVox, uClearValue );
        mComputeTools->clearUavUint( mMergeAccumTex, uClearValue );
        for( size_t h = 0u; h < 2u; ++h )
        {
            mComputeTools->clearUavFloat( mCoverageVox[h], fClearValue );
            mComputeTools->clearUavFloat( mPositionVox[h], fClearValue );
        }
        OgreProfileGpuEnd( "VCT Voxelization Clear" );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelizer::setResolution( uint32 width, uint32 height, uint32 depth )
    {
        destroyVoxelTextures();
        mWidth = width;
        mHeight = height;
        mDepth = depth;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelizer::setInstanceSource( UavBufferPacked *records, UavBufferPacked *ranges )
    {
        mRecords = records;
        mRanges = ranges;
        // The read-only VIEW is the records buffer's own (cached on it, destroyed with
        // it), so it is re-taken on every bind rather than kept across a host's grow.
        mRecordsAsTex = records ? records->getAsReadOnlyBufferView() : 0;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelizer::computePartitionAabbs( HlmsManager *hlmsManager, RenderSystem *renderSystem,
                                              UavBufferPacked *geometryRows,
                                              TexBufferPacked *partitions,
                                              UavBufferPacked *outAabbs, uint32 numPartitions )
    {
        if( !numPartitions || !geometryRows || !partitions || !outAabbs || !hlmsManager ||
            !renderSystem )
        {
            return;
        }
        OgreProfile( "PhotonVoxelizer::computePartitionAabbs" );

        HlmsCompute *hlmsCompute = hlmsManager->getComputeHlms();
        HlmsComputeJob *job = hlmsCompute->findComputeJob( "VCT/AabbCalculator" );

        // ONE WORKGROUP WALKS EVERY PARTITION (the job's own shape: its body loops
        // meshStart..meshEnd and reduces each partition across the whole group), sized
        // to the device's widest group axis exactly as upstream sized it.
        const RenderSystemCapabilities *caps = renderSystem->getCapabilities();
        job->setThreadsPerGroup( caps->getMaxThreadsPerThreadgroupAxis()[0], 1u, 1u );

        renderSystem->endRenderPassDescriptor();

        DescriptorSetUav::BufferSlot bufferSlot( DescriptorSetUav::BufferSlot::makeEmpty() );
        bufferSlot.buffer = geometryRows;
        bufferSlot.access = ResourceAccess::Read;
        job->_setUavBuffer( 0, bufferSlot );
        bufferSlot.buffer = outAabbs;
        bufferSlot.access = ResourceAccess::Write;
        job->_setUavBuffer( 1, bufferSlot );

        DescriptorSetTexture2::BufferSlot texBufSlot( DescriptorSetTexture2::BufferSlot::makeEmpty() );
        texBufSlot.buffer = partitions;
        job->setTexBuffer( 0, texBufSlot );

        const uint32 meshRange[2] = { 0u, numPartitions };
        ShaderParams::Param paramMeshRange;
        paramMeshRange.name = "meshStart_meshEnd";
        paramMeshRange.setManualValue( meshRange, 2u );
        ShaderParams &shaderParams = job->getShaderParams( "default" );
        shaderParams.mParams.clear();
        shaderParams.mParams.push_back( paramMeshRange );
        shaderParams.setDirty();

        OgreProfileGpuBegin( "VCT partition AABBs" );
        ResourceTransitionArray transitions;
        job->analyzeBarriers( transitions );
        renderSystem->executeResourceTransition( transitions );
        hlmsCompute->dispatch( job, 0, 0 );
        OgreProfileGpuEnd( "VCT partition AABBs" );

        // The buffers are the caller's and the job is shared: leave nothing bound.
        job->clearUavBuffers();
        job->clearTexBuffers();
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelizer::build( SceneManager *sceneManager )
    {
        // The scene manager is no longer needed here: build() converts no material (the
        // owner does, in its own bracket, before it gathers), so it touches no temp
        // resource and walks no item. Kept in the signature because it is upstream's.
        (void)sceneManager;
        OgreProfile( "PhotonVoxelizer::build" );
        OgreProfileGpuBegin( "VCT build" );

        OGRE_ASSERT_LOW( !mOctants.empty() );

        mRenderSystem->endRenderPassDescriptor();

        createVoxelTextures();

        // NOTHING TO VOXELISE: no geometry rows, no instance source, or a store that
        // has converted no material. The volumes are cleared and that is the honest
        // empty picture. (A source with zero records is NOT this case - its dispatches
        // run, each looping over none; the host cannot know the counts without a
        // readback, and the thread count is the octant's, so there is nothing to size.)
        const uint32 numBuckets =
            mVctMaterial ? static_cast<uint32>( mVctMaterial->getNumBuckets() ) : 0u;
        if( !mGeometryBuffer || !mRecords || !mRecordsAsTex || !mRanges || !numBuckets )
        {
            mLastDispatchCount = 0u;
            clearVoxels();
            // The coverage's mips are read beside the light volume's (the isotropic
            // tier): an empty store is empty at every mip.
            for( size_t h = 0u; h < 2u; ++h )
            {
                mCoverageVox[h]->_autogenerateMipmaps();
                mPositionVox[h]->_autogenerateMipmaps();
            }
            OgreProfileGpuEnd( "VCT build" );
            return;
        }

        const bool hasTypedUavs = mRenderSystem->getCapabilities()->hasCapability( RSC_TYPED_UAV_LOADS );

        for( size_t i = 0; i < sizeof( mComputeJobs ) / sizeof( mComputeJobs[0] ); ++i )
        {
            // THE GEOMETRY TABLE, the same for every variant (the format is a field of
            // a row, not a permutation).
            DescriptorSetUav::BufferSlot bufferSlot( DescriptorSetUav::BufferSlot::makeEmpty() );
            bufferSlot.buffer = mGeometryBuffer;
            bufferSlot.access = ResourceAccess::Read;
            mComputeJobs[i]->_setUavBuffer( 0, bufferSlot );

            // THE RANGES (U1): each dispatch's (start, count) into the records, written
            // by the host's gather on the device. Beside the geometry table and NOT
            // after the images: Ogre's compute root layout packs a job's UAV buffers
            // and UAV textures as two contiguous ranges (HlmsComputeJob::
            // setupRootLayout), so a buffer after the images would leave the images'
            // bindings undeclared. The shader's instance LOOP BOUND comes
            // from here - which is all the count needs to be, because the dispatch's
            // thread count is the octant's and never the instance count's.
            bufferSlot.buffer = mRanges;
            bufferSlot.access = ResourceAccess::Read;
            mComputeJobs[i]->_setUavBuffer( 1, bufferSlot );

            DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
            uavSlot.access = ResourceAccess::ReadWrite;

            uavSlot.texture = mAlbedoVox;
            if( hasTypedUavs )
                uavSlot.pixelFormat = mAlbedoVox->getPixelFormat();
            else
                uavSlot.pixelFormat = PFG_R32_UINT;
            uavSlot.access = ResourceAccess::ReadWrite;
            mComputeJobs[i]->_setUavTexture( 2, uavSlot );

            uavSlot.texture = mNormalVox;
            if( hasTypedUavs )
                uavSlot.pixelFormat = mNormalVox->getPixelFormat();
            else
                uavSlot.pixelFormat = PFG_R32_UINT;
            uavSlot.access = ResourceAccess::ReadWrite;
            mComputeJobs[i]->_setUavTexture( 3, uavSlot );

            uavSlot.texture = mEmissiveVox;
            if( hasTypedUavs )
                uavSlot.pixelFormat = mEmissiveVox->getPixelFormat();
            else
                uavSlot.pixelFormat = PFG_R32_UINT;
            uavSlot.access = ResourceAccess::ReadWrite;
            mComputeJobs[i]->_setUavTexture( 4, uavSlot );

            // Jahshaka fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065): THIS SLOT WAS THE TRIANGLE COUNTER and is now the
            // per-voxel INTEGER ACCUMULATOR the merge sums into (the count rides in
            // it). Same slot, same uimage3D, so this job's binding shape — and the
            // Vulkan root layout Ogre derives from it — is exactly the pin's. That
            // is not cosmetic: a separate VCT/VoxelResolve compute job was tried
            // first and corrupted the descriptor set of the job dispatched after it
            // (VUID-VkWriteDescriptorSet-descriptorType-00319 on LightInjection's
            // `lightVoxel`, then VK_ERROR_DEVICE_LOST on the Showroom samples).
            uavSlot.texture = mAccumValVox;
            uavSlot.pixelFormat = mAccumValVox->getPixelFormat();
            uavSlot.access = ResourceAccess::ReadWrite;
            mComputeJobs[i]->_setUavTexture( 5, uavSlot );

            // Jahshaka fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065): the per-voxel INTEGER ACCUMULATOR the merge
            // sums into (R32_UINT, thirteen texels per voxel — see
            // VoxelMerge_piece_cs.any for why it is not RGBA32_UINT).
            uavSlot.texture = mMergeAccumTex;
            uavSlot.pixelFormat = mMergeAccumTex->getPixelFormat();
            uavSlot.access = ResourceAccess::ReadWrite;
            mComputeJobs[i]->_setUavTexture( 6, uavSlot );

            // Jahshaka (PHOTON-VOXEL-3/-4): the coverage per half-axis (7, 8) and the
            // surface position per half (9, 10) the resolve writes.
            for( size_t h = 0u; h < 2u; ++h )
            {
                uavSlot.texture = mCoverageVox[h];
                uavSlot.pixelFormat = mCoverageVox[h]->getPixelFormat();
                uavSlot.access = ResourceAccess::Write;
                mComputeJobs[i]->_setUavTexture( uint8( 7u + h ), uavSlot );
                uavSlot.texture = mPositionVox[h];
                uavSlot.pixelFormat = mPositionVox[h]->getPixelFormat();
                uavSlot.access = ResourceAccess::Write;
                mComputeJobs[i]->_setUavTexture( uint8( 9u + h ), uavSlot );
            }

            DescriptorSetTexture2::BufferSlot texBufSlot(
                DescriptorSetTexture2::BufferSlot::makeEmpty() );
            texBufSlot.buffer = mRecordsAsTex;
            mComputeJobs[i]->setTexBuffer( 0, texBufSlot );
        }

        HlmsCompute *hlmsCompute = mHlmsManager->getComputeHlms();
        clearVoxels();
        mLastDispatchCount = 0u;

        const uint32 *threadsPerGroup = mComputeJobs[0]->getThreadsPerGroup();

        ShaderParams::Param paramRange;
        ShaderParams::Param paramVoxelOrigin;
        ShaderParams::Param paramVoxelCellSize;
        ShaderParams::Param paramVoxelPixelOrigin;

        paramRange.name = "rangeIdx_pad";
        paramVoxelOrigin.name = "voxelOrigin";
        paramVoxelCellSize.name = "voxelCellSize";
        paramVoxelPixelOrigin.name = "voxelPixelOrigin";

        paramVoxelCellSize.setManualValue( getVoxelCellSize() );

        OgreProfileGpuBegin( "VCT Voxelization Jobs" );

        // ONE DISPATCH PER (octant, bucket), where a bucket is one of the STORE's
        // material pools. The order is the store's bucket order, which is a stable
        // order; fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065)'s order-independent merge means it decides nothing about
        // the voxels anyway.
        const uint32 numOctants = static_cast<uint32>( mOctants.size() );
        for( uint32 octantIdx = 0u; octantIdx < numOctants; ++octantIdx )
        {
            const Octant &octant = mOctants[octantIdx];
            for( uint32 bucketIdx = 0u; bucketIdx < numBuckets; ++bucketIdx )
            {
                const bool hasDiffuse = mVctMaterial->getBucketHasDiffuse( bucketIdx );
                const bool hasEmissive = mVctMaterial->getBucketHasEmissive( bucketIdx );
                uint32 variant = 0u;
                if( hasDiffuse )
                    variant |= PhotonVoxelizerJobSetting::HasDiffuseTex;
                if( hasEmissive )
                    variant |= PhotonVoxelizerJobSetting::HasEmissiveTex;
                HlmsComputeJob *job = mComputeJobs[variant];

                job->setNumThreadGroups( std::max( 1u, octant.width / threadsPerGroup[0] ),
                                         std::max( 1u, octant.height / threadsPerGroup[1] ),
                                         std::max( 1u, octant.depth / threadsPerGroup[2] ) );

                job->setConstBuffer( 0, mVctMaterial->getBucketBuffer( bucketIdx ) );

                if( variant )
                {
                    DescriptorSetTexture2::TextureSlot texSlot(
                        DescriptorSetTexture2::TextureSlot::makeEmpty() );
                    texSlot.texture = mVctMaterial->getTexturePool();
                    HlmsSamplerblock samplerblock;
                    samplerblock.setAddressingMode( TAM_WRAP );
                    job->setTexture( 1u, texSlot, &samplerblock );
                }

                const uint32 range[2] = { rangeIndex( octantIdx, bucketIdx, numBuckets ), 0u };
                const uint32 voxelPixelOrigin[3] = { octant.x, octant.y, octant.z };

                paramRange.setManualValue( range, 2u );
                paramVoxelOrigin.setManualValue( octant.region.getMinimum() );
                paramVoxelPixelOrigin.setManualValue( voxelPixelOrigin, 3u );

                ShaderParams &shaderParams = job->getShaderParams( "default" );
                shaderParams.mParams.clear();
                shaderParams.mParams.push_back( paramRange );
                shaderParams.mParams.push_back( paramVoxelOrigin );
                shaderParams.mParams.push_back( paramVoxelCellSize );
                shaderParams.mParams.push_back( paramVoxelPixelOrigin );
                shaderParams.setDirty();

                job->analyzeBarriers( mResourceTransitions );
                mRenderSystem->executeResourceTransition( mResourceTransitions );
                hlmsCompute->dispatch( job, 0, 0 );
                ++mLastDispatchCount;
            }
        }

        OgreProfileGpuEnd( "VCT Voxelization Jobs" );

        // The dispatches are recorded; the host's buffers may be grown before the next
        // build, so nothing of theirs stays bound in the shared jobs.
        clearComputeJobResources();

        // These textures are no longer needed, they're not used for the injection
        // phase. Save memory.
        mAccumValVox->scheduleTransitionTo( GpuResidency::OnStorage );

        // THE MERGE ACCUMULATOR STAYS RESIDENT (Jahshaka fork ad452604a+0338ca7f2+c4c80b5f7 (was 0071)). fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065)
        // gave it upstream's transient treatment: OnStorage at the end of every
        // build(), Resident at the start of the next one. On NVIDIA 595.84 that
        // per-build create/destroy of a large 3D storage image, while the
        // dispatches that wrote the previous one may still be executing, HANGS
        // THE CHANNEL: NVRM Xid 109 CTX SWITCH TIMEOUT -> VK_ERROR_DEVICE_LOST.
        // Measured (lane XID-2, the owner's own sequence scripted -- hide and
        // show a plane in an Epic scene, 100 cycles a run): 4/4 and 6/6 runs
        // lost the device with the round trip, 0/6 and 0/6 without it, every
        // failure carrying a kernel Xid line from that pid and no passing run
        // ever carrying one. It is NOT the delayed-block reuse window (fork b028638c1
        // (was 0067)'s subject: a 16-frame window still hangs 4/6), NOT the 512 MB
        // force-flush (disabled: 6/6), NOT the cached image views (purged on
        // residency loss: 6/6) and NOT the ray-query tier (rays off: 6/6).
        // Keeping the image alive is the only arm that cures it, and it is also
        // the right design: under a cascade chain a build happens every frame or
        // two, so the residency round trip never actually saves anything -- it
        // only returns memory the next build immediately asks for again.
        // THE COST is one accumulator per voxeliser held for its lifetime:
        // width * height * depth * 13 * 4 bytes (13.6 MB at 64^3, 109 MB at
        // 128^3). A future lane may share ONE scratch volume across a chain's
        // cascades; that is an optimisation, not a correctness matter.

        if( mNeedsAlbedoMipmaps || mNeedsAllMipmaps )
            mAlbedoVox->_autogenerateMipmaps();
        for( size_t h = 0u; h < 2u; ++h )
        {
            mCoverageVox[h]->_autogenerateMipmaps();
            mPositionVox[h]->_autogenerateMipmaps();
        }
        if( mNeedsAllMipmaps )
        {
            mEmissiveVox->_autogenerateMipmaps();
            mNormalVox->_autogenerateMipmaps();
        }

        OgreProfileGpuEnd( "VCT build" );
    }
}  // namespace Ogre
