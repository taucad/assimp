/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2025, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/

#ifdef ASSIMP_USE_LIB3MF

#include "Lib3MFBridge.h"

#include <assimp/Exceptional.h>
#include <assimp/scene.h>
#include <assimp/mesh.h>
#include <assimp/material.h>
#include <assimp/DefaultLogger.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>

#include <lib3mf_types.hpp>
#include <lib3mf_abi.hpp>

#include <vector>
#include <map>
#include <string>
#include <memory>
#include <cstring>
#include <stdexcept>

namespace Assimp {
namespace D3MF {

namespace {

class Lib3MFHandle {
public:
    explicit Lib3MFHandle(Lib3MF_Base handle = nullptr) : mHandle(handle) {}
    ~Lib3MFHandle() {
        if (mHandle) {
            lib3mf_release(mHandle);
        }
    }
    Lib3MFHandle(const Lib3MFHandle &) = delete;
    Lib3MFHandle &operator=(const Lib3MFHandle &) = delete;
    Lib3MFHandle(Lib3MFHandle &&other) noexcept : mHandle(other.mHandle) { other.mHandle = nullptr; }
    Lib3MFHandle &operator=(Lib3MFHandle &&other) noexcept {
        if (this != &other) {
            if (mHandle) { lib3mf_release(mHandle); }
            mHandle = other.mHandle;
            other.mHandle = nullptr;
        }
        return *this;
    }

    Lib3MF_Base get() const { return mHandle; }
    Lib3MF_Base *ptr() { return &mHandle; }

