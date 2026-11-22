// THE PICTURE FAMILY (SCRIPT-IN-ENGINE-1): a picture's NATIVE reductions (the script language is an interpreter: a
// pixel loop is never a script's).
#include "script/commands.h"

#include <cmath>
#include <cstdio>

namespace jahshaka { namespace engine { namespace script {

namespace {

/// The picture a readPixels answered: {width, height, rgba}.
Param picture(const char *name)
{
    return object(name, {arg("width", Kind::Int), arg("height", Kind::Int), arg("rgba", Kind::Bytes)});
}

/// The checked picture's byte at (x, y, channel); the caller has bounded x and y.
int byteAt(const Value &pic, std::int64_t x, std::int64_t y, int c)
{
    return pic["rgba"].asBytes()[size_t((y * pic["width"].asInt() + x) * 4 + c)];
}

/// The picture's own consistency: width x height x 4 bytes.
bool wellFormed(Call &c, const Value &pic)
{
    const std::int64_t w = pic["width"].asInt(), h = pic["height"].asInt();
    if (w <= 0 || h <= 0 || std::int64_t(pic["rgba"].asBytes().size()) != w * h * 4) {
        c.fail("the picture is not width x height x 4 bytes");
        return false;
    }
    return true;
}

}  // namespace

void registerPictureCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    r.add({"engine.view.hash", {{target(picture("picture"))}, Kind::String},
           "The picture's FNV-1a 64 over its RGBA bytes, as 16 hex digits."},
          [](Call &c) -> Value {
              if (!wellFormed(c, c[0])) return Value();
              unsigned long long h = 14695981039346656037ull;
              for (std::uint8_t b : c[0]["rgba"].asBytes()) {
                  h ^= b;
                  h *= 1099511628211ull;
              }
              char text[17];
              std::snprintf(text, sizeof text, "%016llx", h);
              return Value(text);
          });
    r.add({"engine.view.columnStep",
           {{target(picture("picture")), object("span", {arg("axis", Kind::String).oneOf({"column", "row"}), arg("a", Kind::Int), arg("b", Kind::Int),
                                                 arg("from", Kind::Int), arg("to", Kind::Int)})},
            Kind::Number},
           "The mean absolute step, in codes over R, G and B, between column (axis \"column\") or row (\"row\") a and b, "
           "over the other axis' span [from, to)."},
          [](Call &c) -> Value {
              const Value &p = c[0], &sp = c[1];
              if (!wellFormed(c, p)) return Value();
              const bool columns = sp["axis"].asString() == "column";
              const std::int64_t across = columns ? p["width"].asInt() : p["height"].asInt();
              const std::int64_t along = columns ? p["height"].asInt() : p["width"].asInt();
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
    r.add({"engine.view.mean",
           {{target(picture("picture")), object("rect", {arg("x0", Kind::Int), arg("y0", Kind::Int), arg("x1", Kind::Int),
                                                 arg("y1", Kind::Int)})},
            Kind::Array},
           "The mean of each channel, in codes, over the rectangle [x0, x1) x [y0, y1): [r, g, b, a]."},
          [](Call &c) -> Value {
              const Value &p = c[0], &rc = c[1];
              if (!wellFormed(c, p)) return Value();
              const std::int64_t x0 = rc["x0"].asInt(), y0 = rc["y0"].asInt(), x1 = rc["x1"].asInt(), y1 = rc["y1"].asInt();
              if (x0 < 0 || y0 < 0 || x1 > p["width"].asInt() || y1 > p["height"].asInt() || x0 >= x1 || y0 >= y1)
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
