/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/assets/livetextures.h"

#include "document/assets/texture2d.h"

namespace iris
{

static const QLatin1String kScheme("live://");

QHash<QString, Texture2DPtr> &LiveTextures::table()
{
    static QHash<QString, Texture2DPtr> t;
    return t;
}

QStringList &LiveTextures::order()
{
    static QStringList o;
    return o;
}

QString LiveTextures::refFor(const QString &guid)
{
    return guid.isEmpty() ? QString() : kScheme + guid;
}

bool LiveTextures::isLiveRef(const QString &ref)
{
    return ref.startsWith(kScheme);
}

QString LiveTextures::guidOf(const QString &ref)
{
    return isLiveRef(ref) ? ref.mid(QString(kScheme).size()) : QString();
}

Texture2DPtr LiveTextures::create(const QString &guid, const QString &name,
                                  int width, int height, bool mipmaps)
{
    if (guid.isEmpty() || width <= 0 || height <= 0) return Texture2DPtr();
    if (table().contains(guid)) return Texture2DPtr();
    Texture2DPtr tex = Texture2D::createLive(guid, name, width, height, mipmaps);
    if (!tex) return Texture2DPtr();
    table().insert(guid, tex);
    order().append(guid);
    return tex;
}

Texture2DPtr LiveTextures::find(const QString &guidOrRef)
{
    const QString guid = isLiveRef(guidOrRef) ? guidOf(guidOrRef) : guidOrRef;
    return table().value(guid);
}

bool LiveTextures::destroy(const QString &guidOrRef)
{
    const QString guid = isLiveRef(guidOrRef) ? guidOf(guidOrRef) : guidOrRef;
    if (!table().contains(guid)) return false;
    table().remove(guid);
    order().removeAll(guid);
    return true;
}

QStringList LiveTextures::guids()
{
    return order();
}

void LiveTextures::clear()
{
    table().clear();
    order().clear();
}

}
