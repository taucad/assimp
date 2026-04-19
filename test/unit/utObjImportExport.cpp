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
#include "SceneDiffer.h"
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

static const float VertComponents[24 * 3] = {
    -0.500000, 0.500000, 0.500000,
    -0.500000, 0.500000, -0.500000,
    -0.500000, -0.500000, -0.500000,
    -0.500000, -0.500000, 0.500000,
    -0.500000, -0.500000, -0.500000,
    0.500000, -0.500000, -0.500000,
    0.500000, -0.500000, 0.500000,
    -0.500000, -0.500000, 0.500000,
    -0.500000, 0.500000, -0.500000,
    0.500000, 0.500000, -0.500000,
    0.500000, -0.500000, -0.500000,
    -0.500000, -0.500000, -0.500000,
    0.500000, 0.500000, 0.500000,
    0.500000, 0.500000, -0.500000,
    -0.500000, 0.500000, -0.500000,
    -0.500000, 0.500000, 0.500000,
    0.500000, -0.500000, 0.500000,
    0.500000, 0.500000, 0.500000,
    -0.500000, 0.500000, 0.500000,
    -0.500000, -0.500000, 0.500000,
    0.500000, -0.500000, -0.500000,
    0.500000, 0.500000, -0.500000,
    0.500000, 0.500000, 0.500000f,
    0.500000, -0.500000, 0.500000f
};

static const char *ObjModel =
        "o 1\n"
        "\n"
        "# Vertex list\n"
        "\n"
        "v -0.5 -0.5  0.5\n"
        "v -0.5 -0.5 -0.5\n"
        "v -0.5  0.5 -0.5\n"
        "v -0.5  0.5  0.5\n"
        "v  0.5 -0.5  0.5\n"
        "v  0.5 -0.5 -0.5\n"
        "v  0.5  0.5 -0.5\n"
        "v  0.5  0.5  0.5\n"
        "\n"
        "# Point / Line / Face list\n"
        "\n"
        "g Box01\n"
        "usemtl Default\n"
        "f 4 3 2 1\n"
        "f 2 6 5 1\n"
        "f 3 7 6 2\n"
        "f 8 7 3 4\n"
        "f 5 8 4 1\n"
        "f 6 7 8 5\n"
        "\n"
        "# End of file\n";

static const char *ObjModel_Issue1111 =
        "o 1\n"
        "\n"
        "# Vertex list\n"
        "\n"
        "v -0.5 -0.5  0.5\n"
        "v -0.5 -0.5 -0.5\n"
        "v -0.5  0.5 -0.5\n"
        "\n"
        "usemtl\n"
        "f 1 2 3\n"
        "\n"
        "# End of file\n";

class utObjImportExport : public AbstractImportExportBase {
protected:
    void SetUp() override {
        m_im = new Assimp::Importer;
    }

    void TearDown() override {
        delete m_im;
        m_im = nullptr;
    }

