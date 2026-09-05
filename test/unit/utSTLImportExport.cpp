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
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace Assimp;

namespace {

// Author an in-memory single-mesh box scene. The caller owns the returned
// scene and must `delete` it; aiScene's destructor deletes the meshes,
// materials, and nodes it owns.
aiScene *makeStlBoxScene(const aiVector3D &halfExtents) {
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
        // Normals point radially out from the origin so we can detect a
        // rotation having been applied.
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

aiVector3D meshExtentStl(const aiMesh *mesh) {
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

class utSTLImporterExporter : public AbstractImportExportBase {
public:
    virtual bool importerTest() {
        Assimp::Importer importer;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/STL/Spider_ascii.stl", aiProcess_ValidateDataStructure);
        return nullptr != scene;
    }
};

TEST_F(utSTLImporterExporter, importSTLFromFileTest) {
    EXPECT_TRUE(importerTest());
}

TEST_F(utSTLImporterExporter, importBinarySTLFromFileTest) {
    // Regression test for issue #5509: binary STL was rejected on big-endian
    // hosts because the little-endian on-disk facet count and geometry were
    // read without byte-swapping. Checking concrete counts and coordinates
    // covers both the facet count and the per-float geometry, so a byte-swap
    // regression cannot pass silently.
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(
            ASSIMP_TEST_MODELS_DIR "/STL/Spider_binary.stl", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    ASSERT_EQ(1u, scene->mNumMeshes);
    const aiMesh *mesh = scene->mMeshes[0];
    EXPECT_EQ(1368u, mesh->mNumFaces);
    EXPECT_EQ(4104u, mesh->mNumVertices);
    EXPECT_NEAR(0.90712798f, mesh->mVertices[0].x, 1e-4f);
    EXPECT_NEAR(0.64616501f, mesh->mVertices[0].y, 1e-4f);
    EXPECT_NEAR(0.79519337f, mesh->mVertices[0].z, 1e-4f);
    EXPECT_NEAR(0.46828195f, mesh->mNormals[0].x, 1e-4f);
    EXPECT_NEAR(-0.86349779f, mesh->mNormals[0].y, 1e-4f);
    EXPECT_NEAR(-0.18730624f, mesh->mNormals[0].z, 1e-4f);
}

TEST_F(utSTLImporterExporter, test_multiple) {
    // import same file twice, each with its own importer
    // must work both times and not crash
    Assimp::Importer importer1;
    const aiScene *scene1 = importer1.ReadFile(ASSIMP_TEST_MODELS_DIR "/STL/Spider_ascii.stl", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene1);

    Assimp::Importer importer2;
    const aiScene *scene2 = importer2.ReadFile(ASSIMP_TEST_MODELS_DIR "/STL/Spider_ascii.stl", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene2);
}

TEST_F(utSTLImporterExporter, importSTLformatdetection) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/STL/formatDetection", aiProcess_ValidateDataStructure);

    EXPECT_NE(nullptr, scene);
}

TEST_F(utSTLImporterExporter, test_with_two_solids) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/STL/triangle_with_two_solids.stl", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);
}

TEST_F(utSTLImporterExporter, importBinarySTLWithMisalignedSecondFacet) {
    std::array<unsigned char, 84 + 2 * 50> data{};

    const uint32_t faceCount = 2;
    std::memcpy(data.data() + 80, &faceCount, sizeof(faceCount));

    const std::array<std::array<float, 12>, 2> facets = { {
        { 0.0f, 0.0f, 1.0f, 1.25f, 2.5f, 3.75f, -4.0f, -5.5f, -6.75f, 7.0f, 8.25f, 9.5f },
        { 0.0f, 1.0f, 0.0f, -1.0f, -2.25f, -3.5f, 4.75f, 5.0f, 6.25f, -7.5f, -8.75f, -9.0f }
    } };
    const std::array<uint16_t, 2> attributes = { 0xfc00u, 0x83e0u }; // red, green
    for (size_t i = 0; i < facets.size(); ++i) {
        const size_t offset = 84 + i * 50;
        std::memcpy(data.data() + offset, facets[i].data(), 48);
        std::memcpy(data.data() + offset + 48, &attributes[i], sizeof(attributes[i]));
    }

    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFileFromMemory(data.data(), data.size(), 0, "stl");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    const aiMesh *mesh = scene->mMeshes[0];
    ASSERT_EQ(2u, mesh->mNumFaces);
    ASSERT_EQ(6u, mesh->mNumVertices);
    ASSERT_TRUE(mesh->HasNormals());
    ASSERT_TRUE(mesh->HasVertexColors(0));

    for (size_t face = 0; face < facets.size(); ++face) {
        for (size_t vertex = 0; vertex < 3; ++vertex) {
            const size_t output = face * 3 + vertex;
            const size_t input = 3 + vertex * 3;
            EXPECT_EQ(facets[face][input], mesh->mVertices[output].x);
            EXPECT_EQ(facets[face][input + 1], mesh->mVertices[output].y);
            EXPECT_EQ(facets[face][input + 2], mesh->mVertices[output].z);
            EXPECT_EQ(facets[face][0], mesh->mNormals[output].x);
            EXPECT_EQ(facets[face][1], mesh->mNormals[output].y);
            EXPECT_EQ(facets[face][2], mesh->mNormals[output].z);
        }
    }

    for (size_t vertex = 0; vertex < 3; ++vertex) {
        EXPECT_EQ(aiColor4D(1, 0, 0, 1), mesh->mColors[0][vertex]);
        EXPECT_EQ(aiColor4D(0, 1, 0, 1), mesh->mColors[0][vertex + 3]);
    }
}

// -----------------------------------------------------------------------------
// Unit/axis contract: STL importer must declare scene metadata so downstream
// exporters can normalise to their own spec. STL is unitless + axis-less by
// spec; defaults are mm + Z-up to match the dominant 3D-printing convention.
// -----------------------------------------------------------------------------

TEST_F(utSTLImporterExporter, contractDefaultsAreMmAndZUp) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(
        ASSIMP_TEST_MODELS_DIR "/STL/Spider_ascii.stl", 0);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMetaData);

