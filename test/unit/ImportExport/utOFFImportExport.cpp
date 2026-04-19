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
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>

#include <cstdint>
#include <string>

class utOFFImportExport : public AbstractImportExportBase {
protected:
    virtual bool importerTest() {
        ::Assimp::Importer importer;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OFF/Cube.off", aiProcess_ValidateDataStructure);
        return nullptr != scene;
    }
};

TEST_F(utOFFImportExport, importOFFFromFileTest) {
    EXPECT_TRUE(importerTest());
}

TEST_F(utOFFImportExport, contractDefaultsAreMetersAndYUp) {
    ::Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OFF/Cube.off", 0);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMetaData);

    double meters = 0.0;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, meters));
    EXPECT_NEAR(1.0, meters, 1e-9) << "OFF default unit must be meters (matches PLY/glTF baseline)";

    int32_t upAxis = -1;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, upAxis));
    EXPECT_EQ(1, upAxis) << "OFF default up-axis must be Y (matches glTF/PLY)";

    aiString sourceFormat;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_SOURCE_FORMAT, sourceFormat));
    EXPECT_STREQ("OFF Importer", sourceFormat.C_Str());
}

TEST_F(utOFFImportExport, contractUnitScaleOverrideRespected) {
    ::Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_IMPORT_OFF_UNIT_SCALE_TO_METERS, 0.001f);
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OFF/Cube.off", 0);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMetaData);

    double meters = 0.0;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, meters));
    EXPECT_NEAR(0.001, meters, 1e-9);
}

TEST_F(utOFFImportExport, contractUpAxisOverrideRespected) {
    ::Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_OFF_UP_AXIS, 2); // force Z-up
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OFF/Cube.off", 0);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMetaData);

    int32_t upAxis = -1;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, upAxis));
    EXPECT_EQ(2, upAxis);
}

TEST_F(utOFFImportExport, contractInvalidUpAxisOverrideFailsImport) {
    ::Assimp::Importer importer;
    // -1 collides with Importer::GetPropertyInteger's "not set" sentinel, so
    // the resolver treats it as absent. Out-of-range positive values are the
    // real failure surface — use 7.
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_OFF_UP_AXIS, 7);
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OFF/Cube.off", 0);
    EXPECT_EQ(nullptr, scene);
    EXPECT_NE(std::string::npos, std::string(importer.GetErrorString()).find("IMPORT_OFF_UP_AXIS"));
}
