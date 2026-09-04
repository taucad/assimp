/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2025, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the following
conditions are met:

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
---------------------------------------------------------------------------
*/
#include "AbstractImportExportBase.h"
#include "UnitTestPCH.h"

#include <assimp/ColladaMetaData.h>
#include <assimp/SceneCombiner.h>
#include <assimp/commonMetaData.h>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/mesh.h>
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace Assimp;

class utColladaImportExport : public AbstractImportExportBase {
public:
    // Clones the scene in an exception-safe way
    struct SceneCloner {
        SceneCloner(const aiScene *scene) {
            sceneCopy = nullptr;
            SceneCombiner::CopyScene(&sceneCopy, scene);
        }

        ~SceneCloner() {
            delete sceneCopy;
            sceneCopy = nullptr;
        }
        aiScene *sceneCopy;
    };

    virtual bool importerTest() final {
        Assimp::Importer importer;
        {
          const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/Collada/duck.dae", aiProcess_ValidateDataStructure);
          if (scene == nullptr)
              return false;

          // Expected number of items
          EXPECT_EQ(scene->mNumMeshes, 1u);
          EXPECT_EQ(scene->mNumMaterials, 1u);
          EXPECT_EQ(scene->mNumAnimations, 0u);
          EXPECT_EQ(scene->mNumTextures, 0u);
          EXPECT_EQ(scene->mNumLights, 1u);
          EXPECT_EQ(scene->mNumCameras, 1u);

          // Expected common metadata
          aiString value;
          EXPECT_TRUE(scene->mMetaData->Get(AI_METADATA_SOURCE_FORMAT, value)) << "No importer format metadata";
          EXPECT_STREQ("Collada Importer", value.C_Str());

          EXPECT_TRUE(scene->mMetaData->Get(AI_METADATA_SOURCE_FORMAT_VERSION, value)) << "No format version metadata";
          EXPECT_STREQ("1.4.1", value.C_Str());

          EXPECT_TRUE(scene->mMetaData->Get(AI_METADATA_SOURCE_GENERATOR, value)) << "No generator metadata";
          EXPECT_EQ(strncmp(value.C_Str(), "Maya 8.0", 8), 0) << "AI_METADATA_SOURCE_GENERATOR was: " << value.C_Str();

          EXPECT_TRUE(scene->mMetaData->Get(AI_METADATA_SOURCE_COPYRIGHT, value)) << "No copyright metadata";
          EXPECT_EQ(strncmp(value.C_Str(), "Copyright 2006", 14), 0) << "AI_METADATA_SOURCE_COPYRIGHT was: " << value.C_Str();
        }

        {
          const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/Collada/box_nested_animation.dae", aiProcess_ValidateDataStructure);
          if (scene == nullptr)
              return false;

          // Expect only one animation with the correct name
          EXPECT_EQ(scene->mNumAnimations, 1u);
          EXPECT_EQ(std::string(scene->mAnimations[0]->mName.C_Str()), std::string("Armature"));

        }

        return true;
    }

    typedef std::pair<std::string, std::string> IdNameString;
    typedef std::map<std::string, std::string> IdNameMap;

    template <typename T>
    static inline IdNameString GetColladaIdName(const T *item, size_t index) {
        std::ostringstream stream;
        stream << typeid(T).name() << "@" << index;
        if (item->mMetaData) {
            aiString aiStr;
            if (item->mMetaData->Get(AI_METADATA_COLLADA_ID, aiStr))
                return std::make_pair(std::string(aiStr.C_Str()), stream.str());
        }
        return std::make_pair(std::string(), stream.str());
    }

    template <typename T>
    static inline IdNameString GetItemIdName(const T *item, size_t index) {
        std::ostringstream stream;
        stream << typeid(T).name() << "@" << index;
        return std::make_pair(std::string(item->mName.C_Str()), stream.str());
    }

