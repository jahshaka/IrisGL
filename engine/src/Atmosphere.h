// THE PLANET'S ATMOSPHERE (SKY-ATMOSPHERE-1) — a Component of ours in Ogre's own
// shape: an `Ogre::AtmosphereComponent` (the interface HlmsPbs asks for its
// fog block and its const buffer — the one upstream's AtmosphereNpr implements),
// four Hlms compute jobs for its look-up tables, and a sky quad drawn from them.
// Types.h (AtmosphereSky) states the model; media/Hlms/Jahshaka/JahAtmo*_cs.glsl
// and src/rayquery/include/jah_atmosphere.glsl carry its arithmetic.
//
// WHAT IT OWNS, per scene:
//   * the four tables — transmittance 256x64, multiple scattering 32x32 (both
//     functions of the dials alone), the sky view 192x108 and the aerial
//     perspective 32x64x32 (the dials and the sun) — rebuilt inside a frame,
//     before the sky capture, and only when their inputs changed;
//   * the const buffer HlmsPbs binds while this component is registered on the
//     SceneManager: the World fog's distance block (upstream's fog code reads
//     `atmoSettings.fogDensity` and the breakthrough pair from it) and what the
//     pixel shader needs to read the tables (the sun, the planet, the aerial
//     scale, the sun's illuminance at the top of the air);
//   * the sky quad (render queue 0, subgroup 1, kVisibleBit — the environment
//     capture photographs it) and its per-scene material clone.
//
// ONE COMPONENT, TWO CUSTOMERS, as before it: the sky (setAirOn) and the World
// fog under any sky. It is registered while either wants it (OgreScene::
// syncAtmosphere) — registration is what puts `hlms_fog` in the pass hash.
#pragma once

#include <OgreAtmosphereComponent.h>
#include <OgreMaterial.h>
#include <OgreVector3.h>

#include "jahshaka/engine/Types.h"

namespace Ogre {
class Root; class SceneManager; class TextureGpu; class Rectangle2D; class ConstBufferPacked;
class HlmsComputeJob;
}

namespace jahshaka { namespace engine { namespace detail {

/// The model's dials, in the model's units (km, per km).
struct AtmosphereModel {
    float rayleighScale = 1.0f;
    float mieScale = 1.0f;          ///< AtmosphereSky::sunHaze
    bool  ozone = true;
    float groundAlbedo = 0.3f;
    float planetRadiusKm = 6360.0f;
    float atmosphereHeightKm = 100.0f;
    bool operator==(const AtmosphereModel &o) const {
        return rayleighScale == o.rayleighScale && mieScale == o.mieScale && ozone == o.ozone &&
               groundAlbedo == o.groundAlbedo && planetRadiusKm == o.planetRadiusKm &&
               atmosphereHeightKm == o.atmosphereHeightKm;
    }
    bool operator!=(const AtmosphereModel &o) const { return !(*this == o); }
};

class JahAtmosphere final : public Ogre::AtmosphereComponent {
public:
    /// THE TABLES' SIZES (Hillaire's, and the aerial volume's: two direction
    /// axes in the sky view's own parameterisation, and distance).
    static constexpr unsigned kTransW = 256u, kTransH = 64u;
    static constexpr unsigned kMsSize = 32u;
    static constexpr unsigned kSkyW = 192u, kSkyH = 108u;
    static constexpr unsigned kApW = 32u, kApH = 64u, kApD = 32u;
    /// The aerial volume's far slice (km): past it the air in front of a surface
    /// is held at this distance's. 100 km reaches the cloud sheet's far edge.
    static constexpr float kApMaxKm = 100.0f;
    /// The observer's altitude (km): the tables are evaluated for ONE observer,
    /// at the ground (a camera's own height is not tracked — the air a scene's
    /// cameras see differs from it by their height over the aerosol's 1.2 km
    /// scale height).
    static constexpr float kObserverKm = 0.002f;

    JahAtmosphere(Ogre::Root *root, Ogre::SceneManager *sm, Ogre::uint32 visibleBit);
    ~JahAtmosphere() override;

