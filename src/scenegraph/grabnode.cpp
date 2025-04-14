#include "grabnode.h"

namespace iris
{

GrabNode::GrabNode(HandPoseType poseType)
{
	this->sceneNodeType = SceneNodeType::Grab;
	handPose = nullptr;
	poseFactor = 1.0;
	setPose(poseType);	
}

void GrabNode::setPose(HandPoseType poseType)
{
	if(handPose)
		delete handPose;

	switch (poseType) {
	case HandPoseType::Grab:
		handPose = new GrabHandPose();
	default:
		handPose = new GrabHandPose();
	}
}

}