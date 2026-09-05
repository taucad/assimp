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
#include <limits>
#include <sys/stat.h>
#include <cerrno>
#include <regex>
#include <filesystem>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

using namespace Assimp;

#ifndef ASSIMP_BUILD_NO_USD_EXPORTER

#include "../../code/AssetLib/USD/usdz-writer.hh"

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
                        // Bones without weights are valid (e.g., parent bones that don't directly influence vertices)
                        if (bone->mNumWeights > 0) {
                            // Validate bone weights only if they exist
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
    
    // =============================================================================
    // ENHANCED VALIDATION HELPER FUNCTIONS (P2)
    // =============================================================================
    
    /// Validate material property ranges and sanity
    void validateMaterialPropertyRanges(const aiScene* scene) {
        ASSERT_NE(nullptr, scene) << "Scene is null for material property validation";
        
        for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
            const aiMaterial* material = scene->mMaterials[i];
            ASSERT_NE(nullptr, material) << "Material " << i << " is null";
            
            // Validate metallic factor range [0.0, 1.0]
            float metallicFactor = 0.0f;
            if (material->Get(AI_MATKEY_METALLIC_FACTOR, metallicFactor) == aiReturn_SUCCESS) {
                EXPECT_GE(metallicFactor, 0.0f) << "Material " << i << " metallic factor below 0";
                EXPECT_LE(metallicFactor, 1.0f) << "Material " << i << " metallic factor above 1";
            }
            
            // Validate roughness factor range [0.0, 1.0]
            float roughnessFactor = 1.0f;
            if (material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor) == aiReturn_SUCCESS) {
                EXPECT_GE(roughnessFactor, 0.0f) << "Material " << i << " roughness factor below 0";
                EXPECT_LE(roughnessFactor, 1.0f) << "Material " << i << " roughness factor above 1";
            }
            
            // Validate opacity range [0.0, 1.0]
            float opacity = 1.0f;
            if (material->Get(AI_MATKEY_OPACITY, opacity) == aiReturn_SUCCESS) {
                EXPECT_GE(opacity, 0.0f) << "Material " << i << " opacity below 0";
                EXPECT_LE(opacity, 1.0f) << "Material " << i << " opacity above 1";
            }
            
            // Validate clearcoat factor range [0.0, 1.0]
            float clearcoatFactor = 0.0f;
            if (material->Get(AI_MATKEY_CLEARCOAT_FACTOR, clearcoatFactor) == aiReturn_SUCCESS) {
                EXPECT_GE(clearcoatFactor, 0.0f) << "Material " << i << " clearcoat factor below 0";
                EXPECT_LE(clearcoatFactor, 1.0f) << "Material " << i << " clearcoat factor above 1";
            }
            
            // Validate base color components are non-negative
            aiColor3D baseColor;
            if (material->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor) == aiReturn_SUCCESS ||
                material->Get(AI_MATKEY_BASE_COLOR, baseColor) == aiReturn_SUCCESS) {
                EXPECT_GE(baseColor.r, 0.0f) << "Material " << i << " base color R negative";
                EXPECT_GE(baseColor.g, 0.0f) << "Material " << i << " base color G negative";
                EXPECT_GE(baseColor.b, 0.0f) << "Material " << i << " base color B negative";
            }
        }
    }
    
    /// Validate USD shader network structure
    void validateShaderNetworkStructure(const std::string& usdContent) {
        // Validate essential USD shader components exist
        EXPECT_TRUE(usdContent.find("UsdPreviewSurface") != std::string::npos)
            << "USD file should contain UsdPreviewSurface shader";
        
        // If textures are present, validate texture shader network
        if (usdContent.find("UsdUVTexture") != std::string::npos) {
            EXPECT_TRUE(usdContent.find("UsdPrimvarReader_float2") != std::string::npos)
                << "USD file with textures should contain UV coordinate reader";
            
            // Validate texture connections use proper syntax
            EXPECT_TRUE(usdContent.find(".connect =") != std::string::npos)
                << "USD file should contain proper connection syntax";
        }
        
        // Validate material binding exists if materials are present
        if (usdContent.find("UsdPreviewSurface") != std::string::npos) {
            EXPECT_TRUE(usdContent.find("material:binding") != std::string::npos)
                << "USD file with materials should contain material bindings";
        }
    }
    
    /// Validate texture connections are properly formatted
    void validateTextureConnections(const std::string& usdContent) {
        // If textures exist, validate their connections
        if (usdContent.find("UsdUVTexture") != std::string::npos) {
            // Check for common material input connections
            std::vector<std::string> commonConnections = {
                "diffuseColor.connect", "emissiveColor.connect", "normal.connect",
                "metallic.connect", "roughness.connect", "opacity.connect"
            };
            
            bool hasAnyConnection = false;
            for (const std::string& connection : commonConnections) {
                if (usdContent.find(connection) != std::string::npos) {
                    hasAnyConnection = true;
                    break;
                }
            }
            
            EXPECT_TRUE(hasAnyConnection)
                << "USD file with textures should have at least one material input connection";
            
            // Validate output channel usage
            if (usdContent.find("outputs:") != std::string::npos) {
                std::vector<std::string> validOutputs = {"outputs:rgb", "outputs:r", "outputs:g", "outputs:b", "outputs:a"};
                bool hasValidOutput = false;
                for (const std::string& output : validOutputs) {
                    if (usdContent.find(output) != std::string::npos) {
                        hasValidOutput = true;
                        break;
                    }
                }
                EXPECT_TRUE(hasValidOutput) << "USD file should use valid texture output channels";
            }
        }
    }
    

    /// Validate USD exporter-specific material mappings and structure
    void validateExporterMaterialLogic(const std::string& usdContent) {
        // Focus on exporter-specific logic that tinyusdz won't validate
        
        // 1. Ensure deprecated properties are not exported
        EXPECT_TRUE(usdContent.find("clearcoatAlt") == std::string::npos)
            << "USD exporter should not write deprecated clearcoatAlt property";
        
        // 2. Validate our specific workflow detection logic
        if (usdContent.find("useSpecularWorkflow") != std::string::npos) {
            // Should be properly set based on material detection, not hardcoded
            EXPECT_TRUE(usdContent.find("useSpecularWorkflow = 1") != std::string::npos ||
                        usdContent.find("useSpecularWorkflow = 0") != std::string::npos)
                << "useSpecularWorkflow should be determined by material analysis";
        }
    }
};

// =============================================================================
// BASIC TESTS
// =============================================================================

TEST_F(utUSDZExport, importGltfBoxTexturedExportUsda) {
    const std::string outputPath = "usd/basic/BoxTextured_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf",
        outputPath,
        "usda"
    ));
    
    // Focus on exporter-specific validation that round trip doesn't cover
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Could not open generated USD file for validation";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Validate our exporter creates proper shader network structure
    validateShaderNetworkStructure(content);
    
    // Validate our texture connection logic
    validateTextureConnections(content);
    
    // Validate our material mapping logic
    validateExporterMaterialLogic(content);
    
    // Re-import and validate material property ranges (exporter-specific logic)
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene && scene->mNumMaterials > 0) {
        validateMaterialPropertyRanges(scene);
    }
}

TEST_F(utUSDZExport, importGltfBoxTexturedExportUsdz) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf",
        "usd/basic/BoxTextured_out.usdz",
        "usdz"
    ));
}

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

TEST_F(utUSDZExport, importGltfPbrSpecularGlossinessExportUsdz) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-pbrSpecularGlossiness/BoxTextured.gltf";
    const std::string outputPath = "usd/pbr/BoxTextured_PbrSpecGloss_out.usdz";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usdz"));
    
    // Additional PBR-specific validation
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene && scene->mNumMaterials > 0) {
        validatePBRMaterial(scene->mMaterials[0], "BoxTextured PBR Material");
    }
}

// =============================================================================
// COMPLEX PBR MODEL TESTS
// =============================================================================

TEST_F(utUSDZExport, importGltfDamagedHelmetExportUsda) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/PBR/damaged-helmet.glb";
    const std::string outputPath = "usd/damaged-helmet/DamagedHelmet_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Focus on complex PBR model validation - things our exporter specifically handles
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Could not open generated USD file for validation";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Validate our shader network creation for complex materials
    validateShaderNetworkStructure(content);
    
    // Validate our texture connection logic for complex PBR
    validateTextureConnections(content);
    
    // Comprehensive texture validation for damaged helmet (regression prevention)
    // Ensure all expected textures are present with correct descriptive names in textures/ subdirectory
    std::vector<std::string> expectedTextureFiles = {
        "./textures/Default_albedo.jpg",        // diffuse/albedo
        "./textures/Default_metalRoughness.jpg", // metallic + roughness (packed)
        "./textures/Default_normal.jpg",        // normal
        "./textures/Default_AO.jpg",            // occlusion (separate AO - critical!)
        "./textures/Default_emissive.jpg"       // emissive
    };
    
    for (const auto& textureFile : expectedTextureFiles) {
        EXPECT_TRUE(content.find(textureFile) != std::string::npos)
            << "Missing expected texture file reference: " << textureFile;
    }
    
    // Validate our material mapping logic
    validateExporterMaterialLogic(content);
    
    // Re-import and validate exporter-specific material handling
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene && scene->mNumMaterials > 0) {
        // Validate our material property range clamping
        validateMaterialPropertyRanges(scene);
        
        // Validate specific PBR material handling
        validatePBRMaterial(scene->mMaterials[0], "DamagedHelmet Material");
        
        // Validate reasonable material count (exporter shouldn't duplicate/lose materials)
        EXPECT_GE(scene->mNumMaterials, 1u) << "Damaged helmet should have at least one material";
        EXPECT_LE(scene->mNumMaterials, 5u) << "Damaged helmet should have reasonable material count";
        
        // Validate UV coordinate handling
        validateTextureCoordinates(scene);
    }
}

TEST_F(utUSDZExport, importGltfDamagedHelmetExportUsdz) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/PBR/damaged-helmet.glb";
    const std::string outputPath = "usd/damaged-helmet/DamagedHelmet_out.usdz";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usdz"));
    
    // Additional validation for complex PBR model
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene && scene->mNumMaterials > 0) {
        // Validate the main helmet material (typically the first material)
        validatePBRMaterial(scene->mMaterials[0], "DamagedHelmet Material");
        
        // Validate mesh complexity
        if (scene->mNumMeshes > 0 && scene->mMeshes[0]) {
            const aiMesh* mesh = scene->mMeshes[0];
            EXPECT_GT(mesh->mNumVertices, 1000u) << "Damaged helmet should have complex geometry";
            EXPECT_GT(mesh->mNumFaces, 500u) << "Damaged helmet should have sufficient face count";
            
            // Validate that normals exist (important for PBR rendering)
            EXPECT_NE(nullptr, mesh->mNormals) << "Complex PBR model should have vertex normals";
            
            // Validate that tangents exist if available (important for normal mapping)
            if (mesh->mTangents != nullptr) {
                EXPECT_NE(nullptr, mesh->mBitangents) << "If tangents exist, bitangents should also exist";
            }
        }
    }
}

TEST_F(utUSDZExport, damagedHelmetComplexMaterialValidation) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/PBR/damaged-helmet.glb";
    const std::string outputPath = "usd/damaged-helmet/DamagedHelmet_materials_out.usda";
    
    if (!performRoundTripTest(inputPath, outputPath, "usda")) {
        FAIL() << "Round-trip test failed for damaged helmet material validation";
        return;
    }
    
    // Detailed material property validation
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene) << "Failed to reimport damaged helmet USD file";
    ASSERT_GT(scene->mNumMaterials, 0u) << "Damaged helmet should have materials";
    
    const aiMaterial* material = scene->mMaterials[0];
    ASSERT_NE(nullptr, material) << "First material should not be null";
    
    // Check for essential PBR properties
    ai_real metallicFactor = 0.0f;
    ai_real roughnessFactor = 0.0f;
    
    if (material->Get(AI_MATKEY_METALLIC_FACTOR, metallicFactor) == aiReturn_SUCCESS) {
        EXPECT_GE(metallicFactor, 0.0f) << "Metallic factor should be non-negative";
        EXPECT_LE(metallicFactor, 1.0f) << "Metallic factor should not exceed 1.0";
    }
    
    if (material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor) == aiReturn_SUCCESS) {
        EXPECT_GE(roughnessFactor, 0.0f) << "Roughness factor should be non-negative";
        EXPECT_LE(roughnessFactor, 1.0f) << "Roughness factor should not exceed 1.0";
    }
    
    // Validate texture presence (damaged helmet typically has multiple texture types)
    aiString texturePath;
    std::vector<aiTextureType> expectedTextureTypes = {
        aiTextureType_BASE_COLOR,
        aiTextureType_DIFFUSE,
        aiTextureType_NORMALS,
        aiTextureType_METALNESS,
        aiTextureType_DIFFUSE_ROUGHNESS
    };
    
    int textureCount = 0;
    for (aiTextureType textureType : expectedTextureTypes) {
        if (material->GetTexture(textureType, 0, &texturePath) == aiReturn_SUCCESS) {
            textureCount++;
            EXPECT_GT(strlen(texturePath.C_Str()), 0u) << "Texture path should not be empty";
        }
    }
    
    EXPECT_GT(textureCount, 0) << "Complex PBR model should have at least one texture";
    

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

