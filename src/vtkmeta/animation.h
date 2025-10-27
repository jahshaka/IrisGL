#ifndef ANIMATION_H
#define ANIMATION_H

#include <vector>
#include <memory>
#include "node.h"

namespace vtkmeta {

class Animation {
public:
    Animation();
    ~Animation();

    void addNode(std::shared_ptr<Node> node);
    void update(double dt); // dt: delta time in seconds

private:
    std::vector<std::shared_ptr<Node>> nodes_;
};

}


#endif // ANIMATION_H
