/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/assets/shippedmeshes.h"

#include <QMutex>
#include <QMutexLocker>

#include "document/assets/mesh.h"

namespace iris
{
namespace ShippedMeshes
{

namespace
{
QMutex &lock()
{
    static QMutex m;
    return m;
}

Resolver &installed()
{
    static Resolver r;
    return r;
}
}   // namespace

void setResolver(Resolver resolver)
{
    QMutexLocker locked(&lock());
    installed() = std::move(resolver);
}

bool hasResolver()
{
    QMutexLocker locked(&lock());
    return bool(installed());
}

MeshPtr mesh(const QString &seedKey)
{
    Resolver resolver;
    {
        QMutexLocker locked(&lock());
        resolver = installed();
    }
    // Called OUTSIDE the lock: the resolver reads the catalog and the bake, and
    // a resolver that asked for another key would otherwise deadlock.
    return resolver ? resolver(seedKey) : MeshPtr();
}

}   // namespace ShippedMeshes
}   // namespace iris
