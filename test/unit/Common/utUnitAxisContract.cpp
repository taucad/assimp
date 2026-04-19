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

#include "Common/UnitAxisContract.h"

#include <assimp/Exceptional.h>
#include <assimp/Importer.hpp>
#include <assimp/commonMetaData.h>
#include <assimp/metadata.h>
#include <assimp/scene.h>
#include <assimp/mesh.h>

#include <cmath>
#include <cstring>
#include <vector>

using namespace Assimp;

namespace {

// Construct a tiny scene with `numVertices` deterministic vertices so we can
// later assert exact transformation outcomes (no float drift in the source).
aiScene *makeUnitScene(unsigned int numVertices = 3) {
    auto *scene = new aiScene();
    scene->mNumMeshes = 1;
    scene->mMeshes = new aiMesh*[1];
    auto *mesh = new aiMesh();
    mesh->mNumVertices = numVertices;
    mesh->mVertices = new aiVector3D[numVertices];
    mesh->mNormals = new aiVector3D[numVertices];
    for (unsigned int i = 0; i < numVertices; ++i) {
        const float f = static_cast<float>(i + 1);
        mesh->mVertices[i] = aiVector3D(f, f * 2.0f, f * 3.0f);
        mesh->mNormals[i] = aiVector3D(1.0f, 0.0f, 0.0f);
    }
    scene->mMeshes[0] = mesh;
    scene->mRootNode = new aiNode();
    return scene;
}

// True iff every byte of every vertex matches the snapshot byte-for-byte.
bool verticesAreBitwiseEqual(const aiMesh *mesh, const std::vector<aiVector3D> &snapshot) {
    if (mesh->mNumVertices != snapshot.size()) return false;
    return std::memcmp(mesh->mVertices, snapshot.data(),
                       sizeof(aiVector3D) * snapshot.size()) == 0;
}

std::vector<aiVector3D> snapshotVertices(const aiMesh *mesh) {
    return std::vector<aiVector3D>(mesh->mVertices, mesh->mVertices + mesh->mNumVertices);
}

} // anonymous namespace

class utUnitAxisContract : public ::testing::Test {
};

// ============================================================================
// validateUpAxisInt
// ============================================================================

TEST_F(utUnitAxisContract, validateUpAxisIntAcceptsAllThreeAxes) {
    EXPECT_EQ(0, validateUpAxisInt(0, "test"));
    EXPECT_EQ(1, validateUpAxisInt(1, "test"));
    EXPECT_EQ(2, validateUpAxisInt(2, "test"));
}

TEST_F(utUnitAxisContract, validateUpAxisIntRejectsNegative) {
    try {
        validateUpAxisInt(-1, "TestProperty");
        FAIL() << "Expected DeadlyExportError";
    } catch (const DeadlyExportError &e) {
        std::string msg = e.what();
        EXPECT_NE(std::string::npos, msg.find("TestProperty"));
        EXPECT_NE(std::string::npos, msg.find("-1"));
        EXPECT_NE(std::string::npos, msg.find("[0, 2]"));
    }
}

TEST_F(utUnitAxisContract, validateUpAxisIntRejectsOutOfRangePositive) {
    try {
        validateUpAxisInt(7, "TestProperty");
        FAIL() << "Expected DeadlyExportError";
    } catch (const DeadlyExportError &e) {
        std::string msg = e.what();
        EXPECT_NE(std::string::npos, msg.find("TestProperty"));
        EXPECT_NE(std::string::npos, msg.find("7"));
    }
}

// ============================================================================
// buildAxisRotationMatrix — table-driven over all 9 axis pairs
// ============================================================================

TEST_F(utUnitAxisContract, buildAxisRotationMatrixIdentityForSameAxes) {
    for (int32_t axis = 0; axis < 3; ++axis) {
        aiMatrix4x4 m = buildAxisRotationMatrix(axis, axis);
        EXPECT_TRUE(isApproxIdentity(m)) << "axis " << axis;
    }
}

