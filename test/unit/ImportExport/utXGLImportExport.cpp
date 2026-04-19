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
#include <assimp/commonMetaData.h>
#include <assimp/config.h>
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>

using namespace Assimp;

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

constexpr const char *kFixture = ASSIMP_TEST_MODELS_DIR "/XGL/sample_official.xgl";

} // namespace

TEST(utXGLImporter, importBCN_Epileptic) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/XGL/BCN_Epileptic.zgl", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXGLImporter, importCubesWithAlpha) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/XGL/cubes_with_alpha.zgl", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXGLImporter, importSample_official) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/XGL/sample_official.xgl", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXGLImporter, importSample_official_asxml) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/XGL/sample_official_asxml.xml", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXGLImporter, importSphereWithMatGloss) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/XGL/sphere_with_mat_gloss_10pc.zgl", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXGLImporter, importSpiderASCII) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/XGL/Spider_ascii.zgl", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXGLImporter, importWuson) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/XGL/Wuson.zgl", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

TEST(utXGLImporter, importWusonDXF) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/XGL/wuson_dxf.zgl", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
}

// -----------------------------------------------------------------------------
// Unit / axis contract tests
// -----------------------------------------------------------------------------

TEST(utXGLImporter, contractDefaultsAreMetersAndYUp) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(kFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_DOUBLE_EQ(1.0, readUnitScale(scene));
    EXPECT_EQ(1, readUpAxis(scene));
}

TEST(utXGLImporter, contractUnitScaleOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_IMPORT_XGL_UNIT_SCALE_TO_METERS, 0.001f);
    const aiScene *scene = importer.ReadFile(kFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_NEAR(0.001, readUnitScale(scene), 1e-6);
}

TEST(utXGLImporter, contractUpAxisOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_XGL_UP_AXIS, 2);
    const aiScene *scene = importer.ReadFile(kFixture, 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_EQ(2, readUpAxis(scene));
}

TEST(utXGLImporter, contractInvalidUpAxisOverrideFailsImport) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_XGL_UP_AXIS, 7);
    const aiScene *scene = importer.ReadFile(kFixture, 0);
    EXPECT_EQ(nullptr, scene);
    const std::string errorString = importer.GetErrorString();
    EXPECT_NE(std::string::npos, errorString.find("IMPORT_XGL_UP_AXIS"));
}