    void setModel(const AtmosphereModel &m);
    const AtmosphereModel &model() const { return mModel; }
    /// The sun (towards it) and whether there is one; the sky view and the
    /// aerial volume are rebuilt when either changes.
    void setSun(const Ogre::Vector3 &toSun, bool hasSun);
    /// The sun's NOON illuminance in the renderer's units (AtmosphereSky::
    /// sunIlluminance); the top-of-air value is this over the zenith
    /// transmittance. A constant, never a rebuild.
    void setSunIlluminance(const Ogre::Vector3 &noon);
    void setAerialScale(float s);
    /// The World fog's distance block, in upstream's packing (0 = none).
    void setFogBlock(float density, float breakMinBrightness, float breakFalloff);
    /// The atmosphere is the scene's sky: the quad is drawn and the pass reads
    /// the tables (the aerial perspective).
    void setAirOn(bool on);
    bool airOn() const { return mAirOn; }

    /// INSIDE A FRAME (a command buffer exists), before the sky capture: the
    /// dirty tables are rebuilt, the const buffer uploaded, the quad's
    /// constants pushed. A no-op on a still frame.
    void update();

    /// The transmittance from radius `rKm` along the zenith cosine `mu` to the
    /// top of the air — the table's integral, on the CPU (the sun's tint).
    Ogre::Vector3 transmittance(float rKm, float mu) const;
    /// The observer's radius (km).
    float observerRadiusKm() const { return mModel.planetRadiusKm + kObserverKm; }
    /// Bumped by every model change (the sun tint's memo key).
    unsigned long long modelGeneration() const { return mModelGeneration; }

    const Ogre::Vector3 &sunDir() const { return mToSun; }
    /// The sun at the top of the air, in the renderer's units (0 with no sun).
    Ogre::Vector3 topOfAir() const { return topIlluminance(); }
    Ogre::TextureGpu *skyViewLut() const { return mSkyView; }
    Ogre::TextureGpu *aerialLut() const { return mAerial; }
    AtmosphereStatus status() const;
    bool measure(unsigned iterations, AtmosphereCost &out);

    // ---- Ogre::AtmosphereComponent -----------------------------------------
    Ogre::uint32 preparePassHash(Ogre::Hlms *hlms, size_t constBufferSlot) override;
    Ogre::uint32 getNumConstBuffersSlots() const override { return 1u; }
    Ogre::uint32 bindConstBuffers(Ogre::CommandBuffer *commandBuffer, size_t slotIdx) override;
    void _update(Ogre::SceneManager *sceneManager, Ogre::Camera *camera) override;

private:
    void createTextures();
    void createQuad();
    void pushJobParams(Ogre::HlmsComputeJob *job) const;
    bool runJob(const char *name, Ogre::TextureGpu *target, Ogre::TextureGpu *in0,
                Ogre::TextureGpu *in1, unsigned gx, unsigned gy, unsigned repeats = 1u);
    void handOver();
    Ogre::Vector3 topIlluminance() const;
    void uploadSettings();
    void pushQuadConstants();

    Ogre::Root *mRoot = nullptr;
    Ogre::SceneManager *mSceneMgr = nullptr;
    Ogre::uint32 mVisibleBit = 1u;
    AtmosphereModel mModel;
    unsigned long long mModelGeneration = 1;
    Ogre::Vector3 mToSun = Ogre::Vector3::UNIT_Y;
    bool mHasSun = false;
    Ogre::Vector3 mSunNoon = Ogre::Vector3::ZERO;
    float mAerialScale = 1.0f;
    float mFogDensity = 0.0f, mFogBreakMin = 0.0f, mFogBreakFalloff = 0.0f;
    bool mAirOn = false;

    bool mDirtyTables = true;   ///< transmittance + multiple scattering (+ both below)
    bool mDirtySun = true;      ///< the sky view + the aerial volume
    bool mDirtyBuffer = true;
    bool mDirtyQuad = true;
    AtmosphereStatus mStatus;

    Ogre::TextureGpu *mTrans = nullptr;
    Ogre::TextureGpu *mMs = nullptr;
    Ogre::TextureGpu *mSkyView = nullptr;
    Ogre::TextureGpu *mAerial = nullptr;
    Ogre::ConstBufferPacked *mBuffer = nullptr;
    Ogre::Rectangle2D *mQuad = nullptr;
    Ogre::MaterialPtr mQuadMaterial;
};

}}}  // namespace jahshaka::engine::detail
