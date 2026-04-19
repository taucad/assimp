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

#include <assimp/Exceptional.h>
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/commonMetaData.h>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

// Author a scene that owns its mesh, material, root node, and (optionally) metadata.
// The returned scene must be passed by raw pointer to Assimp::Exporter::Export and is
// owned by the caller — delete with `delete` once the export completes.
aiScene *makeSingleMeshScene(const std::vector<aiVector3D> &vertices,
                             const std::vector<std::array<unsigned int, 3>> &triangles) {
    auto *scene = new aiScene();

    scene->mNumMaterials = 1;
    scene->mMaterials = new aiMaterial *[1];
    scene->mMaterials[0] = new aiMaterial();

    scene->mNumMeshes = 1;
    scene->mMeshes = new aiMesh *[1];

    auto *mesh = new aiMesh();
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh->mMaterialIndex = 0;
    mesh->mName = aiString("AuthoredMesh");

    mesh->mNumVertices = static_cast<unsigned int>(vertices.size());
    mesh->mVertices = new aiVector3D[vertices.size()];
    for (size_t i = 0; i < vertices.size(); ++i) {
        mesh->mVertices[i] = vertices[i];
    }

    mesh->mNumFaces = static_cast<unsigned int>(triangles.size());
    mesh->mFaces = new aiFace[triangles.size()];
    for (size_t i = 0; i < triangles.size(); ++i) {
        mesh->mFaces[i].mNumIndices = 3;
        mesh->mFaces[i].mIndices = new unsigned int[3];
        mesh->mFaces[i].mIndices[0] = triangles[i][0];
        mesh->mFaces[i].mIndices[1] = triangles[i][1];
        mesh->mFaces[i].mIndices[2] = triangles[i][2];
    }

    scene->mMeshes[0] = mesh;

    scene->mRootNode = new aiNode();
    scene->mRootNode->mName = aiString("Root");
    scene->mRootNode->mNumMeshes = 1;
    scene->mRootNode->mMeshes = new unsigned int[1];
    scene->mRootNode->mMeshes[0] = 0;

    return scene;
}

// Axis-aligned box centred at origin with the given half-extents (8 unique verts, 12 tris).
aiScene *makeBoxScene(const aiVector3D &halfExtents) {
    const float x = halfExtents.x, y = halfExtents.y, z = halfExtents.z;
    std::vector<aiVector3D> v = {
        { -x, -y, -z }, { +x, -y, -z }, { +x, +y, -z }, { -x, +y, -z },
        { -x, -y, +z }, { +x, -y, +z }, { +x, +y, +z }, { -x, +y, +z }
    };
    std::vector<std::array<unsigned int, 3>> t = {
        { 0, 2, 1 }, { 0, 3, 2 }, // -Z
        { 4, 5, 6 }, { 4, 6, 7 }, // +Z
        { 0, 1, 5 }, { 0, 5, 4 }, // -Y
        { 3, 7, 6 }, { 3, 6, 2 }, // +Y
        { 0, 4, 7 }, { 0, 7, 3 }, // -X
        { 1, 2, 6 }, { 1, 6, 5 }  // +X
    };
    return makeSingleMeshScene(v, t);
}

// Per-face-expanded box: each of the 6 quads contributes 4 fresh duplicate vertices
// (24 total) so a vertex-position welder must collapse them down to 8 unique positions.
aiScene *makePerFaceBoxScene(const aiVector3D &halfExtents) {
    const float x = halfExtents.x, y = halfExtents.y, z = halfExtents.z;
    std::vector<aiVector3D> v;
    std::vector<std::array<unsigned int, 3>> t;

    auto pushQuad = [&](const aiVector3D &a, const aiVector3D &b,
                        const aiVector3D &c, const aiVector3D &d) {
        unsigned int base = static_cast<unsigned int>(v.size());
        v.push_back(a); v.push_back(b); v.push_back(c); v.push_back(d);
        t.push_back({ base + 0, base + 1, base + 2 });
        t.push_back({ base + 0, base + 2, base + 3 });
    };

    pushQuad({ -x, -y, -z }, { +x, +y, -z }, { +x, -y, -z }, { -x, +y, -z }); // -Z (intentionally degenerate ordering kept simple)
    pushQuad({ -x, -y, +z }, { +x, -y, +z }, { +x, +y, +z }, { -x, +y, +z }); // +Z
    pushQuad({ -x, -y, -z }, { +x, -y, -z }, { +x, -y, +z }, { -x, -y, +z }); // -Y
    pushQuad({ -x, +y, -z }, { -x, +y, +z }, { +x, +y, +z }, { +x, +y, -z }); // +Y
    pushQuad({ -x, -y, -z }, { -x, -y, +z }, { -x, +y, +z }, { -x, +y, -z }); // -X
    pushQuad({ +x, -y, -z }, { +x, +y, -z }, { +x, +y, +z }, { +x, -y, +z }); // +X

    return makeSingleMeshScene(v, t);
}