TEST_F(utUnitAxisContract, buildAxisRotationMatrixYUpToZUpMapsAxesCorrectly) {
    // Y-up to Z-up: +Y maps to +Z, +Z maps to -Y, +X stays.
    aiMatrix4x4 m = buildAxisRotationMatrix(1, 2);
    aiVector3D x(1, 0, 0); applyLinearTransform(m, x);
    aiVector3D y(0, 1, 0); applyLinearTransform(m, y);
    aiVector3D z(0, 0, 1); applyLinearTransform(m, z);
    EXPECT_NEAR(1.0f, x.x, 1e-6f); EXPECT_NEAR(0.0f, x.y, 1e-6f); EXPECT_NEAR(0.0f, x.z, 1e-6f);
    EXPECT_NEAR(0.0f, y.x, 1e-6f); EXPECT_NEAR(0.0f, y.y, 1e-6f); EXPECT_NEAR(1.0f, y.z, 1e-6f);
    EXPECT_NEAR(0.0f, z.x, 1e-6f); EXPECT_NEAR(-1.0f, z.y, 1e-6f); EXPECT_NEAR(0.0f, z.z, 1e-6f);
}

TEST_F(utUnitAxisContract, buildAxisRotationMatrixZUpToYUpMapsAxesCorrectly) {
    // Z-up to Y-up: +Z maps to +Y, +Y maps to -Z, +X stays.
    aiMatrix4x4 m = buildAxisRotationMatrix(2, 1);
    aiVector3D x(1, 0, 0); applyLinearTransform(m, x);
    aiVector3D y(0, 1, 0); applyLinearTransform(m, y);
    aiVector3D z(0, 0, 1); applyLinearTransform(m, z);
    EXPECT_NEAR(1.0f, x.x, 1e-6f); EXPECT_NEAR(0.0f, x.y, 1e-6f); EXPECT_NEAR(0.0f, x.z, 1e-6f);
    EXPECT_NEAR(0.0f, y.x, 1e-6f); EXPECT_NEAR(0.0f, y.y, 1e-6f); EXPECT_NEAR(-1.0f, y.z, 1e-6f);
    EXPECT_NEAR(0.0f, z.x, 1e-6f); EXPECT_NEAR(1.0f, z.y, 1e-6f); EXPECT_NEAR(0.0f, z.z, 1e-6f);
}

TEST_F(utUnitAxisContract, buildAxisRotationMatrixIsInvertibleForAllSixPairs) {
    constexpr int32_t pairs[][2] = {
        {0, 1}, {1, 0}, {0, 2}, {2, 0}, {1, 2}, {2, 1}
    };
    for (const auto &p : pairs) {
        aiMatrix4x4 forward = buildAxisRotationMatrix(p[0], p[1]);
        aiMatrix4x4 reverse = buildAxisRotationMatrix(p[1], p[0]);
        // Round-trip a unit vector through forward then reverse — must equal identity.
        for (int axis = 0; axis < 3; ++axis) {
            aiVector3D v(0, 0, 0);
            (&v.x)[axis] = 1.0f;
            applyLinearTransform(forward, v);
            applyLinearTransform(reverse, v);
            EXPECT_NEAR(1.0f, (&v.x)[axis], 1e-5f) << "pair (" << p[0] << "," << p[1] << ") axis " << axis;
        }
    }
}

// ============================================================================
// readSceneUnitScaleToMeters / readSceneUpAxis
// ============================================================================

TEST_F(utUnitAxisContract, readSceneUnitScaleToMetersReturnsZeroForNullScene) {
    EXPECT_EQ(0.0, readSceneUnitScaleToMeters(nullptr));
}

TEST_F(utUnitAxisContract, readSceneUnitScaleToMetersReturnsZeroForSceneWithoutMetadata) {
    aiScene scene;
    EXPECT_EQ(0.0, readSceneUnitScaleToMeters(&scene));
}

TEST_F(utUnitAxisContract, readSceneUnitScaleToMetersReturnsZeroForSceneMissingKey) {
    aiScene scene;
    scene.mMetaData = new aiMetadata();
    scene.mMetaData->Add("SomeOtherKey", 42);
    EXPECT_EQ(0.0, readSceneUnitScaleToMeters(&scene));
}

TEST_F(utUnitAxisContract, readSceneUpAxisReturnsMinusOneForNullScene) {
    EXPECT_EQ(-1, readSceneUpAxis(nullptr));
}

