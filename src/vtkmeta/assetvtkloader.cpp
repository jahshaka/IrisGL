#include "assetvtkloader.h"

#include <QJsonValue>

namespace vtkmeta {
// -----------------------------
// Inline implementation
// (keeping in header for convenience; move to .cpp if preferred)
// -----------------------------

inline QVector3D ModelDocumentLoader::ReadVec3(const QJsonArray& arr) {
    if (arr.size() < 3) return QVector3D();
    return QVector3D(arr[0].toDouble(), arr[1].toDouble(), arr[2].toDouble());
}

inline QVector4D ModelDocumentLoader::ReadVec4(const QJsonArray& arr) {
    if (arr.size() < 4) return QVector4D();
    return QVector4D(arr[0].toDouble(), arr[1].toDouble(), arr[2].toDouble(), arr[3].toDouble());
}

inline QString ModelDocumentLoader::ReadStringOrEmpty(const QJsonValue& v) {
    return v.isNull() ? QString() : v.toString();
}

inline void ModelDocumentLoader::ParseTextures(const QJsonArray& arr, ModelDocument& doc) {
    doc.textures_.clear();
    doc.textures_.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        QJsonObject jo = v.toObject();
        TextureDef t;
        t.id_ = jo.value("id").toString();
        t.path_ = jo.value("path").toString();
        t.type_ = jo.value("type").toString();
        doc.textures_.append(t);
    }
}

inline void ModelDocumentLoader::ParseMaterials(const QJsonArray& arr, ModelDocument& doc) {
    doc.materials_.clear();
    doc.materials_.reserve(arr.size());

    for (const QJsonValue& v : arr) {
        QJsonObject jo = v.toObject();
        MaterialDef m;
        m.id_ = jo.value("id").toString();
        m.name_ = jo.value("name").toString();

        if (jo.contains("baseColor") && jo.value("baseColor").isArray()) {
            m.base_color_ = ReadVec3(jo.value("baseColor").toArray());
        }
        m.opacity_ = static_cast<float>(jo.value("opacity").toDouble(1.0));
        m.metallic_ = static_cast<float>(jo.value("metallic").toDouble(0.0));
        m.roughness_ = static_cast<float>(jo.value("roughness").toDouble(0.5));

        if (jo.contains("emissiveColor") && jo.value("emissiveColor").isArray()) {
            m.emissive_color_ = ReadVec3(jo.value("emissiveColor").toArray());
        }
        m.emissive_strength_ = static_cast<float>(jo.value("emissiveStrength").toDouble(0.0));

        m.base_color_texture_ = jo.value("baseColorTex").toString();
        m.normal_texture_ = jo.value("normalTex").toString();
        m.metallic_texture_ = jo.value("metallicTex").toString();
        m.roughness_texture_ = jo.value("roughnessTex").toString();
        m.emissive_texture_ = jo.value("emissiveTex").toString();

        m.double_sided_ = jo.value("doubleSided").toBool(false);
        m.alpha_blend_ = jo.value("alphaBlend").toBool(false);

        doc.materials_.append(m);
    }
}

inline void ModelDocumentLoader::ParseMeshes(const QJsonArray& arr, ModelDocument& doc) {
    doc.meshes_.clear();
    doc.meshes_.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        QJsonObject jo = v.toObject();
        MeshDef m;
        m.id_ = jo.value("id").toString();
        m.name_ = jo.value("name").toString();
        m.vertex_count_ = jo.value("vertexCount").toInt(0);
        m.primitive_count_ = jo.value("primitiveCount").toInt(0);
        m.material_id_ = jo.value("materialId").toString();
        m.skinned_ = jo.value("skinned").toBool(false);
        if (jo.contains("boneNames") && jo.value("boneNames").isArray()) {
            QJsonArray b = jo.value("boneNames").toArray();
            for (const QJsonValue& bn : b) m.bone_names_.append(bn.toString());
        }
        doc.meshes_.append(m);
    }
}