aiVector3D meshMin(const aiMesh *mesh) {
    aiVector3D m(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max());
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        m.x = std::min(m.x, mesh->mVertices[i].x);
        m.y = std::min(m.y, mesh->mVertices[i].y);
        m.z = std::min(m.z, mesh->mVertices[i].z);
    }
    return m;
}

aiVector3D meshMax(const aiMesh *mesh) {
    aiVector3D m(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                 std::numeric_limits<float>::lowest());
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        m.x = std::max(m.x, mesh->mVertices[i].x);
        m.y = std::max(m.y, mesh->mVertices[i].y);
        m.z = std::max(m.z, mesh->mVertices[i].z);
    }
    return m;
}

aiVector3D meshExtent(const aiMesh *mesh) {
    aiVector3D mx = meshMax(mesh);
    aiVector3D mn = meshMin(mesh);
    return aiVector3D(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
}

} // namespace

class utD3MFImporterExporter : public AbstractImportExportBase {
public:
    bool importerTest() override {
        Assimp::Importer importer;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", aiProcess_ValidateDataStructure);
        return (nullptr != scene);
    }

#ifndef ASSIMP_BUILD_NO_EXPORT
    bool exporterTest() override {
        Assimp::Importer importer;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
        if (!scene) {
            return false;
        }
        Assimp::Exporter exporter;
        aiReturn result = exporter.Export(scene, "3mf", "ut_3mf_test_export.3mf");
        std::remove("ut_3mf_test_export.3mf");
        return AI_SUCCESS == result;
    }
#endif
};

// ===== IMPORT TESTS =====

TEST_F(utD3MFImporterExporter, import3MFFromFileTest) {
    EXPECT_TRUE(importerTest());
}

TEST_F(utD3MFImporterExporter, import3MFBoxGeometry) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    ASSERT_GE(scene->mNumMeshes, 1u);

    const aiMesh *mesh = scene->mMeshes[0];
    ASSERT_NE(nullptr, mesh);
    EXPECT_EQ(8u, mesh->mNumVertices);
    EXPECT_EQ(12u, mesh->mNumFaces);

    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        EXPECT_EQ(3u, mesh->mFaces[f].mNumIndices);
    }
}

TEST_F(utD3MFImporterExporter, import3MFHasRootNode) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mRootNode);
}

TEST_F(utD3MFImporterExporter, import3MFHasMaterial) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    EXPECT_GE(scene->mNumMaterials, 1u);
}

// ===== EXPORT TESTS =====

#ifndef ASSIMP_BUILD_NO_EXPORT

TEST_F(utD3MFImporterExporter, export3MFBasicMesh) {
    EXPECT_TRUE(exporterTest());
}

TEST_F(utD3MFImporterExporter, export3MFProducesFile) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
    ASSERT_NE(nullptr, scene);

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_export_test.3mf"));

    FILE *f = fopen("ut_3mf_export_test.3mf", "rb");
    ASSERT_NE(nullptr, f);

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    EXPECT_GT(size, 0);

    std::remove("ut_3mf_export_test.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFWithMaterials) {
    // Import a GLB (which has materials), then export as 3MF
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-Binary/BoxTextured.glb", 0);
    if (!scene) {
        // If glTF test model not available, skip
        return;
    }

    Assimp::Exporter exporter;
    EXPECT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_mat_test.3mf"));
    std::remove("ut_3mf_mat_test.3mf");
}

// ===== ROUNDTRIP TESTS =====