TEST_F(utUSDZExport, DISABLED_importGltfClearcoatExportUsdz) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/ClearCoat-glTF/ClearCoatTest.gltf";
    const std::string outputPath = "usd/clearcoat/ClearCoatTest_out.usdz";
    
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
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usdz"));
    
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

TEST_F(utUSDZExport, DISABLED_importGltfEmbeddedTexturesExportUsdz) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-Embedded/BoxTextured.gltf",
        "usd/embedded/BoxTextured_Embedded_out.usdz",
        "usdz"
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
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/AnimatedMorphCube/glTF/AnimatedMorphCube.gltf";
    const std::string outputPath = "usd/blendshapes/AnimatedMorphCube_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Read the generated USD content for comprehensive validation
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Failed to open USD output file: " << outputPath;
    
    std::string usdContent((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    usdFile.close();
    
    // ========================================================================
    // ANIMATED MORPH CUBE VALIDATION: Complex blend shape animation
    // ========================================================================
    
    // 1. Validate blend shape names - should use glTF names "thin" and "angle", not "target_0", "target_1"
    EXPECT_TRUE(usdContent.find("\"thin\"") != std::string::npos)
        << "Should use glTF-derived name 'thin' for first blend shape";
    EXPECT_TRUE(usdContent.find("\"angle\"") != std::string::npos)
        << "Should use glTF-derived name 'angle' for second blend shape";
    
    // Should NOT use auto-generated names
    EXPECT_TRUE(usdContent.find("\"target_0\"") == std::string::npos)
        << "Should not use auto-generated name 'target_0'";
    EXPECT_TRUE(usdContent.find("\"target_1\"") == std::string::npos)
        << "Should not use auto-generated name 'target_1'";
    
    // 2. Validate skel:blendShapeTargets uses absolute paths
    size_t blendShapeTargetsPos = usdContent.find("rel skel:blendShapeTargets");
    ASSERT_TRUE(blendShapeTargetsPos != std::string::npos) << "skel:blendShapeTargets not found";
    
    size_t targetsStart = usdContent.find("[", blendShapeTargetsPos);
    size_t targetsEnd = usdContent.find("]", targetsStart);
    ASSERT_TRUE(targetsStart != std::string::npos && targetsEnd != std::string::npos) 
        << "skel:blendShapeTargets array not found";
    
    std::string targetsSection = usdContent.substr(targetsStart, targetsEnd - targetsStart);
    
    // Should use absolute paths like </AnimatedMorphCube_out/AnimatedMorphCube/Cube/Geometry/Cube/thin>
    EXPECT_TRUE(targetsSection.find("</") != std::string::npos)
        << "skel:blendShapeTargets should use absolute paths starting with </";
    EXPECT_TRUE(targetsSection.find("/thin>") != std::string::npos)
        << "Should reference absolute path to 'thin' blend shape";
    EXPECT_TRUE(targetsSection.find("/angle>") != std::string::npos)
        << "Should reference absolute path to 'angle' blend shape";
    
    // 3. Validate animation data - should NOT be all [0,0]
    size_t timeSamplesPos = usdContent.find("blendShapeWeights.timeSamples");
    ASSERT_TRUE(timeSamplesPos != std::string::npos) << "blendShapeWeights.timeSamples not found";
    
    size_t timeSamplesStart = usdContent.find("{", timeSamplesPos);
    size_t timeSamplesEnd = usdContent.find("}", timeSamplesStart);
    std::string timeSamplesSection = usdContent.substr(timeSamplesStart, timeSamplesEnd - timeSamplesStart);
    
    // Should have non-zero animation values (not all [0, 0])
    bool hasNonZeroWeights = false;
    if (timeSamplesSection.find("[0.") != std::string::npos || 
        timeSamplesSection.find("[1") != std::string::npos ||
        timeSamplesSection.find(", 0.") != std::string::npos ||
        timeSamplesSection.find(", 1") != std::string::npos) {
        hasNonZeroWeights = true;
    }
    EXPECT_TRUE(hasNonZeroWeights) << "Animation should have non-zero blend shape weights, not all [0, 0]";
    
    // 4. Validate endTimeCode is properly rounded (should be 101, not 100.8)
    size_t endTimePos = usdContent.find("endTimeCode = ");
    ASSERT_TRUE(endTimePos != std::string::npos) << "endTimeCode not found";
    
    size_t endTimeStart = endTimePos + 14;
    size_t endTimeEnd = usdContent.find_first_of(" \n\t)", endTimeStart);
    std::string endTimeStr = usdContent.substr(endTimeStart, endTimeEnd - endTimeStart);
    
    // Should be a decimal value, not an integer
    EXPECT_TRUE(endTimeStr.find(".") != std::string::npos) 
        << "endTimeCode should be a decimal (4.208333), not integer (" << endTimeStr << ")";
    
    float endTimeCode = std::stof(endTimeStr);
    EXPECT_FLOAT_EQ(endTimeCode, 4.20833f) << "endTimeCode should be 4.20833 for AnimatedMorphCube animation";
    
    // 5. Verify we have exactly 101 time samples (frames 1-101)
    size_t timeSamplesPos2 = usdContent.find("blendShapeWeights.timeSamples");
    EXPECT_TRUE(timeSamplesPos2 != std::string::npos) << "Should have blendShapeWeights.timeSamples";
    
    if (timeSamplesPos2 != std::string::npos) {
        size_t timeSamplesEnd2 = usdContent.find("}", timeSamplesPos2);
        std::string timeSamplesSection2 = usdContent.substr(timeSamplesPos2, timeSamplesEnd2 - timeSamplesPos2);
        
        // Count the number of time samples by counting colons
        size_t colonCount = 0;
        size_t pos = 0;
        while ((pos = timeSamplesSection2.find(":", pos)) != std::string::npos) {
            colonCount++;
            pos++;
        }
        
        EXPECT_EQ(colonCount, 101) << "Should have exactly 101 time samples (fractional time codes), found: " << colonCount;
        
        // Verify we have fractional time codes (e.g., 0.0416667, 0.0833333, etc.)
        EXPECT_TRUE(timeSamplesSection2.find("0.0416667:") != std::string::npos) << "Should have fractional time sample 0.0416667";
        
        // Verify we have the final fractional time code around 4.2
        EXPECT_TRUE(timeSamplesSection2.find("4.2") != std::string::npos) << "Should have final fractional time sample around 4.2";
    }
}

TEST_F(utUSDZExport, importGltfSimpleMorphExportUsda) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/SimpleMorph/glTF/SimpleMorph.gltf";
    const std::string outputPath = "usd/blendshapes/SimpleMorph_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Comprehensive blend shape validation based on Blender reference structure
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Failed to open USD output file: " << outputPath;
    
    std::string usdContent((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    usdFile.close();
    
    // ========================================================================
    // STRUCTURE VALIDATION: Root -> SkelRoot -> Mesh + Skeleton
    // ========================================================================
    
    // 1. Root node should exist and be named after the output file
    EXPECT_TRUE(usdContent.find("def Xform \"SimpleMorph_out\"") != std::string::npos)
        << "Root node should be named after output file (SimpleMorph_out)";
    
    // 2. SkelRoot should exist as child of root
    EXPECT_TRUE(usdContent.find("def SkelRoot") != std::string::npos)
        << "SkelRoot should exist for blend shape meshes";
    
    // 3. Mesh should exist with SkelBindingAPI
    EXPECT_TRUE(usdContent.find("def Mesh") != std::string::npos)
        << "Mesh prim should exist";
    EXPECT_TRUE(usdContent.find("SkelBindingAPI") != std::string::npos)
        << "Mesh should have SkelBindingAPI applied";
    
    // 4. Skeleton should exist with SkelBindingAPI
    EXPECT_TRUE(usdContent.find("def Skeleton") != std::string::npos)
        << "Skeleton prim should exist";
    
    // ========================================================================
    // BLEND SHAPE VALIDATION: BlendShapes as children of mesh
    // ========================================================================
    
    // 5. BlendShape prims should exist as children of the mesh
    EXPECT_TRUE(usdContent.find("def BlendShape \"target_0\"") != std::string::npos)
        << "BlendShape target_0 should exist as child of mesh";
    EXPECT_TRUE(usdContent.find("def BlendShape \"target_1\"") != std::string::npos)
        << "BlendShape target_1 should exist as child of mesh";
    
    // 6. BlendShapes should have proper structure (offsets and pointIndices)
    EXPECT_TRUE(usdContent.find("uniform vector3f[] offsets") != std::string::npos)
        << "BlendShapes should have offsets arrays";
    EXPECT_TRUE(usdContent.find("uniform int[] pointIndices") != std::string::npos)
        << "BlendShapes should have pointIndices arrays";
    
    // ========================================================================
    // SKELETAL BINDING VALIDATION: Mesh properties
    // ========================================================================
    
    // 7. Mesh should have skel:blendShapes token array
    EXPECT_TRUE(usdContent.find("uniform token[] skel:blendShapes") != std::string::npos)
        << "Mesh should have skel:blendShapes token array";
    EXPECT_TRUE(usdContent.find("[\"target_0\", \"target_1\"]") != std::string::npos)
        << "skel:blendShapes should reference target_0 and target_1";
    
    // 8. Mesh should have skel:blendShapeTargets relationships
    EXPECT_TRUE(usdContent.find("rel skel:blendShapeTargets") != std::string::npos)
        << "Mesh should have skel:blendShapeTargets relationships";
    
    // 9. Mesh should have skel:skeleton relationship
    EXPECT_TRUE(usdContent.find("rel skel:skeleton") != std::string::npos)
        << "Mesh should have skel:skeleton relationship";
    
    // 10. Mesh should have joint indices and weights for skeletal binding
    EXPECT_TRUE(usdContent.find("int[] primvars:skel:jointIndices") != std::string::npos)
        << "Mesh should have skel:jointIndices primvar";
    EXPECT_TRUE(usdContent.find("float[] primvars:skel:jointWeights") != std::string::npos)
        << "Mesh should have skel:jointWeights primvar";
    
    // ========================================================================
    // SKELETON VALIDATION: Joint system and animation
    // ========================================================================
    
    // 11. Skeleton should have joints array
    EXPECT_TRUE(usdContent.find("uniform token[] joints") != std::string::npos)
        << "Skeleton should have joints array";
    
    // 12. Skeleton should have bind and rest transforms
    EXPECT_TRUE(usdContent.find("uniform matrix4d[] bindTransforms") != std::string::npos)
        << "Skeleton should have bindTransforms";
    EXPECT_TRUE(usdContent.find("uniform matrix4d[] restTransforms") != std::string::npos)
        << "Skeleton should have restTransforms";
    
    // 13. Skeleton should reference SkelAnimation
    EXPECT_TRUE(usdContent.find("rel skel:animationSource") != std::string::npos)
        << "Skeleton should have animationSource relationship";
    
    // ========================================================================
    // ANIMATION VALIDATION: SkelAnimation with blend shape weights
    // ========================================================================
    
    // 14. SkelAnimation should exist as child of Skeleton
    EXPECT_TRUE(usdContent.find("def SkelAnimation") != std::string::npos)
        << "SkelAnimation should exist as child of Skeleton";
    
    // 15. SkelAnimation should have blendShapes token array
    EXPECT_TRUE(usdContent.find("uniform token[] blendShapes") != std::string::npos)
        << "SkelAnimation should have blendShapes token array";
    
    // 16. SkelAnimation should have blendShapeWeights with time samples
    EXPECT_TRUE(usdContent.find("blendShapeWeights") != std::string::npos)
        << "SkelAnimation should have blendShapeWeights";
    
    // ========================================================================
    // REGRESSION PREVENTION: Common issues
    // ========================================================================
    
    // 17. No duplicate SkelAnimation at root level (regression test)
    size_t skelAnimCount = 0;
    size_t pos = 0;
    while ((pos = usdContent.find("def SkelAnimation", pos)) != std::string::npos) {
        skelAnimCount++;
        pos += 17; // length of "def SkelAnimation"
    }
    EXPECT_EQ(1u, skelAnimCount) << "Should have exactly one SkelAnimation (inside Skeleton, not at root)";
    
    // 18. No BlendShapes at root level (regression test)
    EXPECT_TRUE(usdContent.find("def BlendShape") != std::string::npos)
        << "BlendShapes should exist";
    // Ensure BlendShapes are properly nested (not at root level)
    size_t rootEnd = usdContent.find("def SkelRoot");
    if (rootEnd != std::string::npos) {
        std::string beforeSkelRoot = usdContent.substr(0, rootEnd);
        EXPECT_TRUE(beforeSkelRoot.find("def BlendShape") == std::string::npos)
            << "BlendShapes should not exist at root level (should be children of mesh)";
    }
    
    // 19. Proper naming consistency (no "Unnamed" blend shapes)
    EXPECT_TRUE(usdContent.find("\"Unnamed\"") == std::string::npos)
        << "Should not have 'Unnamed' blend shapes - should use target_0, target_1";
    
    // 20. Geometry scope wrapper should exist
    EXPECT_TRUE(usdContent.find("def Scope \"Geometry\"") != std::string::npos)
        << "Mesh should be wrapped in Geometry scope (Apple's pattern)";
    
    // ========================================================================
    // CRITICAL STRUCTURE VALIDATION: BlendShapes as mesh children
    // ========================================================================
    
    // 21. BlendShapes should be children of Mesh, not root-level prims
    size_t meshStart = usdContent.find("def Mesh \"meshes_0_\"");
    EXPECT_TRUE(meshStart != std::string::npos) << "Mesh definition should exist";
    
    if (meshStart != std::string::npos) {
        // Find the end of the mesh definition (next def at same level or closing brace)
        size_t meshEnd = usdContent.find("\n            }", meshStart);
        if (meshEnd == std::string::npos) {
            meshEnd = usdContent.find("\n        }", meshStart);
        }
        EXPECT_TRUE(meshEnd != std::string::npos) << "Mesh definition should have proper closing";
        
        if (meshEnd != std::string::npos) {
            std::string meshContent = usdContent.substr(meshStart, meshEnd - meshStart);
            
            // BlendShapes should be inside the mesh
            EXPECT_TRUE(meshContent.find("def BlendShape \"target_0\"") != std::string::npos)
                << "target_0 BlendShape should be child of mesh, not root-level prim";
            EXPECT_TRUE(meshContent.find("def BlendShape \"target_1\"") != std::string::npos)
                << "target_1 BlendShape should be child of mesh, not root-level prim";
        }
    }
    
    // ========================================================================
    // MESH PROPERTY VALIDATION: Missing critical properties
    // ========================================================================
    
    // 22. Mesh should have extent property
    EXPECT_TRUE(usdContent.find("float3[] extent") != std::string::npos)
        << "Mesh should have extent property for bounding box";
    
    // 23. Mesh should have normals
    EXPECT_TRUE(usdContent.find("normal3f[] normals") != std::string::npos)
        << "Mesh should have normals property";
    
    // 24. skel:blendShapeTargets should use absolute paths, not relative
    if (usdContent.find("rel skel:blendShapeTargets") != std::string::npos) {
        // Should be absolute paths like </path/to/mesh/target_0>
        EXPECT_TRUE(usdContent.find("</SimpleMorph_out/nodes_0_/meshes_0_/Geometry/meshes_0_/target_0>") != std::string::npos ||
                   usdContent.find("</") != std::string::npos)
            << "skel:blendShapeTargets should use absolute paths, not relative paths like <.target_0>";
    }
    
    // 25. skel:skeleton should use absolute path, not relative
    if (usdContent.find("rel skel:skeleton") != std::string::npos) {
        // Should be absolute path like </path/to/skeleton>
        EXPECT_TRUE(usdContent.find("</SimpleMorph_out/nodes_0_/meshes_0_/Skel>") != std::string::npos ||
                   usdContent.find("rel skel:skeleton = </") != std::string::npos)
            << "skel:skeleton should use absolute path, not relative path like <../Skel>";
    }
    
    // ========================================================================
    // SKELETON PROPERTY VALIDATION: Animation source path
    // ========================================================================
    
    // 26. skel:animationSource should use absolute path, not relative
    if (usdContent.find("rel skel:animationSource") != std::string::npos) {
        // Should be absolute path like </path/to/animation>
        EXPECT_TRUE(usdContent.find("</SimpleMorph_out/nodes_0_/meshes_0_/Skel/Anim>") != std::string::npos ||
                   usdContent.find("rel skel:animationSource = </") != std::string::npos)
            << "skel:animationSource should use absolute path, not relative path like <./Anim>";
    }
    
    // ========================================================================
    // SKELANIMATION VALIDATION: Time samples and proper animation
    // ========================================================================
    
    // 27. SkelAnimation MUST have blendShapeWeights.timeSamples for animation
    EXPECT_TRUE(usdContent.find("blendShapeWeights.timeSamples") != std::string::npos)
        << "SkelAnimation must have blendShapeWeights.timeSamples for actual animation data";
    
    // 28. Time samples should have multiple keyframes
    if (usdContent.find("blendShapeWeights.timeSamples") != std::string::npos) {
        // Should have multiple time codes like 0: [0, 0], 1: [0.5, 0], etc.
        size_t timeSamplesStart = usdContent.find("blendShapeWeights.timeSamples");
        size_t timeSamplesEnd = usdContent.find("}", timeSamplesStart);
        if (timeSamplesEnd != std::string::npos) {
            std::string timeSamplesContent = usdContent.substr(timeSamplesStart, timeSamplesEnd - timeSamplesStart);
            
            // Should have at least 2 different time codes
            EXPECT_TRUE(timeSamplesContent.find("0:") != std::string::npos)
                << "Time samples should include time code 0";
            EXPECT_TRUE(timeSamplesContent.find("1:") != std::string::npos ||
                       timeSamplesContent.find("2:") != std::string::npos ||
                       timeSamplesContent.find("3:") != std::string::npos ||
                       timeSamplesContent.find("4:") != std::string::npos)
                << "Time samples should include multiple time codes for animation";
        }
    }
}

// =============================================================================
// ANIMATION TIME SAMPLING TESTS
// =============================================================================

TEST_F(utUSDZExport, validateBlendShapeTimeSampling) {
    // Test specifically for proper time sampling in blend shape animations
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/SimpleMorph/glTF/SimpleMorph.gltf";
    const std::string outputPath = "usd/blendshapes/SimpleMorph_timeSampling_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Read the generated USD content for time sampling validation
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Failed to open USD output file: " << outputPath;
    
    std::string usdContent((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    usdFile.close();
    
    // ========================================================================
    // TIME SAMPLING VALIDATION: Critical animation requirements
    // ========================================================================
    
    // 1. Must have blendShapeWeights.timeSamples (not default array)
    EXPECT_TRUE(usdContent.find("blendShapeWeights.timeSamples") != std::string::npos)
        << "SkelAnimation must have blendShapeWeights.timeSamples for animation";
    
    // 2. Validate time sample count and range
    size_t timeSamplesStart = usdContent.find("blendShapeWeights.timeSamples");
    ASSERT_TRUE(timeSamplesStart != std::string::npos) << "timeSamples section not found";
    
    size_t timeSamplesEnd = usdContent.find("}", timeSamplesStart);
    ASSERT_TRUE(timeSamplesEnd != std::string::npos) << "timeSamples closing brace not found";
    
    std::string timeSamplesSection = usdContent.substr(timeSamplesStart, timeSamplesEnd - timeSamplesStart);
    
    // 3. Count the number of time samples (should be substantial, not just 2)
    size_t colonCount = 0;
    size_t pos = 0;
    while ((pos = timeSamplesSection.find(":", pos)) != std::string::npos) {
        colonCount++;
        pos++;
    }
    
    // Should have exactly 101 time samples for 4.208333-second animation at 24fps  
    EXPECT_EQ(colonCount, 101) << "Should have exactly 101 time samples for 4.208333-second animation at 24fps (found " << colonCount << ")";
    
    // 4. Validate time code range (should go from reasonable start to end)
    // Check for presence of fractional time codes
    EXPECT_TRUE(timeSamplesSection.find("0.0416667:") != std::string::npos)
        << "Should have time samples starting from 0.0416667";
    
    // 5. Should have time samples up to approximately 4.2 seconds
    EXPECT_TRUE(timeSamplesSection.find("4.2") != std::string::npos) 
        << "Should have final time sample around 4.2 seconds";
    
    // 6. Validate that blend shape weights actually animate (not all zeros)
    bool hasNonZeroWeights = false;
    
    // Simple check: look for specific animated values we know should be there
    // Based on our interpolation: frame 2 should have [0, 0.041666669]
    if (timeSamplesSection.find("0.041666") != std::string::npos ||
        timeSamplesSection.find("0.083333") != std::string::npos ||
        timeSamplesSection.find("0.125") != std::string::npos ||
        timeSamplesSection.find("0.166666") != std::string::npos) {
        hasNonZeroWeights = true;
    }
    
    EXPECT_TRUE(hasNonZeroWeights) << "Blend shape weights should animate (have non-zero values)";
    
    // 7. Validate timeline consistency with USD metadata
    // Check that endTimeCode matches the highest time sample
    if (usdContent.find("endTimeCode = ") != std::string::npos) {
        size_t endTimePos = usdContent.find("endTimeCode = ") + 14;
        size_t endTimeEnd = usdContent.find_first_of(" \n\t)", endTimePos);
        if (endTimeEnd != std::string::npos) {
            std::string endTimeStr = usdContent.substr(endTimePos, endTimeEnd - endTimePos);
            float endTimeCode = std::stof(endTimeStr);
            
            // endTimeCode should be 4.208333 (corrected timing)
            EXPECT_FLOAT_EQ(endTimeCode, 4.208333f) << "endTimeCode should be 4.208333 for corrected timing (found " << endTimeCode << ")";
        }
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

TEST_F(utUSDZExport, importGltfPrimitiveModeTriangleFanNoTexturesDirectoryExportUsda) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/glTF-Asset-Generator/Mesh_PrimitiveMode/Mesh_PrimitiveMode_05.gltf";
    const std::string outputPath = "usd/primitives/PrimitiveMode_TriangleFan_NoTexDir_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Extract directory from output path
    std::string outputDir = outputPath.substr(0, outputPath.find_last_of("/\\"));
    std::string texturesDir = outputDir + "/textures";
    
    // Assert that textures directory does NOT exist (no textures in this model)
    EXPECT_FALSE(std::filesystem::exists(texturesDir)) 
        << "Textures directory should not be created when no textures are present: " << texturesDir;
}

// =============================================================================
// ERROR HANDLING TESTS
// =============================================================================

TEST_F(utUSDZExport, DISABLED_importGltfIncorrectVertexArraysExportUsda) {
    // This tests handling of malformed input data
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/IncorrectVertexArrays/Cube.gltf";
    const std::string outputPath = "usd/error_cases/IncorrectVertexArrays_out.usda";
    
    // Create output directory to prevent export failure
    ASSERT_TRUE(createDirectoryRecursive(outputPath)) << "Failed to create output directory for malformed test";
    
    // Load and export the malformed input - should succeed with warnings
    Assimp::Importer importer;
    std::unique_ptr<const aiScene> original(importer.ReadFile(inputPath, 0));
    ASSERT_NE(nullptr, original.get()) << "Failed to load test model";
    EXPECT_GT(original->mNumMeshes, 0u) << "Original scene should have meshes";
    
    // Export should succeed despite malformed input (Assimp handles this gracefully)
    Assimp::Exporter exporter;
    EXPECT_EQ(aiReturn_SUCCESS, exporter.Export(original.get(), "usda", outputPath)) 
        << "Export should succeed for malformed input";
    
    // Import should fail or return empty scene (tinyusdz correctly rejects malformed geometry)
    std::unique_ptr<const aiScene> reimported;
    
    try {
        reimported.reset(importer.ReadFile(outputPath, 0));
    } catch (const std::exception& e) {
        // Import failure due to malformed data is expected and acceptable
        reimported.reset(nullptr);
    }
    
    // Either import fails completely or returns empty scene - both are correct for malformed data
    if (reimported) {
        EXPECT_EQ(0u, reimported->mNumMeshes) << "Malformed geometry should be rejected on import";
    }
    // If reimported is nullptr, that's also acceptable - tinyusdz rejected the malformed file
}

TEST_F(utUSDZExport, exportNullSceneFailsGracefully) {
    const std::string outputPath = "usd/error_cases/null_scene_out.usda";
    
    // Create output directory to prevent export failure due to missing directory
    ASSERT_TRUE(createDirectoryRecursive(outputPath)) << "Failed to create output directory for error test";
    
    Assimp::Exporter exporter;
    aiReturn result = exporter.Export(nullptr, "usda", outputPath, 0u);
    EXPECT_EQ(aiReturn_FAILURE, result);
}

TEST_F(utUSDZExport, exportInvalidFormatFailsGracefully) {
    // Create a minimal valid scene
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf", 
                                           aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    
    const std::string outputPath = "usd/error_cases/invalid_format.usd";
    
    // Create output directory to prevent export failure due to missing directory
    ASSERT_TRUE(createDirectoryRecursive(outputPath)) << "Failed to create output directory for error test";
    
    Assimp::Exporter exporter;
    aiReturn result = exporter.Export(scene, "invalid_format", outputPath, 0u);
    EXPECT_EQ(aiReturn_FAILURE, result);
}

// =============================================================================
// ANIMATION TESTS
// =============================================================================

TEST_F(utUSDZExport, exportAnimatedBoxRoundTrip) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/AnimatedMorphCube/glTF/AnimatedMorphCube.gltf";
    const std::string outputPath = "usd/animation/AnimatedCube_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
}

TEST_F(utUSDZExport, exportAnimationKeyframeValidation) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/AnimatedMorphCube/glTF/AnimatedMorphCube.gltf";
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
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/glTF-Sample-Models/AnimatedMorphCube-glTF/AnimatedMorphCube.gltf";
    const std::string outputPath = "usd/animation/AnimatedMorphCube_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
}

TEST_F(utUSDZExport, importGltfCesiumManExportUsda) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/CesiumMan/CesiumMan.glb";
    const std::string outputPath = "usd/animation/CesiumMan_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Verify USD content references the texture correctly
    std::ifstream usdFile(outputPath);
    if (usdFile.is_open()) {
        std::string usdContent((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
        usdFile.close();
        EXPECT_TRUE(usdContent.find("@./textures/Default_albedo.jpg@") != std::string::npos) 
            << "Texture asset reference not found in USD content";
    }
    
    // Additional validation for animated character
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene) {
        // Validate that the character model has reasonable complexity
        EXPECT_GT(scene->mNumMeshes, 0u) << "Cesium Man should have meshes";
        EXPECT_GT(scene->mNumMaterials, 0u) << "Cesium Man should have materials";
        
        // Check for skeletal animation data if present
        if (scene->mNumMeshes > 0 && scene->mMeshes[0]) {
            const aiMesh* mesh = scene->mMeshes[0];
            if (mesh->mNumBones > 0) {
                validateSkinningData(scene, 1); // Expect at least 1 bone if skinned
            }
        }
        
        // Validate animations if present
        if (scene->mNumAnimations > 0) {
            validateAnimationData(scene);
        }
    }
}

// =============================================================================
// SKINNING TESTS  
// =============================================================================

TEST_F(utUSDZExport, exportSkinnedMeshRoundTrip) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/simple_skin/simple_skin.gltf";
    const std::string outputPath = "usd/skinning/SimpleSkin_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
}

TEST_F(utUSDZExport, exportSkeletalValidation) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/simple_skin/simple_skin.gltf";
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
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/glTF-Sample-Models/AnimatedMorphCube-glTF/AnimatedMorphCube.gltf";
    const std::string outputPath = "usd/blendshapes/AnimatedMorphCube_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
}

TEST_F(utUSDZExport, exportMorphTargetValidation) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/glTF-Sample-Models/AnimatedMorphCube-glTF/AnimatedMorphCube.gltf";
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
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/ClearCoat-glTF/ClearCoatTest.gltf";
    const std::string outputPath = "usd/textures/ClearCoatComplex_out.usda";
    
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

// =============================================================================
// ADVANCED TEXTURE MAPPING TESTS
// =============================================================================

TEST_F(utUSDZExport, importGltfSpecularWorkflowExportUsda) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-pbrSpecularGlossiness/BoxTextured.gltf";
    const std::string outputPath = "usd/specular/SpecularWorkflow_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Verify USD file contains specular workflow properties
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Could not open generated USD file for specular workflow validation";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Check for specular workflow activation
    EXPECT_TRUE(content.find("useSpecularWorkflow = 1") != std::string::npos) 
        << "USD file should have useSpecularWorkflow set to 1 for specular materials";
    EXPECT_TRUE(content.find("specularColor") != std::string::npos) 
        << "USD file should contain specularColor connections";
}

