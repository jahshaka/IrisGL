/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "viewernode.h"
#include "../scenegraph/scene.h"
#include "../scenegraph/scenenode.h"

namespace iris
{

ViewerNode::ViewerNode()
{
    this->sceneNodeType = SceneNodeType::Viewer;

    this->setViewScale(2.0f);
	activeCharacterController = false;
    exportable = false;
}

ViewerNode::~ViewerNode()
{
}

void ViewerNode::setViewScale(float scale)
{
    this->viewScale = scale;
    this->scale = QVector3D(scale, scale, scale);
}

float ViewerNode::getViewScale()
{
    return this->viewScale;
}

SceneNodePtr ViewerNode::createDuplicate()
{
	auto viewer = iris::ViewerNode::create();

	viewer->setViewScale(this->viewScale);

	return viewer;
}

ViewerNodePtr ViewerNode::create()
{
    return ViewerNodePtr(new ViewerNode());
}

}
