#include "assetvtkimporter.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <QDateTime>
#include <QUuid>
#include <QDebug>
#include <QFileInfo>

#include <QDebug>

namespace vtkmeta {

QVector3D AssimpImporterNew::AiVecToQ(const aiVector3D& v) const { return QVector3D(v.x,v.y,v.z); }
QVector4D AssimpImporterNew::AiColorToQ(const aiColor4D& c) const { return QVector4D(c.r,c.g,c.b,c.a); }
QVector4D AssimpImporterNew::AiQuatToQ(const aiQuaternion& q) const { return QVector4D(q.x,q.y,q.z,q.w); }

QVector<float> AssimpImporterNew::FlattenMatrix(const aiMatrix4x4& m) const {
    QVector<float> out(16);
    out[0]=m.a1; out[1]=m.a2; out[2]=m.a3; out[3]=m.a4;
    out[4]=m.b1; out[5]=m.b2; out[6]=m.b3; out[7]=m.b4;
    out[8]=m.c1; out[9]=m.c2; out[10]=m.c3; out[11]=m.c4;
    out[12]=m.d1; out[13]=m.d2; out[14]=m.d3; out[15]=m.d4;
    return out;
}

QString AssimpImporterNew::MakeId(const QString& prefix, int index) const { return QString("%1-%2").arg(prefix).arg(index); }

AssimpImporterNew::ImportResult AssimpImporterNew::ImportToDocument(const QString& file_path, ModelDocument* out_doc) {
    ImportResult result;
    if(!out_doc){ result.success=false; result.error_message="out_doc is null"; return result; }

    Assimp::Importer importer;
    unsigned int flags = aiProcess_Triangulate
                         | aiProcess_GenSmoothNormals
                         | aiProcess_JoinIdenticalVertices
                         | aiProcess_ImproveCacheLocality
                         | aiProcess_LimitBoneWeights
                         | aiProcess_OptimizeMeshes
                         | aiProcess_CalcTangentSpace
                         | aiProcess_GlobalScale;

    const aiScene* scene = importer.ReadFile(file_path.toStdString(), flags);
    if(!scene || !scene->mRootNode){ result.success=false; result.error_message=QString::fromUtf8(importer.GetErrorString()); return result; }

    out_doc->id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    out_doc->name_ = QFileInfo(file_path).baseName();
    out_doc->source_file_ = file_path;

    nodePtrToIndex_.clear();

    ProcessMaterials(scene, out_doc);
    ProcessMeshList(scene, out_doc);
    ProcessNodeHierarchy(scene, out_doc);
    ProcessAnimations(scene, out_doc);

    result.success = true;
    return result;
}

// ---------- Materials ----------
void AssimpImporterNew::ProcessMaterials(const aiScene* scene, ModelDocument* out_doc){
    out_doc->materials_.clear();
    out_doc->textures_.clear();

    for(unsigned int i=0;i<scene->mNumMaterials;++i){
        const aiMaterial* mat = scene->mMaterials[i];
        MaterialDef m;
        m.id_ = MakeId("material",i);
        aiString name; mat->Get(AI_MATKEY_NAME,name);
        m.name_ = name.length>0 ? QString::fromUtf8(name.C_Str()) : m.id_;

        aiColor4D diffuse; if(AI_SUCCESS==aiGetMaterialColor(mat, AI_MATKEY_COLOR_DIFFUSE,&diffuse)) m.base_color_ = QVector3D(diffuse.r,diffuse.g,diffuse.b);
        float opacity=1.0f; mat->Get(AI_MATKEY_OPACITY,opacity); m.opacity_=opacity;
        float metallic=0.0f, roughness=0.5f; mat->Get(AI_MATKEY_METALLIC_FACTOR,metallic); mat->Get(AI_MATKEY_ROUGHNESS_FACTOR,roughness);
        m.metallic_=metallic; m.roughness_=roughness;

        aiColor3D emissive; mat->Get(AI_MATKEY_COLOR_EMISSIVE,emissive); m.emissive_color_=QVector3D(emissive.r,emissive.g,emissive.b);
        float emissive_strength=0.0f; mat->Get(AI_MATKEY_EMISSIVE_INTENSITY,emissive_strength); m.emissive_strength_=emissive_strength;

        // textures
        aiString texPath;
        if(mat->GetTextureCount(aiTextureType_BASE_COLOR)>0){ mat->GetTexture(aiTextureType_BASE_COLOR,0,&texPath);
            TextureDef t; t.id_=MakeId("tex",out_doc->textures_.size()); t.path_=QString::fromUtf8(texPath.C_Str()); t.type_="albedo"; m.base_color_texture_=t.id_; out_doc->textures_.append(t);
        }
        if(mat->GetTextureCount(aiTextureType_NORMALS)>0){ mat->GetTexture(aiTextureType_NORMALS,0,&texPath);
            TextureDef t; t.id_=MakeId("tex",out_doc->textures_.size()); t.path_=QString::fromUtf8(texPath.C_Str()); t.type_="normal"; m.normal_texture_=t.id_; out_doc->textures_.append(t);
        }
        if(mat->GetTextureCount(aiTextureType_METALNESS)>0){ mat->GetTexture(aiTextureType_METALNESS,0,&texPath);
            TextureDef t; t.id_=MakeId("tex",out_doc->textures_.size()); t.path_=QString::fromUtf8(texPath.C_Str()); t.type_="metallic"; m.metallic_texture_=t.id_; out_doc->textures_.append(t);
        }
        if(mat->GetTextureCount(aiTextureType_DIFFUSE_ROUGHNESS)>0){ mat->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS,0,&texPath);
            TextureDef t; t.id_=MakeId("tex",out_doc->textures_.size()); t.path_=QString::fromUtf8(texPath.C_Str()); t.type_="roughness"; m.roughness_texture_=t.id_; out_doc->textures_.append(t);
        }
        if(mat->GetTextureCount(aiTextureType_EMISSIVE)>0){ mat->GetTexture(aiTextureType_EMISSIVE,0,&texPath);
            TextureDef t; t.id_=MakeId("tex",out_doc->textures_.size()); t.path_=QString::fromUtf8(texPath.C_Str()); t.type_="emissive"; m.emissive_texture_=t.id_; out_doc->textures_.append(t);
        }

        out_doc->materials_.append(m);
    }
}