TEST_F(utUnitAxisContract, readSceneUpAxisReturnsMinusOneForSceneWithoutMetadata) {
    aiScene scene;
    EXPECT_EQ(-1, readSceneUpAxis(&scene));
}

// ============================================================================
// writeContractMetadata round-trip
// ============================================================================

TEST_F(utUnitAxisContract, writeContractMetadataRoundTripsThroughReadHelpers) {
    aiScene *scene = makeUnitScene();
    writeContractMetadata(scene, 0.001, 2, "TEST");
    EXPECT_DOUBLE_EQ(0.001, readSceneUnitScaleToMeters(scene));
    EXPECT_EQ(2, readSceneUpAxis(scene));
    // writeContractMetadata intentionally never writes
    // AI_METADATA_SOURCE_FORMAT — that responsibility lives in
    // Importer::ReadFile, which fills it from desc->mName when no
    // importer pre-populated it. Pre-populating here would clobber the
    // canonical "<Format> Importer" string downstream consumers expect.
    EXPECT_FALSE(scene->mMetaData->HasKey(AI_METADATA_SOURCE_FORMAT));
    delete scene;
}

TEST_F(utUnitAxisContract, writeContractMetadataAllocatesMetadataWhenAbsent) {
    aiScene *scene = makeUnitScene();
    ASSERT_EQ(nullptr, scene->mMetaData);
    writeContractMetadata(scene, 1.0, 1);
    ASSERT_NE(nullptr, scene->mMetaData);
    EXPECT_TRUE(scene->mMetaData->HasKey(AI_METADATA_UNIT_SCALE_TO_METERS));
    EXPECT_TRUE(scene->mMetaData->HasKey(AI_METADATA_UP_AXIS));
    delete scene;
}

TEST_F(utUnitAxisContract, writeContractMetadataOverwritesExistingKeys) {
    aiScene *scene = makeUnitScene();
    writeContractMetadata(scene, 0.001, 2);
    writeContractMetadata(scene, 1.0, 1);
    EXPECT_DOUBLE_EQ(1.0, readSceneUnitScaleToMeters(scene));
    EXPECT_EQ(1, readSceneUpAxis(scene));
    delete scene;
}

TEST_F(utUnitAxisContract, writeContractMetadataNeverWritesSourceFormat) {
    aiScene *scene = makeUnitScene();
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_SOURCE_FORMAT, aiString(std::string("EXISTING")));
    writeContractMetadata(scene, 1.0, 1, "NEW");
    aiString srcFmt;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_SOURCE_FORMAT, srcFmt));
    EXPECT_STREQ("EXISTING", srcFmt.C_Str()) <<
        "writeContractMetadata must never touch AI_METADATA_SOURCE_FORMAT;"
        " a pre-existing value must be preserved verbatim.";
    delete scene;
}

TEST_F(utUnitAxisContract, writeContractMetadataIsNoOpForNullScene) {
    // Should not crash.
    writeContractMetadata(nullptr, 1.0, 1);
}

// ============================================================================
// resolveImporterContract precedence (config > default)
// ============================================================================

TEST_F(utUnitAxisContract, resolveImporterContractReturnsDefaultsWhenNoConfig) {
    Assimp::Importer importer;
    ContractDefaults defaults{0.001, 2};
    ContractDefaults out = resolveImporterContract(&importer, "STL", defaults);
    EXPECT_DOUBLE_EQ(0.001, out.unit);
    EXPECT_EQ(2, out.upAxis);
}

TEST_F(utUnitAxisContract, resolveImporterContractReturnsDefaultsForNullImporter) {
    ContractDefaults defaults{0.5, 1};
    ContractDefaults out = resolveImporterContract(nullptr, "STL", defaults);
    EXPECT_DOUBLE_EQ(0.5, out.unit);
    EXPECT_EQ(1, out.upAxis);
}

TEST_F(utUnitAxisContract, resolveImporterContractHonorsUnitScaleOverride) {
    Assimp::Importer importer;
    importer.SetPropertyFloat("IMPORT_STL_UNIT_SCALE_TO_METERS", 0.0254f);
    ContractDefaults defaults{0.001, 2};
    ContractDefaults out = resolveImporterContract(&importer, "STL", defaults);
    EXPECT_NEAR(0.0254, out.unit, 1e-6);
    EXPECT_EQ(2, out.upAxis);
}

