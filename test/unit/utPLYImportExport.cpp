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
#include "UnitTestPCH.h"

#include "AbstractImportExportBase.h"
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

using namespace ::Assimp;

namespace {

// Mirrors the STL/3MF exporter test fixtures: axis-aligned box with radial
// normals so we can assert both extent (rescale) and normal direction (rotation)
// after the exporter has baked the contract transform.
aiScene *makePlyBoxScene(const aiVector3D &halfExtents) {
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

aiVector3D meshExtentPly(const aiMesh *mesh) {
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

class utPLYImportExport : public AbstractImportExportBase {
public:
    virtual bool importerTest() {
        Assimp::Importer importer;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/PLY/cube.ply", aiProcess_ValidateDataStructure);
        EXPECT_EQ(1u, scene->mNumMeshes);
        EXPECT_NE(nullptr, scene->mMeshes[0]);
        if (nullptr == scene->mMeshes[0]) {
            return false;
        }
        EXPECT_EQ(8u, scene->mMeshes[0]->mNumVertices);
        EXPECT_EQ(6u, scene->mMeshes[0]->mNumFaces);

        return (nullptr != scene);
    }

#ifndef ASSIMP_BUILD_NO_EXPORT
    virtual bool exporterTest() {
        Importer importer;
        Exporter exporter;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/PLY/cube.ply", aiProcess_ValidateDataStructure);
        EXPECT_NE(nullptr, scene);
        EXPECT_EQ(aiReturn_SUCCESS, exporter.Export(scene, "ply", ASSIMP_TEST_MODELS_DIR "/PLY/cube_out.ply"));

        return true;
    }
#endif // ASSIMP_BUILD_NO_EXPORT
};

TEST_F(utPLYImportExport, importTest_Success) {
    EXPECT_TRUE(importerTest());
}

#ifndef ASSIMP_BUILD_NO_EXPORT

TEST_F(utPLYImportExport, exportTest_Success) {
    EXPECT_TRUE(exporterTest());
}

#endif // ASSIMP_BUILD_NO_EXPORT

// -----------------------------------------------------------------------------
// Unit/axis contract: PLY is unitless and axis-less by spec; defaults are
// 1.0 (m) + Y-up (the neutral baseline matching glTF semantics).
// -----------------------------------------------------------------------------

TEST_F(utPLYImportExport, contractDefaultsAreMetersAndYUp) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(
        ASSIMP_TEST_MODELS_DIR "/PLY/cube.ply", 0);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMetaData);

    double unit = 0.0;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, unit))
        << "PLY importer must declare AI_METADATA_UNIT_SCALE_TO_METERS";
    EXPECT_DOUBLE_EQ(1.0, unit);

    int32_t upAxis = -1;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, upAxis))
        << "PLY importer must declare AI_METADATA_UP_AXIS";
    EXPECT_EQ(1, upAxis);
}

TEST_F(utPLYImportExport, contractUnitScaleOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_IMPORT_PLY_UNIT_SCALE_TO_METERS, 0.001f);
    const aiScene *scene = importer.ReadFile(
        ASSIMP_TEST_MODELS_DIR "/PLY/cube.ply", 0);
    ASSERT_NE(nullptr, scene);
    double unit = 0.0;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, unit));
    EXPECT_NEAR(0.001, unit, 1e-9);
}

TEST_F(utPLYImportExport, contractUpAxisOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_PLY_UP_AXIS, 2);
    const aiScene *scene = importer.ReadFile(
        ASSIMP_TEST_MODELS_DIR "/PLY/cube.ply", 0);
    ASSERT_NE(nullptr, scene);
    int32_t upAxis = -1;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, upAxis));
    EXPECT_EQ(2, upAxis);
}

TEST_F(utPLYImportExport, contractInvalidUpAxisOverrideFailsImport) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_PLY_UP_AXIS, 9);
    const aiScene *scene = importer.ReadFile(
        ASSIMP_TEST_MODELS_DIR "/PLY/cube.ply", 0);
    EXPECT_EQ(nullptr, scene);
    EXPECT_NE(std::string::npos, std::string(importer.GetErrorString()).find("UP_AXIS"));
}