    double unit = 0.0;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, unit))
        << "STL importer must declare AI_METADATA_UNIT_SCALE_TO_METERS";
    EXPECT_DOUBLE_EQ(0.001, unit) << "STL default unit must be mm";

    int32_t upAxis = -1;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, upAxis))
        << "STL importer must declare AI_METADATA_UP_AXIS";
    EXPECT_EQ(2, upAxis) << "STL default up-axis must be Z (2)";
}

TEST_F(utSTLImporterExporter, contractUnitScaleOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_IMPORT_STL_UNIT_SCALE_TO_METERS, 0.0254f);
    const aiScene *scene = importer.ReadFile(
        ASSIMP_TEST_MODELS_DIR "/STL/Spider_ascii.stl", 0);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMetaData);

    double unit = 0.0;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, unit));
    EXPECT_NEAR(0.0254, unit, 1e-6);
}

TEST_F(utSTLImporterExporter, contractUpAxisOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_STL_UP_AXIS, 1);
    const aiScene *scene = importer.ReadFile(
        ASSIMP_TEST_MODELS_DIR "/STL/Spider_ascii.stl", 0);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMetaData);

    int32_t upAxis = -1;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, upAxis));
    EXPECT_EQ(1, upAxis);
}

TEST_F(utSTLImporterExporter, contractInvalidUpAxisOverrideFailsImport) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_STL_UP_AXIS, 7);
    const aiScene *scene = importer.ReadFile(
        ASSIMP_TEST_MODELS_DIR "/STL/Spider_ascii.stl", 0);
    EXPECT_EQ(nullptr, scene)
        << "Out-of-range up-axis override must fail the import";
    EXPECT_NE(std::string::npos, std::string(importer.GetErrorString()).find("UP_AXIS"));
}

TEST_F(utSTLImporterExporter, test_with_empty_solid) {
    Assimp::Importer importer;
    //STL File with empty mesh. We should still be able to import other meshes in this file. ValidateDataStructure should fail.
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/STL/triangle_with_empty_solid.stl", 0);
    EXPECT_NE(nullptr, scene);

    const aiScene *scene2 = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/STL/triangle_with_empty_solid.stl", aiProcess_ValidateDataStructure);
    EXPECT_EQ(nullptr, scene2);
}

#ifndef ASSIMP_BUILD_NO_EXPORT

TEST_F(utSTLImporterExporter, exporterTest) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/STL/Spider_ascii.stl", aiProcess_ValidateDataStructure);

    Assimp::Exporter mAiExporter;
    const char *stlFileName = "spiderExport.stl";
    mAiExporter.Export(scene, "stl", stlFileName);

    const aiScene *scene2 = importer.ReadFile(stlFileName, aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene2);

    // Cleanup, delete the exported file
    std::remove(stlFileName);
}

TEST_F(utSTLImporterExporter, test_export_pointclouds) {
    struct XYZ {
        float x, y, z;
    };

    std::vector<XYZ> points;

    for (size_t i = 0; i < 10; ++i) {
        XYZ current;
        current.x = static_cast<float>(i);
        current.y = static_cast<float>(i);
        current.z = static_cast<float>(i);
        points.push_back(current);
    }
    aiScene scene;
    scene.mRootNode = new aiNode();

    scene.mMeshes = new aiMesh *[1];
    scene.mMeshes[0] = nullptr;
    scene.mNumMeshes = 1;

    scene.mMaterials = new aiMaterial *[1];
    scene.mMaterials[0] = nullptr;
    scene.mNumMaterials = 1;

    scene.mMaterials[0] = new aiMaterial();

    scene.mMeshes[0] = new aiMesh();
    scene.mMeshes[0]->mMaterialIndex = 0;

    scene.mRootNode->mMeshes = new unsigned int[1];
    scene.mRootNode->mMeshes[0] = 0;
    scene.mRootNode->mNumMeshes = 1;

    auto pMesh = scene.mMeshes[0];

    size_t numValidPoints = points.size();

    pMesh->mVertices = new aiVector3D[numValidPoints];
    pMesh->mNumVertices = static_cast<unsigned int>(numValidPoints);

    int i = 0;
    for (XYZ &p : points) {
        pMesh->mVertices[i] = aiVector3D(p.x, p.y, p.z);
        ++i;
    }

    Assimp::Exporter mAiExporter;
    ExportProperties *properties = new ExportProperties;
    properties->SetPropertyBool(AI_CONFIG_EXPORT_POINT_CLOUDS, true);

    const char *stlFileName = "testExport.stl";
    mAiExporter.Export(&scene, "stl", stlFileName, 0, properties);

    // Cleanup, delete the exported file
    ::remove(stlFileName);
    delete properties;
}