    aiScene *createScene() {
        aiScene *expScene = new aiScene;
        expScene->mNumMeshes = 1;
        expScene->mMeshes = new aiMesh *[1];
        aiMesh *mesh = new aiMesh;
        mesh->mName.Set("Box01");
        mesh->mNumVertices = 24;
        mesh->mVertices = new aiVector3D[24];
        ::memcpy(&mesh->mVertices->x, &VertComponents[0], sizeof(float) * 24 * 3);
        mesh->mNumFaces = 6;
        mesh->mFaces = new aiFace[mesh->mNumFaces];

        mesh->mFaces[0].mNumIndices = 4;
        mesh->mFaces[0].mIndices = new unsigned int[mesh->mFaces[0].mNumIndices];
        mesh->mFaces[0].mIndices[0] = 0;
        mesh->mFaces[0].mIndices[1] = 1;
        mesh->mFaces[0].mIndices[2] = 2;
        mesh->mFaces[0].mIndices[3] = 3;

        mesh->mFaces[1].mNumIndices = 4;
        mesh->mFaces[1].mIndices = new unsigned int[mesh->mFaces[0].mNumIndices];
        mesh->mFaces[1].mIndices[0] = 4;
        mesh->mFaces[1].mIndices[1] = 5;
        mesh->mFaces[1].mIndices[2] = 6;
        mesh->mFaces[1].mIndices[3] = 7;

        mesh->mFaces[2].mNumIndices = 4;
        mesh->mFaces[2].mIndices = new unsigned int[mesh->mFaces[0].mNumIndices];
        mesh->mFaces[2].mIndices[0] = 8;
        mesh->mFaces[2].mIndices[1] = 9;
        mesh->mFaces[2].mIndices[2] = 10;
        mesh->mFaces[2].mIndices[3] = 11;

        mesh->mFaces[3].mNumIndices = 4;
        mesh->mFaces[3].mIndices = new unsigned int[mesh->mFaces[0].mNumIndices];
        mesh->mFaces[3].mIndices[0] = 12;
        mesh->mFaces[3].mIndices[1] = 13;
        mesh->mFaces[3].mIndices[2] = 14;
        mesh->mFaces[3].mIndices[3] = 15;

        mesh->mFaces[4].mNumIndices = 4;
        mesh->mFaces[4].mIndices = new unsigned int[mesh->mFaces[0].mNumIndices];
        mesh->mFaces[4].mIndices[0] = 16;
        mesh->mFaces[4].mIndices[1] = 17;
        mesh->mFaces[4].mIndices[2] = 18;
        mesh->mFaces[4].mIndices[3] = 19;

        mesh->mFaces[5].mNumIndices = 4;
        mesh->mFaces[5].mIndices = new unsigned int[mesh->mFaces[0].mNumIndices];
        mesh->mFaces[5].mIndices[0] = 20;
        mesh->mFaces[5].mIndices[1] = 21;
        mesh->mFaces[5].mIndices[2] = 22;
        mesh->mFaces[5].mIndices[3] = 23;

        expScene->mMeshes[0] = mesh;

        expScene->mNumMaterials = 1;
        expScene->mMaterials = new aiMaterial *[expScene->mNumMaterials];

        return expScene;
    }

    bool importerTest() override {
        ::Assimp::Importer importer;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OBJ/spider.obj", aiProcess_ValidateDataStructure);
        return nullptr != scene;
    }

#ifndef ASSIMP_BUILD_NO_EXPORT

    bool exporterTest() override {
        ::Assimp::Importer importer;
        ::Assimp::Exporter exporter;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OBJ/spider.obj", aiProcess_ValidateDataStructure);
        EXPECT_NE(nullptr, scene);
        EXPECT_EQ(aiReturn_SUCCESS, exporter.Export(scene, "obj", ASSIMP_TEST_MODELS_DIR "/OBJ/spider_out.obj"));
        EXPECT_EQ(aiReturn_SUCCESS, exporter.Export(scene, "objnomtl", ASSIMP_TEST_MODELS_DIR "/OBJ/spider_nomtl_out.obj"));

        return true;
    }

#endif // ASSIMP_BUILD_NO_EXPORT

protected:
    ::Assimp::Importer *m_im;
    aiScene *m_expectedScene;
};

TEST_F(utObjImportExport, importObjFromFileTest) {
    EXPECT_TRUE(importerTest());
}

#ifndef ASSIMP_BUILD_NO_EXPORT

TEST_F(utObjImportExport, exportObjFromFileTest) {
    EXPECT_TRUE(exporterTest());
}

#endif // ASSIMP_BUILD_NO_EXPORT

TEST_F(utObjImportExport, obj_import_test) {
    const aiScene *scene = m_im->ReadFileFromMemory((void *)ObjModel, strlen(ObjModel), 0);
    aiScene *expected = createScene();
    EXPECT_NE(nullptr, scene);

    SceneDiffer differ;
    EXPECT_TRUE(differ.isEqual(expected, scene));
    differ.showReport();

    m_im->FreeScene();
    for (unsigned int i = 0; i < expected->mNumMeshes; ++i) {
        delete expected->mMeshes[i];
    }
    delete[] expected->mMeshes;
    expected->mMeshes = nullptr;
    delete[] expected->mMaterials;
    expected->mMaterials = nullptr;
    delete expected;
}