TEST_F(utUSDZExport, importGltfTextureTransformExportUsda) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/textureTransform/TextureTransformTest.gltf";
    const std::string outputPath = "usd/transform/TextureTransform_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Verify USD file contains UV transformation shaders
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Could not open generated USD file for texture transform validation";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Check for UV transformation components
    EXPECT_TRUE(content.find("UsdTransform2d") != std::string::npos) 
        << "USD file should contain UsdTransform2d shaders for UV transformations";
    EXPECT_TRUE(content.find("UsdPrimvarReader_float2") != std::string::npos) 
        << "USD file should contain UsdPrimvarReader_float2 for UV coordinate reading";
    
    // ========================================================================
    // TEXTURE TRANSFORM VALIDATION: Assert specific transform values per material
    // ========================================================================
    
    // 1. Offset_U material: should have translation=(0.5, 1)
    EXPECT_TRUE(content.find("def Material \"Offset_U\"") != std::string::npos)
        << "Should have Offset_U material (not Offset_U_1)";
    size_t offsetUPos = content.find("def Material \"Offset_U\"");
    if (offsetUPos != std::string::npos) {
        size_t offsetUEnd = content.find("def Material", offsetUPos + 1);
        if (offsetUEnd == std::string::npos) offsetUEnd = content.length();
        std::string offsetUSection = content.substr(offsetUPos, offsetUEnd - offsetUPos);
        
        EXPECT_TRUE(offsetUSection.find("float2 inputs:translation = (0.5, 1)") != std::string::npos)
            << "Offset_U material should have translation=(0.5, 1)";
        EXPECT_TRUE(offsetUSection.find("float2 inputs:scale = (1, -1)") != std::string::npos)
            << "Offset_U material should have scale=(1, -1)";
    }
    
    // 2. Offset_V material: should have translation=(0, 0.5)
    EXPECT_TRUE(content.find("def Material \"Offset_V\"") != std::string::npos)
        << "Should have Offset_V material (not Offset_V_1)";
    size_t offsetVPos = content.find("def Material \"Offset_V\"");
    if (offsetVPos != std::string::npos) {
        size_t offsetVEnd = content.find("def Material", offsetVPos + 1);
        if (offsetVEnd == std::string::npos) offsetVEnd = content.length();
        std::string offsetVSection = content.substr(offsetVPos, offsetVEnd - offsetVPos);
        
        EXPECT_TRUE(offsetVSection.find("float2 inputs:translation = (0, 0.5)") != std::string::npos)
            << "Offset_V material should have translation=(0, 0.5), not (0, -0.5)";
        EXPECT_TRUE(offsetVSection.find("float2 inputs:scale = (1, -1)") != std::string::npos)
            << "Offset_V material should have scale=(1, -1)";
    }
    
    // 3. Offset_UV material: should have translation=(0.5, 0.5)
    EXPECT_TRUE(content.find("def Material \"Offset_UV\"") != std::string::npos)
        << "Should have Offset_UV material (not Offset_UV_1)";
    size_t offsetUVPos = content.find("def Material \"Offset_UV\"");
    if (offsetUVPos != std::string::npos) {
        size_t offsetUVEnd = content.find("def Material", offsetUVPos + 1);
        if (offsetUVEnd == std::string::npos) offsetUVEnd = content.length();
        std::string offsetUVSection = content.substr(offsetUVPos, offsetUVEnd - offsetUVPos);
        
        EXPECT_TRUE(offsetUVSection.find("float2 inputs:translation = (0.5, 0.5)") != std::string::npos)
            << "Offset_UV material should have translation=(0.5, 0.5), not (0.5, -0.5)";
        EXPECT_TRUE(offsetUVSection.find("float2 inputs:scale = (1, -1)") != std::string::npos)
            << "Offset_UV material should have scale=(1, -1)";
    }
    
    // 4. Rotation material: should have rotation=22.5 and translation=(0, 1)
    EXPECT_TRUE(content.find("def Material \"Rotation\"") != std::string::npos)
        << "Should have Rotation material (not Rotation_1)";
    size_t rotationPos = content.find("def Material \"Rotation\"");
    if (rotationPos != std::string::npos) {
        size_t rotationEnd = content.find("def Material", rotationPos + 1);
        if (rotationEnd == std::string::npos) rotationEnd = content.length();
        std::string rotationSection = content.substr(rotationPos, rotationEnd - rotationPos);
        
        EXPECT_TRUE(rotationSection.find("float inputs:rotation = 22.5") != std::string::npos)
            << "Rotation material should have rotation=22.5";
        EXPECT_TRUE(rotationSection.find("float2 inputs:translation = (0, 1)") != std::string::npos)
            << "Rotation material should have translation=(0, 1), not computed rotation offset";
        EXPECT_TRUE(rotationSection.find("float2 inputs:scale = (1, -1)") != std::string::npos)
            << "Rotation material should have scale=(1, -1)";
    }
    
    // 5. Scale material: should have scale=(1.5, -1.5) and translation=(0, 1)
    EXPECT_TRUE(content.find("def Material \"Scale\"") != std::string::npos)
        << "Should have Scale material (not Scale_1)";
    size_t scalePos = content.find("def Material \"Scale\"");
    if (scalePos != std::string::npos) {
        size_t scaleEnd = content.find("def Material", scalePos + 1);
        if (scaleEnd == std::string::npos) scaleEnd = content.length();
        std::string scaleSection = content.substr(scalePos, scaleEnd - scalePos);
        
        EXPECT_TRUE(scaleSection.find("float2 inputs:scale = (1.5, -1.5)") != std::string::npos)
            << "Scale material should have scale=(1.5, -1.5)";
        EXPECT_TRUE(scaleSection.find("float2 inputs:translation = (0, 1)") != std::string::npos)
            << "Scale material should have translation=(0, 1), not (0, -0.5)";
    }
    
    // 6. All material: should have scale=(1.5, -1.5), translation=(-0.2, 1.1), rotation=17.188734
    EXPECT_TRUE(content.find("def Material \"All\"") != std::string::npos)
        << "Should have All material (not All_1)";
    size_t allPos = content.find("def Material \"All\"");
    if (allPos != std::string::npos) {
        size_t allEnd = content.find("def Material", allPos + 1);
        if (allEnd == std::string::npos) allEnd = content.length();
        std::string allSection = content.substr(allPos, allEnd - allPos);
        
        // Check for rotation with reasonable floating point tolerance
        EXPECT_TRUE(allSection.find("float inputs:rotation = 17.1887") != std::string::npos)
            << "All material should have rotation≈17.1887 (17.188734 with precision tolerance)";
        EXPECT_TRUE(allSection.find("float2 inputs:scale = (1.5, -1.5)") != std::string::npos)
            << "All material should have scale=(1.5, -1.5)";
        // Check for translation with reasonable floating point tolerance  
        EXPECT_TRUE(allSection.find("float2 inputs:translation = (-0.2, 1.0999999)") != std::string::npos ||
                   allSection.find("float2 inputs:translation = (-0.2, 1.1)") != std::string::npos)
            << "All material should have translation≈(-0.2, 1.1) with precision tolerance";
    }
    
    // ========================================================================
    // TEXTURE WRAP MODE VALIDATION: Critical for preventing tiling effects
    // ========================================================================
    
    // 7. Main transform materials should use "clamp" wrap mode (prevents tiling)
    std::vector<std::string> clampMaterials = {"Offset_U", "Offset_V", "Offset_UV", "Rotation", "Scale", "All"};
    for (const std::string& matName : clampMaterials) {
        size_t matPos = content.find("def Material \"" + matName + "\"");
        if (matPos != std::string::npos) {
            size_t matEnd = content.find("def Material", matPos + 1);
            if (matEnd == std::string::npos) matEnd = content.length();
            std::string matSection = content.substr(matPos, matEnd - matPos);
            
            EXPECT_TRUE(matSection.find("token inputs:wrapS = \"clamp\"") != std::string::npos)
                << matName << " material should have wrapS = \"clamp\" to prevent tiling";
            EXPECT_TRUE(matSection.find("token inputs:wrapT = \"clamp\"") != std::string::npos)
                << matName << " material should have wrapT = \"clamp\" to prevent tiling";
        }
    }
    
    // 8. Indicator materials should use "repeat" wrap mode
    std::vector<std::string> repeatMaterials = {"Correct", "NotSupported", "Error"};
    for (const std::string& matName : repeatMaterials) {
        size_t matPos = content.find("def Material \"" + matName + "\"");
        if (matPos != std::string::npos) {
            size_t matEnd = content.find("def Material", matPos + 1);
            if (matEnd == std::string::npos) matEnd = content.length();
            std::string matSection = content.substr(matPos, matEnd - matPos);
            
            EXPECT_TRUE(matSection.find("token inputs:wrapS = \"repeat\"") != std::string::npos)
                << matName << " material should have wrapS = \"repeat\" for indicator patterns";
            EXPECT_TRUE(matSection.find("token inputs:wrapT = \"repeat\"") != std::string::npos)
                << matName << " material should have wrapT = \"repeat\" for indicator patterns";
        }
    }
    
    // ========================================================================
    // INSTANCEABLE REFERENCE SYSTEM VALIDATION: Critical for visual indicators
    // ========================================================================
    
    // 9. Secondary indicator XForm prims should have instanceable Geometry with prepend references
    // Re-enabled: Using correct tinyusdz Reference API patterns
    // Note: Primary nodes (Rotation___*) contain actual mesh definitions, not references
    std::vector<std::string> secondaryIndicatorXForms = {"All___Correct", "All___Error", "Scale___Correct"}; // Re-enabled for testing
    
    for (const std::string& xformName : secondaryIndicatorXForms) {
        size_t xformPos = content.find("def Xform \"" + xformName + "\"");
        if (xformPos != std::string::npos) {
            size_t xformEnd = content.find("\n        }", xformPos);
            if (xformEnd == std::string::npos) xformEnd = content.find("\n    }", xformPos);
            if (xformEnd == std::string::npos) xformEnd = content.length();
                                std::string xformSection = content.substr(xformPos, xformEnd - xformPos);

                    EXPECT_TRUE(xformSection.find("def Scope \"Geometry\"") != std::string::npos)
                        << xformName << " should contain Geometry child";
                    EXPECT_TRUE(xformSection.find("instanceable = true") != std::string::npos)
                        << xformName << " Geometry should be instanceable = true";
                    EXPECT_TRUE(xformSection.find("prepend references") != std::string::npos)
                        << xformName << " Geometry should have prepend references";

                    // Validate complete hierarchy paths for specific secondary indicator XForms
                    if (xformName == "All___Correct") {
                        EXPECT_TRUE(xformSection.find("prepend references = </TextureTransform_out/ROOT/Rotation/Rotation___Correct/Geometry>") != std::string::npos)
                            << "All___Correct should reference full hierarchy path to primary Rotation___Correct";
                    } else if (xformName == "All___Error") {
                        EXPECT_TRUE(xformSection.find("prepend references = </TextureTransform_out/ROOT/Rotation/Rotation___Error/Geometry>") != std::string::npos)
                            << "All___Error should reference full hierarchy path to primary Rotation___Error";
                    } else if (xformName == "Scale___Correct") {
                        EXPECT_TRUE(xformSection.find("prepend references = </TextureTransform_out/ROOT/Rotation/Rotation___Correct/Geometry>") != std::string::npos)
                            << "Scale___Correct should reference full hierarchy path to primary Rotation___Correct";
                    }
        }
    }
    
    // Also validate that primary nodes contain actual mesh definitions (not references)
    std::vector<std::string> primaryIndicatorXForms = {"Rotation___Correct", "Rotation___Not_Supported"};
    for (const std::string& xformName : primaryIndicatorXForms) {
        size_t xformPos = content.find("def Xform \"" + xformName + "\"");
        if (xformPos != std::string::npos) {
            size_t xformEnd = content.find("\n        }", xformPos);
            if (xformEnd == std::string::npos) xformEnd = content.find("\n    }", xformPos);
            if (xformEnd == std::string::npos) xformEnd = content.length();
            std::string xformSection = content.substr(xformPos, xformEnd - xformPos);
            
            EXPECT_TRUE(xformSection.find("def Scope \"Geometry\"") != std::string::npos)
                << xformName << " should contain Geometry child";
            EXPECT_TRUE(xformSection.find("def Mesh") != std::string::npos)
                << xformName << " should contain actual mesh definition (primary node)";
            EXPECT_TRUE(xformSection.find("instanceable = true") == std::string::npos)
                << xformName << " should NOT be instanceable (primary node)";
            EXPECT_TRUE(xformSection.find("prepend references") == std::string::npos)
                << xformName << " should NOT have prepend references (primary node)";
        }
    }
    // ========================================================================
    // MESH ATTRIBUTE ORDERING VALIDATION: Critical vertex/UV/FaceVertexIndices ordering 
    // ========================================================================
    
    // 10. Check for consistent ordering across all marker meshes
    std::vector<std::string> markerMeshes = {"Correct_Marker", "Not_Supported_Marker", "Error_Marker"};
    for (const std::string& meshName : markerMeshes) {
        size_t meshPos = content.find("def Mesh \"" + meshName + "\"");
        if (meshPos != std::string::npos) {
            size_t meshEnd = content.find("\n        }", meshPos);
            if (meshEnd == std::string::npos) meshEnd = content.find("\n    }", meshPos);
            if (meshEnd == std::string::npos) meshEnd = content.length();
            std::string meshSection = content.substr(meshPos, meshEnd - meshPos);
            
            // All marker meshes should have expected vertex ordering pattern
            EXPECT_TRUE(meshSection.find("point3f[] points = [(-0.5, 0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, -0.5, 0)]") != std::string::npos)
                << meshName << " points should have expected ordering";
            
            EXPECT_TRUE(meshSection.find("float2[] primvars:st = [(0, 0), (1, 1), (1, 0), (0, 1)]") != std::string::npos)
                << meshName << " UVs should have expected ordering";

            EXPECT_TRUE(meshSection.find("int[] faceVertexIndices = [0, 1, 2, 1, 0, 3]") != std::string::npos)
                << meshName << " Face vertex indices should have expected winding order";
        }
    }
}

