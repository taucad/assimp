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

class utXImporterExporter : public AbstractImportExportBase {
public:
    virtual bool importerTest() {
        Assimp::Importer importer;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X/test.x", aiProcess_ValidateDataStructure);
        return nullptr != scene;
    }
};

TEST_F(utXImporterExporter, importXFromFileTest) {
    EXPECT_TRUE(importerTest());
}

TEST_F(utXImporterExporter, heap_overflow_in_tokenizer) {
    Assimp::Importer importer;
    EXPECT_NO_THROW(importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X/OV_GetNextToken", 0));
}

TEST(utXImporter, importAnimTest) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X/anim_test.x", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXImporter, importBCNEpileptic) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X/BCN_Epileptic.X", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXImporter, importFromTrueSpaceBin32) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X/fromtruespace_bin32.x", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXImporter, import_kwxport_test_cubewithvcolors) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X/kwxport_test_cubewithvcolors.x", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXImporter, importTestCubeBinary) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X/test_cube_binary.x", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXImporter, importTestCubeCompressed) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X/test_cube_compressed.x", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXImporter, importTestCubeText) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X/test_cube_text.x", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXImporter, importTestWuson) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X/Testwuson.X", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXImporter, TestFormatDetection) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X/TestFormatDetection", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXImporter, importDwarf) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_NONBSD_DIR "/X/dwarf.x", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
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

constexpr const char *kXFixture = ASSIMP_TEST_MODELS_DIR "/X/test.x";

} // namespace

TEST(utXImporter, contractDefaultsAreMetersAndYUp) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(kXFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_DOUBLE_EQ(1.0, readUnitScale(scene));
    EXPECT_EQ(1, readUpAxis(scene));
}

TEST(utXImporter, contractUnitScaleOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_IMPORT_X_UNIT_SCALE_TO_METERS, 0.0254f);
    const aiScene *scene = importer.ReadFile(kXFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_NEAR(0.0254, readUnitScale(scene), 1e-6);
}

TEST(utXImporter, contractUpAxisOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_X_UP_AXIS, 2);
    const aiScene *scene = importer.ReadFile(kXFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_EQ(2, readUpAxis(scene));
}

TEST(utXImporter, contractInvalidUpAxisOverrideFailsImport) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_X_UP_AXIS, 7);
    const aiScene *scene = importer.ReadFile(kXFixture, 0);
    EXPECT_EQ(nullptr, scene);
    const std::string errorString = importer.GetErrorString();
    EXPECT_NE(std::string::npos, errorString.find("IMPORT_X_UP_AXIS"));
}

namespace {

aiScene *makeXBoxScene(const aiVector3D &halfExtents) {
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
    scene->mRootNode->mNumMeshes = 1;
    scene->mRootNode->mMeshes = new unsigned int[1];
    scene->mRootNode->mMeshes[0] = 0;
    return scene;
}

aiVector3D meshExtentX(const aiMesh *mesh) {
    aiVector3D mn(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max());
    aiVector3D mx(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                  std::numeric_limits<float>::lowest());
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        mn.x = std::min(mn.x, mesh->mVertices[i].x);
        mn.y = std::min(mn.y, mesh->mVertices[i].y);
        mn.z = std::min(mn.z, mesh->mVertices[i].z);
        mx.x = std::max(mx.x, mesh->mVertices[i].x);
        mx.y = std::max(mx.y, mesh->mVertices[i].y);
        mx.z = std::max(mx.z, mesh->mVertices[i].z);
    }
    return aiVector3D(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
}

} // namespace

TEST(utXImporter, exportXBakesAxisRotationWhenSourceIsZUp) {
    // Tall box on +Z (Z-up source) -> after Z->Y bake the tall axis must be
    // +Y on re-import (XFile canonical Y-up).
    aiScene *scene = makeXBoxScene(aiVector3D(1.0f, 1.0f, 5.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0); // metres
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "x", "ut_xfile_axis_zup.x"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_xfile_axis_zup.x", 0);
    ASSERT_NE(nullptr, roundtrip) << importer2.GetErrorString();
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentX(roundtrip->mMeshes[0]);
    EXPECT_NEAR(2.0f, extent.x, 1e-3f);
    EXPECT_NEAR(10.0f, extent.y, 1e-3f);
    EXPECT_NEAR(2.0f, extent.z, 1e-3f);
    std::remove("ut_xfile_axis_zup.x");
}

TEST(utXImporter, exportXIsIdentityWhenSourceAlreadyMetersYUp) {
    // Source frame matches XFile canonical (m + Y-up) -> identity short-circuit.
    aiScene *scene = makeXBoxScene(aiVector3D(2.5f, 7.5f, 2.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "x", "ut_xfile_identity.x"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_xfile_identity.x", 0);
    ASSERT_NE(nullptr, roundtrip) << importer2.GetErrorString();
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentX(roundtrip->mMeshes[0]);
    EXPECT_NEAR(5.0f, extent.x, 1e-4f);
    EXPECT_NEAR(15.0f, extent.y, 1e-4f);
    EXPECT_NEAR(5.0f, extent.z, 1e-4f);
    std::remove("ut_xfile_identity.x");
}

TEST(utXImporter, exportXIsIdentityWhenContractMetadataAbsent) {
    aiScene *scene = makeXBoxScene(aiVector3D(3.0f, 3.0f, 3.0f));
    ASSERT_EQ(nullptr, scene->mMetaData);

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "x", "ut_xfile_no_meta.x"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_xfile_no_meta.x", 0);
    ASSERT_NE(nullptr, roundtrip) << importer2.GetErrorString();
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentX(roundtrip->mMeshes[0]);
    EXPECT_NEAR(6.0f, extent.x, 1e-4f);
    EXPECT_NEAR(6.0f, extent.y, 1e-4f);
    EXPECT_NEAR(6.0f, extent.z, 1e-4f);
    std::remove("ut_xfile_no_meta.x");
}
