/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef MATHHELPER_H
#define MATHHELPER_H

#include "core/math/mat4.h"
#include "core/math/trs.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include <QtMath>

namespace iris
{

class MathHelper
{
public:

    /// Affine 4x4 -> translation / rotation / scale.
    ///
    /// THIS USED TO EXTRACT THE ROTATION FROM normalMatrix(), WHICH IS WRONG.
    /// normalMatrix() is the inverse-transpose of the upper 3x3 — that is
    /// R * S^-1, which equals R only when the scale is 1. Feed it a node scaled
    /// (2, 1, 1) and the "rotation" it hands back is a shear pretending to be a
    /// quaternion; the measured symptom was +55 degrees of yaw appearing on a
    /// scaled character root, compounding on every reparent because
    /// SceneNode::setLocalTransform and CameraNode::lookAt both round-trip
    /// through here.
    ///
    /// The correct decomposition already existed in the tree — decomposeTRS
    /// normalizes each basis column by its own length, which is exactly the
    /// step normalMatrix does not perform — so this is now one implementation,
    /// not two. It also fixes the old sign convention: the mirrored case is
    /// decided by the determinant and folded into the X scale, not by the
    /// sign of a product of four matrix elements (which was arbitrary).
    ///
    /// The shear measure decomposeTRS returns is discarded here; callers that
    /// care (the clip extractor, the mirror's bind poses) call decomposeTRS
    /// directly and check it.
    static void decomposeMatrix(const iris::Mat4& matrix, iris::Vec3& pos, iris::Quat& rot, iris::Vec3& scale)
    {
        decomposeTRS(matrix, pos, rot, scale);
    }

    static float lerp(float norm, float min, float max) {
        return (max - min) * norm + min;
    }

	//https://stackoverflow.com/questions/1903954/is-there-a-standard-sign-function-signum-sgn-in-c-c
    template <typename T>
    static int sign(T val) {
        return (T(0) < val) - (val < T(0));
    }


    // Realtime Collision Detection, page 149
    static float closestPointBetweenSegments(iris::Vec3 p1, iris::Vec3 q1, iris::Vec3 p2, iris::Vec3 q2,
                                             float& s, float& t, iris::Vec3& c1, iris::Vec3& c2)
    {
        const float EPILSON = 0.000001f;
        iris::Vec3 d1 = q1 - p1;
        iris::Vec3 d2 = q2 - p2;
        iris::Vec3 r  = p1 - p2;
        float a = iris::Vec3::dotProduct(d1, d1);
        float e = iris::Vec3::dotProduct(d2, d2);
        float f = iris::Vec3::dotProduct(d2, r);

        if (a <= EPILSON && e <= EPILSON) {
            s = 0;
            t = 0;
            c1 = p1;
            c2 = p2;
            return iris::Vec3::dotProduct(c1-c2, c1-c2);
        }
        if (a <= EPILSON) {
            s = 0;
            t = f/e;
            t = qBound(0.0f, t, 1.0f);
        } else {
            float c = iris::Vec3::dotProduct(d1, r);
            if (e <= EPILSON) {
                t = 0;
                s = qBound(0.0f, -c/a, 1.0f);
            } else {
                float b = iris::Vec3::dotProduct(d1, d2);
                float denom = a*e - b*b;

                if (denom != 0) {
                    s = qBound(0.0f, (b*f - c*e)/denom, 1.0f);
                } else {
                    s = 0;
                }

                t = (b*s + f) / e;

                if (t < 0) {
                    t = 0;
                    s = qBound(0.0f, -c/a, 1.0f);
                } else if (t > 1) {
                    t = 1.0f;
                    s = qBound(0.0f, (b-c)/a, 1.0f);
                }
            }
        }

        c1 = p1 + d1 * s;
        c2 = p2 + d2 * t;

        return iris::Vec3::dotProduct(c1 - c2, c1 - c2);
    }
};

}

#endif // MATHHELPER_H
