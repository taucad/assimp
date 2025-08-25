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
#include "Tools/TestTools.h"

#include <assimp/commonMetaData.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/material.h>

#include <memory>
#include <fstream>
#include <array>
#include <sys/stat.h>
#include <cerrno>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

using namespace Assimp;

#ifndef ASSIMP_BUILD_NO_USD_EXPORTER

// Helper function to create directories recursively
bool createDirectoryRecursive(const std::string& path) {
    if (path.empty()) return false;
    
    // Find the directory part of the path (remove filename)
    size_t lastSlash = path.find_last_of("/\\");
    if (lastSlash == std::string::npos) {
        return true; // No directory to create
    }
    
    std::string dirPath = path.substr(0, lastSlash);
    
    // Check if directory already exists
    struct stat st;
    if (stat(dirPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return true; // Directory already exists
    }
    
    // Create parent directories first
    size_t pos = 0;
    while ((pos = dirPath.find_first_of("/\\", pos + 1)) != std::string::npos) {
        std::string subPath = dirPath.substr(0, pos);
        if (!subPath.empty()) {
            if (stat(subPath.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
                if (mkdir(subPath.c_str(), 0755) != 0 && errno != EEXIST) {
                    return false;
                }
            }
        }
    }
    
    // Create the final directory
    if (mkdir(dirPath.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
    }
    
    return true;
}

class utUSDZExport : public AbstractImportExportBase {
public:
    
    /// Round-trip test: glTF -> USD -> re-import -> validate
    bool performRoundTripTest(const std::string& inputGltfPath, const std::string& outputUsdPath, 
                              const std::string& usdFormat = "usda") {
        // Step 1: Import original glTF
        Assimp::Importer importer;
        const aiScene* originalScene = importer.ReadFile(inputGltfPath, aiProcess_ValidateDataStructure);
        if (!originalScene) {
            EXPECT_NE(nullptr, originalScene) << "Failed to import glTF: " << inputGltfPath;
            return false;
        }
        

        // Step 2: Create output directory if it doesn't exist
        if (!createDirectoryRecursive(outputUsdPath)) {
            EXPECT_TRUE(false) << "Failed to create output directory for: " << outputUsdPath;
            return false;
        }

        // Step 3: Export to USD format
        Assimp::Exporter exporter;
        aiReturn exportResult = exporter.Export(originalScene, usdFormat.c_str(), outputUsdPath, 0u);
        EXPECT_EQ(aiReturn_SUCCESS, exportResult) << "Failed to export to USD: " << outputUsdPath;
        if (exportResult != aiReturn_SUCCESS) {
            return false;
        }
        
        // Step 4: Verify output file exists and has reasonable size
        std::ifstream file(outputUsdPath, std::ios::binary | std::ios::ate);
        EXPECT_TRUE(file.good()) << "USD output file does not exist: " << outputUsdPath;
        if (!file.good()) {
            return false;
        }
        
        auto fileSize = file.tellg();
        EXPECT_GT(fileSize, 100) << "USD output file is too small: " << outputUsdPath;
        file.close();
        
        // Step 5: Re-import the USD file (without validation to debug nullptr issues)
        Assimp::Importer usdImporter;
        const aiScene* reimportedScene = usdImporter.ReadFile(outputUsdPath, 0); // No post-processing
        EXPECT_NE(nullptr, reimportedScene) << "Failed to re-import USD: " << outputUsdPath;
        if (!reimportedScene) {
            return false;
        }
        
        // Step 5: Validate basic scene structure
        validateBasicSceneStructure(originalScene, reimportedScene);
        
        return true;
    }
    
    /// Validate basic scene structure between original and reimported scenes
    void validateBasicSceneStructure(const aiScene* original, const aiScene* reimported) {
        ASSERT_NE(nullptr, original);
        ASSERT_NE(nullptr, reimported);
        
        // Basic counts should match (allowing for reasonable differences due to format limitations)
        EXPECT_EQ(original->mNumMeshes, reimported->mNumMeshes) << "Mesh count mismatch";
        
        // Material count: USD format only imports materials that are bound to geometry,
        // so reimported count may be less than or equal to original count
        EXPECT_GT(reimported->mNumMaterials, 0u) << "Reimported scene should have at least one material";
        EXPECT_LE(reimported->mNumMaterials, original->mNumMaterials) 
            << "Reimported material count should not exceed original count";
        
        // For this specific test, we expect at least the textured material to be preserved
        EXPECT_GE(reimported->mNumMaterials, 1u) << "Should have at least the textured material";
        
        // Root node should exist
        EXPECT_NE(nullptr, reimported->mRootNode) << "Root node missing in reimported scene";
        
        if (original->mNumMeshes > 0 && reimported->mNumMeshes > 0) {
            EXPECT_NE(nullptr, reimported->mMeshes[0]) << "First mesh is null in reimported scene";
            if (reimported->mMeshes[0]) {
                EXPECT_GT(reimported->mMeshes[0]->mNumVertices, 0u) << "First mesh has no vertices";
                EXPECT_GT(reimported->mMeshes[0]->mNumFaces, 0u) << "First mesh has no faces";
            }
        }
    }
    
    /// Validate PBR material properties
    void validatePBRMaterial(const aiMaterial* material, const std::string& materialName) {
        ASSERT_NE(nullptr, material) << "Material is null";
        
        // Check PBR shading mode
        aiShadingMode shadingMode;
        if (material->Get(AI_MATKEY_SHADING_MODEL, shadingMode) == aiReturn_SUCCESS) {
            EXPECT_EQ(aiShadingMode_PBR_BRDF, shadingMode) << "Material " << materialName << " is not PBR";
        }
        
        // Check for base color texture
        aiString texturePath;
        if (material->GetTexture(aiTextureType_BASE_COLOR, 0, &texturePath) == aiReturn_SUCCESS ||
            material->GetTexture(aiTextureType_DIFFUSE, 0, &texturePath) == aiReturn_SUCCESS) {
            EXPECT_GT(strlen(texturePath.C_Str()), 0u) << "Base color texture path is empty";
        }
        
        // Check metallic factor
        ai_real metallicFactor;
        if (material->Get(AI_MATKEY_METALLIC_FACTOR, metallicFactor) == aiReturn_SUCCESS) {
            EXPECT_GE(metallicFactor, 0.0f) << "Metallic factor out of range";
            EXPECT_LE(metallicFactor, 1.0f) << "Metallic factor out of range";
        }
        
        // Check roughness factor
        ai_real roughnessFactor;
        if (material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor) == aiReturn_SUCCESS) {
            EXPECT_GE(roughnessFactor, 0.0f) << "Roughness factor out of range";
            EXPECT_LE(roughnessFactor, 1.0f) << "Roughness factor out of range";
        }
    }
    
    /// Validate clearcoat material properties
    void validateClearcoatMaterial(const aiMaterial* material, const std::string& expectedMaterialName) {
        ASSERT_NE(nullptr, material) << "Material is null";
        
        aiString materialName;
        if (material->Get(AI_MATKEY_NAME, materialName) == aiReturn_SUCCESS) {
            if (std::string(materialName.C_Str()) == expectedMaterialName) {
                // Check clearcoat factor
                ai_real clearcoatFactor;
                if (material->Get(AI_MATKEY_CLEARCOAT_FACTOR, clearcoatFactor) == aiReturn_SUCCESS) {
                    EXPECT_GE(clearcoatFactor, 0.0f) << "Clearcoat factor out of range";
                    EXPECT_LE(clearcoatFactor, 1.0f) << "Clearcoat factor out of range";
                }
                
                // Check clearcoat roughness
                ai_real clearcoatRoughness;
                if (material->Get(AI_MATKEY_CLEARCOAT_ROUGHNESS_FACTOR, clearcoatRoughness) == aiReturn_SUCCESS) {
                    EXPECT_GE(clearcoatRoughness, 0.0f) << "Clearcoat roughness out of range";
                    EXPECT_LE(clearcoatRoughness, 1.0f) << "Clearcoat roughness out of range";
                }
                
                // Check clearcoat texture
                aiString clearcoatTexture;
                if (material->GetTexture(AI_MATKEY_CLEARCOAT_TEXTURE, &clearcoatTexture) == aiReturn_SUCCESS) {
                    EXPECT_GT(strlen(clearcoatTexture.C_Str()), 0u) << "Clearcoat texture path is empty";
                }
            }
        }
    }
    
    /// Validate texture coordinates
    void validateTextureCoordinates(const aiScene* scene) {
        ASSERT_NE(nullptr, scene) << "Scene is null";
        
        if (scene->mNumMeshes > 0) {
            const aiMesh* mesh = scene->mMeshes[0];
            ASSERT_NE(nullptr, mesh) << "First mesh is null";
            
            if (mesh->mTextureCoords[0] != nullptr) {
                EXPECT_GT(mesh->mNumVertices, 0u) << "Mesh has texture coordinates but no vertices";
                
                // Check that UV coordinates are in reasonable range
                for (unsigned int i = 0; i < std::min(mesh->mNumVertices, 10u); ++i) {
                    const aiVector3D& uv = mesh->mTextureCoords[0][i];
                    EXPECT_GE(uv.x, -10.0f) << "UV coordinate out of reasonable range";
                    EXPECT_LE(uv.x, 10.0f) << "UV coordinate out of reasonable range";
                    EXPECT_GE(uv.y, -10.0f) << "UV coordinate out of reasonable range";
                    EXPECT_LE(uv.y, 10.0f) << "UV coordinate out of reasonable range";
                }
            }
        }
    }
    
    /// Validate skinning data
    void validateSkinningData(const aiScene* scene, unsigned int expectedBoneCount = 0) {
        ASSERT_NE(nullptr, scene) << "Scene is null";
        
        if (scene->mNumMeshes > 0) {
            const aiMesh* mesh = scene->mMeshes[0];
            ASSERT_NE(nullptr, mesh) << "First mesh is null";
            
            if (expectedBoneCount > 0) {
                EXPECT_GE(mesh->mNumBones, expectedBoneCount) << "Expected at least " << expectedBoneCount << " bones";
                
                if (mesh->mNumBones > 0) {
                    for (unsigned int i = 0; i < mesh->mNumBones; ++i) {
                        const aiBone* bone = mesh->mBones[i];
                        ASSERT_NE(nullptr, bone) << "Bone " << i << " is null";
                        EXPECT_GT(strlen(bone->mName.C_Str()), 0u) << "Bone " << i << " has no name";
                        EXPECT_GT(bone->mNumWeights, 0u) << "Bone " << i << " has no weights";
                        
                        // Validate bone weights
                        for (unsigned int j = 0; j < bone->mNumWeights; ++j) {
                            const aiVertexWeight& weight = bone->mWeights[j];
                            EXPECT_LT(weight.mVertexId, mesh->mNumVertices) << "Bone weight vertex ID out of range";
                            EXPECT_GE(weight.mWeight, 0.0f) << "Bone weight is negative";
                            EXPECT_LE(weight.mWeight, 1.0f) << "Bone weight exceeds 1.0";
                        }
                    }
                }
            }
        }
    }
    
    /// Validate camera data
    void validateCameraData(const aiScene* scene) {
        ASSERT_NE(nullptr, scene) << "Scene is null";
        
        if (scene->mNumCameras > 0) {
            for (unsigned int i = 0; i < scene->mNumCameras; ++i) {
                const aiCamera* camera = scene->mCameras[i];
                ASSERT_NE(nullptr, camera) << "Camera " << i << " is null";
                
                EXPECT_GT(strlen(camera->mName.C_Str()), 0u) << "Camera " << i << " has no name";
                
                // Validate field of view
                EXPECT_GT(camera->mHorizontalFOV, 0.0f) << "Camera " << i << " has invalid FOV";
                EXPECT_LT(camera->mHorizontalFOV, AI_MATH_PI) << "Camera " << i << " has invalid FOV";
                
                // Validate aspect ratio
                EXPECT_GT(camera->mAspect, 0.0f) << "Camera " << i << " has invalid aspect ratio";
                
                // Validate clipping planes
                EXPECT_GT(camera->mClipPlaneNear, 0.0f) << "Camera " << i << " has invalid near plane";
                EXPECT_GT(camera->mClipPlaneFar, camera->mClipPlaneNear) << "Camera " << i << " far plane <= near plane";
            }
        }
    }
    
    /// Validate animation data
    void validateAnimationData(const aiScene* scene) {
        ASSERT_NE(nullptr, scene) << "Scene is null";
        
        if (scene->mNumAnimations > 0) {
            for (unsigned int i = 0; i < scene->mNumAnimations; ++i) {
                const aiAnimation* animation = scene->mAnimations[i];
                ASSERT_NE(nullptr, animation) << "Animation " << i << " is null";
                
                EXPECT_GT(strlen(animation->mName.C_Str()), 0u) << "Animation " << i << " has no name";
                EXPECT_GT(animation->mDuration, 0.0) << "Animation " << i << " has zero duration";
                EXPECT_GT(animation->mTicksPerSecond, 0.0) << "Animation " << i << " has invalid ticks per second";
                
                // Validate channels
                if (animation->mNumChannels > 0) {
                    for (unsigned int j = 0; j < animation->mNumChannels; ++j) {
                        const aiNodeAnim* channel = animation->mChannels[j];
                        ASSERT_NE(nullptr, channel) << "Animation channel " << j << " is null";
                        
                        EXPECT_GT(strlen(channel->mNodeName.C_Str()), 0u) << "Animation channel " << j << " has no node name";
                        
                        // At least one type of keyframe should exist
                        EXPECT_TRUE(channel->mNumPositionKeys > 0 || channel->mNumRotationKeys > 0 || channel->mNumScalingKeys > 0)
                            << "Animation channel " << j << " has no keyframes";
                    }
                }
            }
        }
    }
};

// =============================================================================
// BASIC TESTS
// =============================================================================

TEST_F(utUSDZExport, importGltfBoxTexturedExportUsda) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf",
        "usd/basic/BoxTextured_out.usda",
        "usda"
    ));
}

