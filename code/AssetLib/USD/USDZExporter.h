/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2024, assimp team

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

/** @file USDZExporter.h
 * @brief Declares the exporter class to write USDZ files using tinyusdz library
 */

#pragma once
#ifndef AI_USDZEXPORTER_H_INC
#define AI_USDZEXPORTER_H_INC

#ifndef ASSIMP_BUILD_NO_USD_EXPORTER

#include <assimp/IOSystem.hpp>
#include <assimp/scene.h>
#include <assimp/Exceptional.h>
#include <assimp/defs.h>

#include <memory>
#include <map>
#include <vector>
#include <string>

struct aiScene;
struct aiNode;
struct aiMesh;
struct aiMaterial; 
struct aiAnimation;
struct aiCamera;
struct aiLight;

// Forward declarations for tinyusdz types to avoid including full headers
namespace tinyusdz {
    class Stage;
    class Prim;
    struct GeomMesh;
    struct Material;
    struct Shader;
    struct UsdPreviewSurface;
    struct UsdUVTexture;
    struct Xform;
    struct GeomCamera;
    struct DistantLight;
    struct SphereLight;
    struct RectLight;
    struct SkelRoot;
    struct Skeleton;
    struct SkelAnimation;
    struct BlendShape;
}

namespace Assimp {

class ExportProperties;
class IOSystem;

// Export function declarations
void ExportSceneUSDA(const char* pFile, IOSystem* pIOSystem, const aiScene* pScene, const ExportProperties* pProperties);

/**
 * @brief USDZ Exporter class using tinyusdz library
 * 
 * Exports Assimp scenes to USD formats (USDA) with comprehensive support for:
 * - PBR materials with texture networks
 * - Animations and skeletal animation 
 * - Skinning and blend shapes
 * - Cameras and lights
 * - AR anchoring for iOS Quick Look
 */
class ASSIMP_API USDZExporter {
public:
    /// Constructor
    USDZExporter(const char* filename, IOSystem* pIOSystem, const aiScene* pScene,
                 const ExportProperties* pProperties, bool isPackaged = false);
    
    /// Destructor  
    ~USDZExporter();

    /// Main export method  
    void ExportScene(const aiScene* scene, IOSystem* pIOSystem, const std::string& file, const ExportProperties* pProperties);
    void ExportScene();

private:
    // Core export methods
    void ExportMetadata();
    void ExportSceneStructure();
    void ExportNodeHierarchy(const aiNode* node);
    void ExportMeshes();
    void ExportMaterials();
    void ExportTextures();
    void ExportSkeletons();
    void ExportAnimations();
    void ExportCameras();
    void ExportLights();

    // Advanced feature export methods
    void ExportARAnchoring();
    void ExportQuickLookMetadata();
    void ExportMaterialX();
    void ExportSubdivisionSurfaces();
    void ExportVolumeRendering();
    void AddARAnchoring();

    // Mesh conversion helpers
    void ConvertMesh(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh);
    void ConvertVertices(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh);
    void ConvertFaces(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh);
    void ConvertNormals(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh);
    void ConvertUVs(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh);
    void ConvertVertexColors(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh);
    void ConvertTangents(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh);

    // Material conversion helpers
    void ConvertMaterial(const aiMaterial* mat, tinyusdz::Material& usdMaterial, 
                        tinyusdz::UsdPreviewSurface& surface);
    void ConvertMaterial(const aiMaterial* mat, tinyusdz::Material& usdMaterial);
    void CreatePreviewSurface(const aiMaterial* mat, tinyusdz::UsdPreviewSurface& surface);
    void MapPBRProperties(const aiMaterial* mat, tinyusdz::UsdPreviewSurface& surface);
    void MapClearcoatProperties(const aiMaterial* mat, tinyusdz::UsdPreviewSurface& surface);
    void MapTextureProperties(const aiMaterial* mat, tinyusdz::UsdPreviewSurface& surface);

    // Texture conversion helpers
    void ConvertTexture(const aiMaterial* mat, aiTextureType type, 
                       tinyusdz::UsdUVTexture& uvTexture);
    void HandleEmbeddedTexture(const std::string& texPath, tinyusdz::UsdUVTexture& uvTexture);
    void HandleExternalTexture(const std::string& texPath, tinyusdz::UsdUVTexture& uvTexture);