    template <typename T>
    T as() const { return static_cast<T>(mHandle); }

private:
    Lib3MF_Base mHandle;
};

void checkResult(Lib3MFResult result, Lib3MF_Base instance, const char *context) {
    if (result != LIB3MF_SUCCESS) {
        std::string errorMsg = context;
        if (instance) {
            Lib3MF_uint32 neededChars = 0;
            bool hasError = false;
            lib3mf_getlasterror(instance, 0, &neededChars, nullptr, &hasError);
            if (hasError && neededChars > 0) {
                std::vector<char> buffer(neededChars + 1, '\0');
                lib3mf_getlasterror(instance, neededChars + 1, &neededChars, buffer.data(), &hasError);
                errorMsg += ": ";
                errorMsg += buffer.data();
            }
        }
        throw DeadlyExportError(errorMsg);
    }
}

void checkImportResult(Lib3MFResult result, Lib3MF_Base instance, const char *context) {
    if (result != LIB3MF_SUCCESS) {
        std::string errorMsg = context;
        if (instance) {
            Lib3MF_uint32 neededChars = 0;
            bool hasError = false;
            lib3mf_getlasterror(instance, 0, &neededChars, nullptr, &hasError);
            if (hasError && neededChars > 0) {
                std::vector<char> buffer(neededChars + 1, '\0');
                lib3mf_getlasterror(instance, neededChars + 1, &neededChars, buffer.data(), &hasError);
                errorMsg += ": ";
                errorMsg += buffer.data();
            }
        }
        throw DeadlyImportError(errorMsg);
    }
}

Lib3MF::sTransform aiMatrixToLib3MFTransform(const aiMatrix4x4 &m) {
    Lib3MF::sTransform t;
    t.m_Fields[0][0] = m.a1; t.m_Fields[0][1] = m.b1; t.m_Fields[0][2] = m.c1;
    t.m_Fields[1][0] = m.a2; t.m_Fields[1][1] = m.b2; t.m_Fields[1][2] = m.c2;
    t.m_Fields[2][0] = m.a3; t.m_Fields[2][1] = m.b3; t.m_Fields[2][2] = m.c3;
    t.m_Fields[3][0] = m.a4; t.m_Fields[3][1] = m.b4; t.m_Fields[3][2] = m.c4;
    return t;
}

aiMatrix4x4 lib3MFTransformToAiMatrix(const Lib3MF::sTransform &t) {
    aiMatrix4x4 m;
    m.a1 = t.m_Fields[0][0]; m.b1 = t.m_Fields[0][1]; m.c1 = t.m_Fields[0][2]; m.d1 = 0.0f;
    m.a2 = t.m_Fields[1][0]; m.b2 = t.m_Fields[1][1]; m.c2 = t.m_Fields[1][2]; m.d2 = 0.0f;
    m.a3 = t.m_Fields[2][0]; m.b3 = t.m_Fields[2][1]; m.c3 = t.m_Fields[2][2]; m.d3 = 0.0f;
    m.a4 = t.m_Fields[3][0]; m.b4 = t.m_Fields[3][1]; m.c4 = t.m_Fields[3][2]; m.d4 = 1.0f;
    return m;
}

Lib3MF::sColor aiColorToLib3MF(const aiColor4D &c) {
    Lib3MF::sColor color;
    color.m_Red = static_cast<Lib3MF_uint8>(c.r * 255.0f);
    color.m_Green = static_cast<Lib3MF_uint8>(c.g * 255.0f);
    color.m_Blue = static_cast<Lib3MF_uint8>(c.b * 255.0f);
    color.m_Alpha = static_cast<Lib3MF_uint8>(c.a * 255.0f);
    return color;
}

aiColor4D lib3MFColorToAi(const Lib3MF::sColor &c) {
    return aiColor4D(
        c.m_Red / 255.0f,
        c.m_Green / 255.0f,
        c.m_Blue / 255.0f,
        c.m_Alpha / 255.0f
    );
}

// ===== EXPORT IMPLEMENTATION =====

void collectMeshNodes(const aiScene *scene, const aiNode *node,
                      const aiMatrix4x4 &parentTransform,
                      std::vector<std::pair<unsigned int, aiMatrix4x4>> &meshEntries) {
    aiMatrix4x4 globalTransform = parentTransform * node->mTransformation;
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        meshEntries.emplace_back(node->mMeshes[i], globalTransform);
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        collectMeshNodes(scene, node->mChildren[i], globalTransform, meshEntries);
    }
}

void exportToLib3MF(const aiScene *pScene, std::vector<Lib3MF_uint8> &outputBuffer) {
    Lib3MFHandle model;
    checkResult(lib3mf_createmodel(model.ptr()), nullptr, "Failed to create lib3mf model");

    Lib3MFHandle baseMaterialGroup;
    std::vector<Lib3MF_uint32> materialPropertyIDs;

    if (pScene->mNumMaterials > 0) {
        checkResult(
            lib3mf_model_addbasematerialgroup(model.as<Lib3MF_Model>(), baseMaterialGroup.ptr()),
            model.get(), "Failed to create base material group"
        );

        for (unsigned int i = 0; i < pScene->mNumMaterials; ++i) {
            const aiMaterial *mat = pScene->mMaterials[i];
            aiColor4D diffuse(0.8f, 0.8f, 0.8f, 1.0f);
            mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse);

            aiString matName;
            if (mat->Get(AI_MATKEY_NAME, matName) != AI_SUCCESS) {
                matName = aiString("Material_" + std::to_string(i));
            }

            Lib3MF::sColor color = aiColorToLib3MF(diffuse);
            Lib3MF_uint32 propID = 0;
            checkResult(
                lib3mf_basematerialgroup_addmaterial(
                    baseMaterialGroup.as<Lib3MF_BaseMaterialGroup>(),
                    matName.C_Str(), &color, &propID
                ),
                baseMaterialGroup.get(), "Failed to add material"
            );
            materialPropertyIDs.push_back(propID);
        }
    }

    std::vector<std::pair<unsigned int, aiMatrix4x4>> meshEntries;
    if (pScene->mRootNode) {
        collectMeshNodes(pScene, pScene->mRootNode, aiMatrix4x4(), meshEntries);
    }

    if (meshEntries.empty()) {
        for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
            meshEntries.emplace_back(i, aiMatrix4x4());
        }
    }

    for (const auto &entry : meshEntries) {
        unsigned int meshIdx = entry.first;
        const aiMatrix4x4 &transform = entry.second;
        const aiMesh *mesh = pScene->mMeshes[meshIdx];

        Lib3MFHandle meshObject;
        checkResult(
            lib3mf_model_addmeshobject(model.as<Lib3MF_Model>(), meshObject.ptr()),
            model.get(), "Failed to add mesh object"
        );

        if (mesh->mName.length > 0) {
            lib3mf_object_setname(meshObject.as<Lib3MF_Object>(), mesh->mName.C_Str());
        }

        std::vector<Lib3MF::sPosition> vertices(mesh->mNumVertices);
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            vertices[v].m_Coordinates[0] = mesh->mVertices[v].x;
            vertices[v].m_Coordinates[1] = mesh->mVertices[v].y;
            vertices[v].m_Coordinates[2] = mesh->mVertices[v].z;
        }