// Test issue 1623, crash when loading two PLY files in a row
TEST_F(utPLYImportExport, importerMultipleTest) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/PLY/cube.ply", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);

    scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/PLY/cube.ply", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
    EXPECT_NE(nullptr, scene->mMeshes[0]);
    EXPECT_EQ(6u, scene->mMeshes[0]->mNumFaces);
}

TEST_F(utPLYImportExport, importPLYwithUV) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/PLY/cube_uv.ply", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
    EXPECT_NE(nullptr, scene->mMeshes[0]);
    // This test model is using n-gons, so 6 faces instead of 12 tris
    EXPECT_EQ(6u, scene->mMeshes[0]->mNumFaces);
    EXPECT_EQ(aiPrimitiveType_POLYGON, scene->mMeshes[0]->mPrimitiveTypes);
    EXPECT_EQ(true, scene->mMeshes[0]->HasTextureCoords(0));
}

TEST_F(utPLYImportExport, importBinaryPLY) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/PLY/cube_binary.ply", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
    EXPECT_NE(nullptr, scene->mMeshes[0]);
    // This test model is double sided, so 12 faces instead of 6
    EXPECT_EQ(12u, scene->mMeshes[0]->mNumFaces);
}

// Tests of a PLY file gets read with \r\n as newlines instead of just \n (i.e. solidwork exported ply files)
TEST_F(utPLYImportExport, importBinaryPLYWithRNNewline) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/PLY/cube_binary_header_with_RN_newline.ply", aiProcess_ValidateDataStructure);

    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMeshes[0]);
    // This test model is double sided, so 12 faces instead of 6
    ASSERT_EQ(12u, scene->mMeshes[0]->mNumFaces);
    // Also check if the indices were parsed correctly
    ASSERT_EQ(3u, scene->mMeshes[0]->mFaces[0].mNumIndices);
    EXPECT_EQ(0u, scene->mMeshes[0]->mFaces[0].mIndices[0]);
    EXPECT_EQ(1u, scene->mMeshes[0]->mFaces[0].mIndices[1]);
    EXPECT_EQ(2u, scene->mMeshes[0]->mFaces[0].mIndices[2]);
}

// Tests of a PLY file gets read with \n as the fist character in the BINARY part
TEST_F(utPLYImportExport, importBinaryPLYWithNewlineInBinary) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/PLY/cube_binary_starts_with_nl.ply", aiProcess_ValidateDataStructure);

    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMeshes[0]);
    ASSERT_EQ(8u, scene->mMeshes[0]->mNumVertices);
    // Make sure the first binary float was read correctly
    ASSERT_FLOAT_EQ(5.967534f, scene->mMeshes[0]->mVertices[0][0]);
    ASSERT_FLOAT_EQ(0, scene->mMeshes[0]->mVertices[0][1]);
    ASSERT_FLOAT_EQ(0, scene->mMeshes[0]->mVertices[0][2]);

    ASSERT_EQ(6u, scene->mMeshes[0]->mNumFaces);
    // Also check if the indices were parsed correctly
    ASSERT_EQ(4u, scene->mMeshes[0]->mFaces[0].mNumIndices);
    EXPECT_EQ(0u, scene->mMeshes[0]->mFaces[0].mIndices[0]);
    EXPECT_EQ(1u, scene->mMeshes[0]->mFaces[0].mIndices[1]);
    EXPECT_EQ(2u, scene->mMeshes[0]->mFaces[0].mIndices[2]);
}

TEST_F(utPLYImportExport, vertexColorTest) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/PLY/float-color.ply", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);
    EXPECT_EQ(1u, scene->mMeshes[0]->mNumFaces);
    EXPECT_EQ(aiPrimitiveType_TRIANGLE, scene->mMeshes[0]->mPrimitiveTypes);
    EXPECT_EQ(true, scene->mMeshes[0]->HasVertexColors(0));

    auto first_face = scene->mMeshes[0]->mFaces[0];
    EXPECT_EQ(3u, first_face.mNumIndices);
    EXPECT_EQ(0u, first_face.mIndices[0]);
    EXPECT_EQ(1u, first_face.mIndices[1]);
    EXPECT_EQ(2u, first_face.mIndices[2]);
}

