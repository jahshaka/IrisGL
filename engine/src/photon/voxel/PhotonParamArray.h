#pragma once
// PhotonParamArray (NONDET-1 D2): the one door for an array parameter of ours longer than 16 elements.
#include "OgreException.h"
#include "OgreShaderParams.h"
#include "OgreStringConverter.h"

#include <algorithm>

namespace Ogre
{
    /// AN ARRAY PARAMETER OVER 16 ELEMENTS (NONDET-1 D2) - the one door for every one of ours.
    /// ShaderParams::Param::setManualValue keeps AT MOST 64 BYTES and drops the rest SILENTLY (the fork's
    /// OgreShaderParams.cpp:250, upstream's line: dataSizeBytes = min( 4 x n, 64 ), "Don't overflow"): the SH bands 4-8 of
    /// envSh, the feedback's activation boxes 4-15, the injection's union boxes 4-15 and a chain's cascades past the 4th
    /// never reached the GPU (they read 0: the PSO's constant list is zero-filled and only the first 16 are written).
    /// setManualValueEx has no cap but keeps only a POINTER, which every later updateParameters reads - the async
    /// publish's forced write included (OgreHlmsCompute.cpp asyncComputePublished, on the main thread like every setter
    /// here: HlmsAsyncCompiler.h's contract) - so the values live in a block as long-lived as the ShaderParams that points
    /// at it: a lighting's own for its local bounce parameters (members), the process's for a SHARED job's (released with
    /// the pools, before Root: PhotonVoxelLighting::releaseParamBlocks). It COPIES, as setManualValue does, so the GPU
    /// gets the values last set - right because every dispatch of these jobs sets its arrays first. REFUSES more than N.
    /// Never "simplify" a call back to setManualValue. Its law: engine.param_array (Studio).
    template <typename T, uint32 N>
    class PhotonParamArray
    {
    public:
        void set( ShaderParams::Param *param, const T *values, uint32 count )
        {
            if( count > N )
            {
                OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                             "the parameter '" + param->name + "' got " + StringConverter::toString( count ) +
                                 " values for a block of " + StringConverter::toString( N ),
                             "PhotonParamArray::set" );
            }
            std::copy( values, values + count, mValues );
            param->setManualValueEx( mValues, count );
        }

    private:
        T mValues[N] = {};
    };
}  // namespace Ogre
