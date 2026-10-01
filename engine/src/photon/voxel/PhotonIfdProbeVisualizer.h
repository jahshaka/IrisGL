
#ifndef JAH_PHOTON_IFD_PROBE_VISUALIZER_H
#define JAH_PHOTON_IFD_PROBE_VISUALIZER_H

#include "OgreHlmsPbsPrerequisites.h"

#include "OgreMovableObject.h"
#include "OgreRenderable.h"

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    struct PhotonIrradianceFieldSettings;

    class PhotonIfdProbeVisualizer : public MovableObject, public Renderable
    {
        void createBuffers();

    public:
        PhotonIfdProbeVisualizer( IdType id, ObjectMemoryManager *objectMemoryManager, SceneManager *manager,
                            uint8 renderQueueId );
        ~PhotonIfdProbeVisualizer() override;

        /**
        @param ifSettings
            See PhotonIrradianceField::mSettings
        @param fieldSize
        @param resolution
            Either ifSettings.mIrradianceResolution or ifSettings.mDepthProbeResolution
        @param ifdTex
            Texture to visualize. Could be colour (irradiance) or depth
        @param tessellation
            Value in range [3; 16]
            Note this value increases exponentially:
                tessellation = 3u -> 24 vertices (per sphere)
                tessellation = 4u -> 112 vertices
                tessellation = 5u -> 480 vertices
                tessellation = 6u -> 1984 vertices
                tessellation = 7u -> 8064 vertices
                tessellation = 8u -> 32512 vertices
                tessellation = 9u -> 130560 vertices
                tessellation = 16u -> 2.147.418.112 vertices
        */
        void setTrackingIfd( const PhotonIrradianceFieldSettings &ifSettings, const Vector3 &fieldSize,
                             uint8 resolution, TextureGpu *ifdTex, const Vector2 &rangeMult,
                             uint8_t tessellation );

        /// Jahshaka (PHOTON-VIEW-1): the field's WINDOW OFFSET (PhotonIrradianceField::scrollWindow,
        /// getWindowOffset): atlas tile s holds the probe at window-local position
        /// ( s - offset ) mod N per axis, so a scrolled field draws each probe's sphere where
        /// that probe stands. All zeros until the field scrolls.
        void setWindowOffset( const uint32 offset[3] );

        /// Jahshaka (D4-PHOTON-TIERS, PROBE-NAN-1): the COLOUR scale the probes are shaded
        /// with (the fragment program's rangeMult). The irradiance atlas holds the field in
        /// the voxels' STORED units (radiance over the decode multiplier D_max / pi, D_max
        /// the brightest light), so a sphere shaded by the raw texel darkens as a lamp gets
        /// brighter - measured: a point light of intensity 20 at 0.3 m over a ground turned
        /// every probe black, the field itself finite and correct. The field sets this to
        /// its PhotonVoxelLighting's decode multiplier on every update, so a probe is drawn with the
        /// radiance a white Lambertian surface there receives - the unit the pixel decodes.
        void setColourScale( float scale );

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
