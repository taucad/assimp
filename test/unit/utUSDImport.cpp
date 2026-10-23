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
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/Exporter.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/MemoryIOWrapper.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace ::Assimp;

class utUSDImport : public AbstractImportExportBase {
};

TEST_F(utUSDImport, meshTest) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usdc/suzanne.usdc", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);
    EXPECT_EQ(1u, scene->mNumMeshes);
    EXPECT_NE(nullptr, scene->mMeshes[0]);
    // suzanne has face-varying normals and UVs, so every triangle corner is
    // its own vertex; welding is left to aiProcess_JoinIdenticalVertices.
    EXPECT_EQ(968u, scene->mMeshes[0]->mNumFaces);
    EXPECT_EQ(3u * 968u, scene->mMeshes[0]->mNumVertices);
    EXPECT_TRUE(scene->mMeshes[0]->HasNormals());
    EXPECT_TRUE(scene->mMeshes[0]->HasTextureCoords(0));
}

TEST_F(utUSDImport, meshJoinIdenticalVerticesWelds) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usdc/suzanne.usdc",
            aiProcess_ValidateDataStructure | aiProcess_JoinIdenticalVertices);
    ASSERT_NE(nullptr, scene);
    ASSERT_EQ(1u, scene->mNumMeshes);
    EXPECT_EQ(968u, scene->mMeshes[0]->mNumFaces);
    EXPECT_LT(scene->mMeshes[0]->mNumVertices, 2000u);
}

TEST_F(utUSDImport, skinnedMeshTest) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usda/simple-skin-test.usda", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);
    EXPECT_TRUE(scene->HasMeshes());

    const aiMesh *mesh = scene->mMeshes[0];
    EXPECT_EQ(2, mesh->mNumBones);

    // Check bone names and make sure scene has nodes of the same name
    EXPECT_EQ(mesh->mBones[0]->mName, aiString("Bone"));
    EXPECT_EQ(mesh->mBones[1]->mName, aiString("Bone/Bone_001"));

    EXPECT_NE(nullptr, scene->mRootNode->FindNode("Bone"));
    EXPECT_NE(nullptr, scene->mRootNode->FindNode("Bone/Bone_001"));
}

TEST_F(utUSDImport, singleAnimationTest) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usda/simple-skin-animation-test.usda", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);
    EXPECT_TRUE(scene->HasAnimations());
    EXPECT_EQ(2, scene->mAnimations[0]->mNumChannels);  // 2 bones. 1 channel for each bone
}

TEST_F(utUSDImport, blendShapeAnimationTest) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/USD/usda/AnimatedMorphCube_blendshapes.usda", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);
    
    // Should have animations (blend shape animations)
    EXPECT_TRUE(scene->HasAnimations());
    EXPECT_GT(scene->mNumAnimations, 0u);
    
    const aiAnimation* animation = scene->mAnimations[0];
    EXPECT_NE(nullptr, animation);
    
    // Should have at least one animation channel (node or morph)
    EXPECT_GT(animation->mNumChannels + animation->mNumMorphMeshChannels, 0u);
    
    // Should have morph mesh channels for blend shapes
    EXPECT_GT(animation->mNumMorphMeshChannels, 0u);
    
    // One morph channel per animated mesh, named after the mesh's node (as
    // the glTF importer does); each key carries one weight per blend shape.
    ASSERT_EQ(1u, animation->mNumMorphMeshChannels);
    const aiMeshMorphAnim *morphAnim = animation->mMorphMeshChannels[0];
    ASSERT_NE(nullptr, morphAnim);
    EXPECT_NE(nullptr, scene->mRootNode->FindNode(morphAnim->mName));
    EXPECT_GT(morphAnim->mNumKeys, 50u); // 101 authored time samples
    for (unsigned int k = 0; k < morphAnim->mNumKeys; ++k) {
        EXPECT_EQ(2u, morphAnim->mKeys[k].mNumValuesAndWeights);
    }
    
    // Should have meshes with blend shapes
    EXPECT_TRUE(scene->HasMeshes());
    bool foundMeshWithBlendShapes = false;
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        if (scene->mMeshes[i]->mNumAnimMeshes > 0) {
            foundMeshWithBlendShapes = true;
            EXPECT_EQ(2u, scene->mMeshes[i]->mNumAnimMeshes);  // "thin" and "angle"
            break;
        }
    }
    EXPECT_TRUE(foundMeshWithBlendShapes) << "Should find at least one mesh with blend shapes";
}

TEST_F(utUSDImport, cameraImportTest) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/USD/usda/camera-lighting.usda", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);
    
    // Should have cameras
    EXPECT_TRUE(scene->HasCameras());
    EXPECT_GT(scene->mNumCameras, 0u);
    
    // Should have at least one camera (the "Camera" from the test file)
    EXPECT_EQ(1u, scene->mNumCameras);
    
    const aiCamera* camera = scene->mCameras[0];
    EXPECT_NE(nullptr, camera);
    
    // Verify camera properties from the test file
    EXPECT_STREQ("Camera", camera->mName.C_Str());
    
    // Check camera parameters (from camera-lighting.usda)
    // focalLength = 0.5, horizontalAperture = 0.36, verticalAperture = 0.2025
    // clippingRange = (0.1, 100), projection = "perspective"
    // Expected FOV = 2 * atan(horizontalAperture / (2 * focalLength)) = 2 * atan(0.36 / (2 * 0.5)) = 2 * atan(0.36) ≈ 0.691
    EXPECT_NEAR(0.691f, camera->mHorizontalFOV, 0.01f);
    EXPECT_NEAR(0.1f, camera->mClipPlaneNear, 0.01f);
    EXPECT_NEAR(100.0f, camera->mClipPlaneFar, 0.01f);
    EXPECT_NEAR(0.0f, camera->mOrthographicWidth, 0.01f);  // Should be 0 for perspective camera
    
    // Check the calculated aspect ratio
    EXPECT_NEAR(0.36f / 0.2025f, camera->mAspect, 0.01f);
}