TEST_F(utObjImportExport, issue1111_no_mat_name_Test) {
    const aiScene *scene = m_im->ReadFileFromMemory((void *)ObjModel_Issue1111, strlen(ObjModel_Issue1111), 0);
    EXPECT_NE(nullptr, scene);
}

TEST_F(utObjImportExport, issue809_vertex_color_Test) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OBJ/cube_with_vertexcolors.obj", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);

#ifndef ASSIMP_BUILD_NO_EXPORT
    ::Assimp::Exporter exporter;
    EXPECT_EQ(aiReturn_SUCCESS, exporter.Export(scene, "obj", ASSIMP_TEST_MODELS_DIR "/OBJ/test_out.obj"));
#endif // ASSIMP_BUILD_NO_EXPORT
}

TEST_F(utObjImportExport, issue1923_vertex_color_Test) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OBJ/cube_with_vertexcolors_uni.obj", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);

    scene = importer.GetOrphanedScene();

#ifndef ASSIMP_BUILD_NO_EXPORT
    ::Assimp::Exporter exporter;
    const aiExportDataBlob *blob = exporter.ExportToBlob(scene, "obj");
    EXPECT_NE(nullptr, blob);

    const aiScene *sceneReImport = importer.ReadFileFromMemory(blob->data, blob->size, aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);

    SceneDiffer differ;
    EXPECT_TRUE(differ.isEqual(scene, sceneReImport));
#endif // ASSIMP_BUILD_NO_EXPORT

    delete scene;
}

TEST_F(utObjImportExport, only_a_part_of_vertex_colors_Test) {
    ::Assimp::Importer importer;
    const aiScene *const scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OBJ/only_a_part_of_vertexcolors.obj", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);

    EXPECT_EQ(scene->mNumMeshes, 1U);
    const aiMesh *const mesh = scene->mMeshes[0];
    EXPECT_EQ(mesh->mNumVertices, 9U);
    EXPECT_EQ(mesh->mNumFaces, 3U);
    EXPECT_TRUE(mesh->HasVertexColors(0));

    const aiVector3D *const vertices = mesh->mVertices;
    const aiColor4D *const colors = mesh->mColors[0];
    EXPECT_EQ(aiVector3D(0.0f, 0.0f, 0.0f), vertices[0]);
    EXPECT_EQ(aiColor4D(0.0f, 0.0f, 0.0f, 1.0f), colors[0]);
    EXPECT_EQ(aiVector3D(0.0f, 0.0f, 1.0f), vertices[1]);
    EXPECT_EQ(aiColor4D(0.0f, 0.0f, 1.0f, 1.0f), colors[1]);
    EXPECT_EQ(aiVector3D(0.0f, 1.0f, 0.0f), vertices[2]);
    EXPECT_EQ(aiColor4D(0.0f, 0.0f, 0.0f, 1.0f), colors[2]);
    EXPECT_EQ(aiVector3D(0.0f, 0.0f, 0.0f), vertices[3]);
    EXPECT_EQ(aiColor4D(0.0f, 0.0f, 0.0f, 1.0f), colors[3]);
    EXPECT_EQ(aiVector3D(1.0f, 0.0f, 0.0f), vertices[4]);
    EXPECT_EQ(aiColor4D(1.0f, 0.6f, 0.3f, 1.0f), colors[4]);
    EXPECT_EQ(aiVector3D(0.0f, 1.0f, 0.0f), vertices[5]);
    EXPECT_EQ(aiColor4D(0.0f, 0.0f, 0.0f, 1.0f), colors[5]);
    EXPECT_EQ(aiVector3D(0.0f, 0.0f, 1.0f), vertices[6]);
    EXPECT_EQ(aiColor4D(0.0f, 0.0f, 1.0f, 1.0f), colors[6]);
    EXPECT_EQ(aiVector3D(1.0f, 1.0f, 0.0f), vertices[7]);
    EXPECT_EQ(aiColor4D(0.0f, 0.0f, 0.0f, 1.0f), colors[7]);
    EXPECT_EQ(aiVector3D(1.0f, 0.0f, 0.0f), vertices[8]);
    EXPECT_EQ(aiColor4D(1.0f, 0.6f, 0.3f, 1.0f), colors[8]);

#ifndef ASSIMP_BUILD_NO_EXPORT
    ::Assimp::Exporter exporter;
    EXPECT_EQ(aiReturn_SUCCESS, exporter.Export(scene, "obj", ASSIMP_TEST_MODELS_DIR "/OBJ/test_out.obj"));
#endif // ASSIMP_BUILD_NO_EXPORT
}