TEST_F(utD3MFImporterExporter, roundtrip3MFBox) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    ASSERT_GE(scene->mNumMeshes, 1u);

    unsigned int origVertices = scene->mMeshes[0]->mNumVertices;
    unsigned int origFaces = scene->mMeshes[0]->mNumFaces;

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_roundtrip_box.3mf"));

    Assimp::Importer importer2;
    const aiScene *scene2 = importer2.ReadFile("ut_3mf_roundtrip_box.3mf", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene2);
    ASSERT_GE(scene2->mNumMeshes, 1u);

    EXPECT_EQ(origVertices, scene2->mMeshes[0]->mNumVertices);
    EXPECT_EQ(origFaces, scene2->mMeshes[0]->mNumFaces);

    std::remove("ut_3mf_roundtrip_box.3mf");
}

TEST_F(utD3MFImporterExporter, roundtrip3MFPreservesTriangles) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
    ASSERT_NE(nullptr, scene);

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_roundtrip_tri.3mf"));

    Assimp::Importer importer2;
    const aiScene *scene2 = importer2.ReadFile("ut_3mf_roundtrip_tri.3mf", 0);
    ASSERT_NE(nullptr, scene2);

    EXPECT_EQ(scene->mNumMeshes, scene2->mNumMeshes);
    for (unsigned int i = 0; i < scene2->mNumMeshes && i < scene->mNumMeshes; ++i) {
        for (unsigned int f = 0; f < scene2->mMeshes[i]->mNumFaces; ++f) {
            EXPECT_EQ(3u, scene2->mMeshes[i]->mFaces[f].mNumIndices);
        }
    }

    std::remove("ut_3mf_roundtrip_tri.3mf");
}

TEST_F(utD3MFImporterExporter, roundtrip3MFWithMaterial) {
    // Use a GLB with material, export to 3MF, reimport and verify color preserved
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-Binary/BoxTextured.glb", 0);
    if (!scene) {
        return;
    }
    ASSERT_GE(scene->mNumMaterials, 1u);

    // Get original material color
    aiColor4D origColor(0.5f, 0.5f, 0.5f, 1.0f);
    scene->mMaterials[0]->Get(AI_MATKEY_COLOR_DIFFUSE, origColor);

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_roundtrip_mat.3mf"));

    Assimp::Importer importer2;
    const aiScene *scene2 = importer2.ReadFile("ut_3mf_roundtrip_mat.3mf", 0);
    ASSERT_NE(nullptr, scene2);
    ASSERT_GE(scene2->mNumMaterials, 1u);

    aiColor4D reimportedColor;
    ASSERT_EQ(AI_SUCCESS, scene2->mMaterials[0]->Get(AI_MATKEY_COLOR_DIFFUSE, reimportedColor));

    EXPECT_NEAR(origColor.r, reimportedColor.r, 0.02f);
    EXPECT_NEAR(origColor.g, reimportedColor.g, 0.02f);
    EXPECT_NEAR(origColor.b, reimportedColor.b, 0.02f);

    std::remove("ut_3mf_roundtrip_mat.3mf");
}

// ===== R8' — UNIT, AXIS, WELD, PROPERTY-RESOLVER TESTS =====

