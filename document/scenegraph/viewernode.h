/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef VIEWERNODE_H
#define VIEWERNODE_H

#include "irisglfwd.h"
#include "document/scenegraph/scenenode.h"

namespace iris
{

class ViewerNode : public SceneNode
{
    float viewScale;
    ViewerNode();

	// viewer-specific properties about how it controls in vr mode`
	bool allowMovement;
	bool allowPicking;
	bool showHands;
	bool activeCharacterController;

public:

	bool isMovementAllowed()
	{
		return allowMovement;
	}

	void setMovementAllowed(bool allow)
	{
		this->allowMovement = allow;
	}

	bool isPickingAllowed()
	{
		return allowPicking;
	}

	void setPickingAllowed(bool pickingAllowed)
	{
		this->allowPicking = pickingAllowed;
	}

	void setShowHands(bool show)
	{
		this->showHands = show;
	}

	bool getShowHands()
	{
		return this->showHands;
	}

	void setActiveCharacterController(bool state) {
		activeCharacterController = state;
	}

	bool isActiveCharacterController() {
		return activeCharacterController;
	}

    void setViewScale(float scale);
    float getViewScale();

    static ViewerNodePtr create();

	SceneNodePtr createDuplicate() override;

    ~ViewerNode();
};

}

#endif // VIEWERNODE_H
