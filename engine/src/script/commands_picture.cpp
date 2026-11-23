// THE PICTURE FAMILY (SCRIPT-IN-ENGINE-1; the lead's S3): PICTURES BY HANDLE. A picture (engine.view.readPixels'
// answer, or engine.picture.create's) is held NATIVELY by the host and addressed by a picture handle: its reductions
// (hash, columnStep, mean) read it in place - the script language is an interpreter, a pixel loop is never a script's,
// and no picture crosses into the script unless engine.picture.bytes asks (one copy, then). A picture LIVES ACROSS
// CALLS until engine.picture.release (MCP-ON-HOST-1: a session drives the host call by call); the host's end releases
// every one. THE CAP: the live pictures hold at most the host's Options::pictureBytesCap (512 MiB unless
// its caller says otherwise) - a picture past it FAILS by name (release one first).
// THE RADIANCE PICTURE (stage 2 B1; RULED 10: the second held kind, `radiance`, bound to ImageF): a view's linear scene
// radiance in float (readPixels' content "radiance", View::readPixelsHdr), or engine.radiance.create's - held as a
// picture is, under the SAME cap, with the same reductions (engine.radiance.*) in radiance units instead of codes.
#include "script/commands.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include "jahshaka/engine/Pictures.h"

namespace jahshaka { namespace engine { namespace script {

namespace {

/// The checked picture's channel value at (x, y, channel) - a code (Image) or a radiance (ImageF); the caller has
/// bounded x and y.
template <class P> inline double valueAt(const P &p, std::int64_t x, std::int64_t y, int c)
{
    return double(p.rgba[size_t((y * std::int64_t(p.width) + x) * 4 + c)]);
}

/// FNV-1a 64 over `n` bytes, as 16 hex digits.
Value fnv1a(const void *data, std::size_t n)
{
    unsigned long long h = 14695981039346656037ull;
    const unsigned char *b = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", h);
    return Value(text);
}

/// THE TWO REDUCTIONS, ONE BODY FOR BOTH KINDS (engine.picture.* and engine.radiance.*): the mean absolute RGB step
/// between two columns or rows over a span, and each channel's mean over a rectangle - null, the call FAILED by name,
/// when the span or the rectangle leaves the picture.
template <class P> Value columnStep(Call &c, const P &p, const Value &sp)
{
    const bool columns = sp["axis"].asString() == "column";
    const std::int64_t across = columns ? p.width : p.height, along = columns ? p.height : p.width;
    const std::int64_t a = sp["a"].asInt(), b = sp["b"].asInt(), from = sp["from"].asInt(), to = sp["to"].asInt();
    if (a < 0 || b < 0 || a >= across || b >= across || from < 0 || to > along || from > to)
        return c.fail("the span lies outside the picture");
    double s = 0.0;
    int n = 0;
    for (std::int64_t t = from; t < to; ++t)
        for (int ch = 0; ch < 3; ++ch, ++n)
            s += std::fabs(columns ? valueAt(p, a, t, ch) - valueAt(p, b, t, ch) : valueAt(p, t, a, ch) - valueAt(p, t, b, ch));
    return Value(n ? s / n : 0.0);
}
template <class P> Value mean(Call &c, const P &p, const Value &rc)
{
    const std::int64_t x0 = rc["x0"].asInt(), y0 = rc["y0"].asInt(), x1 = rc["x1"].asInt(), y1 = rc["y1"].asInt();
    if (x0 < 0 || y0 < 0 || x1 > std::int64_t(p.width) || y1 > std::int64_t(p.height) || x0 >= x1 || y0 >= y1)
        return c.fail("the rectangle is empty or lies outside the picture");
    double sum[4] = {0, 0, 0, 0};
    for (std::int64_t y = y0; y < y1; ++y)
        for (std::int64_t x = x0; x < x1; ++x)
            for (int ch = 0; ch < 4; ++ch) sum[ch] += valueAt(p, x, y, ch);
    const double n = double((x1 - x0) * (y1 - y0));
    return Value(Value::Array{Value(sum[0] / n), Value(sum[1] / n), Value(sum[2] / n), Value(sum[3] / n)});
}

Param spanParam()
{
    return object("span", {arg("axis", Kind::String).oneOf({"column", "row"}), arg("a", Kind::Int).in("px"),
                           arg("b", Kind::Int).in("px"), arg("from", Kind::Int).in("px"), arg("to", Kind::Int).in("px")});
}
Param rectParam()
{
    return object("rect", {arg("x0", Kind::Int).in("px"), arg("y0", Kind::Int).in("px"), arg("x1", Kind::Int).in("px"),
                           arg("y1", Kind::Int).in("px")});
}

}  // namespace

/// A picture held by the host: its handle and size, {picture, width, height}; null (the call FAILED by name) past the
/// cap. The picture's bytes are counted while it lives (its deleter gives them back).
bool Commands::roomForPicture(Call &c, std::size_t bytes) const
{
    if (st->pictureBytes + bytes <= st->pictureBytesCap) return true;
    c.fail("the live pictures would hold " + std::to_string(st->pictureBytes + bytes) + " bytes, over the host's cap " +
           std::to_string(st->pictureBytesCap) + ": release one first (engine.picture.release, engine.radiance.release)");
    return false;
}

Value Commands::holdPicture(Call &c, Image img) const
{
    const std::size_t bytes = img.rgba.size();
    if (!roomForPicture(c, bytes)) return Value();
    const unsigned w = img.width, h = img.height;
    st->pictureBytes += bytes;
    std::weak_ptr<EngineState> owner = st;
    std::shared_ptr<Image> held(new Image(std::move(img)), [owner, bytes](Image *p) {
        if (auto s = owner.lock()) s->pictureBytes -= bytes;
        delete p;
    });
    const Value handle = c.hold(kPicture, std::move(held), true);
    if (handle.isNull()) return Value();
    return Value(Value::Object{{"picture", handle}, {"width", Value(w)}, {"height", Value(h)}});
}

/// THE RADIANCE PICTURE (E0): held as a picture is, under the same cap, its float bytes counted.
Value Commands::holdRadiance(Call &c, ImageF img) const
{
    const std::size_t bytes = img.rgba.size() * sizeof(float);
    if (!roomForPicture(c, bytes)) return Value();
    const unsigned w = img.width, h = img.height;
    st->pictureBytes += bytes;
    std::weak_ptr<EngineState> owner = st;
    std::shared_ptr<ImageF> held(new ImageF(std::move(img)), [owner, bytes](ImageF *p) {
        if (auto s = owner.lock()) s->pictureBytes -= bytes;
        delete p;
    });
    const Value handle = c.hold(kRadiance, std::move(held), true);
    if (handle.isNull()) return Value();
    return Value(Value::Object{{"radiance", handle}, {"width", Value(w)}, {"height", Value(h)}});
}

void registerPictureCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    const Param pic = target(handle("picture", kPicture));
    r.add({"engine.picture.create",
           {Access::Changes,
            {object("picture", {arg("width", Kind::Int).range(1, 16384).in("px"), arg("height", Kind::Int).range(1, 16384).in("px"),
                                arg("rgba", Kind::Bytes)})},
            Kind::Object},
           "A picture from the script's bytes (a reference it built): {width, height, rgba: width x height x 4 bytes, "
           "row-major, top-left origin}. Answers {picture: picture handle, width, height}."},
          [k](Call &c) -> Value {
              const std::int64_t w = c[0]["width"].asInt(), h = c[0]["height"].asInt();
              if (std::int64_t(c[0]["rgba"].asBytes().size()) != w * h * 4)
                  return c.fail("rgba holds " + std::to_string(c[0]["rgba"].asBytes().size()) + " bytes, not width x "
                                "height x 4 = " + std::to_string(w * h * 4));
              if (!k.roomForPicture(c, std::size_t(w * h * 4))) return Value();   // before the copy
              Image img;
              img.width = unsigned(w);
              img.height = unsigned(h);
              img.rgba.assign(c[0]["rgba"].asBytes().begin(), c[0]["rgba"].asBytes().end());
              return k.holdPicture(c, std::move(img));
          });
    r.add({"engine.picture.bytes", {Access::Reads, {pic}, Kind::Bytes},
           "The picture's RGBA bytes, copied into the script (width x height x 4, row-major, top-left origin): for a "
           "script that truly needs the pixels - a reduction is a native command."},
          [](Call &c) -> Value { return Value::bytes(c.object<Image>(c[0])->rgba); });
    r.add({"engine.picture.release", {Access::Changes, {pic}, Kind::Bool},
           "Releases the picture: its handle is stale from here on, its bytes leave the live pictures' cap."},
          [](Call &c) -> Value { return Value(c.registry().release(c[0].asInt())); });
    r.add({"engine.picture.hash", {Access::Reads, {pic}, Kind::String},
           "The picture's FNV-1a 64 over its RGBA bytes, as 16 hex digits."},
          [](Call &c) -> Value {
              const Image &p = *c.object<Image>(c[0]);
              return fnv1a(p.rgba.data(), p.rgba.size());
          });
    r.add({"engine.picture.columnStep", {Access::Reads, {pic, spanParam()}, Kind::Number},
           "The mean absolute step, in codes over R, G and B, between column (axis \"column\") or row (\"row\") a and b, "
           "over the other axis' span [from, to)."},
          [](Call &c) -> Value { return columnStep(c, *c.object<Image>(c[0]), c[1]); });
    r.add({"engine.picture.mean", {Access::Reads, {pic, rectParam()}, Kind::Array},
           "The mean of each channel, in codes, over the rectangle [x0, x1) x [y0, y1): [r, g, b, a]."},
          [](Call &c) -> Value { return mean(c, *c.object<Image>(c[0]), c[1]); });

