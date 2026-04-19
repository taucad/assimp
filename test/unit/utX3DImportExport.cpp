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
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>

using namespace Assimp;

class utX3DImportExport : public AbstractImportExportBase {
public:
    // All tests implemented as individual TEST_F functions below
};

TEST_F(utX3DImportExport, importX3DFromFileTest) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X3D/HelloX3dTrademark.x3d", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    ASSERT_EQ(1u, scene->mNumMeshes);
}

TEST_F(utX3DImportExport, importX3DIndexedLineSet) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X3D/IndexedLineSet.x3d", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    ASSERT_EQ(scene->mNumMeshes, 1u);
    ASSERT_EQ(scene->mMeshes[0]->mNumFaces, 4u);
    ASSERT_EQ(scene->mMeshes[0]->mPrimitiveTypes, aiPrimitiveType_LINE);
    ASSERT_EQ(scene->mMeshes[0]->mNumVertices, 4u);
    for (unsigned int i = 0; i < scene->mMeshes[0]->mNumFaces; i++) {
        ASSERT_EQ(scene->mMeshes[0]->mFaces[i].mNumIndices, 2u);
    }
}

TEST_F(utX3DImportExport, importX3DComputerKeyboard) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X3D/ComputerKeyboard.x3d", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    // TODO: CHANGE INCORRECT VALUE WHEN IMPORTER FIXED
    //   As noted in assimp issue 4992, X3D importer was severely broken with 5 Oct 2020 commit 3b9d4cf.
    //   ComputerKeyboard.x3d should have 100 meshes but broken importer only has 4
    ASSERT_EQ(4u, scene->mNumMeshes);  // Incorrect value from currently broken importer
    ASSERT_NE(100u, scene->mNumMeshes); // Correct value, to be restored when importer fixed
}

TEST_F(utX3DImportExport, importX3DChevyTahoe) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_NONBSD_DIR "/X3D/Chevy/ChevyTahoe.x3d", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    // TODO: CHANGE INCORRECT VALUE WHEN IMPORTER FIXED
    //   As noted in assimp issue 4992, X3D importer was severely broken with 5 Oct 2020 commit 3b9d4cf.
    //   ChevyTahoe.x3d should have 20 meshes but broken importer only has 19
    ASSERT_EQ(19u, scene->mNumMeshes); // Incorrect value from currently broken importer
    ASSERT_NE(20u, scene->mNumMeshes); // Correct value, to be restored when importer fixed
}

// X3DB format tests
TEST_F(utX3DImportExport, importX3DBHelloWorld) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X3DB/HelloWorld.x3db", aiProcess_ValidateDataStructure);
    // X3DB (binary X3D) format is not currently supported by assimp X3D importer
    // The file is recognized but fails to parse due to lack of binary X3D decoder
    ASSERT_EQ(nullptr, scene);
    
    // Verify the importer recognizes the file extension but fails parsing
    std::string error = importer.GetErrorString();
    ASSERT_FALSE(error.empty());
}

// X3DV format tests (Classic VRML)
TEST_F(utX3DImportExport, importX3DVHelloWorld) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X3DV/HelloWorld.x3dv", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    ASSERT_EQ(1u, scene->mNumMeshes);
}

// WRL format tests (VRML97)
TEST_F(utX3DImportExport, importWRLHelloWorld) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/WRL/HelloWorld.wrl", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    ASSERT_EQ(1u, scene->mNumMeshes);
}

TEST_F(utX3DImportExport, importWRLMotionCaptureROM) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/WRL/MotionCaptureROM.WRL", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    ASSERT_EQ(24u, scene->mNumMeshes);
}

TEST_F(utX3DImportExport, importWRLWuson) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/WRL/Wuson.wrl", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    ASSERT_EQ(1u, scene->mNumMeshes);
}

// -----------------------------------------------------------------------------
// Unit / axis contract tests
// -----------------------------------------------------------------------------

