/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_MAT3_H
#define IRIS_MAT3_H


// iris::Mat3 — the 3x3 basis matrix that Quat::fromRotationMatrix consumes and
// Mat4::normalMatrix() produces. Storage and indexing match QMatrix3x3
// (QGenericMatrix<3,3,float>): column-major memory, operator()(row, column).

namespace iris
{

class Mat3
{
public:
    Mat3() noexcept
    {
        for (int col = 0; col < 3; ++col)
            for (int row = 0; row < 3; ++row)
                m[col][row] = (col == row) ? 1.0f : 0.0f;
    }

    struct Uninitialized_t {};
    static constexpr Uninitialized_t Uninitialized{};
    explicit Mat3(Uninitialized_t) noexcept {}

    // Row-major list of values, like QGenericMatrix(const float *values).
    explicit Mat3(const float *values) noexcept
    {
        for (int col = 0; col < 3; ++col)
            for (int row = 0; row < 3; ++row)
                m[col][row] = values[row * 3 + col];
    }

    float &operator()(int row, int column) { return m[column][row]; }
    const float &operator()(int row, int column) const { return m[column][row]; }

    float *data() noexcept { return *m; }
    const float *data() const noexcept { return *m; }
    const float *constData() const noexcept { return *m; }

private:
    float m[3][3]; // column-major, m[column][row]
};

} // namespace iris

#endif // IRIS_MAT3_H
