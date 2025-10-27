#include "animation.h"

namespace vtkmeta {

Animation::Animation() {}
Animation::~Animation() {}

void Animation::addNode(std::shared_ptr<Node> node) {
    nodes_.push_back(node);
}

void Animation::update(double dt) {
    // 简单示例：可以旋转所有节点
    for (auto& node : nodes_) {
        auto actor = node->getVTKActor();
        double* rot = actor->GetOrientation();
        actor->SetOrientation(rot[0], rot[1] + dt * 30.0, rot[2]); // 每秒旋转30度
    }
}

}