TEST_F(utUnitAxisContract, resolveImporterContractHonorsUpAxisOverride) {
    Assimp::Importer importer;
    importer.SetPropertyInteger("IMPORT_STL_UP_AXIS", 1);
    ContractDefaults defaults{0.001, 2};
    ContractDefaults out = resolveImporterContract(&importer, "STL", defaults);
    EXPECT_DOUBLE_EQ(0.001, out.unit);
    EXPECT_EQ(1, out.upAxis);
}

TEST_F(utUnitAxisContract, resolveImporterContractRejectsInvalidUpAxisOverride) {
    Assimp::Importer importer;
    importer.SetPropertyInteger("IMPORT_STL_UP_AXIS", 7);
    ContractDefaults defaults{0.001, 2};
    EXPECT_THROW(
        resolveImporterContract(&importer, "STL", defaults),
        DeadlyImportError
    );
}

// ============================================================================
// bakeContractTransformIntoMeshes — opt-in gate
// ============================================================================

TEST_F(utUnitAxisContract, bakeReturnsSkippedNoOptInForSceneWithoutContract) {
    aiScene *scene = makeUnitScene();
    auto snapshot = snapshotVertices(scene->mMeshes[0]);

    BakeOutcome outcome = bakeContractTransformIntoMeshes(scene, 1.0, 1);
    EXPECT_EQ(BakeOutcome::Skipped_NoOptIn, outcome);
    EXPECT_TRUE(verticesAreBitwiseEqual(scene->mMeshes[0], snapshot));
    delete scene;
}

TEST_F(utUnitAxisContract, bakeReturnsSkippedNoOptInForNullScene) {
    EXPECT_EQ(BakeOutcome::Skipped_NoOptIn,
              bakeContractTransformIntoMeshes(nullptr, 1.0, 1));
}

TEST_F(utUnitAxisContract, bakeIgnoresOptInGateWhenRequireOptInFalse) {
    aiScene *scene = makeUnitScene();
    // Even without metadata, requireOptIn=false means we proceed (and find
    // identity short-circuit applies because both sides are sentinel).
    BakeOutcome outcome = bakeContractTransformIntoMeshes(scene, 1.0, 1, /*requireOptIn=*/false);
    EXPECT_EQ(BakeOutcome::Skipped_Identity, outcome);
    delete scene;
}

// ============================================================================
// bakeContractTransformIntoMeshes — identity short-circuit
// ============================================================================

TEST_F(utUnitAxisContract, bakeIdentityShortCircuitWhenSourceMatchesTarget) {
    aiScene *scene = makeUnitScene();
    writeContractMetadata(scene, 1.0, 1);
    auto snapshot = snapshotVertices(scene->mMeshes[0]);

    BakeOutcome outcome = bakeContractTransformIntoMeshes(scene, 1.0, 1);
    EXPECT_EQ(BakeOutcome::Skipped_Identity, outcome);
    EXPECT_TRUE(verticesAreBitwiseEqual(scene->mMeshes[0], snapshot))
        << "Identity short-circuit must not touch any vertex bytes";
    delete scene;
}

TEST_F(utUnitAxisContract, bakeIdentityShortCircuitWhenSourceMatchesTargetMm) {
    aiScene *scene = makeUnitScene();
    writeContractMetadata(scene, 0.001, 2);
    auto snapshot = snapshotVertices(scene->mMeshes[0]);

    BakeOutcome outcome = bakeContractTransformIntoMeshes(scene, 0.001, 2);
    EXPECT_EQ(BakeOutcome::Skipped_Identity, outcome);
    EXPECT_TRUE(verticesAreBitwiseEqual(scene->mMeshes[0], snapshot));
    delete scene;
}

// ============================================================================
// bakeContractTransformIntoMeshes — applied transforms
// ============================================================================