TEST_F(utObjImportExport, no_vertex_colors_Test) {
    ::Assimp::Importer importer;
    const aiScene *const scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OBJ/box.obj", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);

    EXPECT_EQ(scene->mNumMeshes, 1U);
    const aiMesh *const mesh = scene->mMeshes[0];
    EXPECT_FALSE(mesh->HasVertexColors(0));
}

TEST_F(utObjImportExport, issue1453_segfault) {
    static const char *curObjModel =
            "v  0.0  0.0  0.0\n"
            "v  0.0  0.0  1.0\n"
            "v  0.0  1.0  0.0\n"
            "v  0.0  1.0  1.0\n"
            "v  1.0  0.0  0.0\n"
            "v  1.0  0.0  1.0\n"
            "v  1.0  1.0  0.0\n"
            "v  1.0  1.0  1.0\nB";

    Assimp::Importer myimporter;
    const aiScene *scene = myimporter.ReadFileFromMemory(curObjModel, strlen(curObjModel), aiProcess_ValidateDataStructure);
    EXPECT_EQ(nullptr, scene);
}

TEST_F(utObjImportExport, relative_indices_Test) {
    static const char *curObjModel =
            "v -0.500000 0.000000 0.400000\n"
            "v -0.500000 0.000000 -0.800000\n"
            "v -0.500000 1.000000 -0.800000\n"
            "v -0.500000 1.000000 0.400000\n"
            "f -4 -3 -2 -1\nB";

    Assimp::Importer myimporter;
    const aiScene *scene = myimporter.ReadFileFromMemory(curObjModel, strlen(curObjModel), aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);

    EXPECT_EQ(scene->mNumMeshes, 1U);
    const aiMesh *mesh = scene->mMeshes[0];
    EXPECT_EQ(mesh->mNumVertices, 4U);
    EXPECT_EQ(mesh->mNumFaces, 1U);
    const aiFace face = mesh->mFaces[0];
    EXPECT_EQ(face.mNumIndices, 4U);
    for (unsigned int i = 0; i < face.mNumIndices; ++i) {
        EXPECT_EQ(face.mIndices[i], i);
    }
}

TEST_F(utObjImportExport, homogeneous_coordinates_Test) {
    static const char *curObjModel =
            "v -0.500000 0.000000 0.400000 0.50000\n"
            "v -0.500000 0.000000 -0.800000 1.00000\n"
            "v 0.500000 1.000000 -0.800000 0.5000\n"
            "f 1 2 3\nB";

    Assimp::Importer myimporter;
    const aiScene *scene = myimporter.ReadFileFromMemory(curObjModel, strlen(curObjModel), aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);

    EXPECT_EQ(scene->mNumMeshes, 1U);
    const aiMesh *mesh = scene->mMeshes[0];
    EXPECT_EQ(mesh->mNumVertices, 3U);
    EXPECT_EQ(mesh->mNumFaces, 1U);
    const aiFace face = mesh->mFaces[0];
    EXPECT_EQ(face.mNumIndices, 3U);
    const aiVector3D vertice = mesh->mVertices[0];
    EXPECT_EQ(vertice.x, -1.0f);
    EXPECT_EQ(vertice.y, 0.0f);
    EXPECT_EQ(vertice.z, 0.8f);
}

