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

#include <assimp/commonMetaData.h>
#include <assimp/config.h>
#include <assimp/metadata.h>
#include <assimp/scene.h>
#include "UnitTestPCH.h"

#include <assimp/Importer.hpp>

#include <cstring>

using namespace Assimp;

class utOpenGEXImportExport : public AbstractImportExportBase {
public:
    bool importerTest() override {
        Assimp::Importer importer;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OpenGEX/Example.ogex", 0);
        EXPECT_EQ(1u, scene->mNumMeshes);
        return nullptr != scene;
    }
};

TEST_F(utOpenGEXImportExport, importOpenGexFromFileTest) {
    EXPECT_TRUE(importerTest());
}

TEST_F(utOpenGEXImportExport, Importissue1262_NoCrash) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OpenGEX/light_issue1262.ogex", 0);
    EXPECT_NE(nullptr, scene);
}

TEST_F(utOpenGEXImportExport, Importissue1340_EmptyCameraObject) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OpenGEX/empty_camera.ogex", 0);
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

constexpr const char *kFixtureZUp = ASSIMP_TEST_MODELS_DIR "/OpenGEX/Example.ogex";

// OpenGEX 2.0 §6 (Metric) declares both "distance" and "up". This in-memory
// fixture exercises the YDistance / Y-up branch that no on-disk model
// currently covers.
constexpr const char *kInMemoryYUpCentimeter =
    "Metric (key = \"distance\") {float {0.01}}\n"
    "Metric (key = \"angle\") {float {1.0}}\n"
    "Metric (key = \"time\") {float {1.0}}\n"
    "Metric (key = \"up\") {string {\"y\"}}\n"
    "\n"
    "GeometryNode $node1 {\n"
    "    Name {string {\"Tri\"}}\n"
    "    ObjectRef {ref {$geometry1}}\n"
    "}\n"
    "\n"
    "GeometryObject $geometry1 {\n"
    "    Mesh (primitive = \"triangles\") {\n"
    "        VertexArray (attrib = \"position\") {\n"
    "            float[3] { {0,0,0}, {1,0,0}, {0,1,0} }\n"
    "        }\n"
    "        IndexArray { unsigned_int32[3] { {0,1,2} } }\n"
    "    }\n"
    "}\n";

// Spec-default fixture: no Metric statements at all. Per OpenGEX 2.0 §6
// the producer-omitted defaults are distance=1.0 (metres) and up="z".
constexpr const char *kInMemoryNoMetrics =
    "GeometryNode $node1 {\n"
    "    Name {string {\"Tri\"}}\n"
    "    ObjectRef {ref {$geometry1}}\n"
    "}\n"
    "\n"
    "GeometryObject $geometry1 {\n"
    "    Mesh (primitive = \"triangles\") {\n"
    "        VertexArray (attrib = \"position\") {\n"
    "            float[3] { {0,0,0}, {1,0,0}, {0,1,0} }\n"
    "        }\n"
    "        IndexArray { unsigned_int32[3] { {0,1,2} } }\n"
    "    }\n"
    "}\n";

} // namespace

TEST_F(utOpenGEXImportExport, contractDefaultsToZUpAndOneMeter) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(kFixtureZUp, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_DOUBLE_EQ(1.0, readUnitScale(scene));
    EXPECT_EQ(2, readUpAxis(scene));
}

TEST_F(utOpenGEXImportExport, contractReadsDistanceAndUpFromMetricStatements) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFileFromMemory(
            kInMemoryYUpCentimeter, std::strlen(kInMemoryYUpCentimeter), 0, "ogex");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_NEAR(0.01, readUnitScale(scene), 1e-6);
    EXPECT_EQ(1, readUpAxis(scene));
}

TEST_F(utOpenGEXImportExport, contractFallsBackToSpecDefaultsWhenMetricsOmitted) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFileFromMemory(
            kInMemoryNoMetrics, std::strlen(kInMemoryNoMetrics), 0, "ogex");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_DOUBLE_EQ(1.0, readUnitScale(scene));
    EXPECT_EQ(2, readUpAxis(scene));
}

TEST_F(utOpenGEXImportExport, contractUnitScaleOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_IMPORT_OGEX_UNIT_SCALE_TO_METERS, 0.001f);
    const aiScene *scene = importer.ReadFile(kFixtureZUp, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_NEAR(0.001, readUnitScale(scene), 1e-6);
}

TEST_F(utOpenGEXImportExport, contractUpAxisOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_OGEX_UP_AXIS, 1);
    const aiScene *scene = importer.ReadFile(kFixtureZUp, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_EQ(1, readUpAxis(scene));
}

TEST_F(utOpenGEXImportExport, contractInvalidUpAxisOverrideFailsImport) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_OGEX_UP_AXIS, 7);
    const aiScene *scene = importer.ReadFile(kFixtureZUp, 0);
    EXPECT_EQ(nullptr, scene);
    const std::string errorString = importer.GetErrorString();
    EXPECT_NE(std::string::npos, errorString.find("IMPORT_OGEX_UP_AXIS"));
}
