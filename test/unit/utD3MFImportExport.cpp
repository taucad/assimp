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

#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/material.h>
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>

#include <cstdio>

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

#endif // ASSIMP_BUILD_NO_EXPORT