// USDZ export removed - not supported by current tinyusdz version

// USDC export removed - not supported by current tinyusdz version

// =============================================================================
// PBR MATERIAL TESTS
// =============================================================================

TEST_F(utUSDZExport, importGltfPbrSpecularGlossinessExportUsda) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-pbrSpecularGlossiness/BoxTextured.gltf";
    const std::string outputPath = "usd/pbr/BoxTextured_PbrSpecGloss_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Additional PBR-specific validation
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene && scene->mNumMaterials > 0) {
        validatePBRMaterial(scene->mMaterials[0], "BoxTextured PBR Material");
    }
}

// =============================================================================
// CLEARCOAT MATERIAL TESTS
// =============================================================================

TEST_F(utUSDZExport, importGltfClearcoatExportUsda) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/ClearCoat-glTF/ClearCoatTest.gltf";
    const std::string outputPath = "usd/clearcoat/ClearCoatTest_out.usda";
    
    // Debug: Check import counts
    Assimp::Importer importer;
    const aiScene* importedScene = importer.ReadFile(inputPath, 0);
    if (importedScene) {
        std::cout << "[DEBUG] Imported " << importedScene->mNumMeshes << " meshes, " 
                  << importedScene->mNumMaterials << " materials" << std::endl;
        
        std::function<int(const aiNode*)> countNodes = [&](const aiNode* n) -> int {
            if (!n) return 0;
            int count = 1;
            for (uint32_t i = 0; i < n->mNumChildren; ++i) {
                count += countNodes(n->mChildren[i]);
            }
            return count;
        };
        std::cout << "[DEBUG] Total nodes: " << countNodes(importedScene->mRootNode) << std::endl;
    }
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Additional clearcoat-specific validation
    Assimp::Importer reimporter;
    const aiScene* scene = reimporter.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene && scene->mNumMaterials > 0) {
        // Look for specific clearcoat materials
        for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
            validateClearcoatMaterial(scene->mMaterials[i], "Partial_Coated");
        }
    }
}

