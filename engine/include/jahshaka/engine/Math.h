#pragma once
// THE ENGINE's MATH TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.

namespace jahshaka { namespace engine {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    /// EXACT float equality, for the same reason Colour::operator== is exact:
    /// it answers "is this the value I already pushed?", and a tolerance would
    /// let a dragged slider stop reaching the backend.
    bool operator==(const Vec3 &o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Vec3 &o) const { return !(*this == o); }
};

struct Colour {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
    Colour() = default;
    Colour(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
    /// EXACT float equality, on purpose: callers use this to answer "is this the
    /// same value I already pushed?", where a tolerance would let a slider
    /// creep without ever reaching the backend.
    bool operator==(const Colour &o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
    bool operator!=(const Colour &o) const { return !(*this == o); }
};

/// Rotation as a unit quaternion. Identity by default.
struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    Quat() = default;
    Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};

}}  // namespace jahshaka::engine
