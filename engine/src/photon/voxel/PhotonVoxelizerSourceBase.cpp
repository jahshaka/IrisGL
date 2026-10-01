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

#include "OgreStableHeaders.h"

#include "photon/voxel/PhotonVoxelizerSourceBase.h"

#include "OgreSceneManager.h"
#include "OgreTextureGpuManager.h"
#include "Vao/OgreVaoManager.h"
#include "photon/voxel/PhotonVoxelVisualizer.h"

namespace Ogre
{
    //-------------------------------------------------------------------------
    PhotonVoxelizerSourceBase::PhotonVoxelizerSourceBase( IdType id, RenderSystem *renderSystem,
                                                    HlmsManager *hlmsManager ) :
        IdObject( id ),
        mAlbedoVox( 0 ),
        mEmissiveVox( 0 ),
        mNormalVox( 0 ),
        mAccumValVox( 0 ),
        mCoverageVox{ 0, 0 },
        mPositionVox{ 0, 0 },
        mRenderSystem( renderSystem ),
        mVaoManager( renderSystem->getVaoManager() ),
        mHlmsManager( hlmsManager ),
        mTextureGpuManager( renderSystem->getTextureGpuManager() ),
        mDebugVisualizationMode( DebugVisualizationNone ),
        mDebugVoxelVisualizer( 0 ),
        mWidth( 128u ),
        mHeight( 128u ),
        mDepth( 128u ),
        mRegionToVoxelize( Aabb::BOX_ZERO )
    {
    }
    //-------------------------------------------------------------------------
    PhotonVoxelizerSourceBase::~PhotonVoxelizerSourceBase()
    {
        setDebugVisualization( DebugVisualizationNone, 0 );
        destroyVoxelTextures();
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelizerSourceBase::setTextureToDebugVisualizer()
    {
        TextureGpu *trackedTex = mAlbedoVox;
        switch( mDebugVisualizationMode )
        {
        default:
        case DebugVisualizationAlbedo:
            trackedTex = mAlbedoVox;
            break;
        case DebugVisualizationNormal:
            trackedTex = mNormalVox;
            break;
        case DebugVisualizationEmissive:
            trackedTex = mEmissiveVox;
            break;
        }

        BarrierSolver &solver = mRenderSystem->getBarrierSolver();
        ResourceTransitionArray resourceTransitions;
        solver.resolveTransition( resourceTransitions, mAlbedoVox, ResourceLayout::Texture,
                                  ResourceAccess::Read, 1u << VertexShader );
        if( mAlbedoVox != trackedTex )
        {
            solver.resolveTransition( resourceTransitions, trackedTex, ResourceLayout::Texture,
                                      ResourceAccess::Read, 1u << VertexShader );
        }
        mRenderSystem->executeResourceTransition( resourceTransitions );

        mDebugVoxelVisualizer->setTrackingVoxel( mAlbedoVox, trackedTex,
                                                 mDebugVisualizationMode == DebugVisualizationEmissive );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelizerSourceBase::destroyVoxelTextures()
    {
        if( mAlbedoVox )
        {
            mTextureGpuManager->destroyTexture( mAlbedoVox );
            mTextureGpuManager->destroyTexture( mEmissiveVox );
            mTextureGpuManager->destroyTexture( mNormalVox );
            mTextureGpuManager->destroyTexture( mAccumValVox );
            for( size_t h = 0u; h < 2u; ++h )
            {
                if( mCoverageVox[h] )
                    mTextureGpuManager->destroyTexture( mCoverageVox[h] );
                if( mPositionVox[h] )
                    mTextureGpuManager->destroyTexture( mPositionVox[h] );
                mCoverageVox[h] = 0;
                mPositionVox[h] = 0;
            }

            mAlbedoVox = 0;
            mEmissiveVox = 0;
            mNormalVox = 0;
            mAccumValVox = 0;

            if( mDebugVoxelVisualizer )
                mDebugVoxelVisualizer->setVisible( false );
        }
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelizerSourceBase::setDebugVisualization(
        PhotonVoxelizerSourceBase::DebugVisualizationMode mode, SceneManager *sceneManager )
    {
        if( mDebugVoxelVisualizer )
        {
            SceneNode *sceneNode = mDebugVoxelVisualizer->getParentSceneNode();
            sceneNode->getParentSceneNode()->removeAndDestroyChild( sceneNode );
            OGRE_DELETE mDebugVoxelVisualizer;
            mDebugVoxelVisualizer = 0;
        }

        mDebugVisualizationMode = mode;

        if( mode != DebugVisualizationNone )
        {
            SceneNode *rootNode = sceneManager->getRootSceneNode( SCENE_STATIC );
            SceneNode *visNode = rootNode->createChildSceneNode( SCENE_STATIC );

            mDebugVoxelVisualizer = OGRE_NEW PhotonVoxelVisualizer(
                Ogre::Id::generateNewId<Ogre::MovableObject>(),
                &sceneManager->_getEntityMemoryManager( SCENE_STATIC ), sceneManager, 0u );

            setTextureToDebugVisualizer();

            visNode->setPosition( getVoxelOrigin() );
            visNode->setScale( getVoxelCellSize() );
            visNode->attachObject( mDebugVoxelVisualizer );
        }
    }
    //-------------------------------------------------------------------------
    PhotonVoxelizerSourceBase::DebugVisualizationMode PhotonVoxelizerSourceBase::getDebugVisualizationMode(
        void ) const
    {
        return mDebugVisualizationMode;
    }
    //-------------------------------------------------------------------------
    Vector3 PhotonVoxelizerSourceBase::getVoxelOrigin() const { return mRegionToVoxelize.getMinimum(); }
    //-------------------------------------------------------------------------
    Vector3 PhotonVoxelizerSourceBase::getVoxelCellSize() const
    {
        return mRegionToVoxelize.getSize() / getVoxelResolution();
    }
    //-------------------------------------------------------------------------
    Vector3 PhotonVoxelizerSourceBase::getVoxelSize() const { return mRegionToVoxelize.getSize(); }
    //-------------------------------------------------------------------------
    Vector3 PhotonVoxelizerSourceBase::getVoxelResolution() const
    {
        return Vector3( (Real)mWidth, (Real)mHeight, (Real)mDepth );
    }
    //-------------------------------------------------------------------------
    TextureGpuManager *PhotonVoxelizerSourceBase::getTextureGpuManager() { return mTextureGpuManager; }
    //-------------------------------------------------------------------------
    RenderSystem *PhotonVoxelizerSourceBase::getRenderSystem() { return mRenderSystem; }
    //-------------------------------------------------------------------------
    HlmsManager *PhotonVoxelizerSourceBase::getHlmsManager() { return mHlmsManager; }
}  // namespace Ogre