        std::vector<Lib3MF::sTriangle> triangles(mesh->mNumFaces);
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace &face = mesh->mFaces[f];
            if (face.mNumIndices != 3) {
                continue;
            }
            triangles[f].m_Indices[0] = face.mIndices[0];
            triangles[f].m_Indices[1] = face.mIndices[1];
            triangles[f].m_Indices[2] = face.mIndices[2];
        }

        checkResult(
            lib3mf_meshobject_setgeometry(
                meshObject.as<Lib3MF_MeshObject>(),
                vertices.size(), vertices.data(),
                triangles.size(), triangles.data()
            ),
            meshObject.get(), "Failed to set mesh geometry"
        );

        if (!materialPropertyIDs.empty() && mesh->mMaterialIndex < materialPropertyIDs.size()) {
            Lib3MF_uint32 matPropID = materialPropertyIDs[mesh->mMaterialIndex];

            Lib3MF_uint32 uniqueResID = 0;
            lib3mf_resource_getuniqueresourceid(baseMaterialGroup.as<Lib3MF_Resource>(), &uniqueResID);

            lib3mf_meshobject_setobjectlevelproperty(
                meshObject.as<Lib3MF_MeshObject>(), uniqueResID, matPropID
            );

            std::vector<Lib3MF::sTriangleProperties> triProps(triangles.size());
            for (size_t f = 0; f < triangles.size(); ++f) {
                triProps[f].m_ResourceID = uniqueResID;
                triProps[f].m_PropertyIDs[0] = matPropID;
                triProps[f].m_PropertyIDs[1] = matPropID;
                triProps[f].m_PropertyIDs[2] = matPropID;
            }
            lib3mf_meshobject_setalltriangleproperties(
                meshObject.as<Lib3MF_MeshObject>(),
                triProps.size(), triProps.data()
            );
        }

        Lib3MFHandle buildItem;
        Lib3MF::sTransform lib3mfTransform = aiMatrixToLib3MFTransform(transform);
        checkResult(
            lib3mf_model_addbuilditem(
                model.as<Lib3MF_Model>(),
                meshObject.as<Lib3MF_Object>(),
                &lib3mfTransform,
                buildItem.ptr()
            ),
            model.get(), "Failed to add build item"
        );
    }

    Lib3MFHandle writer;
    checkResult(
        lib3mf_model_querywriter(model.as<Lib3MF_Model>(), "3mf", writer.ptr()),
        model.get(), "Failed to create 3MF writer"
    );

    Lib3MF_uint64 neededSize = 0;
    checkResult(
        lib3mf_writer_writetobuffer(writer.as<Lib3MF_Writer>(), 0, &neededSize, nullptr),
        writer.get(), "Failed to get 3MF buffer size"
    );

    outputBuffer.resize(static_cast<size_t>(neededSize));
    Lib3MF_uint64 writtenSize = 0;
    checkResult(
        lib3mf_writer_writetobuffer(
            writer.as<Lib3MF_Writer>(),
            neededSize, &writtenSize,
            outputBuffer.data()
        ),
        writer.get(), "Failed to write 3MF to buffer"
    );
    outputBuffer.resize(static_cast<size_t>(writtenSize));
}

// ===== IMPORT IMPLEMENTATION =====