TEST_F(utUSDImport, lightImportTest) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/USD/usda/camera-lighting.usda", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);
    
    // Should have lights
    EXPECT_TRUE(scene->HasLights());
    EXPECT_GT(scene->mNumLights, 0u);
    
    // Should have 2 lights (SphereLight "Light" and DomeLight "env_light")
    EXPECT_EQ(2u, scene->mNumLights);
    
    // Find the SphereLight
    const aiLight* sphereLight = nullptr;
    const aiLight* domeLight = nullptr;
    
    for (unsigned int i = 0; i < scene->mNumLights; ++i) {
        const aiLight* light = scene->mLights[i];
        std::string lightName = light->mName.C_Str();
        
        if (lightName.find("Light") != std::string::npos && lightName.find("env_light") == std::string::npos) {
            sphereLight = light;
        } else if (lightName.find("env_light") != std::string::npos) {
            domeLight = light;
        }
    }
    
    // Verify SphereLight properties
    EXPECT_NE(nullptr, sphereLight);
    if (sphereLight) {
        EXPECT_EQ(aiLightSource_POINT, sphereLight->mType);
        // Physical inverse-square falloff; intensity goes into the colour.
        EXPECT_NEAR(0.0f, sphereLight->mAttenuationConstant, 1e-6f);
        EXPECT_NEAR(1.0f, sphereLight->mAttenuationQuadratic, 1e-6f);
        EXPECT_GT(sphereLight->mColorDiffuse.r, 100.0f); // 318.3 * authored colour
        // The light sits at its node's origin; the node carries the Xform.
        const aiNode *node = scene->mRootNode->FindNode(sphereLight->mName);
        ASSERT_NE(nullptr, node);
        aiMatrix4x4 world = node->mTransformation;
        for (const aiNode *p = node->mParent; p != nullptr; p = p->mParent) {
            world = p->mTransformation * world;
        }
        const aiVector3D pos = world * sphereLight->mPosition;
        EXPECT_NEAR(4.076245f, pos.x, 0.01f);
        EXPECT_NEAR(1.005454f, pos.y, 0.01f);
        EXPECT_NEAR(5.903862f, pos.z, 0.01f);
    }
    
    // Verify DomeLight properties  
    EXPECT_NE(nullptr, domeLight);
    if (domeLight) {
        EXPECT_EQ(aiLightSource_AMBIENT, domeLight->mType);  // Dome lights are typically ambient
        EXPECT_NEAR(1.0f, domeLight->mAttenuationConstant, 0.01f);
        EXPECT_NEAR(1.0f, domeLight->mColorAmbient.r, 0.01f);  // intensity = 1
    }
}

// Note: Add multi-animation test once supported by USD
// See https://github.com/lighttransport/tinyusdz/issues/122 for details.

namespace {

constexpr const char *kUsdZupFixture = ASSIMP_TEST_MODELS_DIR "/USD/usda/camera-lighting.usda";
constexpr const char *kUsdYupFixture = ASSIMP_TEST_MODELS_DIR "/USD/usda/AnimatedMorphCube_blendshapes.usda";

double readUnitScale(const aiScene *scene) {
    double value = -1.0;
    EXPECT_TRUE(scene->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, value));
    return value;
}

int readUpAxis(const aiScene *scene) {
    int value = -1;
    EXPECT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, value));
    return value;
}

} // namespace

TEST_F(utUSDImport, contractReadsZUpAndMetersFromAuthoredStage) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(kUsdZupFixture, 0);
    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->mMetaData, nullptr);
    EXPECT_NEAR(readUnitScale(scene), 1.0, 1e-9);
    EXPECT_EQ(readUpAxis(scene), 2);
}

TEST_F(utUSDImport, contractReadsYUpFromAuthoredStage) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(kUsdYupFixture, 0);
    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->mMetaData, nullptr);
    EXPECT_NEAR(readUnitScale(scene), 1.0, 1e-9);
    EXPECT_EQ(readUpAxis(scene), 1);
}

TEST_F(utUSDImport, contractUnitScaleOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_IMPORT_USD_UNIT_SCALE_TO_METERS, 0.001f);
    const aiScene *scene = importer.ReadFile(kUsdYupFixture, 0);
    ASSERT_NE(scene, nullptr);
    EXPECT_NEAR(readUnitScale(scene), 0.001, 1e-6);
    EXPECT_EQ(readUpAxis(scene), 1);
}

TEST_F(utUSDImport, contractUpAxisOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_USD_UP_AXIS, 2);
    const aiScene *scene = importer.ReadFile(kUsdYupFixture, 0);
    ASSERT_NE(scene, nullptr);
    EXPECT_EQ(readUpAxis(scene), 2);
}

TEST_F(utUSDImport, contractInvalidUpAxisOverrideFailsImport) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_USD_UP_AXIS, 7);
    const aiScene *scene = importer.ReadFile(kUsdYupFixture, 0);
    EXPECT_EQ(scene, nullptr);
    const std::string err = importer.GetErrorString();
    EXPECT_NE(err.find("IMPORT_USD_UP_AXIS"), std::string::npos) << err;
}

// ------------------------------------------------------------------------------------------------
// Helpers for the LightUSD-backed importer tests.

namespace {

/// An IOSystem that serves files from a std::map, as libassimp's in-memory
/// IOSystem does in WASM. Nothing touches the file system.
class MapIOSystem : public Assimp::IOSystem {
public:
    explicit MapIOSystem(std::map<std::string, std::string> files) :
            mFiles(std::move(files)) {}

    bool Exists(const char *pFile) const override {
        return mFiles.count(normalize(pFile)) != 0;
    }

    char getOsSeparator() const override { return '/'; }

    Assimp::IOStream *Open(const char *pFile, const char *pMode = "rb") override {
        (void)pMode;
        auto it = mFiles.find(normalize(pFile));
        if (it == mFiles.end()) {
            return nullptr;
        }
        ++mOpened[it->first];
        return new Assimp::MemoryIOStream(reinterpret_cast<const uint8_t *>(it->second.data()), it->second.size(), false);
    }

    void Close(Assimp::IOStream *pFile) override { delete pFile; }

    unsigned openCount(const std::string &name) const {
        auto it = mOpened.find(name);
        return it == mOpened.end() ? 0u : it->second;
    }

private:
    static std::string normalize(const char *p) {
        std::string s = p ? p : "";
        while (s.compare(0, 2, "./") == 0) {
            s.erase(0, 2);
        }
        return s;
    }