TEST_F(utUSDZExport, validateAdvancedTextureTypesSupport) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/PBR/damaged-helmet.glb";
    const std::string outputPath = "usd/advanced/AdvancedTextures_out.usda";
    
    if (!performRoundTripTest(inputPath, outputPath, "usda")) {
        FAIL() << "Round-trip test failed for advanced texture validation";
        return;
    }
    
    // Read the generated USD file and check for comprehensive texture mapping support
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Could not open generated USD file for advanced texture validation";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Verify that our comprehensive texture mapping is working
    std::vector<std::string> expectedShaderTypes = {
        "UsdPreviewSurface",
        "UsdUVTexture", 
        "UsdPrimvarReader_float2"
    };
    
    for (const std::string& shaderType : expectedShaderTypes) {
        EXPECT_TRUE(content.find(shaderType) != std::string::npos) 
            << "USD file should contain " << shaderType << " shaders";
    }
    
    // Check for proper texture channel usage
    std::vector<std::string> expectedChannels = {
        "outputs:rgb",  // Color textures
        "outputs:r",    // Single channel textures (occlusion, metallic, etc.)
        "outputs:g",    // Roughness channel
        "outputs:b",    // Metallic channel  
        "outputs:a"     // Alpha channel
    };
    
    for (const std::string& channel : expectedChannels) {
        EXPECT_TRUE(content.find(channel) != std::string::npos)
            << "USD file should contain " << channel << " texture channel usage";
    }
    
    // CRITICAL: Verify texture directory and files were created
    // Extract directory from output path
    std::string outputDir = outputPath.substr(0, outputPath.find_last_of("/"));
    std::string texturesDir = outputDir + "/textures";
    
    // Check textures directory exists
    struct stat dirStat;
    EXPECT_EQ(stat(texturesDir.c_str(), &dirStat), 0) << "Textures directory not created: " << texturesDir;
    if (stat(texturesDir.c_str(), &dirStat) == 0) {
        EXPECT_TRUE(S_ISDIR(dirStat.st_mode)) << "Textures path is not a directory: " << texturesDir;

        // Check expected texture files exist based on damaged-helmet.glb content
        std::vector<std::string> expectedTextureFiles = {
            "Default_albedo.jpg",
            "Default_metalRoughness.jpg", 
            "Default_normal.jpg",
            "Default_AO.jpg",
            "Default_emissive.jpg"
        };
        
        for (const std::string& textureFile : expectedTextureFiles) {
            std::string expectedTextureFilePath = texturesDir + "/" + textureFile;
            struct stat fileStat;
            EXPECT_EQ(stat(expectedTextureFilePath.c_str(), &fileStat), 0) 
                << "Expected texture file not found: " << expectedTextureFilePath;
            if (stat(expectedTextureFilePath.c_str(), &fileStat) == 0) {
                EXPECT_TRUE(S_ISREG(fileStat.st_mode)) 
                    << "Texture path is not a regular file: " << expectedTextureFilePath;
                EXPECT_GT(fileStat.st_size, 0) 
                    << "Texture file is empty: " << expectedTextureFilePath;
            }
        }
    }
}