void importFromLib3MF(aiScene *pScene, const std::vector<Lib3MF_uint8> &inputBuffer) {
    Lib3MFHandle model;
    checkImportResult(lib3mf_createmodel(model.ptr()), nullptr, "Failed to create lib3mf model");

    Lib3MFHandle reader;
    checkImportResult(
        lib3mf_model_queryreader(model.as<Lib3MF_Model>(), "3mf", reader.ptr()),
        model.get(), "Failed to create 3MF reader"
    );

    lib3mf_reader_setstrictmodeactive(reader.as<Lib3MF_Reader>(), false);

    checkImportResult(
        lib3mf_reader_readfrombuffer(
            reader.as<Lib3MF_Reader>(),
            inputBuffer.size(),
            inputBuffer.data()
        ),
        reader.get(), "Failed to read 3MF from buffer"
    );

    // First pass: count meshes
    Lib3MFHandle meshIterator;
    checkImportResult(
        lib3mf_model_getmeshobjects(model.as<Lib3MF_Model>(), meshIterator.ptr()),
        model.get(), "Failed to get mesh objects"
    );

    struct MeshData {
        std::string name;
        Lib3MF_uint32 vertexCount;
        Lib3MF_uint32 triangleCount;
    };
    std::vector<MeshData> meshDataList;

    bool hasNext = false;
    lib3mf_resourceiterator_movenext(meshIterator.as<Lib3MF_ResourceIterator>(), &hasNext);
    while (hasNext) {
        Lib3MFHandle currentMesh;
        checkImportResult(
            lib3mf_meshobjectiterator_getcurrentmeshobject(
                meshIterator.as<Lib3MF_MeshObjectIterator>(), currentMesh.ptr()
            ),
            meshIterator.get(), "Failed to get current mesh object"
        );

        MeshData md;
        Lib3MF_MeshObject meshObj = currentMesh.as<Lib3MF_MeshObject>();

        Lib3MF_uint32 neededChars = 0;
        lib3mf_object_getname(meshObj, 0, &neededChars, nullptr);
        if (neededChars > 0) {
            std::vector<char> nameBuf(neededChars + 1, '\0');
            lib3mf_object_getname(meshObj, neededChars + 1, &neededChars, nameBuf.data());
            md.name = nameBuf.data();
        }

        lib3mf_meshobject_getvertexcount(meshObj, &md.vertexCount);
        lib3mf_meshobject_gettrianglecount(meshObj, &md.triangleCount);

        meshDataList.push_back(md);

        lib3mf_resourceiterator_movenext(meshIterator.as<Lib3MF_ResourceIterator>(), &hasNext);
    }

    if (meshDataList.empty()) {
        return;
    }

    // Build materials from base material groups
    Lib3MFHandle baseMaterialIterator;
    lib3mf_model_getbasematerialgroups(model.as<Lib3MF_Model>(), baseMaterialIterator.ptr());

    struct MaterialInfo {
        Lib3MF_uint32 resourceID;
        Lib3MF_uint32 propertyID;
        std::string name;
        Lib3MF::sColor color;
    };
    std::vector<MaterialInfo> materialInfos;
    std::map<std::pair<Lib3MF_uint32, Lib3MF_uint32>, unsigned int> materialMap;

    if (baseMaterialIterator.get()) {
        bool hasBMNext = false;
        lib3mf_resourceiterator_movenext(baseMaterialIterator.as<Lib3MF_ResourceIterator>(), &hasBMNext);
        while (hasBMNext) {
            Lib3MFHandle bmGroup;
            lib3mf_basematerialgroupiterator_getcurrentbasematerialgroup(
                baseMaterialIterator.as<Lib3MF_BaseMaterialGroupIterator>(), bmGroup.ptr()
            );

            Lib3MF_uint32 resourceID = 0;
            lib3mf_resource_getuniqueresourceid(bmGroup.as<Lib3MF_Resource>(), &resourceID);

            Lib3MF_uint32 matCount = 0;
            lib3mf_basematerialgroup_getcount(bmGroup.as<Lib3MF_BaseMaterialGroup>(), &matCount);

            std::vector<Lib3MF_uint32> propIDs(matCount);
            Lib3MF_uint64 propCount = 0;
            lib3mf_basematerialgroup_getallpropertyids(
                bmGroup.as<Lib3MF_BaseMaterialGroup>(),
                matCount, &propCount, propIDs.data()
            );

            for (Lib3MF_uint32 m = 0; m < matCount; ++m) {
                MaterialInfo info;
                info.resourceID = resourceID;
                info.propertyID = propIDs[m];

                Lib3MF_uint32 nameChars = 0;
                lib3mf_basematerialgroup_getname(
                    bmGroup.as<Lib3MF_BaseMaterialGroup>(), propIDs[m], 0, &nameChars, nullptr
                );
                if (nameChars > 0) {
                    std::vector<char> nameBuf(nameChars + 1, '\0');
                    lib3mf_basematerialgroup_getname(
                        bmGroup.as<Lib3MF_BaseMaterialGroup>(), propIDs[m],
                        nameChars + 1, &nameChars, nameBuf.data()
                    );
                    info.name = nameBuf.data();
                }

                lib3mf_basematerialgroup_getdisplaycolor(
                    bmGroup.as<Lib3MF_BaseMaterialGroup>(), propIDs[m], &info.color
                );

                unsigned int matIdx = static_cast<unsigned int>(materialInfos.size());
                materialMap[{resourceID, propIDs[m]}] = matIdx;
                materialInfos.push_back(info);
            }

            lib3mf_resourceiterator_movenext(baseMaterialIterator.as<Lib3MF_ResourceIterator>(), &hasBMNext);
        }
    }

    if (materialInfos.empty()) {
        MaterialInfo defaultMat;
        defaultMat.resourceID = 0;
        defaultMat.propertyID = 0;
        defaultMat.name = "DefaultMaterial";
        defaultMat.color = {200, 200, 200, 255};
        materialInfos.push_back(defaultMat);
    }

    pScene->mNumMaterials = static_cast<unsigned int>(materialInfos.size());
    pScene->mMaterials = new aiMaterial *[pScene->mNumMaterials];
    for (unsigned int i = 0; i < pScene->mNumMaterials; ++i) {
        aiMaterial *mat = new aiMaterial();
        aiString name(materialInfos[i].name);
        mat->AddProperty(&name, AI_MATKEY_NAME);

        aiColor4D diffuse = lib3MFColorToAi(materialInfos[i].color);
        mat->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);

        pScene->mMaterials[i] = mat;
    }

    // Second pass: build aiMeshes
    Lib3MFHandle meshIterator2;
    lib3mf_model_getmeshobjects(model.as<Lib3MF_Model>(), meshIterator2.ptr());

    pScene->mNumMeshes = static_cast<unsigned int>(meshDataList.size());
    pScene->mMeshes = new aiMesh *[pScene->mNumMeshes];

    unsigned int meshIndex = 0;
    bool hasNext2 = false;
    lib3mf_resourceiterator_movenext(meshIterator2.as<Lib3MF_ResourceIterator>(), &hasNext2);
    while (hasNext2 && meshIndex < pScene->mNumMeshes) {
        Lib3MFHandle currentMesh;
        lib3mf_meshobjectiterator_getcurrentmeshobject(
            meshIterator2.as<Lib3MF_MeshObjectIterator>(), currentMesh.ptr()
        );

        Lib3MF_MeshObject meshObj = currentMesh.as<Lib3MF_MeshObject>();
        Lib3MF_uint32 vertexCount = 0, triangleCount = 0;
        lib3mf_meshobject_getvertexcount(meshObj, &vertexCount);
        lib3mf_meshobject_gettrianglecount(meshObj, &triangleCount);

        aiMesh *aiMeshPtr = new aiMesh();
        aiMeshPtr->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;

        Lib3MF_uint32 nameChars = 0;
        lib3mf_object_getname(meshObj, 0, &nameChars, nullptr);
        if (nameChars > 0) {
            std::vector<char> nameBuf(nameChars + 1, '\0');
            lib3mf_object_getname(meshObj, nameChars + 1, &nameChars, nameBuf.data());
            aiMeshPtr->mName = aiString(nameBuf.data());
        }

        aiMeshPtr->mNumVertices = vertexCount;
        aiMeshPtr->mVertices = new aiVector3D[vertexCount];
        for (Lib3MF_uint32 v = 0; v < vertexCount; ++v) {
            Lib3MF::sPosition pos;
            lib3mf_meshobject_getvertex(meshObj, v, &pos);
            aiMeshPtr->mVertices[v].x = pos.m_Coordinates[0];
            aiMeshPtr->mVertices[v].y = pos.m_Coordinates[1];
            aiMeshPtr->mVertices[v].z = pos.m_Coordinates[2];
        }

        aiMeshPtr->mNumFaces = triangleCount;
        aiMeshPtr->mFaces = new aiFace[triangleCount];
        for (Lib3MF_uint32 f = 0; f < triangleCount; ++f) {
            Lib3MF::sTriangle tri;
            lib3mf_meshobject_gettriangle(meshObj, f, &tri);
            aiMeshPtr->mFaces[f].mNumIndices = 3;
            aiMeshPtr->mFaces[f].mIndices = new unsigned int[3];
            aiMeshPtr->mFaces[f].mIndices[0] = tri.m_Indices[0];
            aiMeshPtr->mFaces[f].mIndices[1] = tri.m_Indices[1];
            aiMeshPtr->mFaces[f].mIndices[2] = tri.m_Indices[2];
        }

        unsigned int assignedMaterial = 0;
        if (triangleCount > 0) {
            Lib3MF::sTriangleProperties triProp;
            Lib3MFResult propResult = lib3mf_meshobject_gettriangleproperties(meshObj, 0, &triProp);
            if (propResult == LIB3MF_SUCCESS && triProp.m_ResourceID != 0) {
                auto it = materialMap.find({triProp.m_ResourceID, triProp.m_PropertyIDs[0]});
                if (it != materialMap.end()) {
                    assignedMaterial = it->second;
                }
            }
        }
        aiMeshPtr->mMaterialIndex = assignedMaterial;

        pScene->mMeshes[meshIndex] = aiMeshPtr;
        ++meshIndex;

        lib3mf_resourceiterator_movenext(meshIterator2.as<Lib3MF_ResourceIterator>(), &hasNext2);
    }

    // Collect build item transforms
    Lib3MFHandle buildItemIterator;
    lib3mf_model_getbuilditems(model.as<Lib3MF_Model>(), buildItemIterator.ptr());

    struct BuildItemInfo {
        bool hasTransform;
        Lib3MF::sTransform transform;
    };
    std::vector<BuildItemInfo> buildItems;

    if (buildItemIterator.get()) {
        bool hasBINext = false;
        lib3mf_builditemiterator_movenext(
            buildItemIterator.as<Lib3MF_BuildItemIterator>(), &hasBINext
        );
        while (hasBINext) {
            Lib3MFHandle buildItem;
            lib3mf_builditemiterator_getcurrent(
                buildItemIterator.as<Lib3MF_BuildItemIterator>(), buildItem.ptr()
            );

            BuildItemInfo info;
            info.hasTransform = false;
            lib3mf_builditem_hasobjecttransform(
                buildItem.as<Lib3MF_BuildItem>(), &info.hasTransform
            );
            if (info.hasTransform) {
                lib3mf_builditem_getobjecttransform(
                    buildItem.as<Lib3MF_BuildItem>(), &info.transform
                );
            }
            buildItems.push_back(info);

            lib3mf_builditemiterator_movenext(
                buildItemIterator.as<Lib3MF_BuildItemIterator>(), &hasBINext
            );
        }
    }

    pScene->mRootNode = new aiNode();
    pScene->mRootNode->mName = aiString("3MF_Root");

    if (pScene->mNumMeshes > 0) {
        pScene->mRootNode->mNumChildren = pScene->mNumMeshes;
        pScene->mRootNode->mChildren = new aiNode *[pScene->mNumMeshes];
        std::memset(pScene->mRootNode->mChildren, 0, sizeof(aiNode*) * pScene->mNumMeshes);

        for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
            aiNode *child = new aiNode();
            child->mName = aiString("Mesh_" + std::to_string(i));
            child->mParent = pScene->mRootNode;
            child->mNumMeshes = 1;
            child->mMeshes = new unsigned int[1];
            child->mMeshes[0] = i;

            if (i < buildItems.size() && buildItems[i].hasTransform) {
                child->mTransformation = lib3MFTransformToAiMatrix(buildItems[i].transform);
            }

            pScene->mRootNode->mChildren[i] = child;
        }
    }
}

} // anonymous namespace