    // Specialisations
    static inline IdNameString GetItemIdName(aiMaterial *item, size_t index) {
        std::ostringstream stream;
        stream << typeid(aiMaterial).name() << "@" << index;
        return std::make_pair(std::string(item->GetName().C_Str()), stream.str());
    }

    static inline IdNameString GetItemIdName(aiTexture *item, size_t index) {
        std::ostringstream stream;
        stream << typeid(aiTexture).name() << "@" << index;
        return std::make_pair(std::string(item->mFilename.C_Str()), stream.str());
    }

    static inline void ReportDuplicate(IdNameMap &itemIdMap, const IdNameString &namePair, const char *typeNameStr) {
        const auto result = itemIdMap.insert(namePair);
        EXPECT_TRUE(result.second) << "Duplicate '" << typeNameStr << "' name: '" << namePair.first << "'. " << namePair.second << " == " << result.first->second;
    }

    template <typename T>
    static inline void CheckUniqueIds(IdNameMap &itemIdMap, unsigned int itemCount, T **itemArray) {
        for (size_t idx = 0; idx < itemCount; ++idx) {
            IdNameString namePair = GetItemIdName(itemArray[idx], idx);
            ReportDuplicate(itemIdMap, namePair, typeid(T).name());
        }
    }

    static inline void CheckUniqueIds(IdNameMap &itemIdMap, const aiNode *parent, size_t index) {
        IdNameString namePair = GetItemIdName(parent, index);
        ReportDuplicate(itemIdMap, namePair, typeid(aiNode).name());

        for (size_t idx = 0; idx < parent->mNumChildren; ++idx) {
            CheckUniqueIds(itemIdMap, parent->mChildren[idx], idx);
        }
    }

    static inline void CheckNodeIdNames(IdNameMap &nodeIdMap, IdNameMap &nodeNameMap, const aiNode *parent, size_t index) {
        IdNameString namePair = GetItemIdName(parent, index);
        IdNameString idPair = GetColladaIdName(parent, index);
        ReportDuplicate(nodeIdMap, idPair, typeid(aiNode).name());

        for (size_t idx = 0; idx < parent->mNumChildren; ++idx) {
            CheckNodeIdNames(nodeIdMap, nodeNameMap, parent->mChildren[idx], idx);
        }
    }

    static inline void SetAllNodeNames(const aiString &newName, aiNode *node) {
        node->mName = newName;
        for (size_t idx = 0; idx < node->mNumChildren; ++idx) {
            SetAllNodeNames(newName, node->mChildren[idx]);
        }
    }

    void ImportAndCheckIds(const char *file, const aiScene *origScene) {
        // Import the Collada using the 'default' where aiNode and aiMesh names are the Collada ids
        Assimp::Importer importer;
        const aiScene *scene = importer.ReadFile(file, aiProcess_ValidateDataStructure);
        ASSERT_TRUE(scene != nullptr) << "Fatal: could not re-import " << file;
        EXPECT_EQ(origScene->mNumMeshes, scene->mNumMeshes) << "in " << file;

        // Check the ids are unique
        IdNameMap itemIdMap;
        // Recurse the Nodes
        CheckUniqueIds(itemIdMap, scene->mRootNode, 0);
        // Check the lists
        CheckUniqueIds(itemIdMap, scene->mNumMeshes, scene->mMeshes);
        // The remaining will come in using the name, which may not be unique
        // Check we have the right number
        EXPECT_EQ(origScene->mNumAnimations, scene->mNumAnimations);
        EXPECT_EQ(origScene->mNumMaterials, scene->mNumMaterials);
        EXPECT_EQ(origScene->mNumTextures, scene->mNumTextures);
        EXPECT_EQ(origScene->mNumLights, scene->mNumLights);
        EXPECT_EQ(origScene->mNumCameras, scene->mNumCameras);
    }

