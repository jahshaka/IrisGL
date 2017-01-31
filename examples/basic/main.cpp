#include <QApplication>
#include <QSurfaceFormat>
#include <QOpenGLFunctions_3_2_Core>
#include <QOpenGLWidget>

#include "../../src/core/scene.h"
#include "../../src/core/scenenode.h"
#include "../../src/scenegraph/meshnode.h"
#include "../../src/materials/defaultmaterial.h"

namespace iris
{

class RenderWindow : public QOpenGLWidget,
                     protected QOpenGLFunctions_3_2_Core
{
    Q_OBJECT
protected:
    iris::ForwardRendererPtr renderer;
    iris::ScenePtr scene;
    iris::CameraNodePtr camera;

    iris::Viewport* viewport;
    QColor clearColor;

public:
    RenderWindow()
    {
        QSurfaceFormat format;
        format.setDepthBufferSize(32);
        format.setMajorVersion(3);
        format.setMinorVersion(2);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setSamples(1);
        setFormat(format);

        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);


    }

protected:
    void initializeGL()
    {
        makeCurrent();
        initializeOpenGLFunctions();

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);

        renderer = iris::ForwardRenderer::create(this);

        scene = iris::Scene::create();
        scene->setAmbientColor(QColor(50,50,50));
        renderer->setScene(scene);

        viewport = new iris::Viewport();
        clearColor = QColor(50,50,50);

        camera = iris::CameraNode::create();
        camera->pos = QVector3D(0, 5, 7);
        camera->rot = QQuaternion::fromEulerAngles(-5, 0, 0);
        scene->setCamera(camera);

        initialize();

        auto timer = new QTimer(this);
        connect(timer, SIGNAL(timeout()), this, SLOT(update()));
        timer->start();
    }

    void resizeGL(int width, int height)
    {
        glViewport(0, 0, width, height);
        viewport->width = width;
        viewport->height = height;
    }

    void paintGL()
    {
        updateScene();
        scene->update(1.0f/60.0f);
        renderScene();
    }

    void renderScene()
    {
        glClearColor(clearColor.redF(), clearColor.greenF(), clearColor.blueF(), 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        scene->update(dt);
        renderer->renderScene(this->context());
    }

    virtual void initScene()
    {

    }

    virtual void updateScene()
    {

    }


};

}

class BasicWindow : public iris::RenderWindow
{
    iris::MeshNodePtr cube;
    float rotSpeed;
public:

    void initScene()
    {
        cube = new iris::MeshNode::create();
        cube->setMesh("assets/cube.obj");

        auto mat = iris::DefaultMaterial::create();
        mat->setDiffuseTexture(iris::Texture2D::load("assets/tiles.png"));
        cube->setMaterial(mat);

        scene->getRootNode()->addChild(cube);

        camera->pos = QVector3D(0,5,5);
        camera->rot = QQuaternion::fromEulerAngles(0,0,0);
    }

    void updateScene(float dt)
    {
        cube->rot *= QQuaternion::fromEulerAngles(0, rotSpeed * dt, 0);
    }
};

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(textures);

    QApplication app(argc, argv);

    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);

    iris::RenderWindow window;
    window.show();
    return app.exec();
}