TEST_F(utObjImportExport, homogeneous_coordinates_divide_by_zero_Test) {
    static const char *curObjModel =
            "v -0.500000 0.000000 0.400000 0.\n"
            "v -0.500000 0.000000 -0.800000 1.00000\n"
            "v 0.500000 1.000000 -0.800000 0.5000\n"
            "f 1 2 3\nB";

    Assimp::Importer myimporter;
    const aiScene *scene = myimporter.ReadFileFromMemory(curObjModel, std::strlen(curObjModel), aiProcess_ValidateDataStructure);
    EXPECT_EQ(nullptr, scene);
}

TEST_F(utObjImportExport, 0based_array_Test) {
    static const char *curObjModel =
            "v -0.500000 0.000000 0.400000\n"
            "v -0.500000 0.000000 -0.800000\n"
            "v -0.500000 1.000000 -0.800000\n"
            "f 0 1 2\nB";

    Assimp::Importer myImporter;
    const aiScene *scene = myImporter.ReadFileFromMemory(curObjModel, strlen(curObjModel), 0);
    EXPECT_EQ(nullptr, scene);
}

TEST_F(utObjImportExport, invalid_normals_uvs) {
    static const char *curObjModel =
            "v -0.500000 0.000000 0.400000\n"
            "v -0.500000 0.000000 -0.800000\n"
            "v -0.500000 1.000000 -0.800000\n"
            "vt 0 0\n"
            "vn 0 1 0\n"
            "f 1/1/1 1/1/1 2/2/2\nB";

    Assimp::Importer myImporter;
    const aiScene *scene = myImporter.ReadFileFromMemory(curObjModel, strlen(curObjModel), 0);
    EXPECT_NE(nullptr, scene);
}

TEST_F(utObjImportExport, no_vt_just_vns) {
    static const char *curObjModel =
            "v 0 0 0\n"
            "v 0 0 0\n"
            "v 0 0 0\n"
            "v 0 0 0\n"
            "v 0 0 0\n"
            "v 0 0 0\n"
            "v 0 0 0\n"
            "v 0 0 0\n"
            "v 0 0 0\n"
            "v 0 0 0\n"
            "v 10 0 0\n"
            "v 0 10 0\n"
            "vn 0 0 1\n"
            "vn 0 0 1\n"
            "vn 0 0 1\n"
            "vn 0 0 1\n"
            "vn 0 0 1\n"
            "vn 0 0 1\n"
            "vn 0 0 1\n"
            "vn 0 0 1\n"
            "vn 0 0 1\n"
            "vn 0 0 1\n"
            "vn 0 0 1\n"
            "vn 0 0 1\n"
            "f 10/10 11/11 12/12\n";

    Assimp::Importer myImporter;
    const aiScene *scene = myImporter.ReadFileFromMemory(curObjModel, strlen(curObjModel), 0);
    EXPECT_NE(nullptr, scene);
}

TEST_F(utObjImportExport, mtllib_after_g) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OBJ/cube_mtllib_after_g.obj", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);

    EXPECT_EQ(scene->mNumMeshes, 1U);
    const aiMesh *mesh = scene->mMeshes[0];
    const aiMaterial *mat = scene->mMaterials[mesh->mMaterialIndex];
    aiString name;
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_NAME, name));
    EXPECT_STREQ("MyMaterial", name.C_Str());
}

TEST_F(utObjImportExport, import_point_cloud) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OBJ/point_cloud.obj", 0);
    ASSERT_NE(nullptr, scene);
}

TEST_F(utObjImportExport, import_without_linend) {
    Assimp::Importer myImporter;
    const aiScene *scene = myImporter.ReadFile(ASSIMP_TEST_MODELS_DIR "/OBJ/box_without_lineending.obj", 0);
    ASSERT_NE(nullptr, scene);
}