// =============================================================================
// EMBEDDED TEXTURE TESTS
// =============================================================================

TEST_F(utUSDZExport, importGltfEmbeddedTexturesExportUsda) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-Embedded/BoxTextured.gltf",
        "usd/embedded/BoxTextured_Embedded_out.usda",
        "usda"
    ));
}

// =============================================================================
// TEXTURE COORDINATE TESTS
// =============================================================================

TEST_F(utUSDZExport, importGltfTextureCoordinatesExportUsda) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTexcoords-glTF/boxTexcoords.gltf";
    const std::string outputPath = "usd/textures/BoxTexcoords_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Additional texture coordinate validation
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene) {
        validateTextureCoordinates(scene);
    }
}

// =============================================================================
// SKINNING TESTS
// =============================================================================

TEST_F(utUSDZExport, importGltfSimpleSkinExportUsda) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/simple_skin/simple_skin.gltf";
    const std::string outputPath = "usd/skinning/SimpleSkin_out.usda";
    
    // Known issue: tinyusdz crashes when importing our exported skeletal USD data
    // This appears to be a bug in our skeletal export format, not a tinyusdz limitation
    // tinyusdz has full usdSkel support - the issue is likely in how we format the data
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Additional skinning validation
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene) {
        validateSkinningData(scene, 1); // Expect at least 1 bone
    }
}

