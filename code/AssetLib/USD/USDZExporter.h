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
#include <array>
#include <functional>

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
void ExportSceneUSDZ(const char* pFile, IOSystem* pIOSystem, const aiScene* pScene, const ExportProperties* pProperties);

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
    void ExportNodeHierarchy(const aiNode* node, tinyusdz::Prim* parentPrim = nullptr);
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

    // Mesh conversion helpers
    bool IsPointPrimitive(const aiMesh* mesh);
    void ConvertMesh(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh);
    bool NeedsSkeletalTreatment(const aiMesh* mesh);
    tinyusdz::Prim CreateSkelRootForMesh(const aiMesh* mesh, const std::string& meshName, tinyusdz::Prim&& meshPrim, const std::vector<std::string>& blendShapeNames = {}, const std::string& parentNodeName = "");
    
    // Forward declaration for BlendShapeResult
    struct BlendShapeResult;



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

    // Enhanced texture processing system
    struct TextureConfig {
        std::vector<aiTextureType> fallbackTypes;  // Fallback types to check in order
        std::string outputChannel = "rgb";         // Output channel (r, g, b, rgb, a) 
        std::string paramName;                     // USD parameter name
        std::array<float, 4> bias = {0.0f, 0.0f, 0.0f, 0.0f};     // Texture bias
        std::array<float, 4> scale = {1.0f, 1.0f, 1.0f, 1.0f};    // Texture scale
        bool requiresNonZeroFactor = false;       // Check material factor > 0
        std::string factorKey;                    // Material key for factor check
        
        TextureConfig(const std::string& name, const std::string& channel = "rgb") 
            : outputChannel(channel), paramName(name) {}
    };
    
    template<typename SurfacePropertyType>
    bool ProcessTextureProperty(const aiMaterial* mat, const TextureConfig& config, 
                               SurfacePropertyType& surfaceProperty, tinyusdz::UsdPreviewSurface& surface);
    
    // Advanced Shader Builder utility for consistent shader creation
    class ShaderBuilder {
    public:
        explicit ShaderBuilder(const std::string& materialPath) : mMaterialPath(materialPath) {}
        
        // Template method for creating and connecting shaders with move semantics
        template<typename ShaderType>
        tinyusdz::Shader CreateShader(const std::string& name, const std::string& infoId, ShaderType&& shaderValue);
        
        // Create surface shader with proper connections
        tinyusdz::Shader CreateSurfaceShader(tinyusdz::UsdPreviewSurface&& surface);
        
        // Create material with surface connections
        tinyusdz::Material CreateMaterial(const std::string& name, const std::string& surfaceShaderName);
        
    private:
        std::string mMaterialPath;
    };
    
    // Thread-safe NameRegistry for efficient unique name generation
    class NameRegistry {
    public:
        NameRegistry() = default;
        
        // Generate unique name with efficient lookup and minimal allocations
        std::string GenerateUnique(const std::string& baseName) {
            if (baseName.empty()) {
                return GenerateUnique("Unnamed");
            }
            
            // Use operator[] for efficient insertion/lookup
            auto& counter = mCounters[baseName];
            if (counter == 0) {
                ++counter;
                return baseName; // First use, no suffix needed
            }
            
            // Generate suffixed name with minimal string operations
            std::string uniqueName;
            uniqueName.reserve(baseName.size() + 10); // Reserve space for suffix
            uniqueName = baseName + "_" + std::to_string(counter);
            ++counter;
            
            return uniqueName;
        }
        
        // Sanitize name with comprehensive USD compliance
        static std::string Sanitize(const std::string& name) {
            if (name.empty()) return "Unnamed";
            
            std::string sanitized;
            sanitized.reserve(name.size()); // Pre-allocate for performance
            
            // First character must be letter or underscore
            if (std::isalpha(name[0]) || name[0] == '_') {
                sanitized += name[0];
            } else {
                sanitized += '_';
            }
            
            // Subsequent characters can be alphanumeric or underscore
            for (size_t i = 1; i < name.size(); ++i) {
                char c = name[i];
                if (std::isalnum(c) || c == '_') {
                    sanitized += c;
                } else {
                    sanitized += '_';
                }
            }
            
            return sanitized;
        }
        
        // Combined sanitize and unique generation for common workflow
        std::string SanitizeAndGenerateUnique(const std::string& name) {
            return GenerateUnique(Sanitize(name));
        }
        
        void Clear() { mCounters.clear(); }
        
    private:
        std::map<std::string, uint32_t> mCounters;
    };
    
    // RAII-based Mesh Converter Pipeline for efficient and safe mesh processing
        class MeshConverterPipeline {
    public:
        using BoneNameConverter = std::function<std::string(const std::string&)>;
        
        explicit MeshConverterPipeline(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh, const aiScene* scene, 
                                     NameRegistry& nameRegistry, tinyusdz::Stage& stage)
            : mMesh(mesh), mUsdMesh(usdMesh), mScene(scene), mNameRegistry(nameRegistry), mStage(stage), mValid(mesh != nullptr) {
            // Suppress unused parameter warnings - kept for future extensibility
            (void)mStage;
            (void)mNameRegistry;
        }
        
        // Pipeline methods with fluent interface and RAII guarantees
        MeshConverterPipeline& ConvertVertices() { 
            if (mValid && mMesh->mVertices) ExecuteVertexConversion(); 
            return *this; 
        }
        
        MeshConverterPipeline& ConvertFaces() { 
            if (mValid && mMesh->mFaces) ExecuteFaceConversion(); 
            return *this; 
        }
        
        MeshConverterPipeline& ConvertNormals() { 
            if (mValid && mMesh->mNormals) ExecuteNormalConversion(); 
            return *this; 
        }
        
        MeshConverterPipeline& ConvertUVs() { 
            if (mValid && mMesh->mNumUVComponents[0] > 0) ExecuteUVConversion(); 
            return *this; 
        }
        
        MeshConverterPipeline& ConvertVertexColors() { 
            if (mValid && mMesh->mColors[0]) ExecuteVertexColorConversion(); 
            return *this; 
        }
        
        MeshConverterPipeline& ConvertTangents() { 
            if (mValid && mMesh->mTangents) ExecuteTangentConversion(); 
            return *this; 
        }
        
        MeshConverterPipeline& ConvertSkinning() { 
            if (mValid && mMesh->mNumBones > 0) ExecuteSkinningConversion(); 
            return *this; 
        }
        
        MeshConverterPipeline& ConvertBlendShapes() { 
            if (mValid && mMesh->mNumAnimMeshes > 0) ExecuteBlendShapeConversion(); 
            return *this; 
        }
        
        MeshConverterPipeline& ConvertAttributes() { 
            if (mValid) ExecuteAttributeConversion(); 
            return *this; 
        }
        
        // Execute complete pipeline with single method call
        MeshConverterPipeline& ExecuteFullPipeline(BoneNameConverter boneNameConverter = nullptr) {
            if (!mValid) return *this;
            
            ExecuteVertexConversion();
            ExecuteFaceConversion();
            ExecuteNormalConversion();
            ExecuteUVConversion();
            ExecuteVertexColorConversion();
            ExecuteTangentConversion();
            ExecuteSkinningConversion(boneNameConverter);
            ExecuteBlendShapeConversion();
            ExecuteAttributeConversion();
            
            return *this;
        }
        
        bool IsValid() const { return mValid; }
        
        // Note: Blend shapes are now created as separate root-level prims, no longer returned from pipeline
        
        // Get complete mesh prim with blend shapes and skeletal properties
        BlendShapeResult GetCompleteMeshWithBlendShapes(tinyusdz::Prim&& baseMeshPrim);
        
        // Get blend shape names for SkelRoot creation
        const std::vector<std::string>& GetBlendShapeNames() const { return mBlendShapeNames; }
        
        // Get blend shape prims to add as mesh children
        const std::vector<std::unique_ptr<tinyusdz::Prim>>& GetBlendShapePrims() const { return mBlendShapePrims; }
        
        // Public access to specific conversion steps for special cases
        void ExecuteAttributeConversion();
        
    private:
        const aiMesh* mMesh;
        tinyusdz::GeomMesh& mUsdMesh;
        const aiScene* mScene;
        NameRegistry& mNameRegistry;
        tinyusdz::Stage& mStage;
        bool mValid;
        // Store blend shapes to be added as children of the mesh prim
        std::vector<std::string> mBlendShapeNames;  // Store blend shape names for SkelRoot creation
        std::vector<std::unique_ptr<tinyusdz::Prim>> mBlendShapePrims;  // Store blend shape prims to add as mesh children
        
        // Forward declarations for implementation methods
        void ExecuteVertexConversion();
        void ExecuteFaceConversion();
        void ExecuteNormalConversion();
        void ExecuteUVConversion();
        void ExecuteVertexColorConversion();
        void ExecuteTangentConversion();
        void ExecuteSkinningConversion(BoneNameConverter boneNameConverter = nullptr);
        void ExecuteBlendShapeConversion();
    };
    
    // PrimFactory for consistent USD prim creation and hierarchy management
    class PrimFactory {
    public:
        // Create prim with optimal construction and move semantics
        template<typename PrimType>
        static tinyusdz::Prim CreatePrim(PrimType&& primData);
        
        // Create and add child prim to parent with proper hierarchy management
        template<typename PrimType>
        static void AddChildPrim(tinyusdz::Prim& parent, PrimType&& primData);
        
        // Create scope prim for organizing related objects
        static tinyusdz::Prim CreateScope(const std::string& name);
        
        // Create transform prim with identity transform
        static tinyusdz::Prim CreateXform(const std::string& name);
        
        // Batch create and add multiple children efficiently
        template<typename... PrimTypes>
        static void AddChildren(tinyusdz::Prim& parent, PrimTypes&&... prims);
    };
    
    // Shader creation helpers (Apple's NodeGraph pattern implementation)
    tinyusdz::Shader CreateTexCoordReader(const std::string& varName = "st");
    tinyusdz::Shader CreateStTransform(const std::string& inputConnection, bool flipY = true);
    void AddSourceColorSpace(tinyusdz::UsdUVTexture& uvTexture, const std::string& textureType);
    void AddTextureOutputs(tinyusdz::UsdUVTexture& uvTexture, const std::string& textureType);

    // Animation conversion helpers
    void ConvertAnimation(const aiAnimation* anim);
    void ConvertSkeletalAnimation(const aiAnimation* anim);
    void UpdateSkelAnimationWithMorphData(const aiMeshMorphAnim* morphAnim, const std::string& meshName, 
                                         double timeScale, const char* animationName);
    void ConvertSkinning(const aiMesh* mesh);
    void ConvertSkinningToMesh(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh);
    void ConvertBlendShapes(const aiMesh* mesh);

    // Camera/Light conversion helpers
    void ConvertCamera(const aiCamera* camera);
    void ConvertLight(const aiLight* light);

    // Node hierarchy helpers
    tinyusdz::Xform* ConvertNode(const aiNode* node, tinyusdz::Prim* parentPrim = nullptr);
    void SetupNodeTransform(const aiNode* node, tinyusdz::Xform& xform);

    // Texture helpers
    tinyusdz::UsdUVTexture CreateUVTexture(const std::string& filePath, const std::string& paramName);
    
    // Utility methods
    std::string SanitizeName(const std::string& name) const;
    std::string SanitizeFilename(const std::string& filename) const;
    std::string GenerateUniqueName(const std::string& baseName) const;
    std::string GetSceneName() const;
    bool IsEmbeddedTexture(const std::string& texPath) const;
    
    // File output methods
    void SaveAsUSDA(const std::string& filename);
    void SaveAsUSDC(const std::string& filename);
    void SaveAsUSDZ(const std::string& filename);
    // Note: Texture writing now handled by tinyusdz::usdz::SaveAsUSDZ()
    
    // USDZ specific methods
    std::string GenerateUSDContent();
    void CollectTextureDataForUSDZ(std::map<std::string, std::vector<uint8_t>>& textureDataMap);
    bool ConvertRawTextureToPNG(const aiTexture* texture, std::vector<uint8_t>& pngData);

    // Error handling
    void ReportError(const std::string& message);
    void ReportWarning(const std::string& message);
    
    // Directory utilities
    bool CreateTexturesDirectory(const std::string& dirPath);
    
    // Texture naming utilities  
    std::string GenerateDescriptiveTextureName(int textureIndex, const std::string& baseTextureName);

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

    // Advanced name management system
    mutable NameRegistry mNameRegistry;
    
    // Skeletal animation mapping: bone name → hierarchical USD path
    // Critical for ensuring mesh skel:joints references match skeleton joint paths exactly
    std::map<std::string, std::string> mBoneNameToUSDPath;

    // Texture processing helpers (for current material being processed)
    std::string mCurrentMaterialPath;
    std::vector<std::pair<std::string, tinyusdz::UsdUVTexture>> mCurrentMaterialTextureShaders;
    
    // Note: Texture handling is now done by tinyusdz::usdz::SaveAsUSDZ() which extracts
    // texture dependencies directly from the Stage and packages them automatically

    // Note: Texture writing is now handled by tinyusdz::usdz::SaveAsUSDZ()

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