// ---------- Meshes ----------
void AssimpImporterNew::ProcessMeshList(const aiScene* scene, ModelDocument* out_doc){
    out_doc->meshes_.clear();

    for(unsigned int i=0;i<scene->mNumMeshes;++i){
        const aiMesh* aimesh = scene->mMeshes[i];
        MeshDef mesh;
        mesh.id_ = MakeId("mesh",i);
        mesh.name_ = aimesh->mName.length>0 ? QString::fromUtf8(aimesh->mName.C_Str()) : mesh.id_;
        mesh.vertex_count_ = static_cast<int>(aimesh->mNumVertices);
        mesh.primitive_count_ = static_cast<int>(aimesh->mNumFaces);
        if(aimesh->mMaterialIndex<scene->mNumMaterials) mesh.material_id_=MakeId("material",aimesh->mMaterialIndex);

        if(aimesh->HasBones()){
            mesh.skinned_=true;
            for(unsigned int bi=0;bi<aimesh->mNumBones;++bi){
                mesh.bone_names_.append(QString::fromUtf8(aimesh->mBones[bi]->mName.C_Str()));
            }
        }

        out_doc->meshes_.append(mesh);
    }
}

// ---------- Nodes ----------
void AssimpImporterNew::ProcessNodeHierarchy(const aiScene* scene, ModelDocument* out_doc){
    out_doc->nodes_.clear();
    std::function<void(const aiNode*, const QString&)> recurse;
    int node_counter=0;

    recurse=[&](const aiNode* ai_node,const QString& parent_id){
        NodeDef node;
        node.id_=MakeId("node",node_counter++);
        node.name_=ai_node->mName.length>0 ? QString::fromUtf8(ai_node->mName.C_Str()) : node.id_;
        node.parent_id_=parent_id;
        node.children_ids_.clear();
        node.visible_=true;
        node.cast_shadow_=true;
        node.receive_shadow_=true;
        node.type_="Node";

        aiVector3D scaling; aiQuaternion rotation; aiVector3D position;
        ai_node->mTransformation.Decompose(scaling,rotation,position);
        node.translation_=AiVecToQ(position);
        node.scale_=AiVecToQ(scaling);
        node.rotation_=AiQuatToQ(rotation);

        if(ai_node->mNumMeshes>0){
            unsigned int meshIndex=ai_node->mMeshes[0];
            node.mesh_id_=MakeId("mesh",meshIndex);
            if(scene->mMeshes[meshIndex]->HasBones()) node.type_="SkinnedMesh";
            else node.type_="Mesh";
        }

        int created_index=out_doc->nodes_.size();
        out_doc->nodes_.append(node);
        nodePtrToIndex_[ai_node]=created_index;

        for(unsigned int ci=0;ci<ai_node->mNumChildren;++ci){
            recurse(ai_node->mChildren[ci],node.id_);
            int child_idx=nodePtrToIndex_[ai_node->mChildren[ci]];
            out_doc->nodes_[created_index].children_ids_.append(out_doc->nodes_[child_idx].id_);
        }
    };

    recurse(scene->mRootNode,"");
}

