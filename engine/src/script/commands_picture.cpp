// THE PICTURE FAMILY (SCRIPT-IN-ENGINE-1; the lead's S3): PICTURES BY HANDLE. A picture (engine.view.readPixels'
// answer, or engine.picture.create's) is held NATIVELY by the host and addressed by a picture handle: its reductions
// (hash, columnStep, mean) read it in place - the script language is an interpreter, a pixel loop is never a script's,
// and no picture crosses into the script unless engine.picture.bytes asks (one copy, then). A picture LIVES ACROSS
// CALLS until engine.picture.release (MCP-ON-HOST-1: a session drives the host call by call); jahengine releases every
// one at its script's end. THE CAP: the live pictures hold at most the host's Options::pictureBytesCap (512 MiB unless
// its caller says otherwise) - a picture past it FAILS by name (release one first).
#include "script/commands.h"

#include <cmath>
#include <cstdio>

namespace jahshaka { namespace engine { namespace script {

namespace {

/// The checked picture's byte at (x, y, channel); the caller has bounded x and y.
inline int byteAt(const Image &p, std::int64_t x, std::int64_t y, int c)
{
    return p.rgba[size_t((y * std::int64_t(p.width) + x) * 4 + c)];
}

}  // namespace

/// A picture held by the host: its handle and size, {picture, width, height}; null (the call FAILED by name) past the
/// cap. The picture's bytes are counted while it lives (its deleter gives them back).
bool Commands::roomForPicture(Call &c, std::size_t bytes) const
{
    if (st->pictureBytes + bytes <= st->pictureBytesCap) return true;
    c.fail("the live pictures would hold " + std::to_string(st->pictureBytes + bytes) + " bytes, over the host's cap " +
           std::to_string(st->pictureBytesCap) + ": release one first (engine.picture.release)");
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

void registerPictureCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    const Param pic = target(handle("picture", kPicture));
    r.add({"engine.picture.create",
           {{object("picture", {arg("width", Kind::Int).range(1, 16384).in("px"), arg("height", Kind::Int).range(1, 16384).in("px"),
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
    r.add({"engine.picture.bytes", {{pic}, Kind::Bytes},
           "The picture's RGBA bytes, copied into the script (width x height x 4, row-major, top-left origin): for a "
           "script that truly needs the pixels - a reduction is a native command."},
          [](Call &c) -> Value { return Value::bytes(c.object<Image>(c[0])->rgba); });
    r.add({"engine.picture.release", {{pic}, Kind::Bool},
           "Releases the picture: its handle is stale from here on, its bytes leave the live pictures' cap."},
          [](Call &c) -> Value { return Value(c.registry().release(c[0].asInt())); });
    r.add({"engine.picture.hash", {{pic}, Kind::String},
           "The picture's FNV-1a 64 over its RGBA bytes, as 16 hex digits."},
          [](Call &c) -> Value {
              unsigned long long h = 14695981039346656037ull;
              for (std::uint8_t b : c.object<Image>(c[0])->rgba) {
                  h ^= b;
                  h *= 1099511628211ull;
              }
              char text[17];
              std::snprintf(text, sizeof text, "%016llx", h);
              return Value(text);
          });
    r.add({"engine.picture.columnStep",
           {{pic, object("span", {arg("axis", Kind::String).oneOf({"column", "row"}), arg("a", Kind::Int).in("px"),
                                  arg("b", Kind::Int).in("px"), arg("from", Kind::Int).in("px"), arg("to", Kind::Int).in("px")})},
            Kind::Number},
           "The mean absolute step, in codes over R, G and B, between column (axis \"column\") or row (\"row\") a and b, "
           "over the other axis' span [from, to)."},
          [](Call &c) -> Value {
              const Image &p = *c.object<Image>(c[0]);
              const Value &sp = c[1];
              const bool columns = sp["axis"].asString() == "column";
              const std::int64_t across = columns ? p.width : p.height, along = columns ? p.height : p.width;
              const std::int64_t a = sp["a"].asInt(), b = sp["b"].asInt(), from = sp["from"].asInt(), to = sp["to"].asInt();
              if (a < 0 || b < 0 || a >= across || b >= across || from < 0 || to > along || from > to)
                  return c.fail("the span lies outside the picture");
              double s = 0.0;
              int n = 0;
              for (std::int64_t t = from; t < to; ++t)
                  for (int ch = 0; ch < 3; ++ch, ++n)
                      s += std::fabs(double(columns ? byteAt(p, a, t, ch) - byteAt(p, b, t, ch)
                                                    : byteAt(p, t, a, ch) - byteAt(p, t, b, ch)));
              return Value(n ? s / n : 0.0);
          });
    r.add({"engine.picture.mean",
           {{pic, object("rect", {arg("x0", Kind::Int).in("px"), arg("y0", Kind::Int).in("px"), arg("x1", Kind::Int).in("px"),
                                  arg("y1", Kind::Int).in("px")})},
            Kind::Array},
           "The mean of each channel, in codes, over the rectangle [x0, x1) x [y0, y1): [r, g, b, a]."},
          [](Call &c) -> Value {
              const Image &p = *c.object<Image>(c[0]);
              const Value &rc = c[1];
              const std::int64_t x0 = rc["x0"].asInt(), y0 = rc["y0"].asInt(), x1 = rc["x1"].asInt(), y1 = rc["y1"].asInt();
              if (x0 < 0 || y0 < 0 || x1 > std::int64_t(p.width) || y1 > std::int64_t(p.height) || x0 >= x1 || y0 >= y1)
                  return c.fail("the rectangle is empty or lies outside the picture");
              double sum[4] = {0, 0, 0, 0};
              for (std::int64_t y = y0; y < y1; ++y)
                  for (std::int64_t x = x0; x < x1; ++x)
                      for (int ch = 0; ch < 4; ++ch) sum[ch] += byteAt(p, x, y, ch);
              const double n = double((x1 - x0) * (y1 - y0));
              return Value(Value::Array{Value(sum[0] / n), Value(sum[1] / n), Value(sum[2] / n), Value(sum[3] / n)});
          });
}

}}}  // namespace jahshaka::engine::script