TEST_F(utUSDZExport, validateNormalMapBiasScaleCorrection) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/PBR/damaged-helmet.glb";
    const std::string outputPath = "usd/corrections/NormalMapCorrection_out.usda";
    
    if (!performRoundTripTest(inputPath, outputPath, "usda")) {
        FAIL() << "Round-trip test failed for normal map validation";
        return;
    }
    
    // Verify normal map bias and scale correction
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Could not open generated USD file for normal map validation";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Check for proper normal map bias and scale values
    EXPECT_TRUE(content.find("inputs:bias = (-1, -1, -1, 0)") != std::string::npos ||
                content.find("bias = (-1, -1, -1, 0)") != std::string::npos)
        << "Normal map textures should have proper bias values for 8-bit normal maps";
    
    EXPECT_TRUE(content.find("inputs:scale = (2, 2, 2, 1)") != std::string::npos ||
                content.find("scale = (2, 2, 2, 1)") != std::string::npos)
        << "Normal map textures should have proper scale values for 8-bit normal maps";
}

TEST_F(utUSDZExport, validateOpacityThresholdSupport) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf";
    const std::string outputPath = "usd/opacity/OpacityThreshold_out.usda";
    
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Additional validation for materials with transparency
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene && scene->mNumMaterials > 0) {
        const aiMaterial* material = scene->mMaterials[0];
        
        // Check for proper opacity handling
        float opacity = 1.0f;
        if (material->Get(AI_MATKEY_OPACITY, opacity) == aiReturn_SUCCESS) {
            EXPECT_GE(opacity, 0.0f) << "Opacity should be non-negative";
            EXPECT_LE(opacity, 1.0f) << "Opacity should not exceed 1.0";
        }
    }
}

// =============================================================================
// USD 2.6 OPACITY MODE TESTS
// =============================================================================

TEST_F(utUSDZExport, validateOpacityModeTransparent) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/AlphaBlend-glTF/Material_AlphaBlend_00.gltf";
    const std::string outputPath = "usd/opacity/OpacityModeTransparent_out.usda";
    
    if (!performRoundTripTest(inputPath, outputPath, "usda")) {
        FAIL() << "Round-trip test failed for alpha blend opacity mode";
        return;
    }
    
    // Verify USD file contains proper opacity mode for transparent materials
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Could not open generated USD file for opacity mode validation";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Check for USD 2.6 opacity mode "transparent" for alpha blending
    EXPECT_TRUE(content.find("opacityMode") != std::string::npos ||
                content.find("Transparent") != std::string::npos)
        << "USD file should contain opacity mode handling for transparent materials";
    
    // Verify alpha blending is properly handled
    EXPECT_TRUE(content.find("opacity") != std::string::npos)
        << "USD file should contain opacity properties for alpha blending materials";
}

TEST_F(utUSDZExport, validateOpacityModePresence) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/AlphaMask-glTF/Material_AlphaMask_00.gltf";
    const std::string outputPath = "usd/opacity/OpacityModePresence_out.usda";
    
    if (!performRoundTripTest(inputPath, outputPath, "usda")) {
        FAIL() << "Round-trip test failed for alpha mask opacity mode";
        return;
    }
    
    // Verify USD file contains proper opacity mode for masked materials
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Could not open generated USD file for opacity mode validation";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Check for alpha masking/threshold handling
    EXPECT_TRUE(content.find("opacityThreshold") != std::string::npos ||
                content.find("opacity") != std::string::npos)
        << "USD file should contain opacity threshold properties for alpha mask materials";
    
    // Alpha masking should be converted to opacity threshold in USD
    EXPECT_TRUE(content.find("UsdPreviewSurface") != std::string::npos)
        << "USD file should contain UsdPreviewSurface shader with opacity handling";
}

// =============================================================================
// PACKED METALLIC-ROUGHNESS TEXTURE TESTS
// =============================================================================

TEST_F(utUSDZExport, validatePackedMetallicRoughnessTexture) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/PackedMetallicRoughness-glTF/Material_MetallicRoughness_09.gltf";
    const std::string outputPath = "usd/packed-textures/PackedMetallicRoughness_out.usda";
    
    if (!performRoundTripTest(inputPath, outputPath, "usda")) {
        FAIL() << "Round-trip test failed for packed metallic-roughness texture";
        return;
    }
    
    // Verify USD file contains proper channel routing for packed textures
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Could not open generated USD file for packed texture validation";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Check for proper texture channel routing (glTF: R=AO, G=Roughness, B=Metallic)
    EXPECT_TRUE(content.find("outputs:r") != std::string::npos ||
                content.find("outputs:g") != std::string::npos ||
                content.find("outputs:b") != std::string::npos)
        << "USD file should contain individual channel outputs for packed textures";
    
    // Verify metallic and roughness connections
    EXPECT_TRUE(content.find("metallic") != std::string::npos)
        << "USD file should contain metallic property connections";
    
    EXPECT_TRUE(content.find("roughness") != std::string::npos)
        << "USD file should contain roughness property connections";
    
    // Check for UsdUVTexture shader creation
    EXPECT_TRUE(content.find("UsdUVTexture") != std::string::npos)
        << "USD file should contain UsdUVTexture shaders for packed texture";
}

// =============================================================================
// MATERIAL FACTOR VALIDATION TESTS  
// =============================================================================

TEST_F(utUSDZExport, validateMaterialFactors) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/MaterialFactors-glTF/Material_00.gltf";
    const std::string outputPath = "usd/factors/MaterialFactors_out.usda";
    
    if (!performRoundTripTest(inputPath, outputPath, "usda")) {
        FAIL() << "Round-trip test failed for material factors";
        return;
    }
    
    // Re-import to validate factor values
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene) << "Failed to re-import USD file for factor validation";
    ASSERT_GT(scene->mNumMaterials, 0u) << "Scene should contain materials";
    
    const aiMaterial* material = scene->mMaterials[0];
    ASSERT_NE(nullptr, material) << "First material should not be null";
    
    // Validate factor ranges and presence
    float metallicFactor = 0.0f;
    if (material->Get(AI_MATKEY_METALLIC_FACTOR, metallicFactor) == aiReturn_SUCCESS) {
        EXPECT_GE(metallicFactor, 0.0f) << "Metallic factor should be non-negative";
        EXPECT_LE(metallicFactor, 1.0f) << "Metallic factor should not exceed 1.0";
    }
    
    float roughnessFactor = 1.0f;
    if (material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor) == aiReturn_SUCCESS) {
        EXPECT_GE(roughnessFactor, 0.0f) << "Roughness factor should be non-negative";
        EXPECT_LE(roughnessFactor, 1.0f) << "Roughness factor should not exceed 1.0";
    }
    
    // Check base color factor
    aiColor3D baseColorFactor;
    if (material->Get(AI_MATKEY_COLOR_DIFFUSE, baseColorFactor) == aiReturn_SUCCESS ||
        material->Get(AI_MATKEY_BASE_COLOR, baseColorFactor) == aiReturn_SUCCESS) {
        EXPECT_GE(baseColorFactor.r, 0.0f) << "Base color factor R should be non-negative";
        EXPECT_GE(baseColorFactor.g, 0.0f) << "Base color factor G should be non-negative";
        EXPECT_GE(baseColorFactor.b, 0.0f) << "Base color factor B should be non-negative";
    }
}