// -----------------------------------------------------------------------------
// Unit/axis contract: STL exporter bakes a single linear transform into mesh
// vertices/normals so the output file matches STL's de-facto spec target of
// millimetres + Z-up. Behaviour gated on the contract opt-in
// (`AI_METADATA_UNIT_SCALE_TO_METERS`) so legacy callers stay byte-identical.
// -----------------------------------------------------------------------------

TEST_F(utSTLImporterExporter, exportSTLBakesUnitScaleWhenSourceUnitDiffersFromMm) {
    // 0.5m half-extent box authored as metres → re-imported STL must measure
    // 1000mm extent on every axis (metres → millimetres).
    aiScene *scene = makeStlBoxScene(aiVector3D(0.5f, 0.5f, 0.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0); // metres
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2)); // already Z-up

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "stl", "ut_stl_unit_m.stl"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_stl_unit_m.stl", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentStl(roundtrip->mMeshes[0]);
    EXPECT_NEAR(1000.0f, extent.x, 1e-3f);
    EXPECT_NEAR(1000.0f, extent.y, 1e-3f);
    EXPECT_NEAR(1000.0f, extent.z, 1e-3f);
    std::remove("ut_stl_unit_m.stl");
}

TEST_F(utSTLImporterExporter, exportSTLBakesAxisRotationWhenSourceIsYUp) {
    // Tall box on +Y (Y-up source), extents 2 x 10 x 2 mm. After Y→Z bake the
    // tall axis must move from +Y to +Z so the slicer drops it on the bed
    // standing upright.
    aiScene *scene = makeStlBoxScene(aiVector3D(1.0f, 5.0f, 1.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1e-3); // already mm
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1)); // Y-up source

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "stl", "ut_stl_axis_yup.stl"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_stl_axis_yup.stl", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentStl(roundtrip->mMeshes[0]);
    EXPECT_NEAR(2.0f, extent.x, 1e-4f);
    EXPECT_NEAR(2.0f, extent.y, 1e-4f);
    EXPECT_NEAR(10.0f, extent.z, 1e-4f);
    std::remove("ut_stl_axis_yup.stl");
}

TEST_F(utSTLImporterExporter, exportSTLIsIdentityWhenSourceAlreadyMmZUp) {
    // Source frame matches STL spec (mm + Z-up) → identity short-circuit;
    // round-trip must reproduce the source extents to within float epsilon.
    aiScene *scene = makeStlBoxScene(aiVector3D(2.5f, 7.5f, 2.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1e-3);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "stl", "ut_stl_identity.stl"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_stl_identity.stl", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentStl(roundtrip->mMeshes[0]);
    EXPECT_NEAR(5.0f, extent.x, 1e-5f);
    EXPECT_NEAR(15.0f, extent.y, 1e-5f);
    EXPECT_NEAR(5.0f, extent.z, 1e-5f);
    std::remove("ut_stl_identity.stl");
}

TEST_F(utSTLImporterExporter, exportSTLIsIdentityWhenContractMetadataAbsent) {
    // No `UnitScaleToMeters` key on the scene → unmigrated/legacy caller; the
    // exporter must produce byte-for-byte identical output as before the
    // contract landed (no rescale, no rotation).
    aiScene *scene = makeStlBoxScene(aiVector3D(3.0f, 3.0f, 3.0f));
    ASSERT_EQ(nullptr, scene->mMetaData);

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "stl", "ut_stl_no_meta.stl"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_stl_no_meta.stl", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentStl(roundtrip->mMeshes[0]);
    EXPECT_NEAR(6.0f, extent.x, 1e-5f);
    EXPECT_NEAR(6.0f, extent.y, 1e-5f);
    EXPECT_NEAR(6.0f, extent.z, 1e-5f);
    std::remove("ut_stl_no_meta.stl");
}

TEST_F(utSTLImporterExporter, exportSTLBinaryHonorsContractTransform) {
    // Same axis bake as the ASCII test, but exercising the binary writer to
    // guarantee both write paths share the bake.
    aiScene *scene = makeStlBoxScene(aiVector3D(1.0f, 5.0f, 1.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1e-3);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "stlb", "ut_stl_axis_yup_bin.stl"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_stl_axis_yup_bin.stl", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentStl(roundtrip->mMeshes[0]);
    EXPECT_NEAR(2.0f, extent.x, 1e-4f);
    EXPECT_NEAR(2.0f, extent.y, 1e-4f);
    EXPECT_NEAR(10.0f, extent.z, 1e-4f);
    std::remove("ut_stl_axis_yup_bin.stl");
}

#endif