TEST_F(utObjImportExport, import_with_line_continuations) {
    static const char *curObjModel =
            "v -0.5 -0.5 0.5\n"
            "v -0.5 \\\n"
            "  -0.5 -0.5\n"
            "v -0.5 \\\n"
            "   0.5 \\\n"
            "   -0.5\n"
            "f 1 2 3\n";

    Assimp::Importer myImporter;
    const aiScene *scene = myImporter.ReadFileFromMemory(curObjModel, strlen(curObjModel), 0);
    EXPECT_NE(nullptr, scene);

    EXPECT_EQ(scene->mNumMeshes, 1U);
    EXPECT_EQ(scene->mMeshes[0]->mNumVertices, 3U);
    EXPECT_EQ(scene->mMeshes[0]->mNumFaces, 1U);

    auto vertices = scene->mMeshes[0]->mVertices;
    const float threshold = 0.0001f;

    EXPECT_NEAR(vertices[0].x, -0.5f, threshold);
    EXPECT_NEAR(vertices[0].y, -0.5f, threshold);
    EXPECT_NEAR(vertices[0].z, 0.5f, threshold);

    EXPECT_NEAR(vertices[1].x, -0.5f, threshold);
    EXPECT_NEAR(vertices[1].y, -0.5f, threshold);
    EXPECT_NEAR(vertices[1].z, -0.5f, threshold);

    EXPECT_NEAR(vertices[2].x, -0.5f, threshold);
    EXPECT_NEAR(vertices[2].y, 0.5f, threshold);
    EXPECT_NEAR(vertices[2].z, -0.5f, threshold);
}

TEST_F(utObjImportExport, issue2355_mtl_texture_prefix) {
    ::Assimp::Importer importer;
    const aiScene *const scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OBJ/mtl_different_folder.obj", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);

    EXPECT_EQ(scene->mNumMaterials, 2U);
    const aiMaterial *const material = scene->mMaterials[1];

    aiString texturePath;
    material->GetTexture(aiTextureType_DIFFUSE, 0, &texturePath);
    // The MTL file is in `folder`, the image path should have been prefixed with the folder
    EXPECT_STREQ("folder/image.jpg", texturePath.C_Str());
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

constexpr const char *kFixture = ASSIMP_TEST_MODELS_DIR "/OBJ/box.obj";

} // namespace

TEST_F(utObjImportExport, contractDefaultsAreMetersAndYUp) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(kFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_DOUBLE_EQ(1.0, readUnitScale(scene));
    EXPECT_EQ(1, readUpAxis(scene));
}

TEST_F(utObjImportExport, contractUnitScaleOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_IMPORT_OBJ_UNIT_SCALE_TO_METERS, 0.001f);
    const aiScene *scene = importer.ReadFile(kFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_NEAR(0.001, readUnitScale(scene), 1e-6);
}

TEST_F(utObjImportExport, contractUpAxisOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_OBJ_UP_AXIS, 2);
    const aiScene *scene = importer.ReadFile(kFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_EQ(2, readUpAxis(scene));
}

TEST_F(utObjImportExport, contractInvalidUpAxisOverrideFailsImport) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_OBJ_UP_AXIS, 7);
    const aiScene *scene = importer.ReadFile(kFixture, 0);
    EXPECT_EQ(nullptr, scene);
    const std::string errorString = importer.GetErrorString();
    EXPECT_NE(std::string::npos, errorString.find("IMPORT_OBJ_UP_AXIS"));
}

#ifndef ASSIMP_BUILD_NO_EXPORT