namespace {

constexpr const char *kX3dWithCentimeterUnit =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<X3D profile='Immersive' version='3.3'>"
        "  <head>"
        "    <unit category='length' name='centimeter' conversionFactor='0.01'/>"
        "  </head>"
        "  <Scene>"
        "    <Shape>"
        "      <Box size='1 1 1'/>"
        "    </Shape>"
        "  </Scene>"
        "</X3D>";

constexpr const char *kX3dWithoutUnit =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<X3D profile='Immersive' version='3.3'>"
        "  <head/>"
        "  <Scene>"
        "    <Shape>"
        "      <Box size='1 1 1'/>"
        "    </Shape>"
        "  </Scene>"
        "</X3D>";

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

} // namespace

TEST_F(utX3DImportExport, contractDefaultsAreMetersAndYUpWhenUnitAbsent) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFileFromMemory(
            kX3dWithoutUnit, strlen(kX3dWithoutUnit), 0, "x3d");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_DOUBLE_EQ(1.0, readUnitScale(scene));
    EXPECT_EQ(1, readUpAxis(scene));
}

TEST_F(utX3DImportExport, contractReadsCentimeterUnitFromUnitStatement) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFileFromMemory(
            kX3dWithCentimeterUnit, strlen(kX3dWithCentimeterUnit), 0, "x3d");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    // X3D `<unit conversionFactor>` is parsed via fast_atof's float pipeline,
    // so we tolerate float-precision drift on conversion to double.
    EXPECT_NEAR(0.01, readUnitScale(scene), 1e-6);
    EXPECT_EQ(1, readUpAxis(scene));
}

TEST_F(utX3DImportExport, contractUnitScaleOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_IMPORT_X3D_UNIT_SCALE_TO_METERS, 0.0254f);
    const aiScene *scene = importer.ReadFileFromMemory(
            kX3dWithCentimeterUnit, strlen(kX3dWithCentimeterUnit), 0, "x3d");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    // Override wins over the file-declared centimeter unit. Stored as ai_real
    // (float) at the property layer, so compare with float-grade tolerance.
    EXPECT_NEAR(0.0254, readUnitScale(scene), 1e-6);
}

TEST_F(utX3DImportExport, contractUpAxisOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_X3D_UP_AXIS, 2);
    const aiScene *scene = importer.ReadFileFromMemory(
            kX3dWithoutUnit, strlen(kX3dWithoutUnit), 0, "x3d");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_EQ(2, readUpAxis(scene));
}

TEST_F(utX3DImportExport, contractInvalidUpAxisOverrideFailsImport) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_X3D_UP_AXIS, 7);
    const aiScene *scene = importer.ReadFileFromMemory(
            kX3dWithoutUnit, strlen(kX3dWithoutUnit), 0, "x3d");
    EXPECT_EQ(nullptr, scene);
    const std::string errorString = importer.GetErrorString();
    EXPECT_NE(std::string::npos, errorString.find("IMPORT_X3D_UP_AXIS"));
}

#if !defined(ASSIMP_BUILD_NO_VRML_IMPORTER)
// VRML / X3DV files share the X3D importer chain via VrmlConverter, so the
// same Y-up + 1.0 m contract defaults apply. These tests only run when the
// VRML importer is enabled in the build.
TEST_F(utX3DImportExport, contractWritesContractForX3DVHelloWorld) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/X3DV/HelloWorld.x3dv", 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_DOUBLE_EQ(1.0, readUnitScale(scene));
    EXPECT_EQ(1, readUpAxis(scene));
}

TEST_F(utX3DImportExport, contractWritesContractForWRLHelloWorld) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/WRL/HelloWorld.wrl", 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_DOUBLE_EQ(1.0, readUnitScale(scene));
    EXPECT_EQ(1, readUpAxis(scene));
}
#endif

#if !defined(ASSIMP_BUILD_NO_X3D_EXPORTER) && !defined(ASSIMP_BUILD_NO_EXPORT)

