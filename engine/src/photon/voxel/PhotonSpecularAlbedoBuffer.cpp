// THE SPECULAR LOBE'S ALBEDO TABLE ON THE GPU (photon/voxel/PhotonSpecularAlbedoBuffer.h has the design).
#include "photon/voxel/PhotonSpecularAlbedoBuffer.h"

#include "photon/voxel/PhotonSpecularAlbedo.h"

#include "Vao/OgreConstBufferPacked.h"
#include "Vao/OgreVaoManager.h"

namespace Ogre
{
    namespace
    {
        VaoManager *gVaoManager = nullptr;          // render thread only
        ConstBufferPacked *gBuffer = nullptr;       // render thread only
    }  // namespace

    namespace PhotonSpecularAlbedo
    {
        ConstBufferPacked *constBuffer( VaoManager *vaoManager )
        {
            if( !vaoManager )
                return nullptr;
            if( gBuffer && gVaoManager != vaoManager )
                releaseConstBuffer();
            if( !gBuffer )
            {
                // AS THE SHADER DECLARES IT: float4 jahSpecularAlbedoTable[528] (std140 packs a
                // float4 array tight, so the bytes are the table's).
                static_assert( kFloats == 528u * 4u, "the shader's array is 528 float4s" );
                gBuffer = vaoManager->createConstBuffer( sizeof( kTable ), BT_IMMUTABLE,
                                                         const_cast<float *>( kTable ), false );
                gVaoManager = vaoManager;
            }
            return gBuffer;
        }

        ConstBufferPacked *constBufferIfCreated() { return gBuffer; }

        void releaseConstBuffer()
        {
            if( gBuffer && gVaoManager )
                gVaoManager->destroyConstBuffer( gBuffer );
            gBuffer = nullptr;
            gVaoManager = nullptr;
        }
    }  // namespace PhotonSpecularAlbedo
}  // namespace Ogre