// Test issue #623, PLY importer should not automatically create faces
TEST_F(utPLYImportExport, pointcloudTest) {
    Assimp::Importer importer;

    // Could not use aiProcess_ValidateDataStructure since it's missing faces.
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/PLY/issue623.ply", 0);
    EXPECT_NE(nullptr, scene);

    EXPECT_EQ(1u, scene->mNumMeshes);
    EXPECT_NE(nullptr, scene->mMeshes[0]);
    EXPECT_EQ(24u, scene->mMeshes[0]->mNumVertices);
    EXPECT_EQ(aiPrimitiveType::aiPrimitiveType_POINT, scene->mMeshes[0]->mPrimitiveTypes);
    EXPECT_EQ(0u, scene->mMeshes[0]->mNumFaces);
}

static const char *test_file =
        "ply\n"
        "format ascii 1.0\n"
        "element vertex 4\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "property uchar red\n"
        "property uchar green\n"
        "property uchar blue\n"
        "property float nx\n"
        "property float ny\n"
        "property float nz\n"
        "end_header\n"
        "0.0 0.0 0.0 255 255 255 0.0 1.0 0.0\n"
        "0.0 0.0 1.0 255 0 255 0.0 0.0 1.0\n"
        "0.0 1.0 0.0 255 255 0 1.0 0.0 0.0\n"
        "0.0 1.0 1.0 0 255 255 1.0 1.0 0.0\n";

TEST_F(utPLYImportExport, parseErrorTest) {
    Assimp::Importer importer;
    // Could not use aiProcess_ValidateDataStructure since it's missing faces.
    const aiScene *scene = importer.ReadFileFromMemory(test_file, strlen(test_file), 0);
    EXPECT_NE(nullptr, scene);
}

// This file is invalid, we just want to ensure that the importer is not crashing
TEST_F(utPLYImportExport, parseInvalid) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/invalid/crash-30d6d0f7c529b3b66b4131700b7a4580cd7082df.ply", 0);
    EXPECT_EQ(nullptr, scene);
}

TEST_F(utPLYImportExport, payload_JVN42386607) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/PLY/payload_JVN42386607", 0);
    EXPECT_EQ(nullptr, scene);
}

// Tests Issue #5729. Test, if properties defined multiple times. Unclear what to do, better to abort than to crash entirely
TEST_F(utPLYImportExport, parseInvalidDoubleProperty) {
    const char data[] = "ply\n"
                        "format ascii 1.0\n"
                        "element vertex 4\n"
                        "property float x\n"
                        "property float y\n"
                        "property float z\n"
                        "element vertex 8\n"
                        "property float x\n"
                        "property float y\n"
                        "property float z\n"
                        "end_header\n"
                        "0.0 0.0 0.0 0.0 0.0 0.0\n"
                        "0.0 0.0 1.0 0.0 0.0 1.0\n"
                        "0.0 1.0 0.0 0.0 1.0 0.0\n"
                        "0.0 0.0 1.0\n"
                        "0.0 1.0 0.0 0.0 0.0 1.0\n"
                        "0.0 1.0 1.0 0.0 1.0 1.0\n";

    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFileFromMemory(data, sizeof(data), 0);
    EXPECT_EQ(nullptr, scene);
}

// Tests Issue #5729. Test, if properties defined multiple times. Unclear what to do, better to abort than to crash entirely
TEST_F(utPLYImportExport, parseInvalidDoubleCustomProperty) {
    const char data[] = "ply\n"
                        "format ascii 1.0\n"
                        "element vertex 4\n"
                        "property float x\n"
                        "property float y\n"
                        "property float z\n"
                        "element name 8\n"
                        "property float x\n"
                        "element name 5\n"
                        "property float x\n"
                        "end_header\n"
                        "0.0 0.0 0.0 100.0 10.0\n"
                        "0.0 0.0 1.0 200.0 20.0\n"
                        "0.0 1.0 0.0 300.0 30.0\n"
                        "0.0 1.0 1.0 400.0 40.0\n"
                        "0.0 0.0 0.0 500.0 50.0\n"
                        "0.0 0.0 1.0 600.0 60.0\n"
                        "0.0 1.0 0.0 700.0 70.0\n"
                        "0.0 1.0 1.0 800.0 80.0\n";

    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFileFromMemory(data, sizeof(data), 0);
    EXPECT_EQ(nullptr, scene);
}

