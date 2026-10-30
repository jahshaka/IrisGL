/*
-----------------------------------------------------------------------------
This source file is part of OGRE
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
#ifndef JAH_PHOTON_VOXELIZER_SOURCE_BASE_H
#define JAH_PHOTON_VOXELIZER_SOURCE_BASE_H

#include "OgreHlmsPbsPrerequisites.h"

#include "Math/Simple/OgreAabb.h"
#include "OgreId.h"

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class PhotonVoxelVisualizer;

    /**
    @class PhotonVoxelizerSourceBase
        This class contains shared functionality between voxelizer; and
        is used by PhotonVoxelLighting to source its voxel data to generate GI
    */
    class PhotonVoxelizerSourceBase : public IdObject
    {
    public:
        enum DebugVisualizationMode
        {
            DebugVisualizationAlbedo,
            DebugVisualizationNormal,
            DebugVisualizationEmissive,
            DebugVisualizationNone
        };

    protected:
        TextureGpu *mAlbedoVox;
        TextureGpu *mEmissiveVox;
        /// Jahshaka (METAL-BOUNCE-1): THE METAL - its F0 (rgb) and Fresnel-90 weight (alpha), RGBA16_UNORM, level 0
        /// only (VoxelMerge_piece_cs.any, voxelMergeMetal).
        TextureGpu *mMetalVox;
        TextureGpu *mNormalVox;
        TextureGpu *mAccumValVox;
        /// Jahshaka (PHOTON-VOXEL-3/-4): THE PER-HALF-AXIS COVERAGE (RGB10A2 each: O_x, O_y,
        /// O_z) - [0] of the faces looking +a, [1] of those looking -a: the fraction of each
        /// voxel face its surfaces cover along that axis, split by the side they face. A ray
        /// travelling +a reads the -a half (VoxelMerge_piece_cs.any, THE DIRECTIONAL
        /// COVERAGE); PhotonVoxelLighting exposes both as entries of its light-volume list, so every
        /// reader that binds the list binds them too.
        TextureGpu *mCoverageVox[2] = {};
        /// Jahshaka (JUNCTION-NORMAL-1): THE EMISSIVE FACTOR PER HALF-AXIS (RGB10A2 each, level 0 only) -
        /// [0] the faces looking +a, [1] -a: what a half's own faces emit over the voxel's area-mean emissive,
        /// as u = f / (1 + f) on 1022 steps (VoxelMerge_piece_cs.any, THE EMISSIVE PER HALF-AXIS).
        TextureGpu *mEmitVox[2] = {};
        /// Jahshaka (PHOTON-VOXEL-4): THE SURFACE POSITION per half-axis (RGBA16_UNORM, xyz)
        /// - the coverage-weighted mean position of each half's surfaces along each axis,
        /// ABSOLUTE in the volume's normalised coordinate and premultiplied by that half's
        /// coverage (VoxelMerge_piece_cs.any, THE SURFACE POSITION): the origin plane's
        /// test. The list's last two entries.
        TextureGpu *mPositionVox[2] = {};
        /// VOXEL-R13 stage 1: THE HALF'S OWN FACES - where each half-axis's faces lie ACROSS its axis and which way they
        /// look, level 0 only (VoxelMerge_piece_cs.any, THE HALF'S OWN FACES). Per axis a, one volume holding both halves:
        /// [a] of mHalfSideVox (RGBA16_UNORM) the coverage-weighted mean of the faces' centroids on the two other axes,
        /// in the cell's own [0, 1] (rg the +a half's, ba the -a half's; the other axes in increasing order); [a] of
        /// mHalfNrmVox (RGBA8_UNORM) the coverage-weighted mean of the faces' normals as a hemisphere octahedron about
        /// the half's own direction (rg the +a half's, ba the -a half's; 0.5 0.5 the axis itself). A half's gather and
        /// its lamp march start on its own faces with its own normal from them (the light injection, the bounce).
        TextureGpu *mHalfSideVox[3] = {};
        TextureGpu *mHalfNrmVox[3] = {};

        RenderSystem      *mRenderSystem = nullptr;
        VaoManager        *mVaoManager = nullptr;
        HlmsManager       *mHlmsManager = nullptr;
        TextureGpuManager *mTextureGpuManager = nullptr;

        DebugVisualizationMode mDebugVisualizationMode = DebugVisualizationNone;
        PhotonVoxelVisualizer       *mDebugVoxelVisualizer = nullptr;

        uint32 mWidth = 0u;
        uint32 mHeight = 0u;
        uint32 mDepth = 0u;

        Aabb mRegionToVoxelize;

        virtual void destroyVoxelTextures();
        void         setTextureToDebugVisualizer();

    public:
        PhotonVoxelizerSourceBase( IdType id, RenderSystem *renderSystem, HlmsManager *hlmsManager );
        virtual ~PhotonVoxelizerSourceBase();

        void setDebugVisualization( PhotonVoxelizerSourceBase::DebugVisualizationMode mode,
                                    SceneManager                                  *sceneManager );
        PhotonVoxelizerSourceBase::DebugVisualizationMode getDebugVisualizationMode() const;

        Vector3 getVoxelOrigin() const;
        Vector3 getVoxelCellSize() const;
        Vector3 getVoxelSize() const;
        Vector3 getVoxelResolution() const;

        TextureGpu *getAlbedoVox() { return mAlbedoVox; }
        TextureGpu *getNormalVox() { return mNormalVox; }
        TextureGpu *getEmissiveVox() { return mEmissiveVox; }
        TextureGpu *getMetalVox() { return mMetalVox; }
        /// `half`: 0 the faces looking +a, 1 those looking -a.
        TextureGpu *getCoverageVox( size_t half ) { return mCoverageVox[half]; }
        TextureGpu *getPositionVox( size_t half ) { return mPositionVox[half]; }
        TextureGpu *getEmitVox( size_t half ) { return mEmitVox[half]; }
        /// VOXEL-R13: `axis` 0..2 - both halves of that axis (mHalfSideVox / mHalfNrmVox).
        TextureGpu *getHalfSideVox( size_t axis ) { return mHalfSideVox[axis]; }
        TextureGpu *getHalfNrmVox( size_t axis ) { return mHalfNrmVox[axis]; }

        TextureGpuManager *getTextureGpuManager();
        RenderSystem      *getRenderSystem();
        HlmsManager       *getHlmsManager();
    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