    void ImportAsNames(const char *file, const aiScene *origScene) {
        // Import the Collada but using the user-visible names for aiNode and aiMesh
        // Note that this mode may not support bones or animations
        Assimp::Importer importer;
        importer.SetPropertyInteger(AI_CONFIG_IMPORT_COLLADA_USE_COLLADA_NAMES, 1);

        const aiScene *scene = importer.ReadFile(file, aiProcess_ValidateDataStructure);
        ASSERT_TRUE(scene != nullptr) << "Fatal: could not re-import " << file;
        EXPECT_EQ(origScene->mNumMeshes, scene->mNumMeshes) << "in " << file;

        // Check the node ids are unique but the node names are not
        IdNameMap nodeIdMap;
        IdNameMap nodeNameMap;
        // Recurse the Nodes
        CheckNodeIdNames(nodeIdMap, nodeNameMap, scene->mRootNode, 0);

        // nodeNameMap should have fewer than nodeIdMap
        EXPECT_LT(nodeNameMap.size(), nodeIdMap.size()) << "Some nodes should have the same names";
        // Check the counts haven't changed
        EXPECT_EQ(origScene->mNumAnimations, scene->mNumAnimations);
        EXPECT_EQ(origScene->mNumMaterials, scene->mNumMaterials);
        EXPECT_EQ(origScene->mNumTextures, scene->mNumTextures);
        EXPECT_EQ(origScene->mNumLights, scene->mNumLights);
        EXPECT_EQ(origScene->mNumCameras, scene->mNumCameras);
    }
};

TEST_F(utColladaImportExport, importDaeFromFileTest) {
    EXPECT_TRUE(importerTest());
}

unsigned int GetMeshUseCount(const aiNode *rootNode) {
    unsigned int result = rootNode->mNumMeshes;
    for (unsigned int i = 0; i < rootNode->mNumChildren; ++i) {
        result += GetMeshUseCount(rootNode->mChildren[i]);
    }
    return result;
}

#ifndef ASSIMP_BUILD_NO_EXPORT

TEST_F(utColladaImportExport, exportRootNodeMeshTest) {
    Assimp::Importer importer;
    Assimp::Exporter exporter;
    const char *outFile = "exportRootNodeMeshTest_out.dae";

    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/Collada/duck.dae", aiProcess_ValidateDataStructure);
    ASSERT_TRUE(scene != nullptr) << "Fatal: could not import duck.dae!";

    ASSERT_EQ(0u, scene->mRootNode->mNumMeshes) << "Collada import should not give the root node a mesh";

    {
        SceneCloner clone(scene);
        ASSERT_TRUE(clone.sceneCopy != nullptr) << "Fatal: could not copy scene!";
        // Do this by moving the meshes from the first child that has some
        aiNode *rootNode = clone.sceneCopy->mRootNode;
        ASSERT_TRUE(rootNode->mNumChildren > 0) << "Fatal: root has no children";
        aiNode *meshNode = rootNode->mChildren[0];
        ASSERT_EQ(1u, meshNode->mNumMeshes) << "Fatal: First child node has no duck mesh";

        // Move the meshes to the parent
        rootNode->mNumMeshes = meshNode->mNumMeshes;
        rootNode->mMeshes = new unsigned int[rootNode->mNumMeshes];
        for (unsigned int i = 0; i < rootNode->mNumMeshes; ++i) {
            rootNode->mMeshes[i] = meshNode->mMeshes[i];
        }

        // Remove the meshes from the original node
        meshNode->mNumMeshes = 0;
        delete[] meshNode->mMeshes;
        meshNode->mMeshes = nullptr;

        ASSERT_EQ(AI_SUCCESS, exporter.Export(clone.sceneCopy, "collada", outFile)) << "Fatal: Could not export file";
    }

    // Reimport and look for meshes
    scene = importer.ReadFile(outFile, aiProcess_ValidateDataStructure);
    ASSERT_TRUE(scene != nullptr) << "Fatal: could not reimport!";

    // A Collada root node is not allowed to have a mesh
    ASSERT_EQ(0u, scene->mRootNode->mNumMeshes) << "Collada reimport should not give the root node a mesh";

    // Walk nodes and counts used meshes
    // Should be exactly one
    EXPECT_EQ(1u, GetMeshUseCount(scene->mRootNode)) << "Nodes had unexpected number of meshes in use";
}