    std::map<std::string, std::string> mFiles;
    std::map<std::string, unsigned> mOpened;
};

std::string ReadBinaryFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const aiScene *ReadFromMap(Assimp::Importer &importer, std::map<std::string, std::string> files,
        const char *root, unsigned flags = aiProcess_ValidateDataStructure) {
    importer.SetIOHandler(new MapIOSystem(std::move(files)));
    return importer.ReadFile(root, flags);
}

const char *kTriangleLayer = R"(#usda 1.0
(
    defaultPrim = "Tri"
)
def Mesh "Tri"
{
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
}
)";

const aiMetadata *NodeMeta(const aiScene *scene, const char *name) {
    const aiNode *node = scene->mRootNode->FindNode(name);
    return node ? node->mMetaData : nullptr;
}

unsigned CountMeshVertices(const aiScene *scene) {
    unsigned n = 0;
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        n += scene->mMeshes[i]->mNumVertices;
    }
    return n;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Format detection (D8) and malformed input

TEST_F(utUSDImport, usdcWithNonUsdExtensionIsDetectedByContent) {
    const std::string usdc = ReadBinaryFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usdc/suzanne.usdc");
    ASSERT_FALSE(usdc.empty());
    Assimp::Importer importer;
    const aiScene *scene = ReadFromMap(importer, { { "suzanne.bin", usdc } }, "suzanne.bin");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    EXPECT_EQ(968u, scene->mMeshes[0]->mNumFaces);
}

TEST_F(utUSDImport, usdaContentWithUsdcExtensionIsParsedAsText) {
    Assimp::Importer importer;
    const aiScene *scene = ReadFromMap(importer, { { "tri.usdc", kTriangleLayer } }, "tri.usdc");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    EXPECT_EQ(3u, scene->mMeshes[0]->mNumVertices);
}

TEST_F(utUSDImport, usdcFromMemoryBufferWithoutHint) {
    const std::string usdc = ReadBinaryFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usdc/suzanne.usdc");
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFileFromMemory(usdc.data(), usdc.size(), aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_EQ(1u, scene->mNumMeshes);
}

TEST_F(utUSDImport, malformedInputsFailWithoutCrashing) {
    const std::string usdc = ReadBinaryFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usdc/suzanne.usdc");
    ASSERT_GT(usdc.size(), 256u);
    std::string corrupted = usdc;
    for (size_t i = 88; i < corrupted.size(); i += 7) {
        corrupted[i] = static_cast<char>(corrupted[i] ^ 0x5a);
    }
    const std::map<std::string, std::string> cases = {
        { "empty.usda", "" },
        { "garbage.usda", "#usda 1.0\ndef Mesh \"m\" { int[] faceVertexCounts = [3, }}}}" },
        { "truncated.usdc", usdc.substr(0, 128) },
        { "corrupted.usdc", corrupted },
        { "notzip.usdz", std::string("PK\x03\x04", 4) + std::string(64, '\x7f') },
        { "header-only.usdc", "PXR-USDC" },
    };
    for (const auto &c : cases) {
        Assimp::Importer importer;
        const aiScene *scene = ReadFromMap(importer, cases, c.first.c_str());
        // Either a clean failure or a valid (possibly empty) scene; never a crash.
        if (scene == nullptr) {
            EXPECT_FALSE(std::string(importer.GetErrorString()).empty()) << c.first;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Mesh bounds checks (D1-D5)

namespace {

const char *kBrokenMesh = R"(#usda 1.0
(
    defaultPrim = "root"
)
def Xform "root"
{
    def Mesh "broken"
    {
        int[] faceVertexCounts = [3, 2, 4, 3, 1]
        int[] faceVertexIndices = [0, 1, 2, 0, 1, 0, 1, 2, 3, 1, 2, 3, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0)]
        normal3f[] normals = [(0, 0, 1)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:st = [(0, 0), (1, 0)] (
            interpolation = "vertex"
        )
    }
}
)";

const char *kMultiUv = R"(#usda 1.0
(
    defaultPrim = "root"
)
def Xform "root"
{
    def Mesh "quad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:st1 = [(0, 0), (0.5, 0), (0.5, 0.5), (0, 0.5)] (
            interpolation = "faceVarying"
        )
        texCoord2f[] primvars:st2 = [(0.1, 0.1), (0.2, 0.2)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:uv3 = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:uv4 = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:uv5 = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:uv6 = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:uv7 = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:uv8 = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:uv9 = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
    }
}
)";

const char *kBrokenSkin = R"(#usda 1.0
(
    defaultPrim = "root"
)
def SkelRoot "root"
{
    def Skeleton "Skel"
    {
        uniform matrix4d[] bindTransforms = [( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) ), ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 1, 0, 1) )]
        uniform token[] joints = ["Hip", "Hip/Knee"]
        uniform matrix4d[] restTransforms = [( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) ), ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 1, 0, 1) )]
    }
    def Mesh "Leg" (
        prepend apiSchemas = ["SkelBindingAPI"]
    )
    {
        int[] faceVertexCounts = [3, 3]
        int[] faceVertexIndices = [0, 1, 2, 1, 3, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0)]
        matrix4d primvars:skel:geomBindTransform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 2, 1) )
        int[] primvars:skel:jointIndices = [0, 1, 7, -3, 1, 0, 0] (
            elementSize = 2
            interpolation = "vertex"
        )
        float[] primvars:skel:jointWeights = [0.5, 0.5, 1, 0, 0.25, 0.75] (
            elementSize = 2
            interpolation = "vertex"
        )
        rel skel:skeleton = </root/Skel>
    }
}
)";

/// A mesh with a valid dense blend shape "dense" and a second shape "bad"
/// whose body is `badShape`.
std::string BlendShapeScene(const std::string &badShape) {
    return std::string(R"(#usda 1.0
(
    defaultPrim = "root"
)
def SkelRoot "root"
{
    def Skeleton "Skel"
    {
        uniform matrix4d[] bindTransforms = [( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) )]
        uniform token[] joints = ["Root"]
        uniform matrix4d[] restTransforms = [( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) )]
    }
    def Mesh "Face" (
        prepend apiSchemas = ["SkelBindingAPI"]
    )
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        uniform token[] skel:blendShapes = ["dense", "bad"]
        rel skel:blendShapeTargets = [</root/Face/dense>, </root/Face/bad>]
        rel skel:skeleton = </root/Skel>

        def BlendShape "dense"
        {
            uniform vector3f[] offsets = [(0, 0, 1), (0, 0, 1), (0, 0, 1)]
        }
        def BlendShape "bad"
        {
)") + badShape + R"(
        }
    }
}
)";
}

} // namespace

