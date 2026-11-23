// THE GI READERS' FAMILY (SCRIPT-IN-ENGINE-1 stage 2, builder P): engine.gi.* reads - readCardAt, readCardRadiance, the
// voxel stats / volume / light / direct / halves, the changed regions, traceRays - each `reads`, answering its record
// (records_gi_read.h, fields_gi_read.cpp), the big arrays as `bytes` (RULED 3). TEST AND TOOL readbacks: the engine
// flushes and BLOCKS on its downloads (never a frame path). A reader the engine answers false (no VCT arm or no such
// cascade, no card under the point, no list closed yet, the ray tier off) is REFUSED with the reason: its falsy null.
// AN ANSWER IS BOUNDED as a call is (Host::Options::callBounds.bytes): a voxel answer past it is refused by its size.
#include "script/commands.h"
#include "script/records_gi_read.h"
#include "jahshaka/engine/GiModes.h"

namespace jahshaka { namespace engine { namespace script {

namespace {

/// A voxel volume's bytes as answered (every array; a packed level adds its counts).
std::size_t volumeBytes(const GiVoxelVolume &v)
{
    std::size_t n = 0;
    for (const auto *a : {&v.light, &v.albedo, &v.coverageP, &v.coverageN, &v.positionP, &v.positionN, &v.emissive, &v.emitP,
                          &v.emitN, &v.direct, &v.halfLight, &v.halfDirect})
        n += a->size() * sizeof(float);
    n += v.accumWeight.size() * sizeof(unsigned);
    for (const auto &l : v.albedoLevels) n += l.size() * sizeof(float);
    for (int a = 0; a < 3; ++a) {
        n += 4 * (1 + v.directional[a].size()) + v.halfSide[a].size() * sizeof(float) + v.halfNormal[a].size() * sizeof(float);
        for (const auto &l : v.directional[a]) n += l.size() * sizeof(float);
    }
    return n;
}

Param cascadeArg() { return arg("cascade", Kind::Int).range(0, kGiTierMaxCascades - 1); }

/// A voxel reader's options: the cascade and the world box (both corners or neither).
Param boxArg()
{
    return object("read", {cascadeArg(), numbers("lo", 3, Value()).asFloat().in("m"), numbers("hi", 3, Value()).asFloat().in("m")});
}

}  // namespace

void registerGiReadCommands(Host &host, const Commands &k)
{
    Registry &r = host.registry();
    // one voxel volume answered, or refused: the reader's false, or past this host's byte bound
    const auto volume = [k](Call &c, bool ok, const GiVoxelVolume &v, const char *none) -> Value {
        if (!ok) return c.refuse(none);
        const std::size_t bytes = volumeBytes(v), bound = k.st->host->options().callBounds.bytes;
        if (bytes > bound)
            return c.refuse("the volume holds " + std::to_string(bytes) + " bytes, past this host's " + std::to_string(bound) +
                            " (Host::Options::callBounds): read a box of it (engine.gi.voxelLight / voxelHalves)");
        return Record<GiVoxelVolume>::table().toValue(v, &c);
    };
    // the world box of voxelLight / voxelHalves: both corners or neither (FAILS by name), lo below hi
    const auto box = [](Call &c, float lo[3], float hi[3], bool &boxed) -> bool {
        const Value &l = c[1]["lo"], &h = c[1]["hi"];
        if (l.isNull() != h.isNull()) {
            c.fail("read.lo and read.hi are both given or neither (the box's two corners)");
            return false;
        }
        boxed = !l.isNull();
        for (int i = 0; boxed && i < 3; ++i) {
            lo[i] = float(l.asArray()[std::size_t(i)].asNumber());
            hi[i] = float(h.asArray()[std::size_t(i)].asNumber());
            if (!(lo[i] < hi[i])) {
                c.fail("read.lo[" + std::to_string(i) + "] is not below read.hi[" + std::to_string(i) + "]: an empty box");
                return false;
            }
        }
        return true;
    };
    const char *const kNoCascade = "no voxel cascade there: the scene has no VCT GI, or not this cascade (or the device "
                                   "refused the download)";

    Param onlyNode = handle("node", kNode);
    onlyNode.fallback = Value();   // optional: none reads every card
    r.add({"engine.gi.readCardAt",
           {Access::Reads, {target(handle("scene", kScene)),
                            object("at", {numbers("world", 3).asFloat().in("m"), numbers("normal", 3).asFloat(), onlyNode})},
            Kind::Object},
           "What the surface cache holds at a WORLD POINT on a surface facing `normal` (Scene::readCardAt: the cards facing "
           "it, the squarest wins): the texel's CardSample - albedo, normal, emissive, depth, the sun term, radiance and its "
           "halves, the lamps' visibility, which card and texel answered. `node` (a node handle) restricts it to one "
           "instance's cards. Refused when no card covers the point."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              CardSample out;
              const Value &only = c[1]["node"];
              if (!s->readCardAt(toVec3(c[1]["world"]), toVec3(c[1]["normal"]), out, only.isNull() ? 0 : NodeId(Call::idOf(only))))
                  return c.refuse("no card covers the point (or the scene keeps no surface cache)");
              return Record<CardSample>::table().toValue(out, &c);
          });
    r.add({"engine.gi.readCardRadiance", {Access::Reads, {target(handle("scene", kScene))}, Kind::Object},
           "EVERY RESIDENT CARD's lit radiance (Scene::readCardRadiance): {cacheFrame, cards: [CardRadiance]} - each card's "
           "node (a node handle), place, face, side in texels, relight frames and its radiance as BYTES (float32 r g b, "
           "row-major, size x size). Refused when the scene keeps no surface cache."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              std::vector<CardRadiance> cards;
              unsigned long long frame = 0;
              if (!s->readCardRadiance(cards, frame)) return c.refuse("the scene keeps no surface cache");
              std::size_t bytes = 0;
              for (const CardRadiance &x : cards) bytes += x.rgb.size() * sizeof(float);
              if (bytes > k.st->host->options().callBounds.bytes)
                  return c.refuse("the cards hold " + std::to_string(bytes) + " bytes, past this host's call bound");
              Value::Array a;
              a.reserve(cards.size());
              for (const CardRadiance &x : cards) a.push_back(Record<CardRadiance>::table().toValue(x, &c));
              return Value(Value::Object{{"cacheFrame", Value(std::int64_t(frame))}, {"cards", Value(std::move(a))}});
          });
    r.add({"engine.gi.voxelStats", {Access::Reads, {target(handle("scene", kScene)), cascadeArg()}, Kind::Object},
           "One cascade's light volume, counted (Scene::giVoxelStats; GiVoxelStats): its size and format, the peaks, the "
           "mean over lit voxels, the counts at the format's top and above one, the emissive store's, the volumes' "
           "digests. `available` false (every number 0) without a VCT arm or that cascade."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              return Record<GiVoxelStats>::table().toValue(s->giVoxelStats(int(c[1].asInt())), &c);
          });
    r.add({"engine.gi.voxelVolume", {Access::Reads, {target(handle("scene", kScene)), cascadeArg()}, Kind::Object},
           "ONE CASCADE's VOXELS, WHOLE (Scene::giVoxelVolume; GiVoxelVolume): the placement (width, height, depth, "
           "origin, cell) and every level-0 volume as BYTES - float32 RGBA a voxel, x fastest (accumWeight uint32 a voxel); "
           "albedoLevels an array of bytes, level L + 1 each; directional THREE PACKED bytes values, axis X, Y, Z: each a "
           "uint32 n (its levels), then n uint32 element counts (one a level, in order), then the n levels' float32s "
           "concatenated in order - every uint32 and float in this machine's byte order (little-endian on every target we "
           "build); halfSide / halfNormal three bytes values, an axis each. Refused without a VCT arm or that cascade, or "
           "past this host's call bound."},
          [k, volume, kNoCascade](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              GiVoxelVolume v;
              const bool ok = s->giVoxelVolume(int(c[1].asInt()), v);
              return volume(c, ok, v, kNoCascade);
          });
    r.add({"engine.gi.voxelLight", {Access::Reads, {target(handle("scene", kScene)), boxArg()}, Kind::Object},
           "ONE CASCADE's LIGHT ALONE (Scene::giVoxelLight; GiVoxelVolume with halfLight, light and the placement, every "
           "other member empty), {cascade, lo, hi}: the world box lo..hi (metres, both corners or neither) reads the cells "
           "it reaches alone (its own width / height / depth, origin its first cell's corner). Bytes as "
           "engine.gi.voxelVolume's."},
          [k, volume, box, kNoCascade](Call &c) -> Value {
              float lo[3], hi[3];
              bool boxed = false;
              if (!box(c, lo, hi, boxed)) return Value();
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              GiVoxelVolume v;
              const bool ok = s->giVoxelLight(int(c[1]["cascade"].asInt()), v, boxed ? lo : nullptr, boxed ? hi : nullptr);
              return volume(c, ok, v, kNoCascade);
          });
    r.add({"engine.gi.voxelDirect", {Access::Reads, {target(handle("scene", kScene)), cascadeArg()}, Kind::Object},
           "ONE CASCADE's DIRECT TERM ALONE (Scene::giVoxelDirect; GiVoxelVolume with halfDirect, direct and the placement, "
           "every other member empty). Refused where the cascade keeps no direct term (no bounce). Bytes as "
           "engine.gi.voxelVolume's."},
          [k, volume](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              GiVoxelVolume v;
              const bool ok = s->giVoxelDirect(int(c[1].asInt()), v);
              return volume(c, ok, v, "no direct term there: no VCT cascade, or the cascade does not bounce");
          });
    r.add({"engine.gi.voxelHalves", {Access::Reads, {target(handle("scene", kScene)), boxArg()}, Kind::Object},
           "ONE CASCADE's HALF STORE (Scene::giVoxelHalves; GiVoxelVolume with halfLight, halfDirect, halfSide, "
           "halfNormal, the coverage and the position per half, and the placement), {cascade, lo, hi}: the world box as "
           "engine.gi.voxelLight's. Bytes as engine.gi.voxelVolume's."},
          [k, volume, box, kNoCascade](Call &c) -> Value {
              float lo[3], hi[3];
              bool boxed = false;
              if (!box(c, lo, hi, boxed)) return Value();
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              GiVoxelVolume v;
              const bool ok = s->giVoxelHalves(int(c[1]["cascade"].asInt()), v, boxed ? lo : nullptr, boxed ? hi : nullptr);
              return volume(c, ok, v, kNoCascade);
          });
    r.add({"engine.gi.changedRegions", {Access::Reads, {target(handle("scene", kScene))}, Kind::Object},
           "THE CHANGED-REGION LIST as it last closed (Scene::giChangedRegions; GiChangedRegions): its frame, the overflow "
           "bits, the lighting restart, the sun, and at most 128 regions {lo, hi, kind, cascadeMask}. Refused before a list "
           "has closed (no frame has drawn the scene)."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              GiChangedRegions out;
              if (!s->giChangedRegions(out)) return c.refuse("no changed-region list has closed: no frame has drawn the scene");
              return Record<GiChangedRegions>::table().toValue(out, &c);
          });
    r.add({"engine.gi.traceRays",
           {Access::Reads, {target(handle("scene", kScene)),
                            object("trace", {arg("rays", Kind::Array).upTo(12 * kGiReadRays).flat(12).asFloat()})},
            Kind::Object},
           "TRACES A BATCH OF RAYS against the scene's acceleration structure and waits (Scene::traceRays): rays, 12 numbers "
           "a ray - origin xyz, tMin, direction xyz, tMax, the instance mask (bit 0 casters, 1 movers, 2 the still world, 3 "
           "every near copy, 4 the far copies), three unused - at most 65,536 rays; answers {hits}: 4 numbers a ray - the "
           "distance (< 0 a miss), the hit node's index in the scene's item order, the triangle, 1 or 0 (a ray cut at the "
           "loops' bound: its tMax, -1, -1, 1). Refused while the ray tier is off or the scene has no structure yet (draw a "
           "frame first)."},
          [k](Call &c) -> Value {
              Scene *s = k.scene(c, c[0]);
              if (!s) return Value();
              const Value::Array &in = c[1]["rays"].asArray();
              std::vector<float> rays(in.size()), hits;
              for (std::size_t i = 0; i < in.size(); ++i) rays[i] = float(in[i].asNumber());
              if (!s->traceRays(rays, hits))
                  return c.refuse("no ray was traced: the ray tier is off or unavailable, or the scene has no acceleration "
                                  "structure yet (draw a frame first)");
              Value::Array out;
              out.reserve(hits.size());
              for (const float h : hits) out.push_back(Value(double(h)));
              return Value(Value::Object{{"hits", Value(std::move(out))}});
          });
}

}}}  // namespace jahshaka::engine::script
