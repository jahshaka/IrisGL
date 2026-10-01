
#ifndef JAH_PHOTON_VOXEL_VISUALIZER_H
#define JAH_PHOTON_VOXEL_VISUALIZER_H

#include "OgreHlmsPbsPrerequisites.h"

#include "OgreMovableObject.h"
#include "OgreRenderable.h"

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class PhotonVoxelVisualizer : public MovableObject, public Renderable
    {
        void createBuffers();

    public:
        PhotonVoxelVisualizer( IdType id, ObjectMemoryManager *objectMemoryManager, SceneManager *manager,
                         uint8 renderQueueId );
        ~PhotonVoxelVisualizer() override;

        void setTrackingVoxel( TextureGpu *opacityTex, TextureGpu *texture, bool anyColour );

        // Overrides from MovableObject
        const String &getMovableType() const override;

        // Overrides from Renderable
        const LightList &getLights() const override;
        void             getRenderOperation( v1::RenderOperation &op, bool casterPass ) override;
        void             getWorldTransforms( Matrix4 *xform ) const override;
        bool             getCastsShadows() const override;
    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