TEST_F(utUSDImport, degenerateAndOutOfRangeFacesAreSkipped) {
    Assimp::Importer importer;
    const aiScene *scene = ReadFromMap(importer, { { "broken.usda", kBrokenMesh } }, "broken.usda");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    const aiMesh *mesh = scene->mMeshes[0];
    for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
        ASSERT_EQ(3u, mesh->mFaces[f].mNumIndices);
        for (unsigned k = 0; k < 3; ++k) {
            EXPECT_LT(mesh->mFaces[f].mIndices[k], mesh->mNumVertices);
        }
    }
    // [0,1,2], the quad (two triangles) and [1,2,3] survive; the 2-gon and the 1-gon do not.
    EXPECT_EQ(4u, mesh->mNumFaces);
    // Normals and UVs with the wrong element count are dropped (D4), not read out of bounds.
    EXPECT_FALSE(mesh->HasNormals());
    EXPECT_FALSE(mesh->HasTextureCoords(0));
}

TEST_F(utUSDImport, outOfRangeFaceIndexSkipsTheMesh) {
    const char *good = R"(
def Mesh "good"
{
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
}
)";
    const char *mesh = R"(#usda 1.0
def Mesh "bad"
{
    int[] faceVertexCounts = [3, 3]
    int[] faceVertexIndices = [0, 1, 2, 0, 1, 99]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
}
)";
    const char *counts = R"(#usda 1.0
def Mesh "bad"
{
    int[] faceVertexCounts = [3, 3, 3]
    int[] faceVertexIndices = [0, 1, 2, 0, 1]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
}
)";
    for (const char *text : { mesh, counts }) {
        Assimp::Importer importer;
        const aiScene *scene = ReadFromMap(importer, { { "bad.usda", std::string(text) + good } }, "bad.usda");
        // LightUSD's Tydra drops the invalid mesh with a warning (D1); the
        // rest of the scene still imports, never reading out of bounds.
        ASSERT_NE(nullptr, scene) << importer.GetErrorString();
        ASSERT_EQ(1u, scene->mNumMeshes);
        EXPECT_STREQ("good", scene->mMeshes[0]->mName.C_Str());
        // The prim keeps its node (children and transforms stay), without a mesh.
        const aiNode *bad = scene->mRootNode->FindNode("bad");
        EXPECT_TRUE(bad == nullptr || bad->mNumMeshes == 0);
    }
}

TEST_F(utUSDImport, multipleUvSetsAreImportedAndCapped) {
    Assimp::Importer importer;
    const aiScene *scene = ReadFromMap(importer, { { "uv.usda", kMultiUv } }, "uv.usda");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    const aiMesh *mesh = scene->mMeshes[0];
    // st, st1 and uv3..uv9 are valid (st2 has the wrong size): 9 sets, capped to the assimp maximum.
    EXPECT_EQ(static_cast<unsigned>(AI_MAX_NUMBER_OF_TEXTURECOORDS), mesh->GetNumUVChannels());
    ASSERT_TRUE(mesh->HasTextureCoords(0));
    ASSERT_TRUE(mesh->HasTextureCoords(1));
    EXPECT_STREQ("st", mesh->GetTextureCoordsName(0)->C_Str());
    EXPECT_STREQ("st1", mesh->GetTextureCoordsName(1)->C_Str());
    bool foundHalf = false;
    for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
        foundHalf = foundHalf || std::fabs(mesh->mTextureCoords[1][v].x - 0.5f) < 1e-6f;
    }
    EXPECT_TRUE(foundHalf);
}

TEST_F(utUSDImport, outOfRangeJointIndicesAreDropped) {
    Assimp::Importer importer;
    const aiScene *scene = ReadFromMap(importer, { { "skin.usda", kBrokenSkin } }, "skin.usda");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    const aiMesh *mesh = scene->mMeshes[0];
    ASSERT_LE(mesh->mNumBones, 2u);
    for (unsigned b = 0; b < mesh->mNumBones; ++b) {
        const aiBone *bone = mesh->mBones[b];
        EXPECT_NE(nullptr, scene->mRootNode->FindNode(bone->mName)) << bone->mName.C_Str();
        for (unsigned w = 0; w < bone->mNumWeights; ++w) {
            EXPECT_LT(bone->mWeights[w].mVertexId, mesh->mNumVertices);
        }
    }
}

TEST_F(utUSDImport, malformedBlendShapesAreRejected) {
    const char *variants[] = {
        // sparse, point index out of range
        "            uniform vector3f[] offsets = [(0, 0, 1), (0, 0, 1)]\n            uniform int[] pointIndices = [1, 4096]",
        // sparse, fewer offsets than indices
        "            uniform vector3f[] offsets = [(0, 0, 1)]\n            uniform int[] pointIndices = [0, 1, 2]",
        // dense, fewer offsets than points
        "            uniform vector3f[] offsets = [(0, 0, 1)]",
        // sparse, negative index
        "            uniform vector3f[] offsets = [(0, 0, 1)]\n            uniform int[] pointIndices = [-1]",
    };
    for (const char *variant : variants) {
        Assimp::Importer importer;
        const aiScene *scene = ReadFromMap(importer, { { "bs.usda", BlendShapeScene(variant) } }, "bs.usda");
        // The bad shape is dropped with a warning (D5); the mesh and its
        // valid dense shape survive, never an out-of-bounds read.
        ASSERT_NE(nullptr, scene) << variant << ": " << importer.GetErrorString();
        ASSERT_EQ(1u, scene->mNumMeshes) << variant;
        const aiMesh *mesh = scene->mMeshes[0];
        ASSERT_EQ(1u, mesh->mNumAnimMeshes) << variant;
        EXPECT_STREQ("dense", mesh->mAnimMeshes[0]->mName.C_Str()) << variant;
        EXPECT_EQ(mesh->mNumVertices, mesh->mAnimMeshes[0]->mNumVertices);
    }
}

