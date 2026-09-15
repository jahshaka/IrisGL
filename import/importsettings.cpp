/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/importsettings.h"

#include "core/math/mat3.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonValue>
#include <cmath>

namespace iris
{

namespace
{

/// ONE number format for the canonical record (import/importsettings.h says
/// why it is not QJsonDocument's): shortest form that round-trips a double,
/// C locale, no exponent surprises. 'g' with 17 significant digits reproduces
/// every double exactly and prints 1 as "1", 0.01 as "0.01".
QByteArray num(double v)
{
    if (!std::isfinite(v)) return QByteArrayLiteral("0");
    if (v == 0.0) return QByteArrayLiteral("0");   // never "-0"
    return QByteArray::number(v, 'g', 17);
}

QByteArray str(const QString &s)
{
    QByteArray out("\"");
    for (const QChar &c : s) {
        if (c == QLatin1Char('"') || c == QLatin1Char('\\')) out += '\\';
        out += QString(c).toUtf8();
    }
    out += '"';
    return out;
}

struct AxisName { const char *name; float x, y, z; };
const AxisName kAxes[] = {
    { "+X", 1, 0, 0 }, { "-X", -1, 0, 0 },
    { "+Y", 0, 1, 0 }, { "-Y", 0, -1, 0 },
    { "+Z", 0, 0, 1 }, { "-Z", 0, 0, -1 },
};

}   // namespace

// ---------------------------------------------------------------------------

double ImportTransform::globalScaleFactor(double declared) const
{
    if (!overridesUnit()) return scale;
    const double d = declared > 0.0 ? declared : 1.0;
    return scale * unitOverride / d;
}

// ---------------------------------------------------------------------------

double ImportSettings::unitFactor(Units units)
{
    switch (units) {
    case Units::Auto:         return 0.0;
    case Units::Metres:       return 1.0;
    case Units::Centimetres:  return 0.01;
    case Units::Millimetres:  return 0.001;
    case Units::Inches:       return 0.0254;
    case Units::Feet:         return 0.3048;
    }
    return 0.0;
}

const char *ImportSettings::unitName(Units units)
{
    switch (units) {
    case Units::Auto:         return "auto";
    case Units::Metres:       return "m";
    case Units::Centimetres:  return "cm";
    case Units::Millimetres:  return "mm";
    case Units::Inches:       return "in";
    case Units::Feet:         return "ft";
    }
    return "auto";
}

bool ImportSettings::unitFromName(const QString &name, Units *out)
{
    const QString n = name.trimmed().toLower();
    const struct { const char *name; Units units; } table[] = {
        { "auto", Units::Auto }, { "m", Units::Metres }, { "cm", Units::Centimetres },
        { "mm", Units::Millimetres }, { "in", Units::Inches }, { "ft", Units::Feet },
    };
    for (const auto &row : table) {
        if (n == QLatin1String(row.name)) { if (out) *out = row.units; return true; }
    }
    return false;
}

bool ImportSettings::axisFromName(const QString &name, Vec3 *out)
{
    const QString n = name.trimmed().toUpper();
    for (const AxisName &axis : kAxes) {
        if (n == QLatin1String(axis.name)) {
            if (out) *out = Vec3(axis.x, axis.y, axis.z);
            return true;
        }
    }
    return false;
}

bool ImportSettings::wantsClip(const QString &name) const
{
    if (!clips) return false;
    if (clipNames.isEmpty()) return true;
    for (const QString &wanted : clipNames)
        if (wanted.compare(name, Qt::CaseInsensitive) == 0) return true;
    return false;
}

// ---------------------------------------------------------------------------

ImportSettings ImportSettings::fromJson(const QJsonObject &record, QString *errorOut)
{
    if (errorOut) errorOut->clear();
    ImportSettings out;
    if (record.isEmpty()) return out;

    static const QStringList known = { QStringLiteral("version"), QStringLiteral("units"),
                                       QStringLiteral("scale"),   QStringLiteral("axes"),
                                       QStringLiteral("rotate"),  QStringLiteral("translate"),
                                       QStringLiteral("skeleton"),QStringLiteral("clips"),
                                       QStringLiteral("materials") };
    const auto refuse = [&](const QString &why) {
        if (errorOut && errorOut->isEmpty()) *errorOut = why;
        return ImportSettings();
    };

    for (auto it = record.constBegin(); it != record.constEnd(); ++it)
        if (!known.contains(it.key()))
            return refuse(QStringLiteral("unknown import setting '%1' (known: %2)")
                              .arg(it.key(), known.join(QStringLiteral(", "))));

    if (record.contains(QStringLiteral("units"))) {
        if (!unitFromName(record.value(QStringLiteral("units")).toString(), &out.units))
            return refuse(QStringLiteral("unknown units '%1' (auto, m, cm, mm, in, ft)")
                              .arg(record.value(QStringLiteral("units")).toString()));
    }
    if (record.contains(QStringLiteral("scale"))) {
        const QJsonValue v = record.value(QStringLiteral("scale"));
        out.scale = v.toDouble(-1.0);
        if (!v.isDouble() || !(out.scale > 0.0) || !std::isfinite(out.scale))
            return refuse(QStringLiteral("'scale' must be a positive number"));
    }
    if (record.contains(QStringLiteral("axes"))) {
        const QJsonObject axes = record.value(QStringLiteral("axes")).toObject();
        for (auto it = axes.constBegin(); it != axes.constEnd(); ++it)
            if (it.key() != QStringLiteral("up") && it.key() != QStringLiteral("forward"))
                return refuse(QStringLiteral("unknown axes key '%1' (up, forward)").arg(it.key()));
        if (axes.contains(QStringLiteral("up"))) {
            const QString name = axes.value(QStringLiteral("up")).toString();
            if (!axisFromName(name, nullptr))
                return refuse(QStringLiteral("unknown up axis '%1' (+X,-X,+Y,-Y,+Z,-Z)").arg(name));
            out.up = name.trimmed().toUpper();
        }
        if (axes.contains(QStringLiteral("forward"))) {
            const QString name = axes.value(QStringLiteral("forward")).toString();
            if (!axisFromName(name, nullptr))
                return refuse(QStringLiteral("unknown forward axis '%1' (+X,-X,+Y,-Y,+Z,-Z)")
                                  .arg(name));
            out.forward = name.trimmed().toUpper();
        }
        Vec3 u, f;
        axisFromName(out.up, &u);
        axisFromName(out.forward, &f);
        if (std::fabs(double(Vec3::dotProduct(u, f))) > 1e-4)
            return refuse(QStringLiteral("the up axis (%1) and the forward axis (%2) must be "
                                         "perpendicular").arg(out.up, out.forward));
    }
    const auto readTriple = [&](const char *key, double *dst) -> bool {
        if (!record.contains(QLatin1String(key))) return true;
        const QJsonArray arr = record.value(QLatin1String(key)).toArray();
        if (arr.size() != 3) {
            refuse(QStringLiteral("'%1' must be three numbers").arg(QLatin1String(key)));
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            if (!arr.at(i).isDouble() || !std::isfinite(arr.at(i).toDouble())) {
                refuse(QStringLiteral("'%1' must be three finite numbers")
                           .arg(QLatin1String(key)));
                return false;
            }
            dst[i] = arr.at(i).toDouble();
        }
        return true;
    };
    if (!readTriple("rotate", out.rotate)) return ImportSettings();
    if (!readTriple("translate", out.translate)) return ImportSettings();

    if (record.contains(QStringLiteral("skeleton")))
        out.skeleton = record.value(QStringLiteral("skeleton")).toBool(true);

    if (record.contains(QStringLiteral("clips"))) {
        const QJsonValue v = record.value(QStringLiteral("clips"));
        if (v.isBool()) {
            out.clips = v.toBool();
        } else if (v.isArray()) {
            out.clips = true;
            for (const QJsonValue &name : v.toArray()) {
                if (!name.isString())
                    return refuse(QStringLiteral("'clips' must be true, false or a list of "
                                                 "clip names"));
                if (!name.toString().isEmpty()) out.clipNames.append(name.toString());
            }
            // An EMPTY list is "no clips", not "all clips": a caller that
            // filtered every name away meant to filter them away.
            if (out.clipNames.isEmpty()) out.clips = false;
        } else {
            return refuse(QStringLiteral("'clips' must be true, false or a list of clip names"));
        }
    }

    if (record.contains(QStringLiteral("materials"))) {
        const QString mode = record.value(QStringLiteral("materials")).toString();
        if (mode == QStringLiteral("import")) out.materials = MaterialMode::Import;
        else if (mode == QStringLiteral("none")) out.materials = MaterialMode::None;
        else return refuse(QStringLiteral("unknown materials mode '%1' (import, none)").arg(mode));
    }

    if (record.contains(QStringLiteral("version"))) {
        const int version = record.value(QStringLiteral("version")).toInt(kVersion);
        if (version != kVersion)
            return refuse(QStringLiteral("import settings version %1 is not %2 — this build "
                                         "cannot apply it").arg(version).arg(kVersion));
    }
    return out;
}

QJsonObject ImportSettings::toJson() const
{
    QJsonObject axes;
    axes[QStringLiteral("up")] = up;
    axes[QStringLiteral("forward")] = forward;

    QJsonArray rot, trans;
    for (int i = 0; i < 3; ++i) { rot.append(rotate[i]); trans.append(translate[i]); }

    QJsonObject out;
    out[QStringLiteral("version")] = kVersion;
    out[QStringLiteral("units")] = QLatin1String(unitName(units));
    out[QStringLiteral("scale")] = scale;
    out[QStringLiteral("axes")] = axes;
    out[QStringLiteral("rotate")] = rot;
    out[QStringLiteral("translate")] = trans;
    out[QStringLiteral("skeleton")] = skeleton;
    if (!clipNames.isEmpty()) {
        QJsonArray names;
        for (const QString &name : clipNames) names.append(name);
        out[QStringLiteral("clips")] = names;
    } else {
        out[QStringLiteral("clips")] = clips;
    }
    out[QStringLiteral("materials")] = materials == MaterialMode::None
                                           ? QStringLiteral("none") : QStringLiteral("import");
    return out;
}

QByteArray ImportSettings::canonicalJson() const
{
    // Hand-written, keys in ASCII order, EVERY key present. Adding a key means
    // adding it here with its identity default — which MOVES identityHash()
    // and renames every bake in every library, exactly once, on purpose.
    QByteArray out("{");
    out += "\"axes\":{\"forward\":" + str(forward) + ",\"up\":" + str(up) + "},";
    out += "\"clips\":";
    if (!clipNames.isEmpty()) {
        out += '[';
        for (int i = 0; i < clipNames.size(); ++i) {
            if (i) out += ',';
            out += str(clipNames.at(i));
        }
        out += ']';
    } else {
        out += clips ? "true" : "false";
    }
    out += ",\"materials\":";
    out += materials == MaterialMode::None ? "\"none\"" : "\"import\"";
    out += ",\"rotate\":[" + num(rotate[0]) + ',' + num(rotate[1]) + ',' + num(rotate[2]) + "],";
    out += "\"scale\":" + num(scale) + ',';
    out += QByteArray("\"skeleton\":") + (skeleton ? "true" : "false") + ',';
    out += "\"translate\":[" + num(translate[0]) + ',' + num(translate[1]) + ','
           + num(translate[2]) + "],";
    out += QByteArray("\"units\":\"") + unitName(units) + "\",";
    out += "\"version\":" + QByteArray::number(kVersion);
    out += '}';
    return out;
}

QString ImportSettings::hash() const
{
    return QString::fromLatin1(
        QCryptographicHash::hash(canonicalJson(), QCryptographicHash::Sha256).toHex().left(16));
}

QString ImportSettings::identityHash()
{
    static const QString h = ImportSettings().hash();
    return h;
}

QString ImportSettings::hashOf(const QJsonObject &record)
{
    QString error;
    const ImportSettings settings = fromJson(record, &error);
    // A record this build cannot parse APPLIES as identity (fromJson returns
    // defaults), so it must KEY as identity too, or the bake it names could
    // never be found.
    return settings.hash();
}

bool ImportSettings::isIdentity() const
{
    return canonicalJson() == ImportSettings().canonicalJson();
}

ImportTransform ImportSettings::transform(double declaredUnitScale) const
{
    ImportTransform out;
    out.scale = scale;
    out.unitOverride = unitFactor(units);          // 0 for Auto
    out.declaredUnitScale = declaredUnitScale;

    // THE AXES FIX. The user names the axes THE FILE uses; our convention is
    // +Y up, -Z forward. The rotation that carries the file's frame onto ours
    // is the transpose of the file's frame written in our numbers: with
    // right = forward x up, the frame's columns are (right, up, -forward) and
    // a file already in our convention yields the identity, exactly.
    Vec3 u(0, 1, 0), f(0, 0, -1);
    axisFromName(up, &u);
    axisFromName(forward, &f);
    const Vec3 r = Vec3::crossProduct(f, u);
    const Vec3 b = -f;
    // Mat3 takes a ROW-major float list; the rotation is the TRANSPOSE of the
    // column frame, so the rows ARE the axes.
    const float values[9] = { r.x(), r.y(), r.z(),
                              u.x(), u.y(), u.z(),
                              b.x(), b.y(), b.z() };
    const Quat axesFix = Quat::fromRotationMatrix(Mat3(values));

    // The free rotation rides ON TOP of the axes fix (spec §3: "applied after
    // axes"), in the document's own euler convention (Quat::fromEulerAngles,
    // the one the Transform panel writes).
    const Quat free = Quat::fromEulerAngles(float(rotate[0]), float(rotate[1]),
                                            float(rotate[2]));
    out.rotation = (free * axesFix).normalized();
    if (rotate[0] == 0.0 && rotate[1] == 0.0 && rotate[2] == 0.0
        && up == QStringLiteral("+Y") && forward == QStringLiteral("-Z"))
        out.rotation = Quat();   // EXACTLY identity, not identity to 1e-7

    out.translation = Vec3(float(translate[0]), float(translate[1]), float(translate[2]));
    return out;
}

}   // namespace iris