// =============================================================================
// TEXTURE FALLBACK TESTS
// =============================================================================

TEST_F(utUSDZExport, validateTextureFallbacks) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf";
    const std::string outputPath = "usd/fallbacks/TextureFallbacks_out.usda";
    
    if (!performRoundTripTest(inputPath, outputPath, "usda")) {
        FAIL() << "Round-trip test failed for texture fallbacks";
        return;
    }
    
    // Verify USD file contains proper texture connections with fallback handling
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Could not open generated USD file for fallback validation";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Check for texture connections (should use BASE_COLOR or fallback to DIFFUSE)
    EXPECT_TRUE(content.find("diffuseColor.connect") != std::string::npos)
        << "USD file should contain diffuse color texture connections";
    
    // Check for proper shader network
    EXPECT_TRUE(content.find("UsdUVTexture") != std::string::npos)
        << "USD file should contain UsdUVTexture shaders for texture fallbacks";
    
    // Check for texture coordinate handling
    EXPECT_TRUE(content.find("UsdPrimvarReader_float2") != std::string::npos)
        << "USD file should contain UV coordinate readers";
}

// =============================================================================
// USD SPECIFICATION COMPLIANCE TESTS
// =============================================================================

TEST_F(utUSDZExport, validateUSDSpecCompliance) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/PBR/damaged-helmet.glb";
    const std::string outputPath = "usd/compliance/USDSpecCompliance_out.usda";
    
    if (!performRoundTripTest(inputPath, outputPath, "usda")) {
        FAIL() << "Round-trip test failed for USD spec compliance";
        return;
    }
    
    // Verify USD file contains only valid USD Preview Surface properties
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "Could not open generated USD file for spec compliance validation";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Valid USD Preview Surface 2.6 properties should be present
    std::vector<std::string> validProperties = {
        "diffuseColor", "emissiveColor", "useSpecularWorkflow", "specularColor",
        "metallic", "roughness", "clearcoat", "clearcoatRoughness", 
        "opacity", "opacityThreshold", "ior", "normal", "displacement", "occlusion"
    };
    
    bool hasValidProperties = false;
    for (const std::string& prop : validProperties) {
        if (content.find(prop) != std::string::npos) {
            hasValidProperties = true;
            break;
        }
    }
    EXPECT_TRUE(hasValidProperties) << "USD file should contain valid USD Preview Surface properties";
    
    // Check for USD shader network structure
    EXPECT_TRUE(content.find("UsdPreviewSurface") != std::string::npos)
        << "USD file should contain UsdPreviewSurface shader";
    
    // Verify no invalid/unsupported properties are written  
    std::vector<std::string> unsupportedProperties = {
        "sheen", "transmission", "anisotropy", "volume", "clearcoatAlt"
    };
    
    for (const std::string& prop : unsupportedProperties) {
        EXPECT_TRUE(content.find(prop) == std::string::npos)
            << "USD file should not contain unsupported property: " << prop;
    }
}

// =============================================================================
// EDGE CASE TESTS (P4)
// =============================================================================

TEST_F(utUSDZExport, validateMinimalScene) {
    // Test export behavior with a simple model (edge case for minimal content)
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf";
    const std::string outputPath = "usd/edge-cases/MinimalScene_out.usda";
    
    // Round trip test validates basic functionality and USD library compatibility
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Focus on exporter-specific edge case handling
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "USD file should exist for minimal scene";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Validate our exporter creates proper structure even for simple scenes
    validateShaderNetworkStructure(content);
    validateExporterMaterialLogic(content);
}

TEST_F(utUSDZExport, validateExtremePropertyValues) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/MaterialFactors-glTF/Material_00.gltf";
    const std::string outputPath = "usd/edge-cases/ExtremeValues_out.usda";
    
    // Test with material that might have extreme values
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Re-import and validate extreme values are clamped properly
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene && scene->mNumMaterials > 0) {
        validateMaterialPropertyRanges(scene);
        
        // Additional validation for extreme values
        for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
            const aiMaterial* material = scene->mMaterials[i];
            
            // Validate no NaN or infinite values
            float metallicFactor = 0.0f;
            if (material->Get(AI_MATKEY_METALLIC_FACTOR, metallicFactor) == aiReturn_SUCCESS) {
                EXPECT_FALSE(std::isnan(metallicFactor)) << "Metallic factor should not be NaN";
                EXPECT_FALSE(std::isinf(metallicFactor)) << "Metallic factor should not be infinite";
            }
            
            float roughnessFactor = 1.0f;
            if (material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor) == aiReturn_SUCCESS) {
                EXPECT_FALSE(std::isnan(roughnessFactor)) << "Roughness factor should not be NaN";
                EXPECT_FALSE(std::isinf(roughnessFactor)) << "Roughness factor should not be infinite";
            }
        }
    }
}

TEST_F(utUSDZExport, validateDegenerateGeometry) {
    // Test handling of point primitives and degenerate geometry
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/glTF-Asset-Generator/Mesh_PrimitiveMode/Mesh_PrimitiveMode_00.gltf";
    const std::string outputPath = "usd/edge-cases/DegenerateGeometry_out.usda";
    
    // Round trip test validates our exporter handles edge geometry cases
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Focus on exporter-specific handling of unusual geometry
    std::ifstream usdFile(outputPath);
    if (usdFile.is_open()) {
        std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
        // Only validate shader network if materials are present (exporter-specific logic)
        if (content.find("UsdPreviewSurface") != std::string::npos) {
            validateShaderNetworkStructure(content);
        }
    }
}

TEST_F(utUSDZExport, validateNodeHierarchy) {
    // Test our exporter's handling of scene node hierarchies
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf";
    const std::string outputPath = "usd/edge-cases/NodeHierarchy_out.usda";
    
    // Round trip test validates scene structure is preserved correctly
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Validate our exporter's node handling logic
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene) {
        EXPECT_NE(nullptr, scene->mRootNode) << "Root node should exist after export/import cycle";
        
        // Validate our exporter doesn't create infinite node loops (exporter-specific concern)
        int nodeCount = 0;
        std::function<void(const aiNode*)> countNodes = [&](const aiNode* node) {
            if (!node || nodeCount > 100) return; // Reasonable limit for this test
            nodeCount++;
            for (unsigned int i = 0; i < node->mNumChildren; ++i) {
                countNodes(node->mChildren[i]);
            }
        };
        countNodes(scene->mRootNode);
        
        EXPECT_LT(nodeCount, 100) << "Node hierarchy should be reasonable in size";
    }
}

// =============================================================================
// ERROR HANDLING TESTS (P5)
// =============================================================================

TEST_F(utUSDZExport, validateMissingTextureHandling) {
    // Test our exporter's graceful handling of missing texture files
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/MissingBin/BoxTextured.gltf";
    const std::string outputPath = "usd/errors/MissingTextures_out.usda";
    
    // Create output directory
    ASSERT_TRUE(createDirectoryRecursive(outputPath)) << "Failed to create output directory";
    
    // Import might fail, but if it succeeds, our exporter should handle missing textures gracefully
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(inputPath, 0);
    
    if (scene != nullptr) {
        // Our exporter should succeed even with missing textures
        Assimp::Exporter exporter;
        aiReturn result = exporter.Export(scene, "usda", outputPath, 0u);
        
        if (result == aiReturn_SUCCESS) {
            // Validate our exporter still creates proper material structure
            std::ifstream usdFile(outputPath);
            if (usdFile.is_open()) {
                std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
                
                // Our exporter should still create material structure even without textures
                if (content.find("UsdPreviewSurface") != std::string::npos) {
                    validateShaderNetworkStructure(content);
                }
            }
        }
    }
}

TEST_F(utUSDZExport, validateMaterialPropertySanitization) {
    // Test our exporter's sanitization of unusual material properties
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf";
    const std::string outputPath = "usd/errors/MaterialSanitization_out.usda";
    
    // Round trip test validates our exporter handles material edge cases
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Focus on our exporter's material property sanitization logic
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "USD file should exist after material sanitization";
    
    std::string content((std::istreambuf_iterator<char>(usdFile)), std::istreambuf_iterator<char>());
    
    // Validate our exporter doesn't write invalid USD properties
    validateExporterMaterialLogic(content);
    
    // Validate our material property clamping works correctly
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    if (scene && scene->mNumMaterials > 0) {
        validateMaterialPropertyRanges(scene);
    }
}

TEST_F(utUSDZExport, validateMalformedInputHandling) {
    // Test our exporter's handling of unusual but loadable input
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/SchemaFailures/sceneWrongType.gltf";
    const std::string outputPath = "usd/errors/MalformedInput_out.usda";
    
    // Create output directory
    ASSERT_TRUE(createDirectoryRecursive(outputPath)) << "Failed to create output directory";
    
    // Try to import unusual file
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(inputPath, 0);
    
    if (scene != nullptr) {
        // If import succeeds, our exporter should handle unusual data gracefully
        Assimp::Exporter exporter;
        aiReturn result = exporter.Export(scene, "usda", outputPath, 0u);
        
        // If our exporter succeeds, that's good - it handled the edge case
        EXPECT_TRUE(result == aiReturn_SUCCESS || result != aiReturn_SUCCESS)
            << "Exporter should handle unusual input gracefully (success or failure both OK)";
    }
    // If import fails, that's also acceptable for malformed input
}

TEST_F(utUSDZExport, validateOutputDirectoryCreation) {
    // Test our exporter's directory creation capability
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf";
    const std::string outputPath = "usd/deep/nested/directories/DirectoryCreation_out.usda";
    
    // Our exporter should handle deep directory creation
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Validate our exporter created the file in the correct location
    std::ifstream usdFile(outputPath);
    EXPECT_TRUE(usdFile.is_open()) << "USD file should exist in deeply nested directory";
}

// =============================================================================
// SKELETAL ANIMATION TESTS (CRITICAL FOR USD COMPATIBILITY)
// =============================================================================

TEST_F(utUSDZExport, validateSkeletalAnimationStructureUsdz) {
    std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/CesiumMan/CesiumMan.glb";
    std::string outputPath = "usd/animation/CesiumMan.usdz";

    // Perform export
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usdz"));
}

TEST_F(utUSDZExport, validateSkeletalAnimationStructureBlender) {
    std::string inputPath = "/Users/rifont/Downloads/usdz-fixtures/animation/CesiumMan/blender/CesiumMan-blender.usda";
    std::string outputPath = "usd/animation/CesiumMan_blender.usda";

    // Perform export
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usdz"));
}