TEST_F(utUSDImport, sparseBlendShapeIsApplied) {
    Assimp::Importer importer;
    const std::string text = BlendShapeScene(
            "            uniform vector3f[] offsets = [(0, 0, 2)]\n            uniform int[] pointIndices = [1]");
    const aiScene *scene = ReadFromMap(importer, { { "bs.usda", text } }, "bs.usda");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    const aiMesh *mesh = scene->mMeshes[0];
    ASSERT_EQ(2u, mesh->mNumAnimMeshes);
    const aiAnimMesh *dense = nullptr;
    const aiAnimMesh *sparse = nullptr;
    for (unsigned a = 0; a < mesh->mNumAnimMeshes; ++a) {
        const std::string name = mesh->mAnimMeshes[a]->mName.C_Str();
        (name == "dense" ? dense : sparse) = mesh->mAnimMeshes[a];
    }
    ASSERT_NE(nullptr, dense);
    ASSERT_NE(nullptr, sparse);
    EXPECT_STREQ("bad", sparse->mName.C_Str());
    for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
        const bool isPoint1 = (mesh->mVertices[v] - aiVector3D(1, 0, 0)).Length() < 1e-6f;
        EXPECT_NEAR(isPoint1 ? 2.0f : 0.0f, sparse->mVertices[v].z - mesh->mVertices[v].z, 1e-6f);
        // Dense shape without pointIndices: every point moves by +1 in z.
        EXPECT_NEAR(1.0f, dense->mVertices[v].z - mesh->mVertices[v].z, 1e-6f);
    }
}

// ------------------------------------------------------------------------------------------------
// Composition through the IOSystem (D9, D10)

TEST_F(utUSDImport, compositionUsdaReferencesUsda) {
    const char *main = R"(#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 0.01
    upAxis = "Y"
)
def Xform "World"
{
    def "Prop" (
        references = @props/tri.usda@
    )
    {
    }
}
)";
    Assimp::Importer importer;
    const aiScene *scene = ReadFromMap(importer, { { "main.usda", main }, { "props/tri.usda", kTriangleLayer } }, "main.usda");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    EXPECT_EQ(3u, scene->mMeshes[0]->mNumVertices);
    const aiNode *prop = scene->mRootNode->FindNode("Prop");
    ASSERT_NE(nullptr, prop);
    EXPECT_EQ(1u, prop->mNumMeshes);
    double unitScale = 0.0;
    EXPECT_TRUE(scene->mMetaData->Get("UnitScaleFactor", unitScale));
    EXPECT_NEAR(0.01, unitScale, 1e-9);
    EXPECT_NEAR(0.01, readUnitScale(scene), 1e-9);
    EXPECT_EQ(1, readUpAxis(scene));
    aiString format;
    EXPECT_TRUE(scene->mMetaData->Get(AI_METADATA_SOURCE_FORMAT, format));
    EXPECT_STREQ("USD", format.C_Str());
}

TEST_F(utUSDImport, compositionUsdaReferencesUsdc) {
    const char *main = R"(#usda 1.0
(
    defaultPrim = "World"
)
def Xform "World"
{
    def "Monkey" (
        references = @geo/suzanne.usdc@</Suzanne>
    )
    {
    }
}
)";
    const std::string usdc = ReadBinaryFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usdc/suzanne.usdc");
    Assimp::Importer importer;
    const aiScene *scene = ReadFromMap(importer, { { "main.usda", main }, { "geo/suzanne.usdc", usdc } }, "main.usda");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    EXPECT_EQ(968u, scene->mMeshes[0]->mNumFaces);
    EXPECT_NE(nullptr, scene->mRootNode->FindNode("Monkey"));
}

TEST_F(utUSDImport, compositionSublayersAndMissingAssetsWarn) {
    const char *main = R"(#usda 1.0
(
    defaultPrim = "World"
    subLayers = [
        @layers/missing.usda@,
        @layers/geo.usda@
    ]
)
def Xform "World"
{
    def "Gone" (
        references = @nowhere.usda@
    )
    {
    }
}
)";
    const char *geo = R"(#usda 1.0
over "World"
{
    def Mesh "FromSublayer"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
}
)";
    Assimp::Importer importer;
    const aiScene *scene = ReadFromMap(importer, { { "main.usda", main }, { "layers/geo.usda", geo } }, "main.usda");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    EXPECT_NE(nullptr, scene->mRootNode->FindNode("FromSublayer"));
}

TEST_F(utUSDImport, compositionNestedLayerAnchorsTexturePaths) {
    const std::string dir = ASSIMP_TEST_MODELS_DIR "/USD/nested-look-texture/";
    const std::string png = ReadBinaryFile(dir + "tex/checkerboard.png");
    ASSERT_FALSE(png.empty());
    Assimp::Importer importer;
    auto *io = new MapIOSystem({ { "root.usda", ReadBinaryFile(dir + "root.usda") },
            { "look/look.usda", ReadBinaryFile(dir + "look/look.usda") },
            { "tex/checkerboard.png", png } });
    importer.SetIOHandler(io);
    const aiScene *scene = importer.ReadFile("root.usda", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumTextures);
    const aiTexture *tex = scene->mTextures[0];
    // Compressed texture with the original encoded bytes.
    EXPECT_EQ(0u, tex->mHeight);
    ASSERT_EQ(png.size(), static_cast<size_t>(tex->mWidth));
    EXPECT_EQ(0, std::memcmp(png.data(), tex->pcData, png.size()));
    EXPECT_STREQ("png", tex->achFormatHint);
    ASSERT_EQ(1u, scene->mNumMeshes);
    const aiMaterial *mat = scene->mMaterials[scene->mMeshes[0]->mMaterialIndex];
    aiString path;
    ASSERT_EQ(aiReturn_SUCCESS, mat->GetTexture(aiTextureType_BASE_COLOR, 0, &path));
    EXPECT_STREQ("*0", path.C_Str());
    EXPECT_EQ(scene->mTextures[0], scene->GetEmbeddedTexture(path.C_Str()));
    // The importer reads the main file once (Importer::ReadFile opens it once
    // more, only for its size).
    EXPECT_EQ(2u, io->openCount("root.usda"));
}

