#include "modeldocumentserializer.h"

#include <QJsonDocument>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>

namespace vtkmeta {

QJsonObject ModelDocumentSerializer::toJson(const ModelDocument &doc)
{
    QJsonObject root;
    root["schema"] = doc.schema_;
    root["id"] = doc.id_;
    root["name"] = doc.name_;
    root["source_file"] = doc.source_file_;

    // nodes
    QJsonArray nodesArr;
    for (const NodeDef &n : doc.nodes_) {
        QJsonObject no;
        no["id"] = n.id_;
        no["name"] = n.name_;
        no["parent_id"] = n.parent_id_;
        QJsonArray children;
        for (const QString &c : n.children_ids_) children.append(c);

        no["children_ids"] = children;

        QJsonObject t;
        QJsonArray tr{ n.translation_.x(), n.translation_.y(), n.translation_.z() };
        QJsonArray rot{ n.rotation_.x(), n.rotation_.y(), n.rotation_.z(), n.rotation_.w() };
        QJsonArray sc{ n.scale_.x(), n.scale_.y(), n.scale_.z() };
        t["translation"] = tr;
        t["rotation"] = rot;
        t["scale"] = sc;
        no["transform"] = t;

        no["mesh_id"] = n.mesh_id_;
        no["material_override_id"] = n.material_override_id_;
        no["visible"] = n.visible_;
        no["type"] = n.type_;
        nodesArr.append(no);
    }
    root["nodes"] = nodesArr;

    // meshes
    QJsonArray meshesArr;
    for (const MeshDef &m : doc.meshes_) {
        QJsonObject mo;
        mo["id"] = m.id_;
        mo["name"] = m.name_;
        mo["vertex_count"] = m.vertex_count_;
        mo["primitive_count"] = m.primitive_count_;
        mo["material_id"] = m.material_id_;
        mo["skinned"] = m.skinned_;
        QJsonArray bones;
        for (const QString &bn : m.bone_names_) bones.append(bn);
        mo["bone_names"] = bones;
        meshesArr.append(mo);
    }
    root["meshes"] = meshesArr;

    // materials
    QJsonArray matsArr;
    for (const MaterialDef &mat : doc.materials_) {
        QJsonObject mo;
        mo["id"] = mat.id_;
        mo["name"] = mat.name_;
        QJsonArray baseColor{ mat.base_color_.x(), mat.base_color_.y(), mat.base_color_.z() };
        mo["base_color"] = baseColor;
        mo["opacity"] = mat.opacity_;
        mo["metallic"] = mat.metallic_;
        mo["roughness"] = mat.roughness_;
        mo["emissive_color"] = QJsonArray{ mat.emissive_color_.x(), mat.emissive_color_.y(), mat.emissive_color_.z() };
        mo["emissive_strength"] = mat.emissive_strength_;
        mo["double_sided"] = mat.double_sided_;
        mo["alpha_blend"] = mat.alpha_blend_;
        mo["base_color_texture"] = mat.base_color_texture_;
        mo["normal_texture"] = mat.normal_texture_;
        mo["metallic_texture"] = mat.metallic_texture_;
        mo["roughness_texture"] = mat.roughness_texture_;
        mo["emissive_texture"] = mat.emissive_texture_;
        matsArr.append(mo);
    }
    root["materials"] = matsArr;

    // textures
    QJsonArray texArr;
    for (const TextureDef &t : doc.textures_) {
        QJsonObject to;
        to["id"] = t.id_;
        to["path"] = t.path_;
        to["type"] = t.type_;
        texArr.append(to);
    }
    root["textures"] = texArr;

    // animations (simple serialization for AnimationClip)
    QJsonArray animArr;
    for (const AnimationClip &ac : doc.animations_) {
        QJsonObject ao;
        ao["id"] = ac.id_;
        ao["name"] = ac.name_;
        ao["duration"] = ac.duration_;
        ao["fps"] = ac.fps_;
        QJsonObject tracksObj;
        for (const auto &kv : ac.tracks_) {
            QJsonArray kfArr;
            for (const BoneKeyframe &kf : kv.second) {
                QJsonObject kfo;
                kfo["time"] = kf.time_;
                kfo["pos"] = QJsonArray{ kf.position_.x(), kf.position_.y(), kf.position_.z() };
                kfo["rot"] = QJsonArray{ kf.rotation_.x(), kf.rotation_.y(), kf.rotation_.z(), kf.rotation_.w() };
                kfo["scale"] = QJsonArray{ kf.scale_.x(), kf.scale_.y(), kf.scale_.z() };
                kfArr.append(kfo);
            }
            tracksObj[QString(kv.first)] = kfArr;
        }
        ao["tracks"] = tracksObj;
        animArr.append(ao);
    }
    root["animations"] = animArr;

    return root;
}

bool ModelDocumentSerializer::saveToFile(const ModelDocument &doc, const QString &filePath)
{
    QJsonObject j = toJson(doc);
    QJsonDocument d(j);
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(d.toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

bool ModelDocumentSerializer::loadFromFile(ModelDocument &outDoc, const QString &filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonDocument d = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (d.isNull() || !d.isObject()) return false;
    QJsonObject root = d.object();

    outDoc = ModelDocument();
    outDoc.schema_ = root.value("schema").toString(outDoc.schema_);
    outDoc.id_ = root.value("id").toString();
    outDoc.name_ = root.value("name").toString();
    outDoc.source_file_ = root.value("source_file").toString();

    // nodes
    outDoc.nodes_.clear();
    QJsonArray nodesArr = root.value("nodes").toArray();
    for (const QJsonValue &nv : nodesArr) {
        QJsonObject no = nv.toObject();
        NodeDef n;
        n.id_ = no.value("id").toString();
        n.name_ = no.value("name").toString();
        n.parent_id_ = no.value("parent_id").toString();
        n.children_ids_.clear();
        for (const QJsonValue &c : no.value("children_ids").toArray()) n.children_ids_.append(c.toString());
        QJsonObject t = no.value("transform").toObject();
        auto ta = t.value("translation").toArray();
        auto ra = t.value("rotation").toArray();
        auto sa = t.value("scale").toArray();
        if (ta.size() >= 3) n.translation_ = QVector3D(ta[0].toDouble(), ta[1].toDouble(), ta[2].toDouble());
        if (ra.size() >= 4) n.rotation_ = QVector4D(ra[0].toDouble(), ra[1].toDouble(), ra[2].toDouble(), ra[3].toDouble());
        if (sa.size() >= 3) n.scale_ = QVector3D(sa[0].toDouble(), sa[1].toDouble(), sa[2].toDouble());
        n.mesh_id_ = no.value("mesh_id").toString();
        n.material_override_id_ = no.value("material_override_id").toString();
        n.visible_ = no.value("visible").toBool(true);
        n.type_ = no.value("type").toString("Node");
        outDoc.nodes_.append(n);
    }

    // meshes
    outDoc.meshes_.clear();
    for (const QJsonValue &mv : root.value("meshes").toArray()) {
        QJsonObject mo = mv.toObject();
        MeshDef m;
        m.id_ = mo.value("id").toString();
        m.name_ = mo.value("name").toString();
        m.vertex_count_ = mo.value("vertex_count").toInt();
        m.primitive_count_ = mo.value("primitive_count").toInt();
        m.material_id_ = mo.value("material_id").toString();
        m.skinned_ = mo.value("skinned").toBool();
        for (const QJsonValue &bn : mo.value("bone_names").toArray()) m.bone_names_.append(bn.toString());
        outDoc.meshes_.append(m);
    }

    // materials
    outDoc.materials_.clear();
    for (const QJsonValue &mv : root.value("materials").toArray()) {
        QJsonObject mo = mv.toObject();
        MaterialDef mat;
        mat.id_ = mo.value("id").toString();
        mat.name_ = mo.value("name").toString();
        auto bc = mo.value("base_color").toArray();
        if (bc.size() >= 3) mat.base_color_ = QVector3D(bc[0].toDouble(), bc[1].toDouble(), bc[2].toDouble());
        mat.opacity_ = mo.value("opacity").toDouble(1.0);
        mat.metallic_ = mo.value("metallic").toDouble(0.0);
        mat.roughness_ = mo.value("roughness").toDouble(0.5);
        mat.emissive_color_ = QVector3D(
            mo.value("emissive_color").toArray().at(0).toDouble(),
            mo.value("emissive_color").toArray().at(1).toDouble(),
            mo.value("emissive_color").toArray().at(2).toDouble()
            );
        mat.emissive_strength_ = mo.value("emissive_strength").toDouble(0.0);
        mat.double_sided_ = mo.value("double_sided").toBool(false);
        mat.alpha_blend_ = mo.value("alpha_blend").toBool(false);
        mat.base_color_texture_ = mo.value("base_color_texture").toString();
        mat.normal_texture_ = mo.value("normal_texture").toString();
        mat.metallic_texture_ = mo.value("metallic_texture").toString();
        mat.roughness_texture_ = mo.value("roughness_texture").toString();
        mat.emissive_texture_ = mo.value("emissive_texture").toString();
        outDoc.materials_.append(mat);
    }

    // textures
    outDoc.textures_.clear();
    for (const QJsonValue &tv : root.value("textures").toArray()) {
        QJsonObject to = tv.toObject();
        TextureDef t;
        t.id_ = to.value("id").toString();
        t.path_ = to.value("path").toString();
        t.type_ = to.value("type").toString();
        outDoc.textures_.append(t);
    }

    // animations - skipping detailed reconstruction here (can be added)
    outDoc.animations_.clear();

    return true;
}

bool ModelDocumentSerializer::loadFromJson(ModelDocument &outDoc, const QJsonDocument &d)
{
    if (d.isNull() || !d.isObject()) return false;
    QJsonObject root = d.object();

    outDoc = ModelDocument();
    outDoc.schema_ = root.value("schema").toString(outDoc.schema_);
    outDoc.id_ = root.value("id").toString();
    outDoc.name_ = root.value("name").toString();
    outDoc.source_file_ = root.value("source_file").toString();

    // nodes
    outDoc.nodes_.clear();
    QJsonArray nodesArr = root.value("nodes").toArray();
    for (const QJsonValue &nv : nodesArr) {
        QJsonObject no = nv.toObject();
        NodeDef n;
        n.id_ = no.value("id").toString();
        n.name_ = no.value("name").toString();
        n.parent_id_ = no.value("parent_id").toString();
        n.children_ids_.clear();
        for (const QJsonValue &c : no.value("children_ids").toArray()) n.children_ids_.append(c.toString());
        QJsonObject t = no.value("transform").toObject();
        auto ta = t.value("translation").toArray();
        auto ra = t.value("rotation").toArray();
        auto sa = t.value("scale").toArray();
        if (ta.size() >= 3) n.translation_ = QVector3D(ta[0].toDouble(), ta[1].toDouble(), ta[2].toDouble());
        if (ra.size() >= 4) n.rotation_ = QVector4D(ra[0].toDouble(), ra[1].toDouble(), ra[2].toDouble(), ra[3].toDouble());
        if (sa.size() >= 3) n.scale_ = QVector3D(sa[0].toDouble(), sa[1].toDouble(), sa[2].toDouble());
        n.mesh_id_ = no.value("mesh_id").toString();
        n.material_override_id_ = no.value("material_override_id").toString();
        n.visible_ = no.value("visible").toBool(true);
        n.type_ = no.value("type").toString("Node");
        outDoc.nodes_.append(n);
    }

    // meshes
    outDoc.meshes_.clear();
    for (const QJsonValue &mv : root.value("meshes").toArray()) {
        QJsonObject mo = mv.toObject();
        MeshDef m;
        m.id_ = mo.value("id").toString();
        m.name_ = mo.value("name").toString();
        m.vertex_count_ = mo.value("vertex_count").toInt();
        m.primitive_count_ = mo.value("primitive_count").toInt();
        m.material_id_ = mo.value("material_id").toString();
        m.skinned_ = mo.value("skinned").toBool();
        for (const QJsonValue &bn : mo.value("bone_names").toArray()) m.bone_names_.append(bn.toString());
        outDoc.meshes_.append(m);
    }

    // materials
    outDoc.materials_.clear();
    for (const QJsonValue &mv : root.value("materials").toArray()) {
        QJsonObject mo = mv.toObject();
        MaterialDef mat;
        mat.id_ = mo.value("id").toString();
        mat.name_ = mo.value("name").toString();
        auto bc = mo.value("base_color").toArray();
        if (bc.size() >= 3) mat.base_color_ = QVector3D(bc[0].toDouble(), bc[1].toDouble(), bc[2].toDouble());
        mat.opacity_ = mo.value("opacity").toDouble(1.0);
        mat.metallic_ = mo.value("metallic").toDouble(0.0);
        mat.roughness_ = mo.value("roughness").toDouble(0.5);
        mat.emissive_color_ = QVector3D(
            mo.value("emissive_color").toArray().at(0).toDouble(),
            mo.value("emissive_color").toArray().at(1).toDouble(),
            mo.value("emissive_color").toArray().at(2).toDouble()
            );
        mat.emissive_strength_ = mo.value("emissive_strength").toDouble(0.0);
        mat.double_sided_ = mo.value("double_sided").toBool(false);
        mat.alpha_blend_ = mo.value("alpha_blend").toBool(false);
        mat.base_color_texture_ = mo.value("base_color_texture").toString();
        mat.normal_texture_ = mo.value("normal_texture").toString();
        mat.metallic_texture_ = mo.value("metallic_texture").toString();
        mat.roughness_texture_ = mo.value("roughness_texture").toString();
        mat.emissive_texture_ = mo.value("emissive_texture").toString();
        outDoc.materials_.append(mat);
    }

    // textures
    outDoc.textures_.clear();
    for (const QJsonValue &tv : root.value("textures").toArray()) {
        QJsonObject to = tv.toObject();
        TextureDef t;
        t.id_ = to.value("id").toString();
        t.path_ = to.value("path").toString();
        t.type_ = to.value("type").toString();
        outDoc.textures_.append(t);
    }

    // animations - skipping detailed reconstruction here (can be added)
    outDoc.animations_.clear();

    return true;
}

} // namespace vtkmeta