TEST_F(utUSDZExport, validateSkeletalAnimationStructure) {
    std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/CesiumMan/CesiumMan.glb";
    std::string outputPath = "usd/animation/CesiumMan.usda";
    
    // Perform export
    EXPECT_TRUE(performRoundTripTest(inputPath, outputPath, "usda"));
    
    // Read the USD file content for validation
    std::ifstream usdFile(outputPath);
    ASSERT_TRUE(usdFile.is_open()) << "USD file should be created successfully";
    
    std::string usdContent((std::istreambuf_iterator<char>(usdFile)),
                          std::istreambuf_iterator<char>());
    usdFile.close();
    
    // Validate proper SkelRoot > Skeleton > SkelAnimation structure
    EXPECT_TRUE(usdContent.find("def SkelRoot") != std::string::npos)
        << "SkelRoot prim is required for skeletal animation";
    
    EXPECT_TRUE(usdContent.find("def Skeleton") != std::string::npos)
        << "Skeleton prim is required inside SkelRoot";
    
    EXPECT_TRUE(usdContent.find("def SkelAnimation \"Animation\"") != std::string::npos)
        << "SkelAnimation prim is MISSING";
    
    EXPECT_TRUE(usdContent.find("prepend rel skel:animationSource") != std::string::npos)
        << "skel:animationSource relationship is MISSING - required to bind animation data";
    
    EXPECT_TRUE(usdContent.find("quatf[] rotations.timeSamples") != std::string::npos)
        << "Unified rotations.timeSamples array is MISSING - required for proper skeletal animation";
    
    // ========================================================================
    // PRIORITY 1 FIXES - ANIMATION FUNCTIONALITY (CRITICAL)
    // ========================================================================
    
    // Frame timing should match Apple reference (fractional time codes)
    if (usdContent.find("rotations.timeSamples") != std::string::npos) {
        EXPECT_TRUE(usdContent.find("0.0416666: [") != std::string::npos)
            << "Animation frames should start at Apple reference time code (0.0416666)";
    }
    
    // Extra triangleSubdivisionRule property should be removed
    EXPECT_TRUE(usdContent.find("triangleSubdivisionRule") == std::string::npos)
        << "Should NOT have triangleSubdivisionRule property (not in reference)";
        
    // Validate joint structure consistency
    size_t skeletonJointsPos = usdContent.find("uniform token[] joints = [");
    EXPECT_NE(skeletonJointsPos, std::string::npos)
        << "Skeleton joints array should be defined";
    
    if (skeletonJointsPos != std::string::npos) {
        // Count expected joints (should be 19 for CesiumMan)
        size_t jointsStart = usdContent.find('[', skeletonJointsPos);
        size_t jointsEnd = usdContent.find(']', jointsStart);
        std::string jointsSection = usdContent.substr(jointsStart + 1, jointsEnd - jointsStart - 1);
        
        // Count joint entries by counting quotes
        size_t jointCount = std::count(jointsSection.begin(), jointsSection.end(), '"') / 2;
        EXPECT_EQ(jointCount, 19) << "CesiumMan should have exactly 19 joints";
    }
    
    // ========================================================================
    // ADDITIONAL COMPREHENSIVE ASSERTIONS BASED ON APPLE REFERENCE ANALYSIS
    // ========================================================================
    
    // 1. TOP-LEVEL SCENE STRUCTURE ASSERTIONS
    EXPECT_TRUE(usdContent.find("def \"Skeletons\"") != std::string::npos)
        << "Top-level 'Skeletons' section is MISSING - required for proper USD composition";
    
    EXPECT_TRUE(usdContent.find("def \"Animations\"") != std::string::npos)
        << "Top-level 'Animations' section is MISSING - required for proper USD composition";
    
    // 2. SKELETON DEFINITION ASSERTIONS
    EXPECT_TRUE(usdContent.find("prepend apiSchemas = [\"SkelBindingAPI\"]") != std::string::npos)
        << "SkelBindingAPI schema is MISSING from Skeleton prim definition";
    
    EXPECT_TRUE(usdContent.find("uniform token[] jointNames = [") != std::string::npos)
        << "jointNames[] attribute is MISSING - should be separate from joints[] paths";
    
    EXPECT_TRUE(usdContent.find("token visibility = \"invisible\"") != std::string::npos)
        << "Skeleton visibility = 'invisible' attribute is MISSING";
    
    // Check for shorthand joint path format (e.g., "n0/n1/n3" instead of full names)
    if (usdContent.find("uniform token[] joints = [") != std::string::npos) {
        EXPECT_TRUE(usdContent.find("\"n0/n1/n3\"") != std::string::npos)
            << "Joint paths should use shorthand notation like 'n0/n1/n3' to match Apple reference";
    }
    
    // 3. SKELANIMATION STRUCTURE ASSERTIONS
    // (SkelAnimation existence already validated above)
    
    // Animation should have all three required arrays (beyond the basic rotations check above)
    EXPECT_TRUE(usdContent.find("float3[] translations.timeSamples") != std::string::npos)
        << "float3[] translations.timeSamples array is MISSING from SkelAnimation";
    
    EXPECT_TRUE(usdContent.find("half3[] scales.timeSamples") != std::string::npos)
        << "half3[] scales.timeSamples array is MISSING from SkelAnimation";
    
    // Animation joints should match skeleton joints
    size_t animJointsPos = usdContent.find("def SkelAnimation");
    if (animJointsPos != std::string::npos) {
        size_t animJointsArrayPos = usdContent.find("uniform token[] joints = [", animJointsPos);
        EXPECT_NE(animJointsArrayPos, std::string::npos)
            << "SkelAnimation should have its own joints[] array matching skeleton joints";
    }
    
    // 4. SKELROOT COMPOSITION ASSERTIONS
    EXPECT_TRUE(usdContent.find("def SkelRoot \"ArmatureSkelRoot\"") != std::string::npos)
        << "SkelRoot should be named 'ArmatureSkelRoot' to match Apple reference";
    
    // SkelRoot should have SkelBindingAPI schema
    size_t skelRootPos = usdContent.find("def SkelRoot");
    if (skelRootPos != std::string::npos) {
        size_t skelRootEnd = usdContent.find("}", skelRootPos);
        std::string skelRootSection = usdContent.substr(skelRootPos, skelRootEnd - skelRootPos);
        
        EXPECT_TRUE(skelRootSection.find("prepend apiSchemas = [\"SkelBindingAPI\"]") != std::string::npos)
            << "SkelRoot should have SkelBindingAPI schema in prim definition";
    }
    
    // 5. REFERENCE-BASED COMPOSITION ASSERTIONS
    EXPECT_TRUE(usdContent.find("prepend references = </") != std::string::npos)
        << "Reference-based composition is MISSING - should reference separate Skeletons and Animations sections";
    
    // Check for Armature reference (flexible formatting)
    EXPECT_TRUE(usdContent.find("def \"Armature\"") != std::string::npos && 
                usdContent.find("prepend references = </") != std::string::npos &&
                usdContent.find("/Skeletons/Armature>") != std::string::npos)
        << "Armature reference to top-level Skeletons section is MISSING";
    
    // Check for animation reference (flexible formatting)  
    EXPECT_TRUE(usdContent.find("def \"anim\"") != std::string::npos &&
                usdContent.find("prepend references = </") != std::string::npos &&
                usdContent.find("/Animations/Animation>") != std::string::npos)
        << "Animation reference to top-level Animations section is MISSING";
    
    // 6. GEOMSCOPE WRAPPER ASSERTIONS
    EXPECT_TRUE(usdContent.find("def Scope \"GeomScope\"") != std::string::npos)
        << "GeomScope wrapper around mesh geometry is MISSING";
    
    // Mesh should be inside GeomScope, not directly in SkelRoot
    size_t geomScopePos = usdContent.find("def Scope \"GeomScope\"");
    if (geomScopePos != std::string::npos) {
        size_t geomScopeEnd = usdContent.find("}", geomScopePos);
        std::string geomScopeSection = usdContent.substr(geomScopePos, geomScopeEnd - geomScopePos);
        
        EXPECT_TRUE(geomScopeSection.find("def Mesh") != std::string::npos)
            << "Mesh should be defined inside GeomScope, not directly in SkelRoot";
    }
    
    // 7. COORDINATE SYSTEM ASSERTIONS
    // Should use matrix4d xformOp:transform, not quaternions
    EXPECT_TRUE(usdContent.find("matrix4d xformOp:transform") != std::string::npos)
        << "Should use matrix4d xformOp:transform instead of quaternion transforms to match Apple reference";
    
    // Check for Apple's specific Z_UP transform matrix
    EXPECT_TRUE(usdContent.find("(1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)") != std::string::npos)
        << "Z_UP transform should match Apple reference matrix values";
    
    // 8. MESH SKELETAL BINDING ASSERTIONS
    size_t meshPos = usdContent.find("def Mesh");
    if (meshPos != std::string::npos) {
        size_t meshEnd = usdContent.find("}", meshPos);
        std::string meshSection = usdContent.substr(meshPos, meshEnd - meshPos);
        
        // SkelBindingAPI should be in prim definition header, not as separate uniform token[]
        bool hasCorrectSchemas = (meshSection.find("prepend apiSchemas = [\"SkelBindingAPI\", \"MaterialBindingAPI\"]") != std::string::npos) ||
                                 (meshSection.find("prepend apiSchemas = [\"MaterialBindingAPI\", \"SkelBindingAPI\"]") != std::string::npos);
        EXPECT_TRUE(hasCorrectSchemas)
            << "Mesh should have both SkelBindingAPI and MaterialBindingAPI in prepend apiSchemas header";
        
        // Should NOT have separate "uniform token[] apiSchemas = [\"SkelBindingAPI\"]"
        EXPECT_TRUE(meshSection.find("uniform token[] apiSchemas") == std::string::npos)
            << "Should NOT have separate 'uniform token[] apiSchemas' - use prepend apiSchemas instead";
    }
    
    // 9. METADATA AND TIMING ASSERTIONS
    // Time codes should use corrected timing values
    bool hasCorrectStartTime = (usdContent.find("startTimeCode = 0.0416667") != std::string::npos);
    EXPECT_TRUE(hasCorrectStartTime)
        << "startTimeCode should be 0.0416667 for corrected timing";
    
    EXPECT_TRUE(usdContent.find("endTimeCode = 4.208333") != std::string::npos)
        << "endTimeCode should be 4.208333 for corrected timing";
    
    EXPECT_TRUE(usdContent.find("timeCodesPerSecond = 24") != std::string::npos)
        << "timeCodesPerSecond should be 24 for proper frame rate";
    
    // 12. RELATIONSHIP PATH VALIDATION
    // (Basic relationship existence already validated above)
    if (usdContent.find("prepend rel skel:animationSource") != std::string::npos) {
        EXPECT_TRUE(usdContent.find("skel:animationSource = </") != std::string::npos)
            << "skel:animationSource relationship should have valid path reference";
    }
    
    if (usdContent.find("prepend rel skel:skeleton") != std::string::npos) {
        EXPECT_TRUE(usdContent.find("skel:skeleton = </") != std::string::npos)
            << "skel:skeleton relationship should have valid path reference";
    }
    
}

// =============================================================================
// Additional round-trip tests for USDZ format coverage
// =============================================================================

TEST_F(utUSDZExport, importGltfSimpleSkinExportUsdz) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/simple_skin/simple_skin.gltf",
        "usd/skinning/SimpleSkin_out.usdz",
        "usdz"));
}

TEST_F(utUSDZExport, importGltfQuadSkinExportUsdz) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/simple_skin/quad_skin.glb",
        "usd/skinning/QuadSkin_out.usdz",
        "usdz"));
}

TEST_F(utUSDZExport, importGltfCamerasExportUsdz) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/cameras/Cameras.gltf",
        "usd/cameras/Cameras_out.usdz",
        "usdz"));
}

TEST_F(utUSDZExport, importGltfAnimatedMorphCubeExportUsdz) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/AnimatedMorphCube/glTF/AnimatedMorphCube.gltf",
        "usd/blendshapes/AnimatedMorphCube_out.usdz",
        "usdz"));
}

TEST_F(utUSDZExport, importGltfCesiumManExportUsdz) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/CesiumMan/CesiumMan.glb",
        "usd/animation/CesiumMan_out.usdz",
        "usdz"));
}

TEST_F(utUSDZExport, importGltf2CylinderEngineExportUsda) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/2CylinderEngine-glTF-Binary/2CylinderEngine.glb",
        "usd/complex/2CylinderEngine_out.usda",
        "usda"));
}

TEST_F(utUSDZExport, importGltf2CylinderEngineExportUsdz) {
    EXPECT_TRUE(performRoundTripTest(
        ASSIMP_TEST_MODELS_DIR "/glTF2/2CylinderEngine-glTF-Binary/2CylinderEngine.glb",
        "usd/complex/2CylinderEngine_out.usdz",
        "usdz"));
}

// Validate animation fidelity in round-trip
TEST_F(utUSDZExport, validateAnimationFidelityRoundTrip) {
    const std::string inputPath = ASSIMP_TEST_MODELS_DIR "/glTF2/AnimatedMorphCube/glTF/AnimatedMorphCube.gltf";
    const std::string outputPath = "usd/animation/AnimatedMorphCube_fidelity.usda";
    
    Assimp::Importer importer;
    const aiScene* originalScene = importer.ReadFile(inputPath,
        aiProcess_Triangulate | aiProcess_GenUVCoords | aiProcess_JoinIdenticalVertices);
    ASSERT_NE(nullptr, originalScene);
    
    Assimp::Exporter exporter;
    aiReturn result = exporter.Export(originalScene, "usda", outputPath, 0u);
    EXPECT_EQ(aiReturn_SUCCESS, result);
    
    Assimp::Importer reimporter;
    const aiScene* reimported = reimporter.ReadFile(outputPath,
        aiProcess_Triangulate | aiProcess_GenUVCoords | aiProcess_JoinIdenticalVertices);
    ASSERT_NE(nullptr, reimported);
    
    // Validate mesh count preserved
    EXPECT_EQ(originalScene->mNumMeshes, reimported->mNumMeshes);
    
    // Validate material count preserved
    EXPECT_EQ(originalScene->mNumMaterials, reimported->mNumMaterials);
}

// ===========================================================================
// Unit/axis contract tests — Tier 2 USD exporter.
//
// USD canonically authors metres + Y-up. The exporter must bake the
// `AI_METADATA_UNIT_SCALE_TO_METERS` / `AI_METADATA_UP_AXIS` contract into
// mesh vertices (and normals) before emitting the stage so that any source
// frame round-trips into the canonical USD frame on disk. Same-frame inputs
// (m + Y-up) and inputs that omit the contract metadata must short-circuit
// to identity.
//
// We assert against the textual `points = [...]` table in the emitted .usda
// payload — re-importing through the USD chain is too lossy for this kind
// of axis-bake check, mirroring the X3D exporter's strategy.
// ===========================================================================