TEST_F(utUSDImport, compositionReferencesMaterialX) {
    const char *main = R"(#usda 1.0
(
    defaultPrim = "root"
)
def Xform "root"
{
    def Mesh "tri" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        rel material:binding = </root/mtl/Materials/Gold_Material>
    }
    def Scope "mtl" (
        references = @looks/gold.mtlx@</MaterialX>
    )
    {
    }
}
)";
    Assimp::Importer importer;
    const aiScene *scene = ReadFromMap(importer,
            { { "main.usda", main }, { "looks/gold.mtlx", ReadBinaryFile(ASSIMP_TEST_MODELS_DIR "/USD/mtlx/basic_openpbr.mtlx") } },
            "main.usda");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    const aiMaterial *mat = scene->mMaterials[scene->mMeshes[0]->mMaterialIndex];
    aiString name;
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_NAME, name));
    EXPECT_STREQ("Gold_Material", name.C_Str());
    // The referenced material is connected to its shader, so it imports shaded.
    aiColor4D base;
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_BASE_COLOR, base));
    EXPECT_NEAR(0.944f, base.r, 1e-4f);
    EXPECT_NEAR(0.776f, base.g, 1e-4f);
    EXPECT_NEAR(0.373f, base.b, 1e-4f);
    float metallic = 0.0f, roughness = 1.0f;
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic));
    EXPECT_NEAR(1.0f, metallic, 1e-6f);
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness));
    EXPECT_NEAR(0.2f, roughness, 1e-6f);
    // Shader prims are not scene nodes.
    EXPECT_EQ(nullptr, scene->mRootNode->FindNode("OpenPBR_Gold"));
}

TEST_F(utUSDImport, materialXReferencedStandardSurfaceImportsShaded) {
    const char *brass = R"(<?xml version="1.0"?>
<materialx version="1.38" colorspace="lin_rec709">
  <standard_surface name="SR_brass" type="surfaceshader">
    <input name="base" type="float" value="1" />
    <input name="base_color" type="color3" value="1, 0.766, 0.336" />
    <input name="metalness" type="float" value="1" />
    <input name="specular_roughness" type="float" value="0.25" />
  </standard_surface>
  <surfacematerial name="Brass" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR_brass" />
  </surfacematerial>
</materialx>
)";
    const char *main = R"(#usda 1.0
(
    defaultPrim = "World"
)
def Xform "World"
{
    def Mesh "tri" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        rel material:binding = </World/Looks/Materials/Brass>
    }
    def Scope "Looks" (
        references = @./materials/brass.mtlx@</MaterialX>
    )
    {
    }
}
)";
    Assimp::Importer importer;
    const aiScene *scene = ReadFromMap(importer, { { "scene.usda", main }, { "materials/brass.mtlx", brass } }, "scene.usda");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    const aiMaterial *mat = scene->mMaterials[scene->mMeshes[0]->mMaterialIndex];
    aiString name;
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_NAME, name));
    EXPECT_STREQ("Brass", name.C_Str());
    aiColor4D base;
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_BASE_COLOR, base));
    EXPECT_NEAR(1.0f, base.r, 1e-4f);
    EXPECT_NEAR(0.766f, base.g, 1e-4f);
    EXPECT_NEAR(0.336f, base.b, 1e-4f);
    float metallic = 0.0f, roughness = 1.0f;
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic));
    EXPECT_NEAR(1.0f, metallic, 1e-6f);
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness));
    EXPECT_NEAR(0.25f, roughness, 1e-6f);
    EXPECT_EQ(nullptr, scene->mRootNode->FindNode("SR_brass"));
    EXPECT_EQ(nullptr, scene->mRootNode->FindNode("Brass"));
}

// ------------------------------------------------------------------------------------------------
// Materials, cameras and lights

TEST_F(utUSDImport, materialXOnlyOpenPbrMaterial) {
    const char *main = R"(#usda 1.0
(
    defaultPrim = "root"
)
def Xform "root"
{
    def Mesh "tri" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        rel material:binding = </root/Looks/Gold>
    }
    def Scope "Looks"
    {
        def Material "Gold" (
            prepend apiSchemas = ["MaterialXConfigAPI"]
        )
        {
            string config:mtlx:version = "1.39"
            token outputs:mtlx:surface.connect = </root/Looks/Gold/OpenPBR.outputs:surface>

            def Shader "OpenPBR"
            {
                uniform token info:id = "ND_open_pbr_surface_surfaceshader"
                float inputs:base_weight = 1
                color3f inputs:base_color = (0.944, 0.776, 0.373)
                float inputs:base_metalness = 1
                float inputs:specular_roughness = 0.2
                float inputs:specular_ior = 1.5
                float inputs:geometry_opacity = 0.5
                color3f inputs:emission_color = (1, 0, 0)
                float inputs:emission_luminance = 2
                token outputs:surface
            }
        }
    }
}
)";
    Assimp::Importer importer;
    const aiScene *scene = ReadFromMap(importer, { { "gold.usda", main } }, "gold.usda");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    const aiMaterial *mat = scene->mMaterials[scene->mMeshes[0]->mMaterialIndex];
    aiColor4D base;
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_BASE_COLOR, base));
    EXPECT_NEAR(0.944f, base.r, 1e-4f);
    EXPECT_NEAR(0.776f, base.g, 1e-4f);
    EXPECT_NEAR(0.373f, base.b, 1e-4f);
    float metallic = 0.0f, roughness = 1.0f, opacity = 1.0f;
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic));
    EXPECT_NEAR(1.0f, metallic, 1e-6f);
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness));
    EXPECT_NEAR(0.2f, roughness, 1e-6f);
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_OPACITY, opacity));
    EXPECT_NEAR(0.5f, opacity, 1e-6f);
    aiColor3D emissive;
    ASSERT_EQ(aiReturn_SUCCESS, mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive));
    EXPECT_GT(emissive.r, 0.0f);
    EXPECT_EQ(nullptr, scene->mRootNode->FindNode("OpenPBR"));
}