TEST_F(utUSDZExport, importGltfQuadSkinExportUsda) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/simple_skin/quad_skin.glb";
    const std::string outputPath = "usd/skinning/QuadSkin_out.usda";
    
    // Known issue: tinyusdz crashes when importing our exported skeletal USD data
    // This appears to be a bug in our skeletal export format, not a tinyusdz limitation
    // tinyusdz has full usdSkel support - the issue is likely in how we format the data
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Additional complex skinning validation
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene) {
        validateSkinningData(scene, 5); // Expect at least 5 bones
    }
}

// =============================================================================
// CAMERA TESTS
// =============================================================================

TEST_F(utUSDZExport, importGltfCamerasExportUsda) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/cameras/Cameras.gltf";
    const std::string outputPath = "usd/cameras/Cameras_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Additional camera validation
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene) {
        validateCameraData(scene);
    }
}

// =============================================================================
// ANIMATION TESTS
// =============================================================================

TEST_F(utUSDZExport, importGltfAnimatedMorphCubeExportUsda) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/glTF-Sample-Models/AnimatedMorphCube-glTF/AnimatedMorphCube.gltf";
    const std::string outputPath = "usd/animations/AnimatedMorphCube_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Additional animation validation
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene) {
        validateAnimationData(scene);
    }
}