#ifndef ASSIMP_BUILD_NO_EXPORT

// -----------------------------------------------------------------------------
// Unit/axis contract: PLY exporter target = 1.0 m + Y-up. PLY is unitless and
// axis-less by spec, but the importer normalises to (1.0 m, Y-up); aligning
// the exporter target with the importer default makes same-format round-trips
// bitwise identity. Behaviour gated on `AI_METADATA_UNIT_SCALE_TO_METERS` so
// legacy callers stay byte-identical.
// -----------------------------------------------------------------------------

TEST_F(utPLYImportExport, exportPLYBakesUnitScaleWhenSourceUnitDiffersFromMeters) {
    aiScene *scene = makePlyBoxScene(aiVector3D(0.5f, 0.5f, 0.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1e-3); // mm
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "ply", "ut_ply_unit_mm.ply"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_ply_unit_mm.ply", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentPly(roundtrip->mMeshes[0]);
    EXPECT_NEAR(1e-3f, extent.x, 1e-6f);
    EXPECT_NEAR(1e-3f, extent.y, 1e-6f);
    EXPECT_NEAR(1e-3f, extent.z, 1e-6f);
    std::remove("ut_ply_unit_mm.ply");
}

TEST_F(utPLYImportExport, exportPLYBakesAxisRotationWhenSourceIsZUp) {
    // Tall box on +Z (source). After Z->Y bake the tall axis must move to +Y.
    aiScene *scene = makePlyBoxScene(aiVector3D(1.0f, 1.0f, 5.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "ply", "ut_ply_axis_zup.ply"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_ply_axis_zup.ply", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentPly(roundtrip->mMeshes[0]);
    EXPECT_NEAR(2.0f, extent.x, 1e-4f);
    EXPECT_NEAR(10.0f, extent.y, 1e-4f);
    EXPECT_NEAR(2.0f, extent.z, 1e-4f);
    std::remove("ut_ply_axis_zup.ply");
}

TEST_F(utPLYImportExport, exportPLYIsIdentityWhenSourceAlreadyMetersYUp) {
    aiScene *scene = makePlyBoxScene(aiVector3D(2.5f, 7.5f, 2.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "ply", "ut_ply_identity.ply"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_ply_identity.ply", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentPly(roundtrip->mMeshes[0]);
    EXPECT_NEAR(5.0f, extent.x, 1e-5f);
    EXPECT_NEAR(15.0f, extent.y, 1e-5f);
    EXPECT_NEAR(5.0f, extent.z, 1e-5f);
    std::remove("ut_ply_identity.ply");
}

TEST_F(utPLYImportExport, exportPLYIsIdentityWhenContractMetadataAbsent) {
    aiScene *scene = makePlyBoxScene(aiVector3D(3.0f, 3.0f, 3.0f));
    ASSERT_EQ(nullptr, scene->mMetaData);

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "ply", "ut_ply_no_meta.ply"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_ply_no_meta.ply", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentPly(roundtrip->mMeshes[0]);
    EXPECT_NEAR(6.0f, extent.x, 1e-5f);
    EXPECT_NEAR(6.0f, extent.y, 1e-5f);
    EXPECT_NEAR(6.0f, extent.z, 1e-5f);
    std::remove("ut_ply_no_meta.ply");
}

TEST_F(utPLYImportExport, exportPLYBinaryHonorsContractTransform) {
    aiScene *scene = makePlyBoxScene(aiVector3D(1.0f, 1.0f, 5.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "plyb", "ut_ply_axis_zup_bin.ply"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_ply_axis_zup_bin.ply", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentPly(roundtrip->mMeshes[0]);
    EXPECT_NEAR(2.0f, extent.x, 1e-4f);
    EXPECT_NEAR(10.0f, extent.y, 1e-4f);
    EXPECT_NEAR(2.0f, extent.z, 1e-4f);
    std::remove("ut_ply_axis_zup_bin.ply");
}

#endif // ASSIMP_BUILD_NO_EXPORT