TEST_F(utUSDImport, texturedCubeWithLightAndCameraValidates) {
    // D7: lights and cameras get nodes with their names.
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usda/texturedcube.usda", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_GE(scene->mNumLights, 1u);
    ASSERT_GE(scene->mNumCameras, 1u);
    for (unsigned i = 0; i < scene->mNumLights; ++i) {
        EXPECT_NE(nullptr, scene->mRootNode->FindNode(scene->mLights[i]->mName));
    }
    for (unsigned i = 0; i < scene->mNumCameras; ++i) {
        EXPECT_NE(nullptr, scene->mRootNode->FindNode(scene->mCameras[i]->mName));
    }
    ASSERT_EQ(1u, scene->mNumTextures);
    EXPECT_EQ(0u, scene->mTextures[0]->mHeight);
    EXPECT_STREQ("png", scene->mTextures[0]->achFormatHint);
}

TEST_F(utUSDImport, orthographicCameraAndSpotLight) {
    const char *main = R"(#usda 1.0
(
    defaultPrim = "root"
)
def Xform "root"
{
    def Camera "Ortho"
    {
        token projection = "orthographic"
        float horizontalAperture = 40
        float verticalAperture = 20
        float2 clippingRange = (0.5, 50)
    }
    def SphereLight "Spot" (
        prepend apiSchemas = ["ShapingAPI"]
    )
    {
        color3f inputs:color = (1, 0.5, 0.25)
        float inputs:intensity = 4
        float inputs:exposure = 1
        float inputs:shaping:cone:angle = 30
        float inputs:shaping:cone:softness = 0
        double3 xformOp:translate = (0, 2, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def DiskLight "Disk"
    {
        float inputs:radius = 0.5
    }
}
)";
    Assimp::Importer importer;
    const aiScene *scene = ReadFromMap(importer, { { "cl.usda", main } }, "cl.usda");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumCameras);
    const aiCamera *cam = scene->mCameras[0];
    EXPECT_STREQ("Ortho", cam->mName.C_Str());
    // Apertures are in tenths of a scene unit; assimp wants the half width (D17).
    EXPECT_NEAR(2.0f, cam->mOrthographicWidth, 1e-5f);
    EXPECT_NEAR(2.0f, cam->mAspect, 1e-5f);
    EXPECT_NEAR(0.5f, cam->mClipPlaneNear, 1e-5f);
    EXPECT_NEAR(50.0f, cam->mClipPlaneFar, 1e-4f);

    const aiLight *spot = nullptr;
    const aiLight *disk = nullptr;
    for (unsigned i = 0; i < scene->mNumLights; ++i) {
        if (std::string(scene->mLights[i]->mName.C_Str()) == "Spot") {
            spot = scene->mLights[i];
        } else if (std::string(scene->mLights[i]->mName.C_Str()) == "Disk") {
            disk = scene->mLights[i];
        }
    }
    ASSERT_NE(nullptr, spot);
    EXPECT_EQ(aiLightSource_SPOT, spot->mType);
    EXPECT_NEAR(30.0f * static_cast<float>(AI_MATH_PI) / 180.0f, spot->mAngleOuterCone, 1e-4f);
    // colour * intensity * 2^exposure (D12)
    EXPECT_NEAR(8.0f, spot->mColorDiffuse.r, 1e-4f);
    EXPECT_NEAR(4.0f, spot->mColorDiffuse.g, 1e-4f);
    EXPECT_NEAR(2.0f, spot->mColorDiffuse.b, 1e-4f);
    EXPECT_NEAR(-1.0f, spot->mDirection.z, 1e-6f);
    const aiNode *spotNode = scene->mRootNode->FindNode("Spot");
    ASSERT_NE(nullptr, spotNode);
    EXPECT_NEAR(2.0f, spotNode->mTransformation.b4, 1e-6f);
    ASSERT_NE(nullptr, disk);
    EXPECT_EQ(aiLightSource_AREA, disk->mType);
    EXPECT_NEAR(1.0f, disk->mSize.x, 1e-6f);
}

// ------------------------------------------------------------------------------------------------
// Animation (D13) and repeated reads (D19)

TEST_F(utUSDImport, skelAnimationHasKeysAndTiming) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usda/simple-skin-animation-test.usda", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_TRUE(scene->HasAnimations());
    const aiAnimation *anim = scene->mAnimations[0];
    EXPECT_GT(anim->mTicksPerSecond, 0.0);
    EXPECT_GT(anim->mDuration, 0.0);
    for (unsigned c = 0; c < anim->mNumChannels; ++c) {
        const aiNodeAnim *ch = anim->mChannels[c];
        EXPECT_NE(nullptr, scene->mRootNode->FindNode(ch->mNodeName)) << ch->mNodeName.C_Str();
        EXPECT_GT(ch->mNumPositionKeys, 0u);
        EXPECT_GT(ch->mNumRotationKeys, 0u);
        EXPECT_GT(ch->mNumScalingKeys, 0u);
    }
}

TEST_F(utUSDImport, repeatedReadsAreIndependent) {
    Assimp::Importer importer;
    const aiScene *first = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/USD/usda/camera-lighting.usda", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, first);
    const unsigned lights = first->mNumLights;
    const unsigned vertices = CountMeshVertices(first);
    const aiScene *other = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usdc/suzanne.usdc", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, other);
    EXPECT_EQ(0u, other->mNumLights);
    const aiScene *again = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/USD/usda/camera-lighting.usda", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, again);
    EXPECT_EQ(lights, again->mNumLights);
    EXPECT_EQ(vertices, CountMeshVertices(again));
}

// ------------------------------------------------------------------------------------------------
// USDZ

TEST_F(utUSDImport, usdzWithEmbeddedTextures) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usdz/damaged-helmet-gltf.usdz", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    EXPECT_EQ(5u, scene->mNumTextures);
    for (unsigned i = 0; i < scene->mNumTextures; ++i) {
        const aiTexture *tex = scene->mTextures[i];
        EXPECT_EQ(0u, tex->mHeight);
        EXPECT_GT(tex->mWidth, 1000u);
        EXPECT_STREQ("jpg", tex->achFormatHint);
        // JPEG SOI marker: the original encoded bytes are kept.
        const auto *bytes = reinterpret_cast<const unsigned char *>(tex->pcData);
        EXPECT_EQ(0xFF, bytes[0]);
        EXPECT_EQ(0xD8, bytes[1]);
    }
    const aiMaterial *mat = scene->mMaterials[scene->mMeshes[0]->mMaterialIndex];
    EXPECT_GT(mat->GetTextureCount(aiTextureType_BASE_COLOR), 0u);
    EXPECT_GT(mat->GetTextureCount(aiTextureType_NORMALS), 0u);
}