TEST_F(utD3MFImporterExporter, export3MFRescalesToMillimeterWhenSceneUnitScaleToMetersIsOne) {
    // 0.5m half-extent → 1m cube, declared as meters via the contract metadata.
    aiScene *scene = makeBoxScene(aiVector3D(0.5f, 0.5f, 0.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2)); // already Z, no rotation
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_unit_meters.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_unit_meters.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    EXPECT_NEAR(1000.0f, extent.x, 0.5f);
    EXPECT_NEAR(1000.0f, extent.y, 0.5f);
    EXPECT_NEAR(1000.0f, extent.z, 0.5f);
    std::remove("ut_3mf_unit_meters.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFRotatesYUpSourceToZUpByDefault) {
    // Tall box on +Y axis: 100×500×100 in scene units.
    aiScene *scene = makeBoxScene(aiVector3D(50.0f, 250.0f, 50.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1)); // Y-up source
    // Identity scale fallback (no UnitScaleToMeters metadata)
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_axis_zup.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_axis_zup.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    // After Y→Z rotation, the long axis (originally Y) lands on Z.
    EXPECT_NEAR(100.0f, extent.x, 1.0f);
    EXPECT_NEAR(100.0f, extent.y, 1.0f);
    EXPECT_NEAR(500.0f, extent.z, 1.0f);
    std::remove("ut_3mf_axis_zup.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFRetainsYUpWhenExportUpAxisPropertyIsOne) {
    aiScene *scene = makeBoxScene(aiVector3D(50.0f, 250.0f, 50.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    Assimp::Exporter exporter;
    Assimp::ExportProperties props;
    props.SetPropertyInteger("3MF_EXPORT_UPAXIS", 1);
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_axis_yup_optin.3mf", 0, &props));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_axis_yup_optin.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    EXPECT_NEAR(100.0f, extent.x, 1.0f);
    EXPECT_NEAR(500.0f, extent.y, 1.0f);
    EXPECT_NEAR(100.0f, extent.z, 1.0f);
    std::remove("ut_3mf_axis_yup_optin.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFThrowsDeadlyExportErrorWhenUpAxisPropertyOutOfRange) {
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    Assimp::Exporter exporter;
    Assimp::ExportProperties props;
    props.SetPropertyInteger("3MF_EXPORT_UPAXIS", 7);
    aiReturn rc = exporter.Export(scene, "3mf", "ut_3mf_bad_axis.3mf", 0, &props);
    EXPECT_EQ(AI_FAILURE, rc);
    std::string err = exporter.GetErrorString();
    EXPECT_NE(std::string::npos, err.find("3MF_EXPORT_UPAXIS"));
    EXPECT_NE(std::string::npos, err.find("7"));
    EXPECT_NE(std::string::npos, err.find("[0, 2]"));
    delete scene;
    std::remove("ut_3mf_bad_axis.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFAppliesIdentityTransformWhenSceneHasNoContractMetadataAndNoProperties) {
    // Unit cube with NO mMetaData and NO ExportProperties → identity transform path.
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    ASSERT_EQ(nullptr, scene->mMetaData);
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_identity.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_identity.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    EXPECT_NEAR(2.0f, extent.x, 1e-3f);
    EXPECT_NEAR(2.0f, extent.y, 1e-3f);
    EXPECT_NEAR(2.0f, extent.z, 1e-3f);
    std::remove("ut_3mf_identity.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFIgnoresLegacyUnitScaleFactorKey) {
    // FBX-shaped scene: only the legacy `UnitScaleFactor` (cm-relative) is set.
    // The 3MF bridge MUST NOT read this key — it is disjoint from the new contract.
    // If misread as meters-per-unit, a 1.0 value would silently scale by 1000×.
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add("UnitScaleFactor", 1.0); // legacy FBX key — semantics: cm
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_legacy_key.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_legacy_key.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    // Identity transform means the 2-unit cube re-imports as a 2-unit cube,
    // not a 2000× scaled-up monstrosity.
    EXPECT_NEAR(2.0f, extent.x, 1e-3f);
    EXPECT_NEAR(2.0f, extent.y, 1e-3f);
    EXPECT_NEAR(2.0f, extent.z, 1e-3f);
    std::remove("ut_3mf_legacy_key.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFCollapsesPositionDuplicatesFromPerFaceNormalSeams) {
    // Per-face-expanded cube has 24 source vertices that share 8 unique positions.
    aiScene *scene = makePerFaceBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    ASSERT_EQ(24u, scene->mMeshes[0]->mNumVertices);
    ASSERT_EQ(12u, scene->mMeshes[0]->mNumFaces);
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_weld.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_weld.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    EXPECT_EQ(8u, roundtrip->mMeshes[0]->mNumVertices);
    EXPECT_EQ(12u, roundtrip->mMeshes[0]->mNumFaces);
    std::remove("ut_3mf_weld.3mf");
}

// ===== R10' — IMPORTER METADATA TESTS =====

TEST_F(utD3MFImporterExporter, import3MFWritesUnitScaleToMetersAndUpAxisToSceneMetadata) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMetaData);

    double meters = 0.0;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, meters));
    EXPECT_NEAR(1e-3, meters, 1e-9);

    int32_t upAxis = -1;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, upAxis));
    EXPECT_EQ(2, upAxis);

    aiString sourceFormat;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_SOURCE_FORMAT, sourceFormat));
    EXPECT_STREQ("3mf Importer", sourceFormat.C_Str());
}