TEST_F(utUnitAxisContract, bakeAppliesScaleOnlyWhenAxisMatches) {
    // Source: 0.001 m/unit (mm), target: 1.0 m/unit (m). Axis matches (Y-up).
    aiScene *scene = makeUnitScene(1);
    scene->mMeshes[0]->mVertices[0] = aiVector3D(1000.0f, 2000.0f, 3000.0f);
    writeContractMetadata(scene, 0.001, 1);

    BakeOutcome outcome = bakeContractTransformIntoMeshes(scene, 1.0, 1);
    EXPECT_EQ(BakeOutcome::Applied, outcome);

    const aiVector3D &v = scene->mMeshes[0]->mVertices[0];
    EXPECT_NEAR(1.0f, v.x, 1e-3f);
    EXPECT_NEAR(2.0f, v.y, 1e-3f);
    EXPECT_NEAR(3.0f, v.z, 1e-3f);
    delete scene;
}

TEST_F(utUnitAxisContract, bakeAppliesRotationOnlyWhenUnitMatches) {
    // Source Z-up, target Y-up. Unit matches (1.0).
    aiScene *scene = makeUnitScene(1);
    scene->mMeshes[0]->mVertices[0] = aiVector3D(1.0f, 2.0f, 3.0f);
    writeContractMetadata(scene, 1.0, 2);

    BakeOutcome outcome = bakeContractTransformIntoMeshes(scene, 1.0, 1);
    EXPECT_EQ(BakeOutcome::Applied, outcome);

    // Z-up -> Y-up: +Z maps to +Y, +Y maps to -Z, +X stays.
    // (1, 2, 3) source has x=1, y=2 (was Z-axis), z=3 (was -Z which is now +Y direction).
    // Rotation matrix from Z-up to Y-up: x_new=x, y_new=z, z_new=-y.
    const aiVector3D &v = scene->mMeshes[0]->mVertices[0];
    EXPECT_NEAR(1.0f, v.x, 1e-5f);
    EXPECT_NEAR(3.0f, v.y, 1e-5f);
    EXPECT_NEAR(-2.0f, v.z, 1e-5f);
    delete scene;
}

TEST_F(utUnitAxisContract, bakeAppliesScaleAndRotationCombined) {
    // mm + Z-up to m + Y-up — both transforms required.
    aiScene *scene = makeUnitScene(1);
    scene->mMeshes[0]->mVertices[0] = aiVector3D(1000.0f, 2000.0f, 3000.0f);
    writeContractMetadata(scene, 0.001, 2);

    BakeOutcome outcome = bakeContractTransformIntoMeshes(scene, 1.0, 1);
    EXPECT_EQ(BakeOutcome::Applied, outcome);

    const aiVector3D &v = scene->mMeshes[0]->mVertices[0];
    EXPECT_NEAR(1.0f, v.x, 1e-3f);
    EXPECT_NEAR(3.0f, v.y, 1e-3f);
    EXPECT_NEAR(-2.0f, v.z, 1e-3f);
    delete scene;
}

TEST_F(utUnitAxisContract, bakeTransformsNormalsWithRotationButNotScale) {
    aiScene *scene = makeUnitScene(1);
    scene->mMeshes[0]->mVertices[0] = aiVector3D(1.0f, 0.0f, 0.0f);
    scene->mMeshes[0]->mNormals[0] = aiVector3D(0.0f, 0.0f, 1.0f); // +Z
    writeContractMetadata(scene, 0.001, 2);

    BakeOutcome outcome = bakeContractTransformIntoMeshes(scene, 1.0, 1);
    EXPECT_EQ(BakeOutcome::Applied, outcome);

    // Normal +Z under Z-up to Y-up rotation maps to +Y. Magnitude preserved.
    const aiVector3D &n = scene->mMeshes[0]->mNormals[0];
    EXPECT_NEAR(0.0f, n.x, 1e-5f);
    EXPECT_NEAR(1.0f, n.y, 1e-5f);
    EXPECT_NEAR(0.0f, n.z, 1e-5f);
    delete scene;
}

TEST_F(utUnitAxisContract, bakeRejectsInvalidTargetUpAxis) {
    aiScene *scene = makeUnitScene();
    EXPECT_THROW(
        bakeContractTransformIntoMeshes(scene, 1.0, 7, /*requireOptIn=*/false),
        DeadlyExportError
    );
    delete scene;
}