namespace {

aiScene *makeX3DBoxScene(const aiVector3D &halfExtents) {
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
    for (size_t i = 0; i < v.size(); ++i) {
        mesh->mVertices[i] = v[i];
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

// Locate the first space-separated extent (max - min) along the supplied
// axis index in the on-disk X3D file. The exporter writes Coordinate
// `point=` triplets that we can scan textually — round-tripping through the
// X3D *importer* is too lossy for axis-bake assertions on this format.
aiVector3D scanX3DExtent(const std::string &path) {
    std::ifstream in(path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    aiVector3D mn(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max());
    aiVector3D mx(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                  std::numeric_limits<float>::lowest());
    // X3DExporter::NodeHelper_OpenNode writes attributes as `name='value'`
    // (single-quoted), so anchor on that exact form rather than the more
    // common XML double-quoted convention. Scan every Coordinate node so a
    // multi-mesh scene aggregates correctly into a single bounding extent.
    const std::string anchor = "point='";
    size_t cursor = 0;
    bool found = false;
    while (true) {
        size_t pos = content.find(anchor, cursor);
        if (pos == std::string::npos) break;
        pos += anchor.size();
        const size_t end = content.find('\'', pos);
        if (end == std::string::npos) break;
        std::istringstream iss(content.substr(pos, end - pos));
        float x, y, z;
        while (iss >> x >> y >> z) {
            mn.x = std::min(mn.x, x);
            mn.y = std::min(mn.y, y);
            mn.z = std::min(mn.z, z);
            mx.x = std::max(mx.x, x);
            mx.y = std::max(mx.y, y);
            mx.z = std::max(mx.z, z);
            found = true;
        }
        cursor = end + 1;
    }
    if (!found) return aiVector3D(0, 0, 0);
    return aiVector3D(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
}

} // namespace

TEST_F(utX3DImportExport, exportX3DBakesAxisRotationWhenSourceIsZUp) {
    // Tall box on +Z (Z-up source) -> after Z->Y bake the on-disk vertex
    // table must report the tall axis on +Y (X3D canonical Y-up).
    aiScene *scene = makeX3DBoxScene(aiVector3D(1.0f, 1.0f, 5.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0); // metres
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "x3d", "ut_x3d_axis_zup.x3d"));
    delete scene;

    aiVector3D extent = scanX3DExtent("ut_x3d_axis_zup.x3d");
    EXPECT_NEAR(2.0f, extent.x, 1e-3f);
    EXPECT_NEAR(10.0f, extent.y, 1e-3f);
    EXPECT_NEAR(2.0f, extent.z, 1e-3f);
    std::remove("ut_x3d_axis_zup.x3d");
}

TEST_F(utX3DImportExport, exportX3DIsIdentityWhenSourceAlreadyMetersYUp) {
    aiScene *scene = makeX3DBoxScene(aiVector3D(2.5f, 7.5f, 2.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "x3d", "ut_x3d_identity.x3d"));
    delete scene;

    aiVector3D extent = scanX3DExtent("ut_x3d_identity.x3d");
    EXPECT_NEAR(5.0f, extent.x, 1e-4f);
    EXPECT_NEAR(15.0f, extent.y, 1e-4f);
    EXPECT_NEAR(5.0f, extent.z, 1e-4f);
    std::remove("ut_x3d_identity.x3d");
}

TEST_F(utX3DImportExport, exportX3DIsIdentityWhenContractMetadataAbsent) {
    aiScene *scene = makeX3DBoxScene(aiVector3D(3.0f, 3.0f, 3.0f));
    ASSERT_EQ(nullptr, scene->mMetaData);

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "x3d", "ut_x3d_no_meta.x3d"));
    delete scene;

    aiVector3D extent = scanX3DExtent("ut_x3d_no_meta.x3d");
    EXPECT_NEAR(6.0f, extent.x, 1e-4f);
    EXPECT_NEAR(6.0f, extent.y, 1e-4f);
    EXPECT_NEAR(6.0f, extent.z, 1e-4f);
    std::remove("ut_x3d_no_meta.x3d");
}

#endif