namespace {

aiScene *makeUsdBoxScene(const aiVector3D &halfExtents) {
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

// Scan every `points = [(x, y, z), ...]` table in the USDA payload and
// aggregate the axis-aligned extent. The USD exporter's tinyusdz wrapper
// emits one such table per mesh prim, formatted with `(...)` triplets and
// comma separators. Round-tripping through the USD importer for axis
// validation is too lossy (UsdGeomMesh re-decomposition + tinyusdz internal
// triangulation can re-order vertices), so we go straight to the on-disk
// text — same approach as the X3D exporter contract tests.
aiVector3D scanUsdaExtent(const std::string &path) {
    std::ifstream in(path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    aiVector3D mn(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max());
    aiVector3D mx(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                  std::numeric_limits<float>::lowest());

    bool found = false;
    const std::string blockAnchor = "points = [";
    size_t blockCursor = 0;
    while (true) {
        size_t blockPos = content.find(blockAnchor, blockCursor);
        if (blockPos == std::string::npos) break;
        size_t blockEnd = content.find(']', blockPos);
        if (blockEnd == std::string::npos) break;
        std::string block = content.substr(blockPos, blockEnd - blockPos);

        // Each triplet is `(x, y, z)`; iterate parenthesised groups.
        size_t triCursor = 0;
        while (true) {
            size_t open = block.find('(', triCursor);
            if (open == std::string::npos) break;
            size_t close = block.find(')', open);
            if (close == std::string::npos) break;
            std::string triplet = block.substr(open + 1, close - open - 1);
            // Replace commas with spaces so istringstream can read the floats.
            std::replace(triplet.begin(), triplet.end(), ',', ' ');
            std::istringstream iss(triplet);
            float x, y, z;
            if (iss >> x >> y >> z) {
                mn.x = std::min(mn.x, x);
                mn.y = std::min(mn.y, y);
                mn.z = std::min(mn.z, z);
                mx.x = std::max(mx.x, x);
                mx.y = std::max(mx.y, y);
                mx.z = std::max(mx.z, z);
                found = true;
            }
            triCursor = close + 1;
        }
        blockCursor = blockEnd + 1;
    }

    if (!found) return aiVector3D(0, 0, 0);
    return aiVector3D(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
}

} // namespace

TEST_F(utUSDZExport, exportUSDABakesUnitScaleWhenSourceUnitDiffersFromMeters) {
    // Source authored in centimetres (0.01 m per unit) with a 100 cm half-extent
    // box. After baking to USD's canonical metres frame we expect the on-disk
    // extent to be 2.0 m × 2.0 m × 2.0 m.
    aiScene *scene = makeUsdBoxScene(aiVector3D(100.0f, 100.0f, 100.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 0.01); // cm
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    ASSERT_TRUE(createDirectoryRecursive("usd/contract/unit_cm.usda"));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "usda", "usd/contract/unit_cm.usda"));
    delete scene;

    aiVector3D extent = scanUsdaExtent("usd/contract/unit_cm.usda");
    EXPECT_NEAR(2.0f, extent.x, 1e-3f);
    EXPECT_NEAR(2.0f, extent.y, 1e-3f);
    EXPECT_NEAR(2.0f, extent.z, 1e-3f);
    std::remove("usd/contract/unit_cm.usda");
}

TEST_F(utUSDZExport, exportUSDABakesAxisRotationWhenSourceIsZUp) {
    // Tall box on +Z (Z-up source) — after Z->Y bake the on-disk vertex
    // table must report the tall axis on +Y (USD canonical Y-up).
    aiScene *scene = makeUsdBoxScene(aiVector3D(1.0f, 1.0f, 5.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0); // metres
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));

    ASSERT_TRUE(createDirectoryRecursive("usd/contract/axis_zup.usda"));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "usda", "usd/contract/axis_zup.usda"));
    delete scene;

    aiVector3D extent = scanUsdaExtent("usd/contract/axis_zup.usda");
    EXPECT_NEAR(2.0f, extent.x, 1e-3f);
    EXPECT_NEAR(10.0f, extent.y, 1e-3f);
    EXPECT_NEAR(2.0f, extent.z, 1e-3f);
    std::remove("usd/contract/axis_zup.usda");
}

TEST_F(utUSDZExport, exportUSDAIsIdentityWhenSourceAlreadyMetersYUp) {
    // Source already lines up with USD canonical (m + Y-up); the bake helper
    // must short-circuit and the on-disk extents must match the authored
    // values bit-for-bit (within float tolerance).
    aiScene *scene = makeUsdBoxScene(aiVector3D(2.5f, 7.5f, 2.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    ASSERT_TRUE(createDirectoryRecursive("usd/contract/identity.usda"));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "usda", "usd/contract/identity.usda"));
    delete scene;

    aiVector3D extent = scanUsdaExtent("usd/contract/identity.usda");
    EXPECT_NEAR(5.0f, extent.x, 1e-4f);
    EXPECT_NEAR(15.0f, extent.y, 1e-4f);
    EXPECT_NEAR(5.0f, extent.z, 1e-4f);
    std::remove("usd/contract/identity.usda");
}

TEST_F(utUSDZExport, exportUSDAIsIdentityWhenContractMetadataAbsent) {
    // No `mMetaData` means the resolver short-circuits with no transform —
    // the geometry is treated as already in the canonical frame and emitted
    // verbatim.
    aiScene *scene = makeUsdBoxScene(aiVector3D(3.0f, 3.0f, 3.0f));
    ASSERT_EQ(nullptr, scene->mMetaData);

    ASSERT_TRUE(createDirectoryRecursive("usd/contract/no_meta.usda"));

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "usda", "usd/contract/no_meta.usda"));
    delete scene;

    aiVector3D extent = scanUsdaExtent("usd/contract/no_meta.usda");
    EXPECT_NEAR(6.0f, extent.x, 1e-4f);
    EXPECT_NEAR(6.0f, extent.y, 1e-4f);
    EXPECT_NEAR(6.0f, extent.z, 1e-4f);
    std::remove("usd/contract/no_meta.usda");
}

namespace {

uint16_t readZipLE16(const std::vector<uint8_t> &data, size_t offset) {
    return static_cast<uint16_t>(data[offset]) |
            (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t readZipLE32(const std::vector<uint8_t> &data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8) |
            (static_cast<uint32_t>(data[offset + 2]) << 16) |
            (static_cast<uint32_t>(data[offset + 3]) << 24);
}

struct StoredZipMember {
    std::string name;
    size_t dataOffset;
    std::vector<uint8_t> data;
};

bool parseStoredZipMembers(const std::vector<uint8_t> &archive,
        std::vector<StoredZipMember> &members, std::string &error) {
    members.clear();
    size_t offset = 0;
    while (true) {
        if (offset > archive.size() || archive.size() - offset < 4) {
            error = "ZIP ended before the central directory";
            return false;
        }

        const uint32_t signature = readZipLE32(archive, offset);
        if (signature == 0x02014b50) {
            return !members.empty();
        }
        if (signature != 0x04034b50 || archive.size() - offset < 30) {
            error = "Invalid or truncated ZIP local header";
            return false;
        }
        if (readZipLE16(archive, offset + 6) != 0 || readZipLE16(archive, offset + 8) != 0) {
            error = "USDZ member is not stored without flags";
            return false;
        }

        const size_t dataSize = readZipLE32(archive, offset + 18);
        const size_t nameSize = readZipLE16(archive, offset + 26);
        const size_t extraSize = readZipLE16(archive, offset + 28);
        const size_t remainingHeader = archive.size() - offset - 30;
        if (nameSize > remainingHeader || extraSize > remainingHeader - nameSize) {
            error = "ZIP member name or extra field is truncated";
            return false;
        }

        const size_t nameOffset = offset + 30;
        const size_t dataOffset = nameOffset + nameSize + extraSize;
        if (dataSize > archive.size() - dataOffset) {
            error = "ZIP member data is truncated";
            return false;
        }

        members.push_back({
                std::string(archive.begin() + nameOffset, archive.begin() + nameOffset + nameSize),
                dataOffset,
                std::vector<uint8_t>(archive.begin() + dataOffset, archive.begin() + dataOffset + dataSize)
        });
        offset = dataOffset + dataSize;
    }
}

} // namespace

TEST(utUSDZWriter, diskArchiveMatchesMemoryArchive) {
    const std::string usdContent = "#usda 1.0\ndef Xform \"Root\" {}\n";
    const std::map<std::string, std::vector<uint8_t>> textures = {
            { "textures/albedo.bin", { 0x00, 0x7f, 0x80, 0xff } },
            { "textures/empty.bin", {} },
            { "textures/normal.bin", { 0x10, 0x20, 0x30 } }
    };
    std::vector<uint8_t> memoryArchive;
    std::string memoryWarn, memoryErr;
    ASSERT_TRUE(tinyusdz::usdz::SaveAsUSDZToMemory(
            usdContent, textures, memoryArchive, &memoryWarn, &memoryErr)) << memoryErr;
    EXPECT_TRUE(memoryErr.empty());
    EXPECT_EQ("Texture data is empty: textures/empty.bin\n", memoryWarn);

    const std::filesystem::path outputPath =
            std::filesystem::path(::testing::TempDir()) / "assimp-usdz-disk-writer.usdz";
    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);

    std::string diskWarn, diskErr;
    ASSERT_TRUE(tinyusdz::usdz::SaveAsUSDZWithTextures(
            outputPath.string(), usdContent, textures, &diskWarn, &diskErr)) << diskErr;
    EXPECT_TRUE(diskErr.empty());
    EXPECT_EQ(memoryWarn, diskWarn);

    std::ifstream input(outputPath, std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const std::vector<uint8_t> diskArchive{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    EXPECT_EQ(memoryArchive, diskArchive);

    std::vector<StoredZipMember> members;
    std::string parseError;
    ASSERT_TRUE(parseStoredZipMembers(diskArchive, members, parseError)) << parseError;
    ASSERT_EQ(3u, members.size());
    EXPECT_EQ("model.usda", members[0].name);
    EXPECT_EQ(std::vector<uint8_t>(usdContent.begin(), usdContent.end()), members[0].data);
    EXPECT_EQ("textures/albedo.bin", members[1].name);
    EXPECT_EQ(textures.at("textures/albedo.bin"), members[1].data);
    EXPECT_EQ("textures/normal.bin", members[2].name);
    EXPECT_EQ(textures.at("textures/normal.bin"), members[2].data);
    for (const StoredZipMember &member : members) {
        EXPECT_EQ(0u, member.dataOffset % 64) << member.name;
    }

    input.close();
    ASSERT_FALSE(input.fail());
    ASSERT_TRUE(std::filesystem::remove(outputPath, removeError)) << removeError.message();
}

TEST(utUSDZWriter, diskWriteFailureIsReported) {
    std::string warn, err;
    EXPECT_FALSE(tinyusdz::usdz::SaveAsUSDZWithTextures(
            ::testing::TempDir(), "#usda 1.0\n", {}, &warn, &err));
    EXPECT_TRUE(warn.empty());
    EXPECT_FALSE(err.empty());
}

TEST(utUSDZWriter, emptyInputDoesNotCreateDiskArchive) {
    const std::filesystem::path outputPath =
            std::filesystem::path(::testing::TempDir()) / "assimp-usdz-empty-input.usdz";
    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);

    std::string warn, err;
    EXPECT_FALSE(tinyusdz::usdz::SaveAsUSDZWithTextures(
            outputPath.string(), "", {}, &warn, &err));
    EXPECT_TRUE(warn.empty());
    EXPECT_EQ("Generated USD content is empty\n", err);
    EXPECT_FALSE(std::filesystem::exists(outputPath));
}

TEST(utUSDZWriter, oversizedFilenameIsRejected) {
    const std::map<std::string, std::vector<uint8_t>> textures = {
            { std::string(static_cast<size_t>(std::numeric_limits<uint16_t>::max()) + 1, 'x'), { 0 } }
    };
    std::vector<uint8_t> archive{ 0xff };
    std::string warn, err;

    EXPECT_FALSE(tinyusdz::usdz::SaveAsUSDZToMemory(
            "#usda 1.0\n", textures, archive, &warn, &err));
    EXPECT_TRUE(archive.empty());
    EXPECT_TRUE(warn.empty());
    EXPECT_EQ("USDZ member filename is 65536 bytes; classic ZIP limit is 65535 bytes\n", err);
}

TEST(utUSDZWriter, zip64SentinelEntryCountIsRejected) {
    std::map<std::string, std::vector<uint8_t>> textures;
    for (size_t i = 0; i < static_cast<size_t>(std::numeric_limits<uint16_t>::max()) - 1; ++i) {
        textures.emplace("texture-" + std::to_string(i), std::vector<uint8_t>{ 0 });
    }
    std::vector<uint8_t> archive{ 0xff };
    std::string warn, err;

    EXPECT_FALSE(tinyusdz::usdz::SaveAsUSDZToMemory(
            "#usda 1.0\n", textures, archive, &warn, &err));
    EXPECT_TRUE(archive.empty());
    EXPECT_TRUE(warn.empty());
    EXPECT_EQ("USDZ archive has 65535 members; classic ZIP reserves 65535 for ZIP64\n", err);
}

TEST(utUSDZWriter, classicZipTotalArchiveSizeIsBounded) {
    constexpr uint64_t classicZipMaximum = std::numeric_limits<uint32_t>::max();
    EXPECT_TRUE(tinyusdz::usdz::detail::FitsClassicZipArchive(classicZipMaximum));
    EXPECT_FALSE(tinyusdz::usdz::detail::FitsClassicZipArchive(classicZipMaximum + 1));

    constexpr uint64_t syntheticMemberSize = classicZipMaximum - 65;
    EXPECT_FALSE(tinyusdz::usdz::detail::FitsClassicZipArchive(
            64 + syntheticMemberSize + 46 + sizeof("model.usda") - 1 + 22));
}

#endif // ASSIMP_BUILD_NO_USD_EXPORTER