    // THE RADIANCE PICTURE's COMMANDS: the same shapes, in linear radiance (float) instead of codes
    const Param rad = target(handle("radiance", kRadiance));
    r.add({"engine.radiance.create",
           {Access::Changes,
            {object("radiance", {arg("width", Kind::Int).range(1, 16384).in("px"), arg("height", Kind::Int).range(1, 16384).in("px"),
                                 arg("rgba", Kind::Bytes)})},
            Kind::Object},
           "A radiance picture from the script's bytes (a reference it built): {width, height, rgba: width x height x "
           "4 floats - 32-bit IEEE, this machine's byte order (little-endian on every target), row-major, top-left "
           "origin, linear radiance}. Answers {radiance: radiance handle, width, height}."},
          [k](Call &c) -> Value {
              const std::int64_t w = c[0]["width"].asInt(), h = c[0]["height"].asInt();
              const Value::Bytes &b = c[0]["rgba"].asBytes();
              const std::int64_t want = w * h * 4 * std::int64_t(sizeof(float));
              if (std::int64_t(b.size()) != want)
                  return c.fail("rgba holds " + std::to_string(b.size()) + " bytes, not width x height x 4 floats = " +
                                std::to_string(want));
              if (!k.roomForPicture(c, std::size_t(want))) return Value();   // before the copy
              ImageF img;
              img.width = unsigned(w);
              img.height = unsigned(h);
              img.rgba.resize(std::size_t(w * h * 4));
              std::memcpy(img.rgba.data(), b.data(), b.size());
              return k.holdRadiance(c, std::move(img));
          });
    r.add({"engine.radiance.release", {Access::Changes, {rad}, Kind::Bool},
           "Releases the radiance picture: its handle is stale from here on, its bytes leave the live pictures' cap."},
          [](Call &c) -> Value { return Value(c.registry().release(c[0].asInt())); });
    r.add({"engine.radiance.hash", {Access::Reads, {rad}, Kind::String},
           "The radiance picture's FNV-1a 64 over its float bytes (width x height x 4 floats, as held), as 16 hex digits."},
          [](Call &c) -> Value {
              const ImageF &p = *c.object<ImageF>(c[0]);
              return fnv1a(p.rgba.data(), p.rgba.size() * sizeof(float));
          });
    r.add({"engine.radiance.columnStep", {Access::Reads, {rad, spanParam()}, Kind::Number},
           "The mean absolute step, in linear radiance over R, G and B, between column (axis \"column\") or row "
           "(\"row\") a and b, over the other axis' span [from, to)."},
          [](Call &c) -> Value { return columnStep(c, *c.object<ImageF>(c[0]), c[1]); });
    r.add({"engine.radiance.mean", {Access::Reads, {rad, rectParam()}, Kind::Array},
           "The mean of each channel, in linear radiance, over the rectangle [x0, x1) x [y0, y1): [r, g, b, a]."},
          [](Call &c) -> Value { return mean(c, *c.object<ImageF>(c[0]), c[1]); });
}

}}}  // namespace jahshaka::engine::script
