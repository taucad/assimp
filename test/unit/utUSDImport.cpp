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
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>

using namespace ::Assimp;

class utUSDImport : public AbstractImportExportBase {
};

TEST_F(utUSDImport, meshTest) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/../models-nonbsd/USD/usdc/suzanne.usdc", aiProcess_ValidateDataStructure);
    EXPECT_NE(nullptr, scene);
    EXPECT_EQ(1u, scene->mNumMeshes);
    EXPECT_NE(nullptr, scene->mMeshes[0]);
    EXPECT_EQ(1968u, scene->mMeshes[0]->mNumVertices); // Note: suzanne is authored with only 507 vertices, but TinyUSDZ rebuilds the vertex array. see https://github.com/lighttransport/tinyusdz/blob/36f2aabb256b360365989c01a52f839a57dfe2a6/src/tydra/render-data.cc#L2673-L2690 
    EXPECT_EQ(968u, scene->mMeshes[0]->mNumFaces);
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
    
    // Check that we have the correct number of morph channels (2 blend shapes: "thin", "angle")
    EXPECT_EQ(2u, animation->mNumMorphMeshChannels);
    
    // Verify morph channel names
    bool foundThin = false, foundAngle = false;
    for (unsigned int i = 0; i < animation->mNumMorphMeshChannels; ++i) {
        const aiMeshMorphAnim* morphAnim = animation->mMorphMeshChannels[i];
        EXPECT_NE(nullptr, morphAnim);
        
        std::string meshName = morphAnim->mName.C_Str();
        if (meshName.find("thin") != std::string::npos) {
            foundThin = true;
        }
        if (meshName.find("angle") != std::string::npos) {
            foundAngle = true;
        }
        
        // Should have time samples (101 frames)
        EXPECT_GT(morphAnim->mNumKeys, 50u);  // At least 50+ keyframes
    }
    
    EXPECT_TRUE(foundThin) << "Should find 'thin' blend shape animation";
    EXPECT_TRUE(foundAngle) << "Should find 'angle' blend shape animation";
    
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
        EXPECT_NEAR(318.30988f, sphereLight->mAttenuationConstant, 1.0f);  // intensity from USD
        // Position should be set from the Xform parent: (4.076245, 1.005454, 5.903862)
        EXPECT_NEAR(4.076245f, sphereLight->mPosition.x, 0.01f);
        EXPECT_NEAR(1.005454f, sphereLight->mPosition.y, 0.01f);
        EXPECT_NEAR(5.903862f, sphereLight->mPosition.z, 0.01f);
    }
    
    // Verify DomeLight properties  
    EXPECT_NE(nullptr, domeLight);
    if (domeLight) {
        EXPECT_EQ(aiLightSource_AMBIENT, domeLight->mType);  // Dome lights are typically ambient
        EXPECT_NEAR(1.0f, domeLight->mAttenuationConstant, 0.01f);  // intensity = 1
    }
}

// Note: Add multi-animation test once supported by USD
// See https://github.com/lighttransport/tinyusdz/issues/122 for details.