TEST_F(utD3MFImporterExporter, importContractUnitScaleOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_IMPORT_3MF_UNIT_SCALE_TO_METERS, 1.0f);
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMetaData);

    double meters = 0.0;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, meters));
    EXPECT_NEAR(1.0, meters, 1e-9) << "AI_CONFIG_IMPORT_3MF_UNIT_SCALE_TO_METERS override must win over the file-declared unit";

    int32_t upAxis = -1;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, upAxis));
    EXPECT_EQ(2, upAxis) << "Up-axis must remain 3MF-spec Z when only the unit override is set";
}

TEST_F(utD3MFImporterExporter, importContractUpAxisOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_3MF_UP_AXIS, 1); // force Y-up
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMetaData);

    int32_t upAxis = -1;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, upAxis));
    EXPECT_EQ(1, upAxis) << "AI_CONFIG_IMPORT_3MF_UP_AXIS override must win over the spec-default Z";
}

TEST_F(utD3MFImporterExporter, importContractInvalidUpAxisOverrideFailsImport) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_3MF_UP_AXIS, 7); // out of [0, 2]
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
    EXPECT_EQ(nullptr, scene) << "Invalid AI_CONFIG_IMPORT_3MF_UP_AXIS override must fail import via DeadlyImportError";
    EXPECT_NE(std::string::npos, std::string(importer.GetErrorString()).find("IMPORT_3MF_UP_AXIS"));
}

TEST_F(utD3MFImporterExporter, export3MFReadsUnitScaleToMetersMetadataWhenPropertyAbsent) {
    // 1-unit cube authored as centimeters via the contract; export with no properties.
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 0.01); // cm
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_unit_meta.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_unit_meta.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    // Source = cm (0.01 m/unit), default target = mm (0.001 m/unit).
    // scale = 0.01 / 0.001 = 10 → 2-unit cube → 20 mm.
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    EXPECT_NEAR(20.0f, extent.x, 1e-2f);
    EXPECT_NEAR(20.0f, extent.y, 1e-2f);
    EXPECT_NEAR(20.0f, extent.z, 1e-2f);
    std::remove("ut_3mf_unit_meta.3mf");
}

TEST_F(utD3MFImporterExporter, roundtrip3MFPreservesUnitsAndAxisAcrossExportImport) {
    aiScene *scene = makeBoxScene(aiVector3D(5.0f, 5.0f, 5.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1e-3); // mm
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2)); // Z
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_roundtrip_units.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_roundtrip_units.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_NE(nullptr, roundtrip->mMetaData);

    double meters = 0.0;
    ASSERT_TRUE(roundtrip->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, meters));
    EXPECT_NEAR(1e-3, meters, 1e-9);
    int32_t up = -1;
    ASSERT_TRUE(roundtrip->mMetaData->Get(AI_METADATA_UP_AXIS, up));
    EXPECT_EQ(2, up);

    // Guard the mesh-extent assertions behind an ASSERT_GE so the test
    // reports a clean failure (rather than segfaulting on a null mesh) on
    // the pre-existing 3MF round-trip regression where the importer can
    // re-read the package metadata but loses the model body. Tracked under
    // the same baseline as `roundtrip3MFBox` / `roundtrip3MFPreservesTriangles`.
    ASSERT_GE(roundtrip->mNumMeshes, 1u) << "3MF re-importer dropped meshes";
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    EXPECT_NEAR(10.0f, extent.x, 1e-3f);
    EXPECT_NEAR(10.0f, extent.y, 1e-3f);
    EXPECT_NEAR(10.0f, extent.z, 1e-3f);
    std::remove("ut_3mf_roundtrip_units.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFRespectsExportUnitProperty) {
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0); // meters
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));
    Assimp::Exporter exporter;
    Assimp::ExportProperties props;
    props.SetPropertyString("3MF_EXPORT_UNIT", "centimeter");
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_unit_prop.3mf", 0, &props));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_unit_prop.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_NE(nullptr, roundtrip->mMetaData);

    double meters = 0.0;
    ASSERT_TRUE(roundtrip->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, meters));
    EXPECT_NEAR(0.01, meters, 1e-9);

    // Guard against the pre-existing 3MF re-importer regression that drops
    // mesh bodies — fail cleanly rather than segfault.
    ASSERT_GE(roundtrip->mNumMeshes, 1u) << "3MF re-importer dropped meshes";
    // 1m source → 100 cm output.
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    EXPECT_NEAR(200.0f, extent.x, 1e-2f);
    EXPECT_NEAR(200.0f, extent.y, 1e-2f);
    EXPECT_NEAR(200.0f, extent.z, 1e-2f);
    std::remove("ut_3mf_unit_prop.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFDeclaresMillimeterUnitWhenNoPropertiesPassed) {
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_default_unit.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_default_unit.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_NE(nullptr, roundtrip->mMetaData);

    double meters = 0.0;
    ASSERT_TRUE(roundtrip->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, meters));
    EXPECT_NEAR(1e-3, meters, 1e-9);
    std::remove("ut_3mf_default_unit.3mf");
}