// ---------- Animations ----------
void AssimpImporterNew::ProcessAnimations(const aiScene* scene, ModelDocument* out_doc){
    out_doc->animations_.clear();
    if(!scene->HasAnimations()) return;

    for(unsigned int ai=0;ai<scene->mNumAnimations;++ai){
        const aiAnimation* aianim = scene->mAnimations[ai];
        AnimationClip clip;
        clip.id_=MakeId("anim",ai);
        clip.name_=aianim->mName.length>0 ? QString::fromUtf8(aianim->mName.C_Str()) : clip.id_;
        clip.duration_=aianim->mDuration;
        clip.fps_ = aianim->mTicksPerSecond!=0 ? static_cast<int>(aianim->mTicksPerSecond) : 30;

        for(unsigned int ch=0; ch<aianim->mNumChannels; ++ch){
            const aiNodeAnim* chan = aianim->mChannels[ch];
            QString nodeName=QString::fromUtf8(chan->mNodeName.C_Str());
            std::vector<BoneKeyframe> keyframes;

            QMap<double,BoneKeyframe> merged;
            for(unsigned int pi=0;pi<chan->mNumPositionKeys;++pi){
                double t=chan->mPositionKeys[pi].mTime;
                BoneKeyframe kf; kf.time_=t; kf.position_=AiVecToQ(chan->mPositionKeys[pi].mValue); merged[t]=kf;
            }
            for(unsigned int ri=0;ri<chan->mNumRotationKeys;++ri){
                double t=chan->mRotationKeys[ri].mTime;
                if(!merged.contains(t)) merged[t]=BoneKeyframe();
                merged[t].rotation_=AiQuatToQ(chan->mRotationKeys[ri].mValue);
            }
            for(unsigned int si=0;si<chan->mNumScalingKeys;++si){
                double t=chan->mScalingKeys[si].mTime;
                if(!merged.contains(t)) merged[t]=BoneKeyframe();
                merged[t].scale_=AiVecToQ(chan->mScalingKeys[si].mValue);
            }

            for(auto it=merged.constBegin(); it!=merged.constEnd(); ++it) keyframes.push_back(it.value());
            clip.tracks_[nodeName]=keyframes;
        }

        out_doc->animations_.append(clip);
    }
}

}