// ===== PUBLIC API =====

void Lib3MFBridge::ExportScene(const aiScene *pScene, const std::string &pFile, IOSystem *pIOSystem) {
    if (!pScene) {
        throw DeadlyExportError("lib3mf export: null scene");
    }

    std::vector<Lib3MF_uint8> buffer;
    exportToLib3MF(pScene, buffer);

    std::unique_ptr<IOStream> outfile(pIOSystem->Open(pFile, "wb"));
    if (!outfile) {
        throw DeadlyExportError("lib3mf export: could not open output file: " + pFile);
    }
    outfile->Write(buffer.data(), 1, buffer.size());
}

void Lib3MFBridge::ImportScene(aiScene *pScene, const std::string &pFile, IOSystem *pIOSystem) {
    if (!pScene) {
        throw DeadlyImportError("lib3mf import: null scene");
    }

    std::unique_ptr<IOStream> infile(pIOSystem->Open(pFile, "rb"));
    if (!infile) {
        throw DeadlyImportError("lib3mf import: could not open file: " + pFile);
    }

    size_t fileSize = infile->FileSize();
    std::vector<Lib3MF_uint8> buffer(fileSize);
    infile->Read(buffer.data(), 1, fileSize);

    importFromLib3MF(pScene, buffer);
}

} // namespace D3MF
} // namespace Assimp

#endif // ASSIMP_USE_LIB3MF