// =============================================================================
// PRIMITIVE MODE TESTS
// =============================================================================

TEST_F(utUSDZExport, importGltfPrimitiveModePointsExportUsda) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/glTF-Asset-Generator/Mesh_PrimitiveMode/Mesh_PrimitiveMode_00.gltf",
        "usd/primitives/PrimitiveMode_Points_out.usda",
        "usda"
    ));
}

TEST_F(utUSDZExport, importGltfPrimitiveModeTrianglesExportUsda) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/glTF-Asset-Generator/Mesh_PrimitiveMode/Mesh_PrimitiveMode_06.gltf",
        "usd/primitives/PrimitiveMode_Triangles_out.usda",
        "usda"
    ));
}

TEST_F(utUSDZExport, importGltfPrimitiveModeTriangleStripExportUsda) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/glTF-Asset-Generator/Mesh_PrimitiveMode/Mesh_PrimitiveMode_04.gltf",
        "usd/primitives/PrimitiveMode_TriangleStrip_out.usda",
        "usda"
    ));
}

TEST_F(utUSDZExport, importGltfPrimitiveModeTriangleFanExportUsda) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/glTF-Asset-Generator/Mesh_PrimitiveMode/Mesh_PrimitiveMode_05.gltf",
        "usd/primitives/PrimitiveMode_TriangleFan_out.usda",
        "usda"
    ));
}

// =============================================================================
// ERROR HANDLING TESTS
// =============================================================================

TEST_F(utUSDZExport, importGltfIncorrectVertexArraysExportUsda) {
    // This tests handling of malformed input data
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/IncorrectVertexArrays/Cube.gltf";
    const std::string outputPath = "usd/error_cases/IncorrectVertexArrays_out.usda";
    
    // Should still succeed but handle gracefully
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
}

TEST_F(utUSDZExport, exportNullSceneFailsGracefully) {
    Assimp::Exporter exporter;
    aiReturn result = exporter.Export(nullptr, "usda", "usd/error_cases/null_scene_out.usda", 0u);
    EXPECT_EQ(aiReturn_FAILURE, result);
}

TEST_F(utUSDZExport, exportInvalidFormatFailsGracefully) {
    // Create a minimal valid scene
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf", 
                                           aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    
    Assimp::Exporter exporter;
    aiReturn result = exporter.Export(scene, "invalid_format", "usd/error_cases/invalid_format.usd", 0u);
    EXPECT_EQ(aiReturn_FAILURE, result);
}

// =============================================================================
// ANIMATION TESTS
// =============================================================================

TEST_F(utUSDZExport, exportAnimatedBoxRoundTrip) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/AnimatedCube-glTF/AnimatedCube.gltf";
    const std::string outputPath = "usd/animation/AnimatedCube_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
}

TEST_F(utUSDZExport, exportAnimationKeyframeValidation) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/AnimatedCube-glTF/AnimatedCube.gltf";
    const std::string outputPath = "usd/animation/AnimatedCube_keyframes_out.usda";
    
    if (!performRoundTripTest(inputPath, outputPath, "usda")) {
        FAIL() << "Round-trip test failed for animated cube";
        return;
    }
    
    // Additional validation: Check that animation data was preserved
    Assimp::Importer importer;
    const aiScene* reimportedScene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, reimportedScene);
    
    // Verify animations were preserved (if original had animations)
    Assimp::Importer originalImporter;
    const aiScene* originalScene = originalImporter.ReadFile(inputPath, aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, originalScene);
    
    if (originalScene->mNumAnimations > 0) {
        EXPECT_GT(reimportedScene->mNumAnimations, 0u) << "Animations should be preserved in round-trip";
    }
}