TEST_F(utColladaImportExport, exporterUniqueIdsTest) {
    Assimp::Importer importer;
    Assimp::Exporter exporter;
    const char *outFileEmpty = "exportMeshIdTest_empty_out.dae";
    const char *outFileNamed = "exportMeshIdTest_named_out.dae";

    // Load a sample file containing multiple meshes
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/Collada/teapots.DAE", aiProcess_ValidateDataStructure);

    ASSERT_TRUE(scene != nullptr) << "Fatal: could not import teapots.DAE!";
    ASSERT_EQ(3u, scene->mNumMeshes) << "Fatal: teapots.DAE initial load failed";

    // Clear all the names
    for (size_t idx = 0; idx < scene->mNumMeshes; ++idx) {
        scene->mMeshes[idx]->mName.Clear();
    }
    for (size_t idx = 0; idx < scene->mNumMaterials; ++idx) {
        scene->mMaterials[idx]->RemoveProperty(AI_MATKEY_NAME);
    }
    for (size_t idx = 0; idx < scene->mNumAnimations; ++idx) {
        scene->mAnimations[idx]->mName.Clear();
    }
    // Can't clear texture names
    for (size_t idx = 0; idx < scene->mNumLights; ++idx) {
        scene->mLights[idx]->mName.Clear();
    }
    for (size_t idx = 0; idx < scene->mNumCameras; ++idx) {
        scene->mCameras[idx]->mName.Clear();
    }

    SetAllNodeNames(aiString(), scene->mRootNode);

    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "collada", outFileEmpty)) << "Fatal: Could not export un-named meshes file";

    ImportAndCheckIds(outFileEmpty, scene);

    // Force everything to have the same non-empty name
    aiString testName("test_name");
    for (size_t idx = 0; idx < scene->mNumMeshes; ++idx) {
        scene->mMeshes[idx]->mName = testName;
    }
    for (size_t idx = 0; idx < scene->mNumMaterials; ++idx) {
        scene->mMaterials[idx]->AddProperty(&testName, AI_MATKEY_NAME);
    }
    for (size_t idx = 0; idx < scene->mNumAnimations; ++idx) {
        scene->mAnimations[idx]->mName = testName;
    }
    // Can't clear texture names
    for (size_t idx = 0; idx < scene->mNumLights; ++idx) {
        scene->mLights[idx]->mName = testName;
    }
    for (size_t idx = 0; idx < scene->mNumCameras; ++idx) {
        scene->mCameras[idx]->mName = testName;
    }

    SetAllNodeNames(testName, scene->mRootNode);

    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "collada", outFileNamed)) << "Fatal: Could not export named meshes file";

    ImportAndCheckIds(outFileNamed, scene);
    ImportAsNames(outFileNamed, scene);
}

// This file is invalid, we just want to ensure that the importer is not crashing
// This was reported as GH#4286. The "count" parameter in "Cube-mesh-positions-array" is too small.
TEST_F(utColladaImportExport, parseInvalid4286) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/invalid/box_nested_animation_4286.dae", 0);
    EXPECT_EQ(nullptr, scene);
}

#endif

