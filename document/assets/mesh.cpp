/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/


#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "document/assets/mesh.h"

#include <QString>
#include <QFile>

#include "irisglfwd.h"



#include "document/assets/vertexbuffer.h"
#include "document/assets/vertexlayout.h"
#include "core/geometry/trimesh.h"
#include "document/assets/skeleton.h"
#include "core/geometry/boundingsphere.h"
#include "core/geometry/aabb.h"


namespace iris
{

Mesh::Mesh()
{
	triMesh = nullptr;
	numVerts = 0;
	// UNINITIALISED UNTIL 2026-09-19 (lane STATS-1), in both of the ctors that
	// do not parse a file: the assimp ctor and the .jmb bake reader both assign
	// it, so every mesh the document normally holds was fine — but a mesh built
	// in code (gizmo geometry, a line mesh, a test fixture) carried whatever was
	// on the heap, and `numFaces` is now READ: the F3 readout and
	// app.renderStats().sceneTriangles count a scene's triangles with it
	// (src/services/scenestats.h). A garbage face count would have been a
	// garbage readout, silently.
	numFaces = 0;
	usesIndexBuffer = false;
}

//todo: extract trimesh from data
Mesh::Mesh(void* data,int dataSize,int numElements,VertexLayout* vertexLayout)
{
    triMesh = nullptr;
    numVerts = numElements;
    // The caller knows the element count, not the face count — see the default
    // ctor for why this is written rather than left.
    numFaces = 0;

    auto vb = VertexBuffer::create(*vertexLayout);
    vb->setData(data, dataSize);
    vertexBuffers.append(vb);

    usesIndexBuffer = false;
    this->setPrimitiveMode(PrimitiveMode::Triangles);
}

void Mesh::setSkeleton(const SkeletonPtr &value)
{
    skeleton = value;
}

PrimitiveMode Mesh::getPrimitiveMode() const
{
    return primitiveMode;
}

void Mesh::setPrimitiveMode(const PrimitiveMode &value)
{
    primitiveMode = value;
}

void Mesh::clearVertexBuffers()
{
    this->vertexBuffers.clear();
}

void Mesh::addVertexBuffer(VertexBufferPtr vertexBuffer)
{
    this->vertexBuffers.append(vertexBuffer);
}

void Mesh::setIndexBuffer(IndexBufferPtr indexBuffer)
{
    this->idxBuffer = indexBuffer;
}

bool Mesh::hasSkeleton()
{
    return !!skeleton;
}

SkeletonPtr Mesh::getSkeleton()
{
    return skeleton;
}

void Mesh::addSkeletalAnimation(QString name, SkeletalAnimationPtr anim)
{
    skeletalAnimations.insert(name, anim);
}

QMap<QString, SkeletalAnimationPtr> Mesh::getSkeletalAnimations()
{
    return skeletalAnimations;
}

bool Mesh::hasSkeletalAnimations()
{
    return skeletalAnimations.count() != 0;
}

// (THE PARSE CACHE, `loadMesh`, the PIN and `clearLoadCache` are DELETED —
// ATOM P2, 2026-09-22. The shipped primitives, the Ground, the avatar's cube and
// the preview spheres were the only things that ever took that route: an assimp
// parse of a Qt resource per file, a weak cache with a pin over the shipped
// paths, and `MeshBake::buildCards` AT CREATION — ~6 ms on the thread that draws
// per added node — with the LOD chain deliberately withheld. They are baked
// library assets now, imported once through the one pipeline
// (jahshaka/src/services/primitiveassets.h), so every one of them carries the
// chain, the cards that name real levels, the cluster DAG and the measured bounds that
// an imported model has, and nothing in the product parses a model outside an
// import.)

Mesh* Mesh::create(void* data,int dataSize,int numVerts,VertexLayout* vertexLayout)
{
    return new Mesh(data,dataSize,numVerts,vertexLayout);
}

MeshPtr Mesh::create()
{
	auto mesh = new Mesh();
	return MeshPtr(mesh);
}

Mesh::~Mesh()
{
    //delete vertexLayout;
	if (triMesh)
		delete triMesh;
}

void Mesh::setVertexCount(const unsigned int count)
{
	numVerts = count;
}

}