    // Animation conversion helpers
    void ConvertAnimation(const aiAnimation* anim);
    void ConvertSkeletalAnimation(const aiAnimation* anim);
    void ConvertSkinning(const aiMesh* mesh);
    void ConvertSkinningToMesh(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh);
    void ConvertBlendShapes(const aiMesh* mesh);
    void ConvertBlendShapesToMesh(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh);

    // Camera/Light conversion helpers
    void ConvertCamera(const aiCamera* camera);
    void ConvertLight(const aiLight* light);

    // Node hierarchy helpers
    tinyusdz::Xform* ConvertNode(const aiNode* node);
    void SetupNodeTransform(const aiNode* node, tinyusdz::Xform& xform);

    // Texture helpers
    tinyusdz::UsdUVTexture CreateUVTexture(const std::string& filePath, const std::string& paramName);
    
    // Utility methods
    std::string SanitizeName(const std::string& name) const;
    std::string SanitizeFilename(const std::string& filename) const;
    std::string GenerateUniqueName(const std::string& baseName) const;
    bool IsEmbeddedTexture(const std::string& texPath) const;
    
    // File output methods
    void SaveAsUSDA(const std::string& filename);
    void SaveAsUSDC(const std::string& filename);
    void SaveAsUSDZ(const std::string& filename);
    void WriteTextureFilesAlongsideMainFile(const std::string& mainFilename);

    // Error handling
    void ReportError(const std::string& message);
    void ReportWarning(const std::string& message);

    // Core members
    std::unique_ptr<tinyusdz::Stage> mStage;
    const aiScene* mScene;
    const ExportProperties* mProperties;
    std::string mFilename;
    IOSystem* mIOSystem;
    bool mIsPackaged;

    // Export control flags
    bool mExportAnimations;
    bool mExportCameras; 
    bool mExportLights;
    bool mExportMaterials;
    bool mExportTextures;
    bool mExportClearcoat;
    bool mExportARAnchoring;
    bool mExportMaterialX;
    bool mExportSubdivision;
    bool mExportVolumes;
    bool mOptimizeForMobile;

    // Mapping tables for object relationships
    std::map<const aiMaterial*, std::string> mMaterialIdMap;
    std::map<const aiMesh*, std::string> mMeshIdMap;
    std::map<const aiNode*, std::string> mNodeIdMap;
    std::map<const aiCamera*, std::string> mCameraIdMap;
    std::map<const aiLight*, std::string> mLightIdMap;

    // Name tracking for uniqueness
    mutable std::map<std::string, uint32_t> mNameCounters;

    // Texture processing helpers (for current material being processed)
    std::string mCurrentMaterialPath;
    std::vector<std::pair<std::string, tinyusdz::UsdUVTexture>> mCurrentMaterialTextureShaders;
    
    // Texture files to write alongside USDA (following glTF2 pattern)
    // Handles both embedded textures (from aiScene->mTextures) and external textures (loaded into memory)
    struct TextureToWrite {
        std::string originalPath;
        std::string sanitizedFilename;
        const aiTexture* embeddedTexture; // For textures from aiScene->mTextures
        std::vector<uint8_t> externalTextureData; // For external textures loaded into memory
        bool isEmbedded; // true if from aiScene->mTextures, false if loaded from external file
    };
    std::vector<TextureToWrite> mTexturesToWrite;

    // Texture writing helper methods (private - only used internally)
    void WriteEmbeddedTextureToFile(const aiTexture* texture, const std::string& outputPath);
    void WriteExternalTextureFromMemory(const TextureToWrite& textureToWrite, const std::string& outputPath);

    // Helper methods
    std::string ai_to_string(uint32_t value) const;
    std::string GetFileExtension(const std::string& filename) const;

    // Error and warning storage
    std::vector<std::string> mErrors;
    std::vector<std::string> mWarnings;
};

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_USD_EXPORTER

#endif // AI_USDZEXPORTER_H_INC
