/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "core/math/vec.h"
#include "document/scenegraph/viewernode.h"
#include "document/scenegraph/scene.h"
#include "document/scenegraph/scenenode.h"
#include "core/properties/property.h"

namespace iris
{

QList<Property*> ViewerNode::getProperties()
{
    auto props = SceneNode::getProperties();

    auto prop = new FloatProperty();
    prop->displayName = "View Scale";
    prop->name = "viewScale";
    prop->value = viewScale;
    props.append(prop);

    // activeCharacterController is a plain bool on the node (not a controller
    // object), so it reflects like any other flag.
    auto boolProp = new BoolProperty();
    boolProp->displayName = "Active Character Controller";
    boolProp->name = "activeCharacterController";
    boolProp->value = activeCharacterController;
    props.append(boolProp);

    return props;
}

QVariant ViewerNode::getPropertyValue(QString valueName)
{
    if (valueName == "viewScale")                 return viewScale;
    if (valueName == "activeCharacterController") return activeCharacterController;

    return SceneNode::getPropertyValue(valueName);
}

bool ViewerNode::setPropertyValue(QString valueName, const QVariant &value)
{
    // setViewScale, not a raw assignment: it also drives the node's own scale.
    if (valueName == "viewScale")                 { setViewScale(value.toFloat());               return true; }
    if (valueName == "activeCharacterController") { setActiveCharacterController(value.toBool()); return true; }

    return SceneNode::setPropertyValue(valueName, value);
}

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
    setLocalScale(iris::Vec3(scale, scale, scale));
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