namespace {

aiScene *makeObjBoxScene(const aiVector3D &halfExtents) {
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

aiVector3D meshExtentObj(const aiMesh *mesh) {
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
// Unit/axis contract: OBJ exporter target = 1.0 m + Y-up. OBJ is unitless and
// axis-less by spec, but the importer normalises absent declarations to (1.0
// m, Y-up) — the de-facto Wavefront convention; aligning the exporter target
// with the importer default makes same-format round-trips bitwise identity.
// Behaviour gated on `AI_METADATA_UNIT_SCALE_TO_METERS` so legacy callers
// stay byte-identical.
// -----------------------------------------------------------------------------

TEST_F(utObjImportExport, exportOBJBakesUnitScaleWhenSourceUnitDiffersFromMeters) {
    aiScene *scene = makeObjBoxScene(aiVector3D(0.5f, 0.5f, 0.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1e-3); // mm
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "obj", "ut_obj_unit_mm.obj"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_obj_unit_mm.obj", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentObj(roundtrip->mMeshes[0]);
    EXPECT_NEAR(1e-3f, extent.x, 1e-6f);
    EXPECT_NEAR(1e-3f, extent.y, 1e-6f);
    EXPECT_NEAR(1e-3f, extent.z, 1e-6f);
    std::remove("ut_obj_unit_mm.obj");
    std::remove("ut_obj_unit_mm.obj.mtl");
}

TEST_F(utObjImportExport, exportOBJBakesAxisRotationWhenSourceIsZUp) {
    aiScene *scene = makeObjBoxScene(aiVector3D(1.0f, 1.0f, 5.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "obj", "ut_obj_axis_zup.obj"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_obj_axis_zup.obj", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentObj(roundtrip->mMeshes[0]);
    EXPECT_NEAR(2.0f, extent.x, 1e-4f);
    EXPECT_NEAR(10.0f, extent.y, 1e-4f);
    EXPECT_NEAR(2.0f, extent.z, 1e-4f);
    std::remove("ut_obj_axis_zup.obj");
    std::remove("ut_obj_axis_zup.obj.mtl");
}

TEST_F(utObjImportExport, exportOBJIsIdentityWhenSourceAlreadyMetersYUp) {
    aiScene *scene = makeObjBoxScene(aiVector3D(2.5f, 7.5f, 2.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "obj", "ut_obj_identity.obj"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_obj_identity.obj", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentObj(roundtrip->mMeshes[0]);
    EXPECT_NEAR(5.0f, extent.x, 1e-5f);
    EXPECT_NEAR(15.0f, extent.y, 1e-5f);
    EXPECT_NEAR(5.0f, extent.z, 1e-5f);
    std::remove("ut_obj_identity.obj");
    std::remove("ut_obj_identity.obj.mtl");
}

TEST_F(utObjImportExport, exportOBJIsIdentityWhenContractMetadataAbsent) {
    aiScene *scene = makeObjBoxScene(aiVector3D(3.0f, 3.0f, 3.0f));
    ASSERT_EQ(nullptr, scene->mMetaData);

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "obj", "ut_obj_no_meta.obj"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_obj_no_meta.obj", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentObj(roundtrip->mMeshes[0]);
    EXPECT_NEAR(6.0f, extent.x, 1e-5f);
    EXPECT_NEAR(6.0f, extent.y, 1e-5f);
    EXPECT_NEAR(6.0f, extent.z, 1e-5f);
    std::remove("ut_obj_no_meta.obj");
    std::remove("ut_obj_no_meta.obj.mtl");
}

TEST_F(utObjImportExport, exportOBJNoMtlHonorsContractTransform) {
    // The "objnomtl" exporter shares the bake — guarantee both write paths
    // produce the same transformed geometry.
    aiScene *scene = makeObjBoxScene(aiVector3D(1.0f, 1.0f, 5.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "objnomtl", "ut_obj_axis_zup_nomtl.obj"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_obj_axis_zup_nomtl.obj", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtentObj(roundtrip->mMeshes[0]);
    EXPECT_NEAR(2.0f, extent.x, 1e-4f);
    EXPECT_NEAR(10.0f, extent.y, 1e-4f);
    EXPECT_NEAR(2.0f, extent.z, 1e-4f);
    std::remove("ut_obj_axis_zup_nomtl.obj");
}

#endif // ASSIMP_BUILD_NO_EXPORT