class utColladaZaeImportExport : public AbstractImportExportBase {
public:
    virtual bool importerTest() final {
        {
            Assimp::Importer importer;
            const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/Collada/duck.zae", aiProcess_ValidateDataStructure);
            if (scene == nullptr)
                return false;

            // Expected number of items
            EXPECT_EQ(scene->mNumMeshes, 1u);
            EXPECT_EQ(scene->mNumMaterials, 1u);
            EXPECT_EQ(scene->mNumAnimations, 0u);
            //EXPECT_EQ(scene->mNumTextures, 1u);
            EXPECT_EQ(scene->mNumLights, 1u);
            EXPECT_EQ(scene->mNumCameras, 1u);
        }

        {
            Assimp::Importer importer;
            const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/Collada/duck_nomanifest.zae", aiProcess_ValidateDataStructure);
            if (scene == nullptr)
                return false;

            // Expected number of items
            EXPECT_EQ(scene->mNumMeshes, 1u);
            EXPECT_EQ(scene->mNumMaterials, 1u);
            EXPECT_EQ(scene->mNumAnimations, 0u);
            //EXPECT_EQ(scene->mNumTextures, 1u);
            EXPECT_EQ(scene->mNumLights, 1u);
            EXPECT_EQ(scene->mNumCameras, 1u);
        }

        return true;
    }
};

TEST_F(utColladaZaeImportExport, importBlenFromFileTest) {
    EXPECT_TRUE(importerTest());
}

TEST_F(utColladaZaeImportExport, importMakeHumanTest) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/Collada/human.zae", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);

    // Expected number of items
    EXPECT_EQ(scene->mNumMeshes, 2u);
    EXPECT_EQ(scene->mNumMaterials, 2u);
    EXPECT_EQ(scene->mNumAnimations, 0u);
    EXPECT_EQ(scene->mNumTextures, 2u);
    EXPECT_EQ(scene->mNumLights, 0u);
    EXPECT_EQ(scene->mNumCameras, 0u);

    // Expected common metadata
    aiString value;
    EXPECT_TRUE(scene->mMetaData->Get(AI_METADATA_SOURCE_FORMAT, value)) << "No importer format metadata";
    EXPECT_STREQ("Collada Importer", value.C_Str());

    EXPECT_TRUE(scene->mMetaData->Get(AI_METADATA_SOURCE_FORMAT_VERSION, value)) << "No format version metadata";
    EXPECT_STREQ("1.4.1", value.C_Str());
}

namespace {

double readUnitScale(const aiScene *scene) {
    double value = 0.0;
    EXPECT_NE(nullptr, scene);
    EXPECT_NE(nullptr, scene->mMetaData);
    EXPECT_TRUE(scene->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, value));
    return value;
}

int32_t readUpAxis(const aiScene *scene) {
    int32_t value = -1;
    EXPECT_NE(nullptr, scene);
    EXPECT_NE(nullptr, scene->mMetaData);
    EXPECT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, value));
    return value;
}

constexpr const char *kFixture = ASSIMP_TEST_MODELS_DIR "/Collada/duck.dae";

} // namespace

TEST_F(utColladaImportExport, contractDefaultsAreMetersAndYUpPostNormalisation) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(kFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_DOUBLE_EQ(1.0, readUnitScale(scene));
    EXPECT_EQ(1, readUpAxis(scene));
}

TEST_F(utColladaImportExport, contractUnitScaleOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_IMPORT_COLLADA_UNIT_SCALE_TO_METERS, 0.001f);
    const aiScene *scene = importer.ReadFile(kFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_NEAR(0.001, readUnitScale(scene), 1e-6);
}

TEST_F(utColladaImportExport, contractUpAxisOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_COLLADA_UP_AXIS, 2);
    const aiScene *scene = importer.ReadFile(kFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_EQ(2, readUpAxis(scene));
}

TEST_F(utColladaImportExport, contractInvalidUpAxisOverrideFailsImport) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_COLLADA_UP_AXIS, 7);
    for (unsigned int i = 0; i < 3; ++i) {
        EXPECT_EQ(nullptr, importer.ReadFile(kFixture, 0));
        EXPECT_NE(std::string::npos, std::string(importer.GetErrorString()).find("IMPORT_COLLADA_UP_AXIS"));
    }

    importer.SetPropertyInteger(AI_CONFIG_IMPORT_COLLADA_UP_AXIS, 1);
    const aiScene *scene = importer.ReadFile(kFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    ASSERT_NE(nullptr, scene->mMeshes[0]);
    EXPECT_EQ(8500u, scene->mMeshes[0]->mNumVertices);
    EXPECT_EQ(2144u, scene->mMeshes[0]->mNumFaces);
}

