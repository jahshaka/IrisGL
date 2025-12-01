#ifndef ASSETDATATYPES_H
#define ASSETDATATYPES_H

#include <QString>
#include <QVector>
#include <QVector3D>
#include <QVector4D>
#include <QMap>
#include <unordered_map>

namespace vtkmeta {

struct TextureDef {
    QString id_; // texture id (uuid)
    QString path_; // path relative to asset root or absolute
    QString type_; // optional ("albedo", "normal", ...)
};

// -----------------------------
// Material definition (PBR-centric)
// -----------------------------
struct MaterialDef {
    QString id_;
    QString name_;

    QVector3D base_color_{1.0f, 1.0f, 1.0f};
    float opacity_ = 1.0f;
    float metallic_ = 0.0f;
    float roughness_ = 0.5f;
    QVector3D emissive_color_{0.0f, 0.0f, 0.0f};
    float emissive_strength_ = 0.0f;

    // texture ids referencing TextureDef.id_
    QString base_color_texture_;
    QString normal_texture_;
    QString metallic_texture_;
    QString roughness_texture_;
    QString emissive_texture_;

    bool double_sided_ = false;
    bool alpha_blend_ = false; // if true, use alpha blending
};

// Basic transform
struct Transform {
    QVector3D translation{0.0f, 0.0f, 0.0f};
    QVector4D rotationQuaternion{0.0f, 0.0f, 0.0f, 1.0f}; // x,y,z,w
    QVector3D scale{1.0f, 1.0f, 1.0f};
};

// -----------------------------
// Mesh definition (lightweight)
// -----------------------------
struct MeshDef {
    QString id_;
    QString name_;
    int vertex_count_ = 0;
    int primitive_count_ = 0;
    QString material_id_; // reference to MaterialDef.id_
    bool skinned_ = false; // has bones/weights
    // bone list optional; importer fills if skinned
    QVector<QString> bone_names_;

    // NEW: original model mesh index (Assimp mesh index). Importer MUST fill if possible.
    int original_index_ = -1;
};

// -----------------------------
// Node definition (scene graph)
// -----------------------------
struct NodeDef {
    QString id_;
    QString name_;
    QString parent_id_; // empty if root
    QVector<QString> children_ids_;

    QVector3D translation_{0.0f, 0.0f, 0.0f};
    QVector4D rotation_{0.0f, 0.0f, 0.0f, 1.0f}; // quaternion
    QVector3D scale_{1.0f, 1.0f, 1.0f};

    QString mesh_id_; // optional, reference to MeshDef.id_
    QString material_override_id_; // optional

    bool visible_ = true;
    bool cast_shadow_ = true;
    bool receive_shadow_ = true;

    QString type_ = "Node"; // "Mesh", "SkinnedMesh", "Bone", etc.
};

// Texture metadata
struct TextureInfo {
    QString id;
    QString guid;
    QString path; // relative path inside asset (or original path)
};

// // Material (PBR-ish)
// struct MaterialInfo {
//     QString id;
//     QString name;

//     QVector4D base_color{1.0f, 1.0f, 1.0f, 1.0f};
//     float metallic = 0.0f;
//     float roughness = 1.0f;
//     float opacity = 1.0f;
//     QVector3D emissive{0.0f, 0.0f, 0.0f};

//     QString base_color_texture;
//     QString normal_texture;
//     QString metallic_roughness_texture;
// };

// Skinning support
struct BoneWeight {
    int vertex_index = -1;
    float weight = 0.0f;
};

struct Bone {
    QString name;
    int node_index = -1; // index in nodes_ array, if resolved
    QVector<float> offset_matrix_flat; // 16 floats (row-major) from aiBone->mOffsetMatrix
    QVector<BoneWeight> weights;
    QString id; // uuid-like id (optional)
};

// Mesh info (we don't store vertex buffers here — loader/renderer will re-read model file)
struct MeshInfo {
    QString id;
    QString name;
    int primitive_count = 0;
    QString material_id;
    QVector<Bone> bones; // if skinned
    int vertex_count = 0;
};

// Scene Node
struct NodeInfo {
    QString id;
    QString name;
    QString parent_id;                 // empty if root
    QVector<QString> children_ids;     // children ids
    Transform transform;
    QString mesh_id;                   // optional
    QString type;                      // "Node", "Mesh", "SkinnedMesh", "Bone"
    bool visible = true;
};

struct BoneKeyframe
{
    double time_ = 0.0;

    QVector3D position_;   // translation
    QVector4D rotation_;   // quaternion (x,y,z,w)
    QVector3D scale_;      // scaling
};

struct AnimationClip
{
    QString id_;
    QString name_;
    double duration_ = 0.0;
    int fps_ = 30;

    // bone name -> list of keyframes
    std::unordered_map<QString, std::vector<BoneKeyframe>> tracks_;
};

struct Keyframe {
    float time = 0.0f;
    QVector3D translation{0.0f,0.0f,0.0f};
    QVector4D rotationQuaternion{0.0f,0.0f,0.0f,1.0f};
    QVector3D scale{1.0f,1.0f,1.0f};
};

struct Track {
    QString node_id;           // which node this track targets
    QVector<Keyframe> keys_;
};

struct AnimationInfo {
    QString id;
    QString name;
    float duration = 0.0f;
    float ticks_per_second = 0.0f;
    QVector<Track> tracks;
};

class ModelDocument {
public:
    QString schema_ = QStringLiteral("com.jahshaka.model.v1");
    QString id_;
    QString name_;
    QString source_file_; // original model path (if any)

    QVector<NodeDef> nodes_;
    QVector<MeshDef> meshes_;
    QVector<MaterialDef> materials_;
    QVector<TextureDef> textures_;
    QVector<AnimationClip> animations_;

    // Utility lookups (not serialized)
    QMap<QString, int> NodeIdToIndex() const {
        QMap<QString, int> m;
        for (int i = 0; i < nodes_.size(); ++i) m[nodes_[i].id_] = i;
        return m;
    }
    QMap<QString, int> MeshIdToIndex() const {
        QMap<QString, int> m;
        for (int i = 0; i < meshes_.size(); ++i) m[meshes_[i].id_] = i;
        return m;
    }
    QMap<QString, int> MaterialIdToIndex() const {
        QMap<QString, int> m;
        for (int i = 0; i < materials_.size(); ++i) m[materials_[i].id_] = i;
        return m;
    }
    QMap<QString, int> TextureIdToIndex() const {
        QMap<QString, int> m;
        for (int i = 0; i < textures_.size(); ++i) m[textures_[i].id_] = i;
        return m;
    }
};

} // namespace vtkmeta

#endif // ASSETDATATYPES_H
