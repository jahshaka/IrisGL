#include "document/materials/postprocessmanager.h"
#include "document/materials/postprocess.h"

namespace iris
{

PostProcessManager::PostProcessManager()
{
}

PostProcessManagerPtr PostProcessManager::create()
{
    return PostProcessManagerPtr(new PostProcessManager());
}

void PostProcessManager::addPostProcess(PostProcessPtr process)
{
    postProcesses.append(process);
}

void PostProcessManager::setPostProcesses(QList<PostProcessPtr> processes)
{
    this->postProcesses = processes;
}

QList<PostProcessPtr> PostProcessManager::getPostProcesses()
{
    return postProcesses;
}

void PostProcessManager::clearPostProcesses()
{
    postProcesses.clear();
}

}
