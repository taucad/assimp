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

class ut3DSImportExport : public AbstractImportExportBase {
public:
    bool importerTest() override {
        Assimp::Importer importer;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3DS/fels.3ds", aiProcess_ValidateDataStructure);
#ifndef ASSIMP_BUILD_NO_3DS_IMPORTER
        return nullptr != scene;
#else
        return nullptr == scene;
#endif // ASSIMP_BUILD_NO_3DS_IMPORTER
    }
};

TEST_F(ut3DSImportExport, import3DSFromFileTest) {
    EXPECT_TRUE(importerTest());
}

TEST_F(ut3DSImportExport, import3DSformatdetection) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3DS/testFormatDetection", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importCameraRollAnim) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3DS/CameraRollAnim.3ds", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importCameraRollAnimWithChildObject) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3DS/CameraRollAnimWithChildObject.3ds", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importCubesWithAlpha) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3DS/cubes_with_alpha.3DS", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importCubeWithDiffuseTexture) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3DS/cube_with_diffuse_texture.3DS", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importCubeWithSpecularTexture) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3DS/cube_with_specular_texture.3DS", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importRotatingCube) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3DS/RotatingCube.3DS", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importTargetCameraAnim) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3DS/TargetCameraAnim.3ds", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importTest1) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3DS/test1.3ds", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importCartWheel) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_NONBSD_DIR "/3DS/cart_wheel.3DS", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importGranate) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_NONBSD_DIR "/3DS/Granate.3DS", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importJeep1) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_NONBSD_DIR "/3DS/jeep1.3ds", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importMarRifle) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_NONBSD_DIR "/3DS/mar_rifle.3ds", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importMp5Sil) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_NONBSD_DIR "/3DS/mp5_sil.3ds", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}


TEST_F(ut3DSImportExport, importPyramob) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_NONBSD_DIR "/3DS/pyramob.3DS", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
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

constexpr const char *k3dsFixture = ASSIMP_TEST_MODELS_DIR "/3DS/fels.3ds";

} // namespace

TEST_F(ut3DSImportExport, contractDefaultsAreMetersAndZUp) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(k3dsFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_DOUBLE_EQ(1.0, readUnitScale(scene));
    EXPECT_EQ(2, readUpAxis(scene));
}

TEST_F(ut3DSImportExport, contractUnitScaleOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_IMPORT_3DS_UNIT_SCALE_TO_METERS, 0.0254f);
    const aiScene *scene = importer.ReadFile(k3dsFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_NEAR(0.0254, readUnitScale(scene), 1e-6);
}

TEST_F(ut3DSImportExport, contractUpAxisOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_3DS_UP_AXIS, 1);
    const aiScene *scene = importer.ReadFile(k3dsFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_EQ(1, readUpAxis(scene));
}

TEST_F(ut3DSImportExport, contractInvalidUpAxisOverrideFailsImport) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_3DS_UP_AXIS, 7);
    const aiScene *scene = importer.ReadFile(k3dsFixture, 0);
    EXPECT_EQ(nullptr, scene);
    const std::string errorString = importer.GetErrorString();
    EXPECT_NE(std::string::npos, errorString.find("IMPORT_3DS_UP_AXIS"));
}

namespace {

aiScene *make3dsBoxScene(const aiVector3D &halfExtents) {
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

aiVector3D sceneExtent3ds(const aiScene *scene) {
    // 3DS write/import path may split the source mesh; aggregate extents
    // across every output mesh so the assertion still references the same
    // physical bounding box.
    aiVector3D mn(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max());
    aiVector3D mx(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                  std::numeric_limits<float>::lowest());
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh *mesh = scene->mMeshes[m];
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            mn.x = std::min(mn.x, mesh->mVertices[i].x);
            mn.y = std::min(mn.y, mesh->mVertices[i].y);
            mn.z = std::min(mn.z, mesh->mVertices[i].z);
            mx.x = std::max(mx.x, mesh->mVertices[i].x);
            mx.y = std::max(mx.y, mesh->mVertices[i].y);
            mx.z = std::max(mx.z, mesh->mVertices[i].z);
        }
    }
    return aiVector3D(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
}

} // namespace

TEST_F(ut3DSImportExport, export3DSBakesAxisRotationWhenSourceIsYUp) {
    // Tall box on +Y (Y-up source), extents 2 x 10 x 2 m. After Y->Z bake
    // the tall axis must move from +Y to +Z on re-import (3DS canonical Z-up).
    aiScene *scene = make3dsBoxScene(aiVector3D(1.0f, 5.0f, 1.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0); // metres
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3ds", "ut_3ds_axis_yup.3ds"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3ds_axis_yup.3ds", 0);
    ASSERT_NE(nullptr, roundtrip) << importer2.GetErrorString();
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = sceneExtent3ds(roundtrip);
    EXPECT_NEAR(2.0f, extent.x, 1e-3f);
    EXPECT_NEAR(2.0f, extent.y, 1e-3f);
    EXPECT_NEAR(10.0f, extent.z, 1e-3f);
    std::remove("ut_3ds_axis_yup.3ds");
}

TEST_F(ut3DSImportExport, export3DSIsIdentityWhenSourceAlreadyMetersZUp) {
    // Source frame matches 3DS canonical (m + Z-up) -> identity short-circuit;
    // round-trip extents must reproduce the source 1:1.
    aiScene *scene = make3dsBoxScene(aiVector3D(2.5f, 7.5f, 2.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3ds", "ut_3ds_identity.3ds"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3ds_identity.3ds", 0);
    ASSERT_NE(nullptr, roundtrip) << importer2.GetErrorString();
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = sceneExtent3ds(roundtrip);
    EXPECT_NEAR(5.0f, extent.x, 1e-4f);
    EXPECT_NEAR(15.0f, extent.y, 1e-4f);
    EXPECT_NEAR(5.0f, extent.z, 1e-4f);
    std::remove("ut_3ds_identity.3ds");
}

TEST_F(ut3DSImportExport, export3DSIsIdentityWhenContractMetadataAbsent) {
    // No contract metadata -> exporter must produce the same byte layout as
    // pre-contract callers (no rescale, no rotation).
    aiScene *scene = make3dsBoxScene(aiVector3D(3.0f, 3.0f, 3.0f));
    ASSERT_EQ(nullptr, scene->mMetaData);

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3ds", "ut_3ds_no_meta.3ds"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3ds_no_meta.3ds", 0);
    ASSERT_NE(nullptr, roundtrip) << importer2.GetErrorString();
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = sceneExtent3ds(roundtrip);
    EXPECT_NEAR(6.0f, extent.x, 1e-4f);
    EXPECT_NEAR(6.0f, extent.y, 1e-4f);
    EXPECT_NEAR(6.0f, extent.z, 1e-4f);
    std::remove("ut_3ds_no_meta.3ds");
}