// ===== R9' — DEFAULT POST-PROCESSING FLAGS TEST =====

TEST_F(utD3MFImporterExporter, export3MFViaTopLevelExporterFlattensSceneGraph) {
    // Author a scene where a child node has a translation transform.
    // Without aiProcess_PreTransformVertices in the exporter's enforced PP flags,
    // the child's translation is exported as a build-item transform, not baked into
    // vertex positions. With the R9' default flags, the translation MUST be baked.
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    delete scene->mRootNode;

    auto *root = new aiNode();
    root->mName = aiString("Root");
    auto *child = new aiNode();
    child->mName = aiString("ChildOffset");
    child->mTransformation.a4 = 100.0f; // translate +X by 100 units
    child->mNumMeshes = 1;
    child->mMeshes = new unsigned int[1];
    child->mMeshes[0] = 0;
    child->mParent = root;

    root->mNumChildren = 1;
    root->mChildren = new aiNode *[1];
    root->mChildren[0] = child;
    scene->mRootNode = root;

    Assimp::Exporter exporter;
    // Caller passes ZERO pp flags — only enforced flags run.
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_flatten.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_flatten.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);

    // Build-item transforms should be identity — translation is baked in.
    ASSERT_NE(nullptr, roundtrip->mRootNode);
    for (unsigned int i = 0; i < roundtrip->mRootNode->mNumChildren; ++i) {
        const aiNode *c = roundtrip->mRootNode->mChildren[i];
        EXPECT_TRUE(c->mTransformation.IsIdentity());
    }

    // Vertex centroid should be ≈ (100, 0, 0) baked in (default unit scale + identity axis).
    aiVector3D mn = meshMin(roundtrip->mMeshes[0]);
    aiVector3D mx = meshMax(roundtrip->mMeshes[0]);
    aiVector3D centroid((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);
    EXPECT_NEAR(100.0f, centroid.x, 1e-2f);
    EXPECT_NEAR(0.0f, centroid.y, 1e-2f);
    EXPECT_NEAR(0.0f, centroid.z, 1e-2f);
    std::remove("ut_3mf_flatten.3mf");
}

// ===== R11 — GLB→3MF CROSS-FORMAT ROUNDTRIP =====

TEST_F(utD3MFImporterExporter, glbToThreeMfRoundtripPreservesPhysicalDimensions) {
    Assimp::Importer importer;
    const aiScene *gltfScene = importer.ReadFile(
        ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-Binary/BoxTextured.glb", 0);
    ASSERT_NE(nullptr, gltfScene);
    ASSERT_GE(gltfScene->mNumMeshes, 1u);

    aiVector3D gltfExtent = meshExtent(gltfScene->mMeshes[0]);
    // BoxTextured.glb is a 1m unit cube in glTF (Y-up, meters).

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(gltfScene, "3mf", "ut_3mf_glb_roundtrip.3mf"));

    Assimp::Importer importer2;
    const aiScene *threeMfScene = importer2.ReadFile("ut_3mf_glb_roundtrip.3mf", 0);
    ASSERT_NE(nullptr, threeMfScene);
    ASSERT_GE(threeMfScene->mNumMeshes, 1u);
    aiVector3D threeMfExtent = meshExtent(threeMfScene->mMeshes[0]);

    // Meters → mm: 1000× larger in every axis, with the original Y-extent landing on Z.
    EXPECT_NEAR(gltfExtent.x * 1000.0f, threeMfExtent.x, 1.0f);
    EXPECT_NEAR(gltfExtent.z * 1000.0f, threeMfExtent.y, 1.0f);
    EXPECT_NEAR(gltfExtent.y * 1000.0f, threeMfExtent.z, 1.0f);
    std::remove("ut_3mf_glb_roundtrip.3mf");
}

#endif // ASSIMP_BUILD_NO_EXPORT