#ifndef ASSIMP_BUILD_NO_EXPORT

namespace {

aiScene *makeColladaBoxScene(const aiVector3D &halfExtents) {
    const float x = halfExtents.x, y = halfExtents.y, z = halfExtents.z;
    const std::array<aiVector3D, 8> v = {
        aiVector3D(-x, -y, -z), aiVector3D(+x, -y, -z), aiVector3D(+x, +y, -z), aiVector3D(-x, +y, -z),
        aiVector3D(-x, -y, +z), aiVector3D(+x, -y, +z), aiVector3D(+x, +y, +z), aiVector3D(-x, +y, +z)
    };
    const std::array<std::array<unsigned int, 3>, 12> t = { {
        { 0, 2, 1 }, { 0, 3, 2 },
        { 4, 5, 6 }, { 4, 6, 7 },
        { 0, 1, 5 }, { 0, 5, 4 },
        { 3, 7, 6 }, { 3, 6, 2 },
        { 0, 4, 7 }, { 0, 7, 3 },
        { 1, 2, 6 }, { 1, 6, 5 }
    } };

    auto *scene = new aiScene();
    scene->mNumMaterials = 1;
    scene->mMaterials = new aiMaterial *[1];
    scene->mMaterials[0] = new aiMaterial();

    auto *mesh = new aiMesh();
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh->mMaterialIndex = 0;
    mesh->mName = aiString("AuthoredBox");
    mesh->mNumVertices = static_cast<unsigned int>(v.size());
    mesh->mVertices = new aiVector3D[v.size()];
    mesh->mNormals = new aiVector3D[v.size()];
    for (size_t i = 0; i < v.size(); ++i) {
        mesh->mVertices[i] = v[i];
        aiVector3D n = v[i];
        n.Normalize();
        mesh->mNormals[i] = n;
    }
    mesh->mNumFaces = static_cast<unsigned int>(t.size());
    mesh->mFaces = new aiFace[t.size()];
    for (size_t i = 0; i < t.size(); ++i) {
        mesh->mFaces[i].mNumIndices = 3;
        mesh->mFaces[i].mIndices = new unsigned int[3];
        mesh->mFaces[i].mIndices[0] = t[i][0];
        mesh->mFaces[i].mIndices[1] = t[i][1];
        mesh->mFaces[i].mIndices[2] = t[i][2];
    }
    scene->mNumMeshes = 1;
    scene->mMeshes = new aiMesh *[1];
    scene->mMeshes[0] = mesh;

    scene->mRootNode = new aiNode();
    scene->mRootNode->mName = aiString("Root");
    scene->mRootNode->mNumChildren = 1;
    scene->mRootNode->mChildren = new aiNode *[1];
    auto *child = new aiNode();
    child->mName = aiString("Geometry");
    child->mParent = scene->mRootNode;
    child->mNumMeshes = 1;
    child->mMeshes = new unsigned int[1];
    child->mMeshes[0] = 0;
    scene->mRootNode->mChildren[0] = child;
    return scene;
}

aiVector3D meshExtentCollada(const aiMesh *mesh) {
    aiVector3D mn(std::numeric_limits<float>::infinity());
    aiVector3D mx(-std::numeric_limits<float>::infinity());
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        const aiVector3D &v = mesh->mVertices[i];
        mn.x = std::min(mn.x, v.x);
        mn.y = std::min(mn.y, v.y);
        mn.z = std::min(mn.z, v.z);
        mx.x = std::max(mx.x, v.x);
        mx.y = std::max(mx.y, v.y);
        mx.z = std::max(mx.z, v.z);
    }
    return aiVector3D(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
}

} // namespace