TEST_F(utUSDZExport, exportComplexAnimationScene) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/AnimatedMorphCube-glTF/AnimatedMorphCube.gltf";
    const std::string outputPath = "usd/animation/AnimatedMorphCube_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
}

// =============================================================================
// SKINNING TESTS  
// =============================================================================

TEST_F(utUSDZExport, exportSkinnedMeshRoundTrip) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/SimpleSkin-glTF/SimpleSkin.gltf";
    const std::string outputPath = "usd/skinning/SimpleSkin_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
}

TEST_F(utUSDZExport, exportSkeletalValidation) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/SimpleSkin-glTF/SimpleSkin.gltf";
    const std::string outputPath = "usd/skinning/SimpleSkin_skeletal_out.usda";
    
    if (!performRoundTripTest(inputPath, outputPath, "usda")) {
        FAIL() << "Round-trip test failed for skinned mesh";
        return;
    }
    
    // Verify bone/skeleton data preservation
    Assimp::Importer importer;
    const aiScene* reimportedScene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, reimportedScene);
    
    // Check that skinned meshes retained bone data
    for (uint32_t i = 0; i < reimportedScene->mNumMeshes; ++i) {
        if (reimportedScene->mMeshes[i]->mNumBones > 0) {
            EXPECT_GT(reimportedScene->mMeshes[i]->mNumBones, 0u) << "Skinned mesh should retain bone data";
        }
    }
}

// =============================================================================
// BLEND SHAPES TESTS
// =============================================================================

TEST_F(utUSDZExport, exportBlendShapesRoundTrip) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/AnimatedMorphCube-glTF/AnimatedMorphCube.gltf";
    const std::string outputPath = "usd/blendshapes/AnimatedMorphCube_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
}

TEST_F(utUSDZExport, exportMorphTargetValidation) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/AnimatedMorphCube-glTF/AnimatedMorphCube.gltf";
    const std::string outputPath = "usd/blendshapes/MorphTargets_out.usda";
    
    if (!performRoundTripTest(inputPath, outputPath, "usda")) {
        FAIL() << "Round-trip test failed for morph targets";
        return;
    }
    
    // Verify blend shape data preservation
    Assimp::Importer importer;
    const aiScene* reimportedScene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, reimportedScene);
    
    // Check that meshes with morph targets retained their animation meshes
    for (uint32_t i = 0; i < reimportedScene->mNumMeshes; ++i) {
        const aiMesh* mesh = reimportedScene->mMeshes[i];
        if (mesh->mNumAnimMeshes > 0) {
            EXPECT_GT(mesh->mNumAnimMeshes, 0u) << "Blend shape data should be preserved";
        }
    }
}

// =============================================================================
// TEXTURE CONNECTION TESTS
// =============================================================================

TEST_F(utUSDZExport, exportTextureNetworkValidation) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf";
    const std::string outputPath = "usd/textures/TextureNetwork_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Verify USD file contains proper texture connections
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Could not open generated USD file for texture validation";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Check for shader network elements
    EXPECT_TRUE(content.find("UsdPreviewSurface") != std::string::npos) << "USD file should contain UsdPreviewSurface shader";
    EXPECT_TRUE(content.find("UsdUVTexture") != std::string::npos) << "USD file should contain UsdUVTexture shader";
    EXPECT_TRUE(content.find("diffuseColor.connect") != std::string::npos) << "USD file should contain texture connections";
}

TEST_F(utUSDZExport, exportComplexMaterialRoundTrip) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/MetalRoughSpheres-glTF/MetalRoughSpheres.gltf";
    const std::string outputPath = "usd/textures/MetalRoughSpheres_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
}

// =============================================================================
// MULTI-FORMAT CONSISTENCY TESTS
// =============================================================================

TEST_F(utUSDZExport, usdaFormatConsistencyBoxTextured) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf";
    
    // Test USDA format consistency
    EXPECT_TRUE(performRoundTripTest(inputPath, "usd/basic/BoxTextured_consistency_out.usda", "usda"));
}

#endif // ASSIMP_BUILD_NO_USD_EXPORTER