inline void ModelDocumentLoader::ParseNodes(const QJsonArray& arr, ModelDocument& doc) {
    doc.nodes_.clear();
    doc.nodes_.reserve(arr.size());

    for (const QJsonValue& v : arr) {
        QJsonObject jo = v.toObject();
        NodeDef n;
        n.id_ = jo.value("id").toString();
        n.name_ = jo.value("name").toString();
        n.parent_id_ = jo.value("parent").toString();

        if (jo.contains("translation") && jo.value("translation").isArray())
            n.translation_ = ReadVec3(jo.value("translation").toArray());
        if (jo.contains("rotation") && jo.value("rotation").isArray())
            n.rotation_ = ReadVec4(jo.value("rotation").toArray());
        if (jo.contains("scale") && jo.value("scale").isArray())
            n.scale_ = ReadVec3(jo.value("scale").toArray());

        n.mesh_id_ = jo.value("meshId").toString();
        n.material_override_id_ = jo.value("materialId").toString();

        n.visible_ = jo.value("visible").toBool(true);
        n.cast_shadow_ = jo.value("castShadow").toBool(true);
        n.receive_shadow_ = jo.value("receiveShadow").toBool(true);

        n.type_ = jo.value("type").toString("Node");

        if (jo.contains("overrideOpacity")) {
            n.has_opacity_override_ = true;
            n.override_opacity_ = static_cast<float>(jo.value("overrideOpacity").toDouble(1.0));
        }

        if (jo.contains("children") && jo.value("children").isArray()) {
            for (const QJsonValue& c : jo.value("children").toArray()) {
                n.children_ids_.append(c.toString());
            }
        }

        doc.nodes_.append(n);
    }
}

inline void ModelDocumentLoader::ParseAnimations(const QJsonArray& arr, ModelDocument& doc) {
    doc.animations_.clear();
    doc.animations_.reserve(arr.size());

    for (const QJsonValue& v : arr) {
        QJsonObject jo = v.toObject();
        AnimationClip clip;
        clip.id_ = jo.value("id").toString();
        clip.name_ = jo.value("name").toString();
        clip.duration_ = jo.value("duration").toDouble(0.0);
        clip.fps_ = jo.value("fps").toInt(30);

        if (jo.contains("tracks") && jo.value("tracks").isObject()) {
            QJsonObject tracks = jo.value("tracks").toObject();
            for (auto it = tracks.begin(); it != tracks.end(); ++it) {
                QString boneName = it.key();
                QJsonArray kfArr = it.value().toArray();
                std::vector<BoneKeyframe> frames;
                frames.reserve(kfArr.size());
                for (const QJsonValue& kfv : kfArr) {
                    QJsonObject jk = kfv.toObject();
                    BoneKeyframe kf;
                    kf.time_ = jk.value("time").toDouble(0.0);
                    if (jk.contains("position") && jk.value("position").isArray())
                        kf.position_ = ReadVec3(jk.value("position").toArray());
                    if (jk.contains("rotation") && jk.value("rotation").isArray())
                        kf.rotation_ = ReadVec4(jk.value("rotation").toArray());
                    if (jk.contains("scale") && jk.value("scale").isArray())
                        kf.scale_ = ReadVec3(jk.value("scale").toArray());
                    frames.push_back(std::move(kf));
                }
                clip.tracks_.emplace(boneName, std::move(frames));
            }
        }

        doc.animations_.append(std::move(clip));
    }
}

inline bool ModelDocumentLoader::LoadFromJson(const QJsonObject& json, ModelDocument& out_doc, QString& error_message) {
    out_doc = ModelDocument(); // clear

    // meta
    if (json.contains("schema")) out_doc.schema_ = json.value("schema").toString();
    out_doc.id_ = json.value("id").toString();
    out_doc.name_ = json.value("name").toString();
    out_doc.source_file_ = json.value("sourceFile").toString();

    // textures
    if (json.contains("textures") && json.value("textures").isArray()) {
        ParseTextures(json.value("textures").toArray(), out_doc);
    }

    // materials
    if (json.contains("materials") && json.value("materials").isArray()) {
        ParseMaterials(json.value("materials").toArray(), out_doc);
    }

    // meshes
    if (json.contains("meshes") && json.value("meshes").isArray()) {
        ParseMeshes(json.value("meshes").toArray(), out_doc);
    }

    // nodes
    if (json.contains("nodes") && json.value("nodes").isArray()) {
        ParseNodes(json.value("nodes").toArray(), out_doc);
    }

    // animations
    if (json.contains("animations") && json.value("animations").isArray()) {
        ParseAnimations(json.value("animations").toArray(), out_doc);
    }

    // Basic validation
    // e.g., ensure node ids unique
    QSet<QString> nodeIds;
    for (const NodeDef& n : out_doc.nodes_) {
        if (n.id_.isEmpty()) {
            error_message = "Node with empty id";
            return false;
        }
        if (nodeIds.contains(n.id_)) {
            error_message = QStringLiteral("Duplicate node id: %1").arg(n.id_);
            return false;
        }
        nodeIds.insert(n.id_);
    }

    // success
    error_message.clear();
    return true;
}

}