// -----------------------------------------------------------------------------
// Unit/axis contract: COLLADA exporter target = 1.0 m + Y-up. The companion
// importer normalises every authored frame to (1.0 m, Y-up) post-import,
// matching the spec defaults (`<unit meter="1"/>` + `<up_axis>Y_UP</up_axis>`);
// aligning the exporter target with the importer default makes same-format
// round-trips bitwise identity. Behaviour gated on
// `AI_METADATA_UNIT_SCALE_TO_METERS` so legacy callers stay byte-identical.
// -----------------------------------------------------------------------------

TEST_F(utColladaImportExport, exportDAEBakesUnitScaleWhenSourceUnitDiffersFromMeters) {
    aiScene *scene = makeColladaBoxScene(aiVector3D(0.5f, 0.5f, 0.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1e-3); // mm
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "collada", "ut_dae_unit_mm.dae"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_dae_unit_mm.dae", 0);
    ASSERT_NE(nullptr, roundtrip) << importer2.GetErrorString();
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentCollada(roundtrip->mMeshes[0]);
    EXPECT_NEAR(1e-3f, extent.x, 1e-6f);
    EXPECT_NEAR(1e-3f, extent.y, 1e-6f);
    EXPECT_NEAR(1e-3f, extent.z, 1e-6f);
    std::remove("ut_dae_unit_mm.dae");
}

TEST_F(utColladaImportExport, exportDAEBakesAxisRotationWhenSourceIsZUp) {
    // Tall box on +Z. After Z->Y bake the tall axis must move to +Y.
    aiScene *scene = makeColladaBoxScene(aiVector3D(1.0f, 1.0f, 5.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "collada", "ut_dae_axis_zup.dae"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_dae_axis_zup.dae", 0);
    ASSERT_NE(nullptr, roundtrip) << importer2.GetErrorString();
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentCollada(roundtrip->mMeshes[0]);
    EXPECT_NEAR(2.0f, extent.x, 1e-4f);
    EXPECT_NEAR(10.0f, extent.y, 1e-4f);
    EXPECT_NEAR(2.0f, extent.z, 1e-4f);
    std::remove("ut_dae_axis_zup.dae");
}

TEST_F(utColladaImportExport, exportDAEIsIdentityWhenSourceAlreadyMetersYUp) {
    aiScene *scene = makeColladaBoxScene(aiVector3D(2.5f, 7.5f, 2.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "collada", "ut_dae_identity.dae"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_dae_identity.dae", 0);
    ASSERT_NE(nullptr, roundtrip) << importer2.GetErrorString();
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentCollada(roundtrip->mMeshes[0]);
    EXPECT_NEAR(5.0f, extent.x, 1e-4f);
    EXPECT_NEAR(15.0f, extent.y, 1e-4f);
    EXPECT_NEAR(5.0f, extent.z, 1e-4f);
    std::remove("ut_dae_identity.dae");
}

TEST_F(utColladaImportExport, exportDAEIsIdentityWhenContractMetadataAbsent) {
    aiScene *scene = makeColladaBoxScene(aiVector3D(3.0f, 3.0f, 3.0f));
    ASSERT_EQ(nullptr, scene->mMetaData);

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "collada", "ut_dae_no_meta.dae"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_dae_no_meta.dae", 0);
    ASSERT_NE(nullptr, roundtrip) << importer2.GetErrorString();
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentCollada(roundtrip->mMeshes[0]);
    EXPECT_NEAR(6.0f, extent.x, 1e-4f);
    EXPECT_NEAR(6.0f, extent.y, 1e-4f);
    EXPECT_NEAR(6.0f, extent.z, 1e-4f);
    std::remove("ut_dae_no_meta.dae");
}

#endif // ASSIMP_BUILD_NO_EXPORT