TEST_F(utUSDImport, usdzFromInMemoryIOSystem) {
    const std::string usdz = ReadBinaryFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usdz/cube.usdz");
    Assimp::Importer importer;
    const aiScene *scene = ReadFromMap(importer, { { "package.usdz", usdz } }, "package.usdz");
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_EQ(1u, scene->mNumMeshes);
}

// ------------------------------------------------------------------------------------------------
// UsdPhysics metadata

TEST_F(utUSDImport, physicsJointAndBodies) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/USD/usda/physics-joint.usda", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    const aiNode *joint = scene->mRootNode->FindNode("D6Joint");
    ASSERT_NE(nullptr, joint);
    ASSERT_NE(nullptr, joint->mParent);
    EXPECT_STREQ("Root", joint->mParent->mName.C_Str());
    const aiMetadata *jm = joint->mMetaData;
    ASSERT_NE(nullptr, jm);
    aiString s;
    ASSERT_TRUE(jm->Get("usdPhysics:jointType", s));
    EXPECT_STREQ("joint", s.C_Str());
    ASSERT_TRUE(jm->Get("usdPhysics:body0", s));
    EXPECT_STREQ("Box0", s.C_Str());
    ASSERT_TRUE(jm->Get("usdPhysics:body1", s));
    EXPECT_STREQ("Box1", s.C_Str());
    aiVector3D pos;
    ASSERT_TRUE(jm->Get("usdPhysics:localPos0", pos));
    EXPECT_NEAR(1.0f, pos.x, 1e-6f);
    ASSERT_TRUE(jm->Get("usdPhysics:localPos1", pos));
    EXPECT_NEAR(-1.0f, pos.x, 1e-6f);
    ASSERT_TRUE(jm->Get("usdPhysics:localRot0", s));
    EXPECT_STREQ("1 0 0 0", s.C_Str());

    for (const char *body : { "Box0", "Box1" }) {
        const aiMetadata *bm = NodeMeta(scene, body);
        ASSERT_NE(nullptr, bm) << body;
        bool enabled = false;
        ASSERT_TRUE(bm->Get("usdPhysics:rigidBodyEnabled", enabled)) << body;
        EXPECT_TRUE(enabled);
    }
}

TEST_F(utUSDImport, physicsBlenderSceneMetadata) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/USD/usda/blender-physics.usda", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    aiVector3D gravity;
    ASSERT_TRUE(scene->mMetaData->Get("usdPhysics:gravityDirection", gravity));
    EXPECT_NEAR(-1.0f, gravity.z, 1e-6f);
    float magnitude = 0.0f;
    ASSERT_TRUE(scene->mMetaData->Get("usdPhysics:gravityMagnitude", magnitude));
    EXPECT_NEAR(9.81f, magnitude, 1e-5f);
    EXPECT_EQ(2, readUpAxis(scene));

    const aiMetadata *ground = NodeMeta(scene, "Ground");
    ASSERT_NE(nullptr, ground);
    bool b = true;
    ASSERT_TRUE(ground->Get("usdPhysics:rigidBodyEnabled", b));
    EXPECT_FALSE(b);
    ASSERT_TRUE(ground->Get("usdPhysics:collisionEnabled", b));
    EXPECT_TRUE(b);
    float f = 0.0f;
    ASSERT_TRUE(ground->Get("usdPhysics:mass", f));
    EXPECT_NEAR(1.0f, f, 1e-6f);
    ASSERT_TRUE(ground->Get("usdPhysics:staticFriction", f));
    EXPECT_NEAR(0.8f, f, 1e-6f);
    ASSERT_TRUE(ground->Get("usdPhysics:dynamicFriction", f));
    EXPECT_NEAR(0.8f, f, 1e-6f);
    ASSERT_TRUE(ground->Get("usdPhysics:restitution", f));
    EXPECT_NEAR(0.3f, f, 1e-6f);

    const aiMetadata *anchor = NodeMeta(scene, "Anchor");
    ASSERT_NE(nullptr, anchor);
    ASSERT_TRUE(anchor->Get("usdPhysics:kinematicEnabled", b));
    EXPECT_TRUE(b);

    // Joint prims become nodes under their parent prim ("/Joints").
    const aiNode *joints = scene->mRootNode->FindNode("Joints");
    ASSERT_NE(nullptr, joints);
    const aiNode *hinge = joints->FindNode("HingeConstraint");
    ASSERT_NE(nullptr, hinge);
    ASSERT_NE(nullptr, hinge->mMetaData);
    aiString s;
    ASSERT_TRUE(hinge->mMetaData->Get("usdPhysics:jointType", s));
    EXPECT_STREQ("revolute", s.C_Str());
    ASSERT_TRUE(hinge->mMetaData->Get("usdPhysics:axis", s));
    EXPECT_STREQ("Z", s.C_Str());
    ASSERT_TRUE(hinge->mMetaData->Get("usdPhysics:body0", s));
    EXPECT_STREQ("Anchor", s.C_Str());
    ASSERT_TRUE(hinge->mMetaData->Get("usdPhysics:lowerLimit", f));
    EXPECT_NEAR(-90.0f, f, 1e-5f);
    ASSERT_TRUE(hinge->mMetaData->Get("usdPhysics:upperLimit", f));
    EXPECT_NEAR(90.0f, f, 1e-5f);
    const aiNode *fixed = joints->FindNode("FixedConstraint");
    ASSERT_NE(nullptr, fixed);
    ASSERT_TRUE(fixed->mMetaData->Get("usdPhysics:jointType", s));
    EXPECT_STREQ("fixed", s.C_Str());
}

TEST_F(utUSDImport, maxMemoryPropertyIsHonoured) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_USD_MAX_MEMORY_MB, 1);
    const std::string usdc = ReadBinaryFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usdz/damaged-helmet-gltf.usdz");
    const aiScene *scene = ReadFromMap(importer, { { "helmet.usdz", usdc } }, "helmet.usdz");
    // A 1 MiB budget is too small for a 4 MiB package: the import fails cleanly.
    EXPECT_EQ(nullptr, scene);
}
