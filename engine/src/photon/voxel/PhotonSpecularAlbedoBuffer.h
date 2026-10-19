// THE SPECULAR LOBE'S ALBEDO TABLE ON THE GPU (PHOTON-3 stage 1): PhotonSpecularAlbedo.h's 2,112
// floats (8,448 B, 528 float4s) in ONE immutable constant buffer for the whole process, read by
// every shader that evaluates the lobe's integral through JahSpecularAlbedo (the fork's
// Hlms/Pbs/Any/JahSpecularAlbedo_piece_all.any):
//
//   * every PBS-family colour pass - BRDF_EnvMap's split sum, the environment's specular term -
//     at the slot FogHlmsListener names (jah_spec_albedo_cb: past the pass's own buffers and the
//     manual probe's) and binds on every Hlms-type switch;
//   * the Photon jobs: the light injection (its constant buffer 1), the bounce (0) and the cards'
//     relight (0).
//
// The ray tier's raw-Vulkan jobs read the same floats out of their hit table, which they reach by
// one device address (OgreRayQuery.cpp), not through Ogre's descriptor tables.
//
// Created at the engine's start (OgreEngine, after the render system: an immutable buffer's
// upload is a copy the device queue records, never inside a render pass) and destroyed by
// releaseConstBuffer at its teardown, before Root. A different VaoManager than the one that
// made it (only a missed release could leave one) releases the old buffer first.
#ifndef JAH_PHOTON_SPECULAR_ALBEDO_BUFFER_H
#define JAH_PHOTON_SPECULAR_ALBEDO_BUFFER_H

namespace Ogre
{
    class ConstBufferPacked;
    class VaoManager;

    namespace PhotonSpecularAlbedo
    {
        /// The one buffer, created on the first call with `vaoManager` (render thread only).
        ConstBufferPacked *constBuffer( VaoManager *vaoManager );
        /// The buffer if it exists, else null (a hook that must never create it).
        ConstBufferPacked *constBufferIfCreated();
        /// Destroys it - the engine's teardown, before the render system dies.
        void releaseConstBuffer();
    }  // namespace PhotonSpecularAlbedo
}  // namespace Ogre

#endif
