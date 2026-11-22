#pragma once
// THE ENGINE's VOXELREADER TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {

/// ONE CONE FOR THE ONE VOXEL READER'S PARITY HARNESS (PHOTON-READER-1;
/// Engine::voxelReaderParity, engine.voxel_reader_parity). Everything is in the
/// normalised space of cascade 0 of the scene's chain, as the reader's callers
/// hand it (the pixel shader after its own start bias, the irradiance field's
/// probe rays from the probe's position).
struct VoxelReaderCone {
    Vec3     posLS;                  ///< where the march starts
    Vec3     dirLS;                  ///< unit direction
    Vec3     biasDirLS;              ///< the hop's bias direction (zero: a point in free space)
    float    tanHalfAngle = 0.577f;  ///< the diffuse cone set's half angle
    unsigned flags = 0u;             ///< JAH_MARCH_* (1 specular, 2 SDF, 32 one step), and kAxisRead
    /// THE DIRECTIONAL READ ITSELF (PHOTON-7 stage 1's read F2): with this flag the answer's `hitRead` is
    /// jahVoxelReadAxis at `posLS` along `dirLS`'s major axis (the half its sign reads), `lod` taken AS THE
    /// LEVEL - fractional and past the chain's last level, the seam clamp's own cases (gi.directional_seam).
    static constexpr unsigned kAxisRead = 65536u;
    /// THE OCCUPANCY RAY (PHOTON-7 stage 10, 10-1; jah_voxel_ray.glsl): with this flag the answer is the ray the world
    /// caches gather by, from `posLS` along `dirLS` (its origin plane from `biasDirLS`; zero: a ray in free space) to
    /// `rayLastShell` - `march` = its hit's radiance and its exit (0 exhausted, 1 hit, 2 escaped), `escape` = its
    /// distance (cascade 0's units), the cascade it ended in, the cells it read; `hitRead` / `marchRead` zero.
    static constexpr unsigned kRay = 131072u;
    unsigned cascade = 0u;           ///< which cascade the point reads take
    float    lod = 0.0f;             ///< ...at which mip
    float    rayLastShell = 0.0f;    ///< kRay: the last cascade's world shell, cascade 0's units (0: its box alone)
};

/// Every answer the reader gives for one cone, as raw floats (compared bit for
/// bit by the suite): the march's colour/alpha, its escape opacity, its age in
/// cascade 0's units, the cascade it stopped in and its age there, the ray
/// hit's read where the march's first sample lands and the march's own one-step
/// read of it.
struct VoxelReaderAnswer {
    float march[4] = {};     ///< colour.rgb, alpha
    float escape[4] = {};    ///< alpha (the escape rides it), travelledC0, lastCascade, travelled
    float hitRead[4] = {};   ///< jahVoxelSample (what jah_rq_hit.glsl calls) at the centre of the march's first texel plane
    float marchRead[4] = {}; ///< the march at zero length: that plane, read whole by the march
};

/// ONE CONE FOR THE ENVIRONMENT'S CONE-LOOKUP HARNESS (PHOTON-ENV-1;
/// Engine::environmentCones, gi.env_cone): a WORLD direction and the cone's
/// half-angle as its tangent — exactly what jahEnvCone takes.
struct EnvironmentConeQuery {
    Vec3  dirWorld;              ///< unit direction, world axes
    /// The cone's half angle as its tangent. Every caller sets its own; the default is
    /// the ONE diffuse set's, the four 44.5-degree cones (jah_voxel_cones.glsl's
    /// coneAngleTan) - the six-cone set it named is deleted (SIX-CONE-DEAD-1).
    float tanHalfAngle = 0.98269f;
};

/// What the harness answers for one cone, linear radiance with the Sky Light's
/// gain NOT applied (the lookup and its reference see the same raw cube):
/// `lookup` is jahEnvCone (the prefiltered chain at the cone's mip), and
/// `reference` is the mean of the cube's FINEST mip over 64 directions spread
/// uniformly over the cone's solid angle (the cone integral the lookup stands
/// in for).
struct EnvironmentConeAnswer {
    float lookup[3] = {};
    float reference[3] = {};
    float lod = 0.0f;            ///< the mip the lookup read
};

}}  // namespace jahshaka::engine
