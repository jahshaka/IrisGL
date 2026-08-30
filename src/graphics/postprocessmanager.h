#ifndef POSTPROCESSMANAGER_H
#define POSTPROCESSMANAGER_H

#include "../irisglfwd.h"

namespace iris {

// Holds the scene's post-process settings list (serialized by SceneWriter).
// The GL blit/render half died with the legacy renderer at step 14.
class PostProcessManager
{
    QList<PostProcessPtr> postProcesses;

public:
    PostProcessManager();

    static PostProcessManagerPtr create();

    void addPostProcess(PostProcessPtr process);
    void setPostProcesses(QList<PostProcessPtr> processes);
    QList<PostProcessPtr> getPostProcesses();
    void clearPostProcesses();
};

}

#endif // POSTPROCESSMANAGER_H
