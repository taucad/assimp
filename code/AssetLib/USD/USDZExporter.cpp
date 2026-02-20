/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2025, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

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

----------------------------------------------------------------------
*/
#ifndef ASSIMP_BUILD_NO_EXPORT
#ifndef ASSIMP_BUILD_NO_USD_EXPORTER

#include "USDZExporter.h"
#include "usdz-writer.hh"

// BlendShapeResult definition
struct Assimp::USDZExporter::BlendShapeResult {
    tinyusdz::Prim meshPrim;
    std::vector<std::string> blendShapeNames;
    
    BlendShapeResult(tinyusdz::Prim&& prim, std::vector<std::string>&& names) 
        : meshPrim(std::move(prim)), blendShapeNames(std::move(names)) {}
};

// Assimp includes
#include <assimp/Exceptional.h>
#include <assimp/IOSystem.hpp>
#include <assimp/scene.h>
#include <assimp/StringUtils.h>

// Math includes for animation calculations
#include <cmath>
#include <assimp/DefaultLogger.hpp>
#include <assimp/ai_assert.h>
#include <assimp/StringComparison.h>
#include <assimp/CreateAnimMesh.h>
#include <assimp/Exporter.hpp>
#include <assimp/GltfMaterial.h>

// Standard library
#include <algorithm>
#include <cmath>
#include <set>

// tinyusdz includes
#include "../../../contrib/tinyusdz/autoclone/tinyusdz_repo-src/src/usda-writer.hh"
#include "../../../contrib/tinyusdz/autoclone/tinyusdz_repo-src/src/usdc-writer.hh"
#include "../../../contrib/tinyusdz/autoclone/tinyusdz_repo-src/src/usdGeom.hh"
#include "../../../contrib/tinyusdz/autoclone/tinyusdz_repo-src/src/usdShade.hh"
#include "../../../contrib/tinyusdz/autoclone/tinyusdz_repo-src/src/usdSkel.hh"
#include "../../../contrib/tinyusdz/autoclone/tinyusdz_repo-src/src/usdLux.hh"
#include "../../../contrib/tinyusdz/autoclone/tinyusdz_repo-src/src/stage.hh"
#include "../../../contrib/tinyusdz/autoclone/tinyusdz_repo-src/src/path-util.hh"

using namespace Assimp;

namespace {
    [[maybe_unused]] static const char* TAG = "USDZExporter";
    
    // Export configuration keys
    static const char* PROP_EXPORT_ANIMATIONS = "USDZ_EXPORT_ANIMATIONS";
    static const char* PROP_EXPORT_CLEARCOAT = "USDZ_EXPORT_CLEARCOAT";
    static const char* PROP_EXPORT_MATERIALX = "USDZ_EXPORT_MATERIALX";
    static const char* PROP_EXPORT_SUBDIVISION = "USDZ_EXPORT_SUBDIVISION";
    static const char* PROP_EXPORT_VOLUMES = "USDZ_EXPORT_VOLUMES";
    static const char* PROP_OPTIMIZE_FOR_MOBILE = "USDZ_OPTIMIZE_FOR_MOBILE";
}

// ------------------------------------------------------------------------------------------------
// Constructor
USDZExporter::USDZExporter(const char* filename, IOSystem* pIOSystem, const aiScene* pScene,
                           const ExportProperties* pProperties, bool isPackaged) :
    mStage(std::make_unique<tinyusdz::Stage>()),
    mScene(pScene),
    mProperties(pProperties),
    mFilename(filename),
    mIOSystem(pIOSystem),
    mIsPackaged(isPackaged),
    mExportAnimations(true),
    mExportClearcoat(true),
    mExportMaterialX(false),
    mExportSubdivision(false),
    mExportVolumes(false),
    mOptimizeForMobile(true) {

    // Parse export properties
    if (mProperties) {
        mExportAnimations = mProperties->GetPropertyBool(PROP_EXPORT_ANIMATIONS, true);
        mExportClearcoat = mProperties->GetPropertyBool(PROP_EXPORT_CLEARCOAT, true);
        mExportMaterialX = mProperties->GetPropertyBool(PROP_EXPORT_MATERIALX, false);
        mExportSubdivision = mProperties->GetPropertyBool(PROP_EXPORT_SUBDIVISION, false);
        mExportVolumes = mProperties->GetPropertyBool(PROP_EXPORT_VOLUMES, false);
        mOptimizeForMobile = mProperties->GetPropertyBool(PROP_OPTIMIZE_FOR_MOBILE, true);
    }

    if (!pScene) {
        throw DeadlyExportError("USDZExporter: No scene provided");
    }

    if (!mStage) {
        throw DeadlyExportError("USDZExporter: Failed to create USD Stage");
    }

    try {
        // Execute the export pipeline
        BuildEmbeddedTextureFilenameMap();
        ExportMetadata();
        ExportScene();  // Create root prim FIRST
        
        // Get reference to root prim for node hierarchy
        tinyusdz::Prim* rootPrim = nullptr;
        if (!mStage->root_prims().empty()) {
            rootPrim = &mStage->root_prims()[0];  // Our root prim
        }
        
        if (mScene->mRootNode && rootPrim) {
            // Initialize bone discriminator early to ensure bone nodes are skipped during hierarchy export
            InitializeBoneDiscriminator();
            
            ExportNodeHierarchy(mScene->mRootNode, rootPrim);  // Pass root prim as parent
        }
        
        // Pre-scan meshes to identify materials used by vertex-colored meshes
        for (uint32_t i = 0; i < mScene->mNumMeshes; ++i) {
            const aiMesh* mesh = mScene->mMeshes[i];
            if (mesh->mColors[0] && mesh->mMaterialIndex < mScene->mNumMaterials) {
                mMaterialsWithVertexColors.insert(mesh->mMaterialIndex);
            }
        }

        ExportMaterials();
        ExportSkeletons();
        
        if (mExportAnimations) {
            ExportAnimations();
        }
        
        ExportMeshes();
        
        // Complete SkelRoot with animation relationships and GeomScope after meshes are exported
        if (mExportAnimations) {
            // Check if we have skeletal data and complete SkelRoot
            bool hasSkeletalMeshes = false;
            for (uint32_t i = 0; i < mScene->mNumMeshes; ++i) {
                if (mScene->mMeshes[i]->HasBones()) {
                    hasSkeletalMeshes = true;
                    break;
                }
            }
            
            if (hasSkeletalMeshes) {
                // Complete SkelRoot by moving skeletal meshes from root to GeomScope
                CompleteSkelRootWithAnimation();
                ASSIMP_LOG_DEBUG("USDZExporter: Completed SkelRoot with skeletal meshes moved to GeomScope");
            }
        }
        
        ExportTextures();
        
        ExportCameras();
        ExportLights();

        if (mExportMaterialX) {
            ExportMaterialX();
        }
        
        if (mExportSubdivision) {
            ExportSubdivisionSurfaces();
        }
        
        if (mExportVolumes) {
            ExportVolumeRendering();
        }
        
        // Ensure no sibling prims share names (fixes OpenUSD "Duplicate prim" errors)
        DeduplicateSiblingPrimNames();
        
        // Re-apply animation time codes after all prims are created
        // (ensures stage metadata reflects frame-based convention even for morph-only models)
        if (mExportAnimations && mScene->mNumAnimations > 0) {
            auto& finalMeta = mStage->metas();
            double maxDuration = 0.0;
            for (uint32_t i = 0; i < mScene->mNumAnimations; ++i) {
                const aiAnimation* anim = mScene->mAnimations[i];
                double duration = anim->mDuration / (anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 24.0);
                maxDuration = std::max(maxDuration, duration);
            }
            if (maxDuration > 0.0) {
                double fr = 24.0;
                int frames = static_cast<int>(std::round(maxDuration * fr));
                if (frames < 1) frames = 1;
                finalMeta.timeCodesPerSecond.set_value(fr);
                finalMeta.startTimeCode.set_value(1.0);
                finalMeta.endTimeCode.set_value(static_cast<double>(frames));
            }
        }
        
        // Save the file
        if (mIsPackaged) {
            SaveAsUSDZ(filename);
        } else {
            std::string ext = GetFileExtension(filename);
            if (ext == "usda") {
                SaveAsUSDA(filename);
            } else {
                SaveAsUSDC(filename);
            }
        }

    } catch (const std::exception& e) {
        ReportError(std::string("Export failed: ") + e.what());
        throw DeadlyImportError("USDZExporter: ", e.what());
    }
}

// ------------------------------------------------------------------------------------------------
// Destructor
USDZExporter::~USDZExporter() = default;

// ------------------------------------------------------------------------------------------------
// Export metadata
void USDZExporter::ExportMetadata() {
    static int metadataCallCount = 0;
    metadataCallCount++;
    ASSIMP_LOG_DEBUG("USDZExporter: ExportMetadata() call #" + std::to_string(metadataCallCount));
    
    auto& stageMeta = mStage->metas();
    
    // Basic USD metadata  
    stageMeta.metersPerUnit = 1.0; // Default to meters for realistic AR scaling
    stageMeta.upAxis = tinyusdz::Axis::Y; // Y-up coordinate system for AR
    
    // Add generator information to customLayerData
    stageMeta.customLayerData["generator"] = tinyusdz::MetaVariable(std::string("Assimp"));
    
    // Set defaultPrim to root scene node (will be set after scene structure is created)
    // Single root prim containing the entire scene
    
    if (mExportAnimations && mScene->mNumAnimations > 0) {
        // Find the highest frame count across all animations
        double maxDuration = 0.0;
        for (uint32_t i = 0; i < mScene->mNumAnimations; ++i) {
            const aiAnimation* anim = mScene->mAnimations[i];
            double duration = anim->mDuration / (anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 24.0);
            maxDuration = std::max(maxDuration, duration);
        }
        
        if (maxDuration > 0.0) {
            double frameRate = 24.0;
            int totalFrames = static_cast<int>(std::round(maxDuration * frameRate));
            if (totalFrames < 1) totalFrames = 1;
            
            stageMeta.startTimeCode.set_value(1.0);
            stageMeta.endTimeCode.set_value(static_cast<double>(totalFrames));
            stageMeta.timeCodesPerSecond.set_value(frameRate);
        }
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Metadata exported successfully - final check");
    ASSIMP_LOG_DEBUG("USDZExporter: Final metadata check completed");
}

// ------------------------------------------------------------------------------------------------
// Export scene structure
void USDZExporter::ExportScene() {
    // Create root scene prim
    std::string rootPrimName = GetSceneName();
    
    // Register root prim name with NameRegistry so that child nodes with
    // the same name get unique suffixes (prevents mesh placement bug where
    // meshes attach to the root prim instead of the child Xform node)
    GenerateUniqueName(rootPrimName);
    
    tinyusdz::Xform rootXform;
    rootXform.name = rootPrimName;
    
    // Create root prim and add to stage
    tinyusdz::Prim rootPrim(rootXform);
    mStage->root_prims().emplace_back(std::move(rootPrim));
    
    // Set defaultPrim to our root scene prim
    auto& stageMeta = mStage->metas();
    stageMeta.defaultPrim = tinyusdz::value::token(rootPrimName);
    
    ASSIMP_LOG_DEBUG("USDZExporter: Scene structure exported successfully - root prim: " + rootPrimName);
}

// ------------------------------------------------------------------------------------------------
// Helper function to find main scene prim
tinyusdz::Prim* USDZExporter::FindMainScenePrim() {
    std::string sceneName = GetSceneName();
    for (auto& prim : mStage->root_prims()) {
        if (prim.element_name() == sceneName) {
            return &prim;
        }
    }
    return nullptr;
}

// ------------------------------------------------------------------------------------------------  
// Helper: compute the full USD path to a prim by DFS
static bool ComputePrimPathDFS(const tinyusdz::Prim& prim, const tinyusdz::Prim* target,
                               const std::string& currentPath, std::string& outPath) {
    std::string path = currentPath + "/" + prim.element_name();
    if (&prim == target) {
        outPath = path;
        return true;
    }
    for (const auto& child : prim.children()) {
        if (ComputePrimPathDFS(child, target, path, outPath)) return true;
    }
    return false;
}

std::string USDZExporter::ComputePrimPath(const tinyusdz::Prim* target) {
    if (!target) return "";
    std::string result;
    for (const auto& root : mStage->root_prims()) {
        if (ComputePrimPathDFS(root, target, "", result)) return result;
    }
    return "";
}

// Generic: find the parent prim of the skeleton root bone(s) in the exported USD stage.
// This replaces the old hardcoded FindArmaturePrim that only worked for Z_UP/Armature hierarchies.
tinyusdz::Prim* USDZExporter::FindSkeletonParentPrim() {
    if (!mBoneNamesInitialized) {
        InitializeBoneDiscriminator();
    }
    if (mBoneNames.empty()) return nullptr;

    // Walk up from any bone node to find the first non-bone ancestor
    const aiNode* skeletonParentNode = nullptr;
    for (const auto& boneName : mBoneNames) {
        const aiNode* boneNode = FindNodeByName(mScene->mRootNode, boneName);
        if (!boneNode) continue;

        const aiNode* node = boneNode;
        while (node->mParent) {
            std::string parentName = node->mParent->mName.C_Str();
            if (mBoneNames.find(parentName) == mBoneNames.end()) {
                skeletonParentNode = node->mParent;
                break;
            }
            node = node->mParent;
        }
        if (skeletonParentNode) break;
    }

    if (!skeletonParentNode) {
        ASSIMP_LOG_WARN("USDZExporter: Could not find skeleton parent node, falling back to main scene prim");
        return FindMainScenePrim();
    }

    std::string targetName = SanitizeName(skeletonParentNode->mName.C_Str());
    ASSIMP_LOG_DEBUG("USDZExporter: Skeleton parent node is '" + targetName + "'");

    // Search the exported prim tree for this name
    std::function<tinyusdz::Prim*(tinyusdz::Prim&)> findPrim;
    findPrim = [&](tinyusdz::Prim& prim) -> tinyusdz::Prim* {
        if (prim.element_name() == targetName) return &prim;
        for (auto& child : prim.children()) {
            auto* found = findPrim(child);
            if (found) return found;
        }
        return nullptr;
    };

    for (auto& rootPrim : mStage->root_prims()) {
        auto* found = findPrim(rootPrim);
        if (found) return found;
    }

    ASSIMP_LOG_WARN("USDZExporter: Skeleton parent prim '" + targetName + "' not found in USD stage, falling back to main scene prim");
    return FindMainScenePrim();
}

// ------------------------------------------------------------------------------------------------
// Export node hierarchy  
void USDZExporter::ExportNodeHierarchy(const aiNode* node, tinyusdz::Prim* parentPrim) {
    if (!node) return;

    // Skip bone nodes handled by centralized skeleton structure
    std::string nodeName = node->mName.C_Str();
    if (ShouldSkipBoneNode(nodeName)) {
        ASSIMP_LOG_DEBUG("USDZExporter: Skipping bone node '" + nodeName + "' - handled by centralized skeleton structure");
        
        // Still process children in case there are non-bone nodes under bones
        for (uint32_t i = 0; i < node->mNumChildren; ++i) {
            ExportNodeHierarchy(node->mChildren[i], parentPrim);
        }
        return;
    }
    
    // Check if this node only contains skeletal meshes - if so, skip it since skeletal meshes are handled by SkelRoot
    if (NodeOnlyContainsSkeletalMeshes(node)) {
        ASSIMP_LOG_DEBUG("USDZExporter: Skipping node '" + nodeName + "' - contains only skeletal meshes handled by SkelRoot system");
        
        // Still process children in case there are non-skeletal nodes under this node
        for (uint32_t i = 0; i < node->mNumChildren; ++i) {
            ExportNodeHierarchy(node->mChildren[i], parentPrim);
        }
        return;
    }

    tinyusdz::Xform* xform = ConvertNode(node, parentPrim);
    if (!xform) {
        ReportWarning("Failed to convert node: " + std::string(node->mName.C_Str()));
        return;
    }

    // Find the USD prim we just created to pass as parent to children
    tinyusdz::Prim* currentPrim = nullptr;
    if (parentPrim) {
        // Find the newly created child prim in the parent's children
        for (auto& child : parentPrim->children()) {
            if (child.element_name() == xform->name) {
                currentPrim = &child;
                break;
            }
        }
    } else {
        // This is a root node, find it in stage root prims
        for (auto& rootPrim : mStage->root_prims()) {
            if (rootPrim.element_name() == xform->name) {
                currentPrim = &rootPrim;
                break;
            }
        }
    }

    // Process children recursively with the current prim as parent
    for (uint32_t i = 0; i < node->mNumChildren; ++i) {
        ExportNodeHierarchy(node->mChildren[i], currentPrim);
    }

    ASSIMP_LOG_DEBUG("USDZExporter: Node exported successfully");
}


// ------------------------------------------------------------------------------------------------
// Export all meshes - associating them with their parent nodes
void USDZExporter::ExportMeshes() {
    if (mScene->mNumMeshes == 0) {
        ASSIMP_LOG_DEBUG("USDZExporter: No meshes to export");
        return;
    }

    // Build a mapping from mesh index to the nodes that reference it
    std::map<uint32_t, std::vector<const aiNode*>> meshToNodes;
    std::function<void(const aiNode*)> collectMeshReferences = [&](const aiNode* node) {
        // Add this node to all meshes it references
        for (uint32_t i = 0; i < node->mNumMeshes; ++i) {
            uint32_t meshIndex = node->mMeshes[i];
            if (meshIndex < mScene->mNumMeshes) {
                meshToNodes[meshIndex].push_back(node);
            }
        }
        
        // Recursively process children
        for (uint32_t i = 0; i < node->mNumChildren; ++i) {
            collectMeshReferences(node->mChildren[i]);
        }
    };
    
    if (mScene->mRootNode) {
        collectMeshReferences(mScene->mRootNode);
    }

    // Track shared meshes for programmatic reference system (data-driven instancing)
    std::map<uint32_t, std::string> primaryMeshPaths; // mesh index -> first occurrence GeomScope path
    mSharedMeshInfo.clear(); // Reset shared mesh tracking
    
    // Detect shared meshes and log them for reference system implementation
    for (const auto& meshMapping : meshToNodes) {
        uint32_t meshIndex = meshMapping.first;
        const std::vector<const aiNode*>& referencingNodes = meshMapping.second;
        
        if (referencingNodes.size() > 1) {
            std::string meshName = SanitizeName(mScene->mMeshes[meshIndex]->mName.C_Str());
            if (meshName.empty()) {
                meshName = "mesh_" + ai_to_string(meshIndex);
            }
            
            ASSIMP_LOG_DEBUG("USDZExporter: Detected shared mesh '" + meshName + "' referenced by " + 
                           ai_to_string(referencingNodes.size()) + " nodes - will use USD reference system");
            
            // Store first node as primary (will contain actual mesh definition)
            const aiNode* primaryNode = referencingNodes[0];
            std::string rootPrimName = GetSceneName();
            std::string primaryPath = BuildFullHierarchyPath(primaryNode, rootPrimName) + "/Geometry";
            primaryMeshPaths[meshIndex] = primaryPath;
            
            // Track shared mesh info for ConvertNode to access
            mSharedMeshInfo[meshIndex] = {primaryPath, referencingNodes};
        }
    }

    for (uint32_t i = 0; i < mScene->mNumMeshes; ++i) {
        const aiMesh* mesh = mScene->mMeshes[i];
        
        std::string meshName = SanitizeName(mesh->mName.C_Str());
        if (meshName.empty()) {
            meshName = "mesh_" + ai_to_string(i);
        }
        meshName = mNameRegistry.GenerateUnique(meshName);
        
        mMeshIdMap[mesh] = meshName;
        
        // Convert to appropriate primitive type
        tinyusdz::Prim meshPrim(tinyusdz::GeomMesh{});
        std::vector<std::string> meshBlendShapeNames;  // Store blend shape names for later use
        
        if (IsPointPrimitive(mesh)) {
            // For point primitives, create a GeomMesh with point-sized faces to ensure tinyusdz compatibility
            tinyusdz::GeomMesh usdMesh;
            usdMesh.name = meshName;
            
            // Convert vertices
            if (mesh->mVertices && mesh->mNumVertices > 0) {
                std::vector<tinyusdz::value::point3f> points;
                points.reserve(mesh->mNumVertices);
                
                for (uint32_t i = 0; i < mesh->mNumVertices; ++i) {
                    const aiVector3D& v = mesh->mVertices[i];
                    points.push_back({v.x, v.y, v.z});
                }
                
                usdMesh.points.set_value(std::move(points));
                
                // Create degenerate triangles (each point becomes a triangle with all 3 vertices at same location)
                std::vector<int> faceVertexCounts;
                std::vector<int> faceVertexIndices;
                faceVertexCounts.reserve(mesh->mNumVertices);
                faceVertexIndices.reserve(mesh->mNumVertices * 3);
                
                for (uint32_t i = 0; i < mesh->mNumVertices; ++i) {
                    faceVertexCounts.push_back(3);  // Each face has 3 vertices (degenerate triangle)
                    // All 3 vertices point to the same vertex index (degenerate triangle at point location)
                    faceVertexIndices.push_back(static_cast<int>(i));
                    faceVertexIndices.push_back(static_cast<int>(i));
                    faceVertexIndices.push_back(static_cast<int>(i));
                }
                
                usdMesh.faceVertexCounts.set_value(std::move(faceVertexCounts));
                usdMesh.faceVertexIndices.set_value(std::move(faceVertexIndices));
                
                // Convert normals if present
                if (mesh->mNormals) {
                    std::vector<tinyusdz::value::normal3f> normals;
                    normals.reserve(mesh->mNumVertices);
                    
                    for (uint32_t i = 0; i < mesh->mNumVertices; ++i) {
                        const aiVector3D& n = mesh->mNormals[i];
                        normals.push_back({n.x, n.y, n.z});
                    }
                    
                    usdMesh.normals.set_value(std::move(normals));
                }
            }
            
            // Bind material to mesh using proper tinyusdz API
            if (mesh->mMaterialIndex < mScene->mNumMaterials) {
                const aiMaterial* material = mScene->mMaterials[mesh->mMaterialIndex];
                auto matIt = mMaterialIdMap.find(material);
                if (matIt != mMaterialIdMap.end()) {
                    tinyusdz::Relationship materialRel;
                    std::string rootPrimName = GetSceneName();
                    std::string materialPathStr = "/" + rootPrimName + "/Materials/" + matIt->second;
                    tinyusdz::Path materialPath(materialPathStr, "");
                    materialRel.set(materialPath);
                    
                    usdMesh.set_materialBinding(materialRel);
                    
                    ASSIMP_LOG_DEBUG("USDZExporter: Bound material " + matIt->second + " to point GeomMesh " + meshName);
                }
            }
            
            // Add standard USD mesh attributes for point primitives too using pipeline
            MeshConverterPipeline pipeline(mesh, usdMesh, mScene, mNameRegistry, *mStage, this);
            pipeline.ExecuteAttributeConversion();
            
            meshPrim = tinyusdz::Prim(usdMesh);
            
            // Apply MaterialBindingAPI to point primitive mesh
            tinyusdz::APISchemas pointMeshBindingAPI;
            pointMeshBindingAPI.listOpQual = tinyusdz::ListEditQual::Prepend;
            pointMeshBindingAPI.names.push_back({tinyusdz::APISchemas::APIName::MaterialBindingAPI, ""});
            meshPrim.metas().apiSchemas = pointMeshBindingAPI;
            ASSIMP_LOG_DEBUG("USDZExporter: Created point GeomMesh primitive with " + ai_to_string(mesh->mNumVertices) + " individual point faces");
            
        } else {
            // Use GeomMesh for regular meshes
            tinyusdz::GeomMesh usdMesh;
            ConvertMesh(mesh, usdMesh);
            
            usdMesh.name = meshName;
            
            // Add MaterialBindingAPI to mesh (USD schema requirement)
            tinyusdz::APISchemas materialBindingAPI;
            materialBindingAPI.listOpQual = tinyusdz::ListEditQual::Prepend;
            materialBindingAPI.names.push_back({tinyusdz::APISchemas::APIName::MaterialBindingAPI, ""});
            // Note: Will be set on the Prim after creation
            
            if (mesh->mMaterialIndex < mScene->mNumMaterials) {
                const aiMaterial* material = mScene->mMaterials[mesh->mMaterialIndex];
                auto matIt = mMaterialIdMap.find(material);
                if (matIt != mMaterialIdMap.end()) {
                    tinyusdz::Relationship materialRel;
                    std::string rootPrimName = GetSceneName();
                    std::string materialPathStr = "/" + rootPrimName + "/Materials/" + matIt->second;
                    tinyusdz::Path materialPath(materialPathStr, "");
                    materialRel.set(materialPath);
                    
                    usdMesh.set_materialBinding(materialRel);
                }
            }
            
            // Create bone name converter function
            auto boneNameConverter = [this](const std::string& boneName) -> std::string {
                // Use the exact USD path from skeleton (critical for tinyusdz validation)
                auto pathIt = mBoneNameToUSDPath.find(boneName);
                if (pathIt != mBoneNameToUSDPath.end()) {
                    return pathIt->second;
                }
                // Fallback to sanitized name (shouldn't happen if skeleton was built correctly)
                return mNameRegistry.Sanitize(boneName);
            };
            
            // Use pipeline to get complete mesh with blend shapes if they exist
            MeshConverterPipeline pipeline(mesh, usdMesh, mScene, mNameRegistry, *mStage, this);
            pipeline.ExecuteFullPipeline(boneNameConverter);
            
            if (!pipeline.IsValid()) {
                ASSIMP_LOG_WARN("USDZExporter: Mesh conversion pipeline validation failed");
            }
            
            // Create mesh prim from the processed usdMesh
            meshPrim = tinyusdz::Prim(usdMesh);
            
            // Add BlendShape prims as children of the mesh
            const auto& blendShapePrims = pipeline.GetBlendShapePrims();
            for (const auto& blendShapePrim : blendShapePrims) {
                meshPrim.children().emplace_back(*blendShapePrim);
            }
            
            // Set MaterialBindingAPI on the prim (USD schema requirement)
            meshPrim.metas().apiSchemas = materialBindingAPI;
            
            // Store blend shape names from pipeline for later use
            meshBlendShapeNames = pipeline.GetBlendShapeNames();
            
            ASSIMP_LOG_DEBUG("USDZExporter: Added " + std::to_string(blendShapePrims.size()) + " BlendShape children to mesh");
        }
        
        // Check if this mesh needs skeletal treatment (bones or blend shapes)
        bool hasSkinnedBones = mesh->mNumBones > 0;
        bool hasBlendShapesOnly = mesh->mNumAnimMeshes > 0 && mesh->mNumBones == 0;
        
        tinyusdz::Prim finalMeshPrim = std::move(meshPrim);
        
        // Find the parent nodes that reference this mesh and add it as their child
        bool meshPlaced = false;
        // Implement proper reference system using correct tinyusdz APIs
        bool isSharedMesh = mSharedMeshInfo.count(i) > 0; // Re-enabled with correct API usage
        
        if (meshToNodes.count(i)) {
            // Build full USD prim path from root to the parent node for relationship paths
            const aiNode* firstParentNode = meshToNodes[i][0];
            std::string rootName = GetSceneName();
            std::string firstParentPrimPath = BuildFullHierarchyPath(firstParentNode, rootName);
            std::string firstParentOrigName = firstParentNode->mName.C_Str();
            
            // Apply skeletal treatment based on mesh type
            if (hasBlendShapesOnly) {
                // Blend shapes only: create dedicated SkelRoot with dummy skeleton
                finalMeshPrim = CreateSkelRootForMesh(mesh, meshName, std::move(finalMeshPrim), meshBlendShapeNames, firstParentPrimPath, firstParentOrigName);
                ASSIMP_LOG_DEBUG("USDZExporter: Created SkelRoot for blend-shape-only mesh: " + meshName);
            } else if (hasSkinnedBones) {
                // ✅ FIX: Add SkelBindingAPI to skeletal mesh (USD schema requirement)
                // Meshes with skeletal properties MUST have SkelBindingAPI applied
                if (!finalMeshPrim.metas().apiSchemas.has_value() || finalMeshPrim.metas().apiSchemas->names.empty()) {
                    // No existing API schemas, create new
                    tinyusdz::APISchemas meshSkelBindingAPI;
                    meshSkelBindingAPI.listOpQual = tinyusdz::ListEditQual::Prepend;
                    meshSkelBindingAPI.names.push_back({tinyusdz::APISchemas::APIName::SkelBindingAPI, ""});
                    finalMeshPrim.metas().apiSchemas = meshSkelBindingAPI;
                } else {
                    // Append to existing API schemas (like MaterialBindingAPI)
                    finalMeshPrim.metas().apiSchemas->names.push_back({tinyusdz::APISchemas::APIName::SkelBindingAPI, ""});
                }
                ASSIMP_LOG_DEBUG("USDZExporter: Added SkelBindingAPI to skeletal mesh " + meshName);
                
                // ✅ FIX: Add skel:skeleton relationship to skeletal mesh (USD schema requirement)
                // Without this, tinyusdz crashes because skin weights exist but no skeleton reference
                std::string skeletonPath = mSkeletonPrimPath;
                tinyusdz::Path skelPath(skeletonPath, "");
                tinyusdz::Relationship skelRel;
                skelRel.set(skelPath);
                
                // Try different approaches to set skeleton relationship
                if (auto* meshData = finalMeshPrim.as<tinyusdz::GeomMesh>()) {
                    // Method 1: Direct assignment (if skeleton is mutable)
                    const_cast<nonstd::optional<tinyusdz::Relationship>&>(meshData->skeleton) = skelRel;
                    ASSIMP_LOG_DEBUG("USDZExporter: Added skel:skeleton relationship to mesh " + meshName + " -> " + skeletonPath);
                } else {
                    ASSIMP_LOG_ERROR("USDZExporter: Could not cast prim to GeomMesh to set skeleton relationship");
                }
                
                // Skinned mesh: place directly under existing GeomScope in SkelRoot (avoid double handling)
                tinyusdz::Prim* geomScopePrim = FindGeomScopeInSkelRoot();
                if (geomScopePrim) {
                    geomScopePrim->children().emplace_back(std::move(finalMeshPrim));
                    meshPlaced = true;
                    ASSIMP_LOG_DEBUG("USDZExporter: Placed skeletal mesh " + meshName + " directly under GeomScope in SkelRoot");
                } else {
                    // No SkelRoot found: remove SkelBindingAPI and skeletal primvars
                    // to avoid USD validation errors (these require a SkelRoot ancestor)
                    if (finalMeshPrim.metas().apiSchemas.has_value()) {
                        auto& names = finalMeshPrim.metas().apiSchemas->names;
                        names.erase(
                            std::remove_if(names.begin(), names.end(),
                                [](const std::pair<tinyusdz::APISchemas::APIName, std::string>& n) {
                                    return n.first == tinyusdz::APISchemas::APIName::SkelBindingAPI;
                                }),
                            names.end());
                    }
                    // Remove all skeletal data from the mesh
                    if (auto* meshData = finalMeshPrim.as<tinyusdz::GeomMesh>()) {
                        auto& props = const_cast<std::map<std::string, tinyusdz::Property>&>(meshData->props);
                        props.erase("primvars:skel:geomBindTransform");
                        props.erase("primvars:skel:jointIndices");
                        props.erase("primvars:skel:jointWeights");
                        props.erase("skel:skeleton");
                        props.erase("skel:animationSource");
                        props.erase("skel:blendShapes");
                        props.erase("skel:blendShapeTargets");
                        const_cast<nonstd::optional<tinyusdz::Relationship>&>(meshData->skeleton).reset();
                    }
                    ASSIMP_LOG_WARN("USDZExporter: Could not find GeomScope for skeletal mesh " + meshName + " - removed skeletal data");
                }
                
            }
            
            // Only process through node hierarchy if mesh hasn't been placed yet
            if (!meshPlaced) {
            for (const aiNode* parentNode : meshToNodes[i]) {
                auto nodeMapIt = mNodeIdMap.find(parentNode);
                std::string parentNodeName = (nodeMapIt != mNodeIdMap.end())
                    ? nodeMapIt->second
                    : SanitizeName(parentNode->mName.C_Str());
                
                // Find the corresponding USD node in our stage
                std::function<bool(tinyusdz::Prim&)> addMeshToNode = [&](tinyusdz::Prim& prim) -> bool {
                    if (prim.element_name() == parentNodeName) {
                        // At this point, skeletal meshes have already been handled above
                        // Only process non-skeletal meshes here
                            // Implement programmatic reference system for shared meshes (data-driven approach)
                            if (isSharedMesh) {
                                // Check if this is the primary node (first node in the shared mesh list)
                                bool isPrimaryNode = (meshToNodes[i][0] == parentNode);
                                
                                if (isPrimaryNode) {
                                    tinyusdz::Scope geomScope;
                                    geomScope.name = "Geometry_" + meshName;
                                    
                                    tinyusdz::Prim geomScopePrim(geomScope);
                                    geomScopePrim.children().emplace_back(std::move(finalMeshPrim));
                                    prim.children().emplace_back(std::move(geomScopePrim));
                                    
                                    ASSIMP_LOG_DEBUG("USDZExporter: Added shared mesh " + meshName + " PRIMARY definition to node " + parentNodeName);
                                } else {
                                    // Secondary node: duplicate the mesh directly (no Geometry scope to avoid name collisions)
                                    tinyusdz::GeomMesh dupMesh;
                                    ConvertMesh(mesh, dupMesh);
                                    dupMesh.name = meshName + "_inst";

                                    if (mesh->mMaterialIndex < mScene->mNumMaterials) {
                                        const aiMaterial* material = mScene->mMaterials[mesh->mMaterialIndex];
                                        auto matIt = mMaterialIdMap.find(material);
                                        if (matIt != mMaterialIdMap.end()) {
                                            tinyusdz::Relationship materialRel;
                                            std::string rootPrimName = GetSceneName();
                                            std::string materialPathStr = "/" + rootPrimName + "/Materials/" + matIt->second;
                                            tinyusdz::Path materialPath(materialPathStr, "");
                                            materialRel.set(materialPath);
                                            dupMesh.set_materialBinding(materialRel);
                                        }
                                    }

                                    tinyusdz::Prim dupPrim(dupMesh);
                                    tinyusdz::APISchemas dupBindingAPI;
                                    dupBindingAPI.listOpQual = tinyusdz::ListEditQual::Prepend;
                                    dupBindingAPI.names.push_back({tinyusdz::APISchemas::APIName::MaterialBindingAPI, ""});
                                    dupPrim.metas().apiSchemas = dupBindingAPI;

                                    prim.children().emplace_back(std::move(dupPrim));

                                    ASSIMP_LOG_DEBUG("USDZExporter: Added shared mesh " + meshName + " COPY to node " + parentNodeName);
                                }
                            } else if (!hasBlendShapesOnly) {
                                tinyusdz::Scope geomScope;
                                geomScope.name = "Geometry_" + meshName;
                                
                                tinyusdz::Prim geomScopePrim(geomScope);
                                geomScopePrim.children().emplace_back(std::move(finalMeshPrim));
                                prim.children().emplace_back(std::move(geomScopePrim));
                                
                            ASSIMP_LOG_DEBUG("USDZExporter: Added non-skeletal mesh " + meshName + " to node " + parentNodeName + " with Geometry wrapper");
                            } else {
                            // Blend-shape mesh: already processed by CreateSkelRootForMesh, place SkelRoot directly
                                prim.children().emplace_back(std::move(finalMeshPrim));
                                
                            ASSIMP_LOG_DEBUG("USDZExporter: Added blend-shape SkelRoot " + meshName + " directly to node " + parentNodeName + " (no additional Geometry wrapping)");
                        }
                        
                        meshPlaced = true;
                        // For shared meshes, continue searching to process all parent nodes
                        // For regular meshes, stop after first placement
                        return !isSharedMesh;
                    }
                    
                    // Search recursively in children
                    for (auto& child : prim.children()) {
                        if (addMeshToNode(child)) {
                            return true;
                        }
                    }
                    return false;
                };
                
                // Search all root prims for the parent node
                for (auto& rootPrim : mStage->root_prims()) {
                    bool stopSearch = addMeshToNode(rootPrim);
                    if (stopSearch) {
                        break; // For regular meshes, stop after first placement. For shared meshes, continue searching.
                    }
                }
                
                // For shared meshes, continue processing all parent nodes to create references
                // For regular meshes, break after first placement to avoid duplicates
                if (meshPlaced && !isSharedMesh) break;
            }
        }
        } // Close if (meshToNodes.count(i))
        
        // Fallback: if mesh isn't referenced by any node
        if (!meshPlaced) {
            if (!mStage->root_prims().empty()) {
                if (hasBlendShapesOnly) {
                    // Create SkelRoot for blend-shape-only mesh and add to root
                    std::string rootPath = "/" + GetSceneName();
                    finalMeshPrim = CreateSkelRootForMesh(mesh, meshName, std::move(finalMeshPrim), meshBlendShapeNames, rootPath, "");
                    mStage->root_prims()[0].children().emplace_back(std::move(finalMeshPrim));
                    ASSIMP_LOG_DEBUG("USDZExporter: Added SkelRoot for blend-shape-only mesh " + meshName + " to root prim");
                } else {
                    tinyusdz::Scope geomScope;
                    geomScope.name = "Geometry_" + meshName;
                    
                    tinyusdz::Prim geomScopePrim(geomScope);
                    geomScopePrim.children().emplace_back(std::move(finalMeshPrim));
                    mStage->root_prims()[0].children().emplace_back(std::move(geomScopePrim));
                    
                    ASSIMP_LOG_DEBUG("USDZExporter: Added mesh " + meshName + " to root prim with Geometry wrapper");
                }
            } else {
                // Absolute fallback: add directly to root level
                mStage->root_prims().emplace_back(std::move(finalMeshPrim));
                ASSIMP_LOG_WARN("USDZExporter: Added mesh " + meshName + " to root level (no scene root found)");
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Export all materials
void USDZExporter::ExportMaterials() {
    if (mScene->mNumMaterials == 0) {
        ASSIMP_LOG_DEBUG("USDZExporter: No materials to export");
        return;
    }

    // Create Materials container using Model prim (not Scope) for broader renderer compatibility
    tinyusdz::Model materialsModel;
    materialsModel.name = "Materials";
    tinyusdz::Prim materialsScopePrim(materialsModel);
    
    for (uint32_t i = 0; i < mScene->mNumMaterials; ++i) {
        const aiMaterial* mat = mScene->mMaterials[i];
        
        aiString aiName;
        std::string matName = "DefaultMaterial";
        if (mat->Get(AI_MATKEY_NAME, aiName) == AI_SUCCESS) {
            matName = SanitizeName(aiName.C_Str());
        }
        if (matName.empty()) {
            matName = "material_" + ai_to_string(i);
        }
        matName = mNameRegistry.GenerateUnique(matName);
        
        // Set current material path for texture processing (include root prim name)
        std::string rootPrimName = GetSceneName();
        mCurrentMaterialPath = "/" + rootPrimName + "/Materials/" + matName;
        
        // Create UsdPreviewSurface shader using ShaderBuilder for consistency
        tinyusdz::UsdPreviewSurface surface;
        CreatePreviewSurface(mat, surface);
        
        // If this material is used by a vertex-colored mesh and has no diffuse texture,
        // connect diffuseColor to a UsdPrimvarReader_float3 for vertex color support
        bool hasDiffuseTexture = std::any_of(
            mCurrentMaterialTextureShaders.begin(),
            mCurrentMaterialTextureShaders.end(),
            [](const auto& p) { return p.first == "diffuseColor"; });
        bool needsVertexColorReader = false;
        if (mMaterialsWithVertexColors.count(i) > 0 && !hasDiffuseTexture) {
            needsVertexColorReader = true;
            std::string readerPath = mCurrentMaterialPath + "/displayColorReader";
            tinyusdz::Path connPath(readerPath, "outputs:result");
            surface.diffuseColor.set_connection(connPath);
            surface.diffuseColor.set_value_empty();
            ASSIMP_LOG_DEBUG("USDZExporter: Connected diffuseColor to vertex color reader for material " + matName);
        }
        
        // Use ShaderBuilder for optimized shader and material creation
        ShaderBuilder builder(mCurrentMaterialPath);
        std::string shaderName = "UsdPreviewSurface";
        tinyusdz::Shader surfaceShader = builder.CreateSurfaceShader(std::move(surface));
        tinyusdz::Material usdMaterial = builder.CreateMaterial(matName, shaderName);
        
        // Set stPrimvarName input for UV coordinate binding
        tinyusdz::Attribute stPrimvarAttr;
        stPrimvarAttr.set_value(tinyusdz::value::token("st"));
        tinyusdz::Property stPrimvarProp(stPrimvarAttr, false);
        usdMaterial.props["inputs:stPrimvarName"] = stPrimvarProp;
        
        mMaterialIdMap[mat] = matName;
        
        // Convert material and shader to Prims
        tinyusdz::Prim materialPrim(usdMaterial);
        tinyusdz::Prim shaderPrim(surfaceShader);
        
        // Add main shader as child of material
        materialPrim.children().emplace_back(std::move(shaderPrim));
        
        // Collect unique UV indices needed across all textures and create readers
        std::map<int, std::string> uvReaderNames;
        if (!mCurrentMaterialTextureShaders.empty()) {
            std::set<int> uvIndices;
            for (const auto& texPair : mCurrentMaterialTextureShaders) {
                auto it = mCurrentMaterialTextureUVIndices.find(texPair.first);
                int idx = (it != mCurrentMaterialTextureUVIndices.end()) ? it->second : 0;
                uvIndices.insert(idx);
            }
            for (int idx : uvIndices) {
                std::string varName = (idx == 0) ? "st" : ("st" + std::to_string(idx));
                std::string readerName = (idx == 0) ? "texCoordReader" : ("texCoordReader_st" + std::to_string(idx));
                uvReaderNames[idx] = readerName;
                
                tinyusdz::Shader reader = CreateTexCoordReader(varName);
                reader.name = readerName;
                tinyusdz::Prim readerPrim(reader);
                materialPrim.children().emplace_back(std::move(readerPrim));
                ASSIMP_LOG_DEBUG("USDZExporter: Added " + readerName + " shader (primvar: " + varName + ")");
            }
        }
        
        // Process texture shaders and create stTransform shaders
        for (const auto& texPair : mCurrentMaterialTextureShaders) {
            const std::string& texName = texPair.first;
            const tinyusdz::UsdUVTexture& texUV = texPair.second;
            
            // Create stTransform shader for this texture
            std::string stTransformName = texName + "_stTransform";
            auto uvIt = mCurrentMaterialTextureUVIndices.find(texName);
            int texUvIndex = (uvIt != mCurrentMaterialTextureUVIndices.end()) ? uvIt->second : 0;
            std::string readerName = uvReaderNames[texUvIndex];
            std::string texCoordReaderConnection = mCurrentMaterialPath + "/" + readerName + ".outputs:result";
            
            // Get texture transform if available
            const aiUVTransform* uvTransform = nullptr;
            auto transformIt = mCurrentMaterialTextureTransforms.find(texName);
            if (transformIt != mCurrentMaterialTextureTransforms.end()) {
                uvTransform = &transformIt->second;
            }
            
            tinyusdz::Shader stTransformShader = CreateStTransform(texCoordReaderConnection, uvTransform);
            stTransformShader.name = stTransformName;
            
            // Create texture shader with proper st connection
            tinyusdz::Shader textureShader;
            textureShader.name = texName;
            textureShader.info_id = tinyusdz::kUsdUVTexture;
            
            // Copy texture properties and SET the crucial st connection on UsdUVTexture object
            tinyusdz::UsdUVTexture connectedTexture = texUV;
            std::string stConnection = mCurrentMaterialPath + "/" + stTransformName + ".outputs:result";
            
            tinyusdz::Path stPath(stConnection, "");
            connectedTexture.st.set_connection(stPath);
            connectedTexture.st.set_value_empty();
            
            textureShader.value = connectedTexture;
            
            // Set explicit USD-compliant types for UsdUVTexture inputs with correct float4 values
            tinyusdz::Attribute fallbackAttr;
            if (texName == "diffuseColor" || texName == "emissiveColor") {
                fallbackAttr.set_value(tinyusdz::value::float4{0.0f, 0.0f, 0.0f, 1.0f});
            } else {
                fallbackAttr.set_value(tinyusdz::value::float4{1.0f, 1.0f, 1.0f, 1.0f});
            }
            fallbackAttr.set_type_name("float4");
            tinyusdz::Property fallbackProp(fallbackAttr, false);
            textureShader.props["inputs:fallback"] = fallbackProp;
            
            // Add both shaders as children of material
            tinyusdz::Prim stTransformPrim(stTransformShader);
            tinyusdz::Prim textureShaderPrim(textureShader);
            
            materialPrim.children().emplace_back(std::move(stTransformPrim));
            materialPrim.children().emplace_back(std::move(textureShaderPrim));
            
            ASSIMP_LOG_DEBUG("USDZExporter: Added texture pipeline for " + texName + " (UV" + std::to_string(texUvIndex) + ")");
        }
        
        // Add vertex color PrimvarReader if this material needs it
        if (needsVertexColorReader) {
            tinyusdz::Shader colorReader;
            colorReader.name = "displayColorReader";
            colorReader.info_id = "UsdPrimvarReader_float3";
            
            tinyusdz::UsdPrimvarReader_float3 primvarReader;
            primvarReader.varname.set_value(std::string("displayColor"));
            primvarReader.result.set_authored(true);
            
            colorReader.value = primvarReader;
            tinyusdz::Prim readerPrim(colorReader);
            materialPrim.children().emplace_back(std::move(readerPrim));
            ASSIMP_LOG_DEBUG("USDZExporter: Added displayColor PrimvarReader shader");
        }
        
        // Add material to Materials scope
        materialsScopePrim.children().emplace_back(std::move(materialPrim));
        
        ASSIMP_LOG_DEBUG("USDZExporter: Material exported successfully");
    }
    
    // Add Materials scope as child of root prim
    if (!mStage->root_prims().empty()) {
        mStage->root_prims()[0].children().emplace_back(std::move(materialsScopePrim));
        ASSIMP_LOG_DEBUG("USDZExporter: Materials scope added to root prim");
    } else {
        // Fallback: add to root if no root prim exists
        mStage->root_prims().emplace_back(std::move(materialsScopePrim));
        ASSIMP_LOG_WARN("USDZExporter: No root prim found, added Materials to root level");
    }
}

// ------------------------------------------------------------------------------------------------
// Export textures using proper tinyusdz APIs
void USDZExporter::ExportTextures() {
    // Embedded textures are now handled by HandleEmbeddedTexture() when materials reference them
    // This avoids duplicate texture writing and ensures proper directory structure
    // External textures are handled by HandleExternalTexture() 
    ASSIMP_LOG_DEBUG("USDZExporter: Texture processing delegated to material handlers");
}

// ------------------------------------------------------------------------------------------------
// Export skeletons for skinned meshes
void USDZExporter::ExportSkeletons() {
    // Check if any meshes have bones using Assimp discriminators
    bool hasSkinnedMeshes = false;
    for (uint32_t i = 0; i < mScene->mNumMeshes; ++i) {
        if (mScene->mMeshes[i]->HasBones()) {
            hasSkinnedMeshes = true;
            break;
        }
    }
    
    if (!hasSkinnedMeshes) {
        ASSIMP_LOG_DEBUG("USDZExporter: No skeletons to export");
        return;
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Creating centralized skeleton structure");
    
    // PHASE 2: Collect all unique bones from skeletal meshes using Assimp discriminators
    std::set<std::string> allBoneNames;
    std::map<std::string, const aiBone*> boneDataMap;
    
    for (uint32_t meshIdx = 0; meshIdx < mScene->mNumMeshes; ++meshIdx) {
        const aiMesh* mesh = mScene->mMeshes[meshIdx];
        if (mesh->HasBones()) {
        for (uint32_t boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
                        const aiBone* bone = mesh->mBones[boneIdx];
                std::string boneName = bone->mName.C_Str();
                allBoneNames.insert(boneName);
                boneDataMap[boneName] = bone;
            }
        }
    }
    
    // PHASE 3: Create Skeleton with generic naming from scene data
    std::string skeletonName = "Skeleton";
    if (mScene->HasSkeletons() && mScene->mSkeletons[0]->mName.length > 0) {
        skeletonName = SanitizeName(mScene->mSkeletons[0]->mName.C_Str());
    } else {
        // Derive skeleton name from the parent node of skeleton root bones
        if (!mBoneNamesInitialized) InitializeBoneDiscriminator();
        for (const auto& boneName : mBoneNames) {
            const aiNode* boneNode = FindNodeByName(mScene->mRootNode, boneName);
            if (!boneNode) continue;
            const aiNode* node = boneNode;
            while (node->mParent) {
                std::string parentName = node->mParent->mName.C_Str();
                if (mBoneNames.find(parentName) == mBoneNames.end()) {
                    skeletonName = SanitizeName(parentName);
                    break;
                }
                node = node->mParent;
            }
            break;
        }
    }
    
    tinyusdz::Skeleton skeleton;
    skeleton.name = skeletonName;
    
    // Build scene node hierarchy first (like gltfImport.cpp buildSkeletonNodeNames) 
    NodeHierarchyMapping nodeMapping = BuildSceneNodeHierarchy();
    
    // Then build joint paths using skeleton bone ordering (like gltfImport.cpp skin.joints)
    JointPathMapping jointPaths = BuildJointPathsFromNodeHierarchy(nodeMapping, allBoneNames, boneDataMap);
    
    // Populate global bone-to-skeleton-index mapping for use in ConvertMesh
    mGlobalBoneToSkeletonIndex.clear();
    for (size_t i = 0; i < jointPaths.skeletonJointNames.size(); ++i) {
        mGlobalBoneToSkeletonIndex[jointPaths.skeletonJointNames[i]] = static_cast<uint32_t>(i);
    }
    
    // Create joint data using the hierarchical ordering (like gltfImport.cpp)
    std::vector<tinyusdz::value::token> jointNameTokens;  // Descriptive names in skeleton order
    std::vector<tinyusdz::value::matrix4d> bindTransforms;
    std::vector<tinyusdz::value::matrix4d> restTransforms;
    
    // Process bones in the skeleton joint order (not original bone order)
    for (size_t i = 0; i < jointPaths.skeletonJoints.size(); ++i) {
        const std::string& boneName = jointPaths.skeletonJointNames[i];
        const std::string& jointPath = jointPaths.skeletonJoints[i];
        
        jointNameTokens.push_back(tinyusdz::value::token(SanitizeName(boneName)));
        
        // bindTransform = inverseBindMatrix.GetInverse()
        const aiBone* boneData = jointPaths.skeletonBonePointers[i];
        tinyusdz::value::matrix4d bindTransform;
        
        // Invert the inverse bind matrix from glTF (stored as aiBone->mOffsetMatrix)
        aiMatrix4x4 bindMatrix = boneData->mOffsetMatrix;
        bindMatrix.Inverse();  // ← KEY: invert the inverse bind matrix
        
        // Convert to USD matrix format with transpose
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                bindTransform.m[row][col] = static_cast<double>(bindMatrix[col][row]); // Transpose
            }
        }
        
        // Calculate rest transform from bone node's local transform
        const aiNode* boneNode = FindNodeByName(mScene->mRootNode, boneName);
        tinyusdz::value::matrix4d restTransform;
        
        if (boneNode) {
            const aiMatrix4x4& localTransform = boneNode->mTransformation;
            for (int row = 0; row < 4; ++row) {
                for (int col = 0; col < 4; ++col) {
                    restTransform.m[row][col] = static_cast<double>(localTransform[col][row]); // Transpose during copy
                }
            }
            ASSIMP_LOG_DEBUG("USDZExporter: Skeleton joint[" + std::to_string(i) + "]: '" + boneName + 
                             "' → '" + jointPath + "' bindTransform calculated from mOffsetMatrix.Inverse()");
    } else {
            // Fallback to identity if bone node not found
            tinyusdz::Identity(&restTransform);
            ASSIMP_LOG_WARN("USDZExporter: Bone node not found for '" + boneName + "', using identity restTransform");
        }
        bindTransforms.push_back(bindTransform);
        restTransforms.push_back(restTransform);
    }
    
    // Convert string joint paths to tokens
    std::vector<tinyusdz::value::token> skeletonJointTokens;
    for (const std::string& jointPath : jointPaths.skeletonJoints) {
        skeletonJointTokens.push_back(tinyusdz::value::token(jointPath));
    }
    
    // Set skeleton attributes using hierarchical joint paths
    skeleton.joints.set_value(skeletonJointTokens);          // Hierarchical paths (e.g., n0/n1/n3, n0/n1/n3/n12)
    skeleton.jointNames.set_value(jointNameTokens);          // Descriptive names
    skeleton.bindTransforms.set_value(bindTransforms);
    skeleton.restTransforms.set_value(restTransforms);
    
    tinyusdz::Prim skeletonPrim(skeleton);
    
    // Add SkelBindingAPI to Skeleton prim
    tinyusdz::APISchemas skelBindAPI;
    skelBindAPI.listOpQual = tinyusdz::ListEditQual::Prepend;
    skelBindAPI.names.push_back({tinyusdz::APISchemas::APIName::SkelBindingAPI, ""});
    skeletonPrim.metas().apiSchemas = skelBindAPI;
    
    // Find the skeleton parent prim generically (not hardcoded to Z_UP/Armature)
    mSkeletonParentPrim = FindSkeletonParentPrim();
    if (!mSkeletonParentPrim) {
        ASSIMP_LOG_ERROR("USDZExporter: Could not find parent prim for skeleton placement");
        return;
    }
    
    mSkeletonParentPrimPath = ComputePrimPath(mSkeletonParentPrim);
    mSkelRootName = skeletonName + "SkelRoot";
    mSkelRootPrimPath = mSkeletonParentPrimPath + "/" + mSkelRootName;
    mSkeletonPrimPath = mSkelRootPrimPath + "/" + skeletonName;
    
    // Compute animation name using same logic as CreateCentralizedSkelAnimation
    std::string animName = "Animation";
    if (mScene->mNumAnimations > 0 && mScene->mAnimations[0]->mName.length > 0) {
        animName = SanitizeName(mScene->mAnimations[0]->mName.C_Str());
    }
    mAnimationPrimPath = mSkeletonPrimPath + "/" + animName;
    
    ASSIMP_LOG_DEBUG("USDZExporter: Skeleton parent prim path: " + mSkeletonParentPrimPath);
    ASSIMP_LOG_DEBUG("USDZExporter: SkelRoot path: " + mSkelRootPrimPath);
    ASSIMP_LOG_DEBUG("USDZExporter: Animation path: " + mAnimationPrimPath);
    
    // Set skel:animationSource on the Skeleton prim (will point to SkelAnimation child)
    {
        tinyusdz::Relationship animRel;
        animRel.set(tinyusdz::Path(mAnimationPrimPath, ""));
        if (auto* skelData = skeletonPrim.as<tinyusdz::Skeleton>()) {
            const_cast<tinyusdz::Skeleton*>(skelData)->props["skel:animationSource"] = tinyusdz::Property(animRel, false);
        }
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Created Skeleton '" + skeletonName + "' with " + std::to_string(allBoneNames.size()) + " joints");
    
    // Create SkelRoot to contain the Skeleton and meshes
    tinyusdz::SkelRoot skelRoot;
    skelRoot.name = mSkelRootName;
    
    tinyusdz::Prim skelRootPrim(skelRoot);
    
    // Add Skeleton as first child of SkelRoot
    skelRootPrim.children().emplace_back(std::move(skeletonPrim));
    
    // Add GeomScope for mesh placement
    tinyusdz::Scope geomScope;
    geomScope.name = "GeomScope";
    tinyusdz::Prim geomScopePrim(geomScope);
    skelRootPrim.children().emplace_back(std::move(geomScopePrim));
    ASSIMP_LOG_DEBUG("USDZExporter: Added GeomScope to SkelRoot");
    
    // Add SkelRoot under skeleton parent prim
    mSkeletonParentPrim->children().emplace_back(std::move(skelRootPrim));
    ASSIMP_LOG_DEBUG("USDZExporter: Added SkelRoot under " + mSkeletonParentPrimPath);
    
    ASSIMP_LOG_DEBUG("USDZExporter: Created SkelRoot '" + mSkelRootName + "' with Skeleton inside");
}

// ------------------------------------------------------------------------------------------------
// Export animations
void USDZExporter::ExportAnimations() {
    if (mScene->mNumAnimations == 0) {
        ASSIMP_LOG_DEBUG("USDZExporter: No animations to export");
        return;
    }

    // Check if we have skeletal data using Assimp discriminators
    bool hasSkeletalMeshes = false;
    for (uint32_t i = 0; i < mScene->mNumMeshes; ++i) {
        if (mScene->mMeshes[i]->HasBones()) {
            hasSkeletalMeshes = true;
            break;
        }
    }

    if (hasSkeletalMeshes) {
        ASSIMP_LOG_DEBUG("USDZExporter: Creating centralized SkelAnimation for skeletal data");
        CreateCentralizedSkelAnimation();
        // Also apply Xform animations for non-bone node channels
        for (uint32_t i = 0; i < mScene->mNumAnimations; ++i) {
            const aiAnimation* anim = mScene->mAnimations[i];
            ConvertAnimation(anim);
        }
    } else {
        ASSIMP_LOG_DEBUG("USDZExporter: Using traditional animation export for non-skeletal data");
        for (uint32_t i = 0; i < mScene->mNumAnimations; ++i) {
            const aiAnimation* anim = mScene->mAnimations[i];
            ConvertAnimation(anim);
        }
    }
    
    // Process property animations (KHR_animation_pointer) regardless of skeletal status
    for (uint32_t i = 0; i < mScene->mNumAnimations; ++i) {
        ExportPropertyAnimations(mScene->mAnimations[i]);
    }
}

// ------------------------------------------------------------------------------------------------
// Export cameras
void USDZExporter::ExportCameras() {
    if (mScene->mNumCameras == 0) {
        ASSIMP_LOG_DEBUG("USDZExporter: No cameras to export");
        return;
    }

    for (uint32_t i = 0; i < mScene->mNumCameras; ++i) {
        const aiCamera* camera = mScene->mCameras[i];
        ConvertCamera(camera);
        
        ASSIMP_LOG_DEBUG("USDZExporter: Camera exported successfully");
    }
}

// ------------------------------------------------------------------------------------------------
// Export lights
void USDZExporter::ExportLights() {
    if (mScene->mNumLights == 0) {
        ASSIMP_LOG_DEBUG("USDZExporter: No lights to export");
        return;
    }

    for (uint32_t i = 0; i < mScene->mNumLights; ++i) {
        const aiLight* light = mScene->mLights[i];
        ConvertLight(light);
        
        ASSIMP_LOG_DEBUG("USDZExporter: Light exported successfully");
    }
}

// ------------------------------------------------------------------------------------------------
// Export AR anchoring metadata
void USDZExporter::ExportARAnchoring() {
    auto& stageMeta = mStage->metas();
    
    // Add AR anchoring properties through layer meta comment
    stageMeta.comment = "arKit:planeDetection=horizontal arKit:initialPlacement=planeAnchor";
    
    // TODO: Add preliminary anchoring to root prim if available
    // This requires a deeper understanding of tinyusdz Property API
    
    ASSIMP_LOG_DEBUG("USDZExporter: AR anchoring metadata exported");
}

// ------------------------------------------------------------------------------------------------
// Export MaterialX node graphs using tinyusdz APIs
void USDZExporter::ExportMaterialX() {
    // MaterialX support in tinyusdz is currently import-only
    // As noted in usdMtlx.hh: "Import only. Export is not supported(yet)."
    
    ASSIMP_LOG_DEBUG("USDZExporter: MaterialX export not available - tinyusdz supports import only");
    ASSIMP_LOG_DEBUG("USDZExporter: Using UsdPreviewSurface materials for USD compatibility");
    
    // Our current approach with UsdPreviewSurface provides comprehensive material support:
    // - PBR materials (metallic/roughness workflow) 
    // - Textures (diffuse, normal, emissive, occlusion, metallic, roughness)
    // - Clearcoat properties
    // - Proper shader network connections
    
    // This covers the essential use cases for USD materials without requiring MaterialX
    ASSIMP_LOG_DEBUG("USDZExporter: MaterialX export completed (using UsdPreviewSurface fallback)");
}

// ------------------------------------------------------------------------------------------------
// Export subdivision surfaces using tinyusdz subdivision APIs
void USDZExporter::ExportSubdivisionSurfaces() {
    if (!mScene) {
        return;
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Checking for subdivision surface requirements");
    
    // Check if any meshes would benefit from subdivision surface treatment
    // This is a heuristic-based approach since Assimp doesn't have explicit subdivision metadata
    bool hasHighPolyMeshes = false;
    bool hasQuadDominantMeshes = false;
    
    for (uint32_t i = 0; i < mScene->mNumMeshes; ++i) {
        const aiMesh* mesh = mScene->mMeshes[i];
        if (!mesh) continue;
        
        // Check for high poly count (may benefit from subdivision representation)
        if (mesh->mNumVertices > 10000) {
            hasHighPolyMeshes = true;
        }
        
        // Check for quad-dominant meshes (good candidates for Catmull-Clark)
        uint32_t quadCount = 0;
        for (uint32_t f = 0; f < mesh->mNumFaces; ++f) {
            if (mesh->mFaces[f].mNumIndices == 4) {
                quadCount++;
            }
        }
        
        if (mesh->mNumFaces > 0 && (float(quadCount) / float(mesh->mNumFaces)) > 0.7f) {
            hasQuadDominantMeshes = true;
        }
    }
    
    // Apply subdivision schemes to appropriate meshes
    // Note: In a real implementation, this would be integrated into the mesh conversion process
    // For now, we log the recommendation for subdivision surface treatment
    
    if (hasHighPolyMeshes) {
        ASSIMP_LOG_DEBUG("USDZExporter: High-poly meshes detected - could benefit from subdivision surface optimization");
    }
    
    if (hasQuadDominantMeshes) {
        ASSIMP_LOG_DEBUG("USDZExporter: Quad-dominant meshes detected - suitable for Catmull-Clark subdivision");
    }
    
    // The actual subdivision surface configuration would be set on individual GeomMesh objects:
    // mesh.subdivisionScheme.set_value(tinyusdz::GeomMesh::SubdivisionScheme::CatmullClark);
    // mesh.interpolateBoundary.set_value(tinyusdz::GeomMesh::InterpolateBoundary::EdgeAndCorner);
    
    ASSIMP_LOG_DEBUG("USDZExporter: Subdivision surface analysis completed");
    ASSIMP_LOG_DEBUG("USDZExporter: Available schemes: none, catmullClark, loop, bilinear");
}

// ------------------------------------------------------------------------------------------------
// Export volume rendering using tinyusdz volume APIs  
void USDZExporter::ExportVolumeRendering() {
    // Volume rendering support in tinyusdz is currently not implemented
    // As noted in prim-types.hh: "Simple volume class. Currently this is just a placeholder. Not implemented."
    // And in status.md: "[ ] Volume(usdVol)" is listed as TODO
    
    ASSIMP_LOG_DEBUG("USDZExporter: Volume rendering not yet available in tinyusdz");
    ASSIMP_LOG_DEBUG("USDZExporter: Future volume support planned for:");
    ASSIMP_LOG_DEBUG("  - OpenVDB volumes (VDBVolume)");  
    ASSIMP_LOG_DEBUG("  - MagicaVoxel vox format");
    ASSIMP_LOG_DEBUG("  - USD Volume (usdVol) specification");
    
    // When implemented, volume support would include:
    // - OpenVDBAsset: fieldDataType, fieldName, filePath properties
    // - VoxAsset: for MagicaVoxel .vox files  
    // - Volume prim with proper USD volume representation
    
    // For now, any volumetric data in the Assimp scene would need to be:
    // - Converted to mesh representation, or
    // - Exported as separate volume files with references
    
    if (mScene) {
        for (uint32_t i = 0; i < mScene->mNumMeshes; ++i) {
            const aiMesh* mesh = mScene->mMeshes[i];
            if (mesh && mesh->mName.length > 0) {
                std::string meshName(mesh->mName.C_Str());
                if (meshName.find("volume") != std::string::npos || 
                    meshName.find("Volume") != std::string::npos) {
                    ASSIMP_LOG_DEBUG("USDZExporter: Found potential volume mesh: " + meshName);
                    ASSIMP_LOG_DEBUG("USDZExporter: Exporting as regular geometry until volume support is available");
                }
            }
        }
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Volume rendering export completed (placeholder)");
}

// ------------------------------------------------------------------------------------------------
// Check if mesh is point primitive
bool USDZExporter::IsPointPrimitive(const aiMesh* mesh) {
    if (!mesh || !mesh->mFaces || mesh->mNumFaces == 0) return false;
    
    for (uint32_t i = 0; i < mesh->mNumFaces; ++i) {
        if (mesh->mFaces[i].mNumIndices != 1) {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// Check if mesh needs skeletal treatment (has bones or blend shapes)
bool USDZExporter::NeedsSkeletalTreatment(const aiMesh* mesh) {
    return mesh && (mesh->mNumBones > 0 || mesh->mNumAnimMeshes > 0);
}

// ------------------------------------------------------------------------------------------------
// Create SkelRoot structure for meshes that need skeletal treatment
// NOTE: For blend-shape-only meshes, USD requires a skeleton infrastructure even without actual bones.
// This "dummy skeleton" approach is the standard USD pattern for blend-shape-only meshes.
// USD's blendShapeWeights can only exist within SkelAnimation, which requires a Skeleton reference.
tinyusdz::Prim USDZExporter::CreateSkelRootForMesh(const aiMesh* mesh, const std::string& meshName, tinyusdz::Prim&& meshPrim, const std::vector<std::string>& blendShapeNames, const std::string& parentPrimPath, const std::string& parentNodeOrigName) {
    tinyusdz::SkelRoot skelRoot;
    skelRoot.name = meshName;
    
    // Create Skeleton with dummy joint for blend shapes
    // Standard USD approach: dummy joint required for SkelAnimation even without real bones
    tinyusdz::Skeleton skeleton;
    skeleton.name = "Skel";
    
    // Create dummy joint system (required for USD skeletal binding, even for pure blend shapes)
    std::vector<tinyusdz::value::token> jointTokens = {tinyusdz::value::token("joint1")};
    skeleton.joints.set_value(jointTokens);
    
    // Create identity bind and rest transforms
    std::vector<tinyusdz::value::matrix4d> bindTransforms(1);
    std::vector<tinyusdz::value::matrix4d> restTransforms(1);
    tinyusdz::Identity(&bindTransforms[0]);
    tinyusdz::Identity(&restTransforms[0]);
    
    skeleton.bindTransforms.set_value(bindTransforms);
    skeleton.restTransforms.set_value(restTransforms);
    
    // Create skeleton prim with SkelBindingAPI (required because skeleton will have skel:animationSource property)
    tinyusdz::Prim skeletonPrim(skeleton);
    
    // ✅ FIX: Add SkelBindingAPI to skeleton prim (USD schema requirement for prims with skel:animationSource)
    tinyusdz::APISchemas skelBindingAPI;
    skelBindingAPI.listOpQual = tinyusdz::ListEditQual::Prepend;
    skelBindingAPI.names.push_back({tinyusdz::APISchemas::APIName::SkelBindingAPI, ""});
    skeletonPrim.metas().apiSchemas = skelBindingAPI;
    
    // Create SkelAnimation if mesh has blend shapes
    if (!blendShapeNames.empty()) {
        tinyusdz::SkelAnimation skelAnim;
        skelAnim.name = "Anim";
        
        std::vector<tinyusdz::value::token> blendShapeTokens;
        for (const std::string& name : blendShapeNames) {
            blendShapeTokens.push_back(tinyusdz::value::token(SanitizeName(name)));
        }
        skelAnim.blendShapes.set_value(blendShapeTokens);
        
        // Read default weights from aiAnimMesh::mWeight (set by glTF importer from mesh.weights[])
        std::vector<float> defaultWeights(blendShapeNames.size(), 0.0f);
        if (mesh && mesh->mAnimMeshes) {
            for (size_t i = 0; i < blendShapeNames.size() && i < mesh->mNumAnimMeshes; ++i) {
                if (mesh->mAnimMeshes[i]) {
                    defaultWeights[i] = mesh->mAnimMeshes[i]->mWeight;
                }
            }
        }
        
        // Check if any morph animation channel in the scene targets this mesh
        bool hasMorphAnimation = false;
        std::string origMeshName = mesh ? mesh->mName.C_Str() : "";
        if (mScene && mScene->mNumAnimations > 0) {
            for (uint32_t animIdx = 0; animIdx < mScene->mNumAnimations && !hasMorphAnimation; ++animIdx) {
                const aiAnimation* anim = mScene->mAnimations[animIdx];
                for (uint32_t morphIdx = 0; morphIdx < anim->mNumMorphMeshChannels; ++morphIdx) {
                    const aiMeshMorphAnim* morphAnim = anim->mMorphMeshChannels[morphIdx];
                    std::string morphMeshName = morphAnim->mName.C_Str();
                    bool nameMatches = morphMeshName.empty() || 
                                     morphMeshName == origMeshName || 
                                     morphMeshName == parentNodeOrigName ||
                                     morphMeshName.find("node") != std::string::npos ||
                                     morphMeshName.find(origMeshName) != std::string::npos ||
                                     origMeshName.find(morphMeshName) != std::string::npos;
                    if (nameMatches && morphAnim->mNumKeys > 0) {
                        hasMorphAnimation = true;
                        break;
                    }
                }
            }
        }
        
        if (hasMorphAnimation) {
            // Animated morph targets: generate time samples from scene animation data
            tinyusdz::Animatable<std::vector<float>> animatedWeights;
            double frameRate = 24.0;
            int totalFrames = 1;
            
            double maxDuration = 0.0;
            for (uint32_t i = 0; i < mScene->mNumAnimations; ++i) {
                const aiAnimation* anim = mScene->mAnimations[i];
                double duration = anim->mDuration / (anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 24.0);
                maxDuration = std::max(maxDuration, duration);
            }
            if (maxDuration > 0.0) {
                totalFrames = static_cast<int>(std::round(maxDuration * frameRate));
                if (totalFrames < 1) totalFrames = 1;
            }
            
            ASSIMP_LOG_DEBUG("USDZExporter: Generating " + std::to_string(totalFrames) + 
                            " time samples (frames 1-" + std::to_string(totalFrames) + ") at " + std::to_string(frameRate) + "fps");
            
            for (int frame = 0; frame < totalFrames; ++frame) {
                double timeCode = frame + 1;
                std::vector<float> frameWeights(blendShapeNames.size(), 0.0f);
                
                for (uint32_t animIdx = 0; animIdx < mScene->mNumAnimations; ++animIdx) {
                    const aiAnimation* anim = mScene->mAnimations[animIdx];
                    double ticksPerSecond = anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 24.0;
                    double animTime = (timeCode / frameRate) * ticksPerSecond;
                    
                    for (uint32_t morphIdx = 0; morphIdx < anim->mNumMorphMeshChannels; ++morphIdx) {
                        const aiMeshMorphAnim* morphAnim = anim->mMorphMeshChannels[morphIdx];
                        std::string morphMeshName = morphAnim->mName.C_Str();
                        
                        bool nameMatches = morphMeshName.empty() || 
                                         morphMeshName == origMeshName || 
                                         morphMeshName == parentNodeOrigName ||
                                         morphMeshName.find("node") != std::string::npos ||
                                         morphMeshName.find(origMeshName) != std::string::npos ||
                                         origMeshName.find(morphMeshName) != std::string::npos;
                        
                        if (nameMatches && morphAnim->mNumKeys > 0 && morphAnim->mKeys) {
                            for (uint32_t keyIdx = 0; keyIdx < morphAnim->mNumKeys; ++keyIdx) {
                                const aiMeshMorphKey& key = morphAnim->mKeys[keyIdx];
                                if (keyIdx == morphAnim->mNumKeys - 1) {
                                    if (animTime >= key.mTime) {
                                        for (uint32_t weightIdx = 0; weightIdx < key.mNumValuesAndWeights && weightIdx < frameWeights.size(); ++weightIdx) {
                                            frameWeights[weightIdx] = static_cast<float>(key.mWeights[weightIdx]);
                                        }
                                        break;
                                    }
                                } else if (animTime >= key.mTime && animTime < morphAnim->mKeys[keyIdx + 1].mTime) {
                                    const aiMeshMorphKey& nextKey = morphAnim->mKeys[keyIdx + 1];
                                    double t = (animTime - key.mTime) / (nextKey.mTime - key.mTime);
                                    for (uint32_t weightIdx = 0; weightIdx < key.mNumValuesAndWeights && weightIdx < frameWeights.size(); ++weightIdx) {
                                        frameWeights[weightIdx] = static_cast<float>(
                                            key.mWeights[weightIdx] + t * (nextKey.mWeights[weightIdx] - key.mWeights[weightIdx]));
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
                
                animatedWeights.add_sample(timeCode, frameWeights);
            }
            
            std::vector<float> animDefault(blendShapeNames.size(), 0.0f);
            animatedWeights.set_default(animDefault);
            skelAnim.blendShapeWeights.set_value(animatedWeights);
            
            ASSIMP_LOG_DEBUG("USDZExporter: Added blendShapeWeights.timeSamples with " + std::to_string(totalFrames) + 
                            " samples for " + std::to_string(blendShapeNames.size()) + " blend shapes");
        } else {
            // Static morph targets (no animation): use default weights from aiAnimMesh::mWeight
            skelAnim.blendShapeWeights.set_value(defaultWeights);
            
            ASSIMP_LOG_DEBUG("USDZExporter: Set static blendShapeWeights for " + std::to_string(blendShapeNames.size()) + " blend shapes (no animation)");
        }
        
        // Add SkelAnimation as child of skeleton
        tinyusdz::Prim skelAnimPrim(skelAnim);
        skeletonPrim.children().emplace_back(std::move(skelAnimPrim));
        
        // Add animation source reference to skeleton using full parent path
        tinyusdz::Relationship animSourceRel;
        
        std::string skelAnimPath = parentPrimPath + "/" + meshName + "/Skel/Anim";
        tinyusdz::Path animSourcePath(skelAnimPath, "");
        animSourceRel.set(animSourcePath);
        
        // Set prepend list edit qualifier for skel:animationSource
        animSourceRel.set_listedit_qual(tinyusdz::ListEditQual::Prepend);
        
        ASSIMP_LOG_DEBUG("USDZExporter: Set skel:animationSource to absolute path: " + skelAnimPath);
        tinyusdz::Property animSourceProp(animSourceRel);
        
        // Access skeleton data and set the property
        if (auto* skelData = skeletonPrim.as<tinyusdz::Skeleton>()) {
            const_cast<std::map<std::string, tinyusdz::Property>&>(skelData->props)["skel:animationSource"] = animSourceProp;
        }
        
        ASSIMP_LOG_DEBUG("USDZExporter: Added SkelAnimation with " + std::to_string(blendShapeTokens.size()) + " blend shapes");
    }
    
    // Create SkelRoot prim and add skeleton as child
    tinyusdz::Prim skelRootPrim(skelRoot);
    skelRootPrim.children().emplace_back(std::move(skeletonPrim));
    
    // Add SkelBindingAPI to the mesh prim (append to existing API schemas)
    if (!meshPrim.metas().apiSchemas.has_value() || meshPrim.metas().apiSchemas->names.empty()) {
        // No existing API schemas, create new
        tinyusdz::APISchemas meshSkelBindingAPI;
        meshSkelBindingAPI.listOpQual = tinyusdz::ListEditQual::Prepend;
        meshSkelBindingAPI.names.push_back({tinyusdz::APISchemas::APIName::SkelBindingAPI, ""});
        meshPrim.metas().apiSchemas = meshSkelBindingAPI;
    } else {
        // Append to existing API schemas (like MaterialBindingAPI)
        meshPrim.metas().apiSchemas->names.push_back({tinyusdz::APISchemas::APIName::SkelBindingAPI, ""});
    }
    
    if (auto* meshData = meshPrim.as<tinyusdz::GeomMesh>()) {
        std::string skeletonPath = parentPrimPath + "/" + meshName + "/Skel";
        tinyusdz::Path skelPath(skeletonPath, "");
        tinyusdz::Relationship skelRel;
        skelRel.set(skelPath);
        
        const_cast<nonstd::optional<tinyusdz::Relationship>&>(meshData->skeleton) = skelRel;
        ASSIMP_LOG_DEBUG("USDZExporter: Added skel:skeleton relationship to blend-shape mesh " + meshName + " -> " + skeletonPath);
        
        if (!blendShapeNames.empty()) {
            std::string basePath = parentPrimPath + "/" + meshName + "/Geometry_" + meshName + "/" + meshName;
            std::vector<tinyusdz::Path> blendShapeTargetPaths;
            for (const std::string& blendShapeName : blendShapeNames) {
                blendShapeTargetPaths.emplace_back(basePath + "/" + blendShapeName, "");
            }
            
            tinyusdz::Relationship blendShapeTargetsRel;
            blendShapeTargetsRel.set(blendShapeTargetPaths);
            blendShapeTargetsRel.set_listedit_qual(tinyusdz::ListEditQual::Prepend);
            tinyusdz::Property blendShapeTargetsProp(blendShapeTargetsRel);
            
            const_cast<std::map<std::string, tinyusdz::Property>&>(meshData->props)["skel:blendShapeTargets"] = blendShapeTargetsProp;
            
            ASSIMP_LOG_DEBUG("USDZExporter: Set skel:blendShapeTargets to absolute paths");
        }
    }
    
    tinyusdz::Scope geomScope;
    geomScope.name = "Geometry_" + meshName;
    
    tinyusdz::Prim geomScopePrim(geomScope);
    geomScopePrim.children().emplace_back(std::move(meshPrim));
    
    skelRootPrim.children().emplace_back(std::move(geomScopePrim));
    
    ASSIMP_LOG_DEBUG("USDZExporter: Created SkelRoot structure for mesh: " + meshName);
    return skelRootPrim;
}

// ------------------------------------------------------------------------------------------------
// Convert mesh
void USDZExporter::ConvertMesh(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh) {
    if (!mesh) return;
    
    // Create bone name converter function that follows backup's pattern
    auto boneNameConverter = [this](const std::string& boneName) -> std::string {
        // Use the exact USD path from skeleton (critical for tinyusdz validation)
        auto pathIt = mBoneNameToUSDPath.find(boneName);
        if (pathIt != mBoneNameToUSDPath.end()) {
            return pathIt->second;
        }
        // Fallback to sanitized name (shouldn't happen if skeleton was built correctly)
        return mNameRegistry.Sanitize(boneName);
    };
    
                // Use MeshConverterPipeline for structured conversion with scene context
            MeshConverterPipeline pipeline(mesh, usdMesh, mScene, mNameRegistry, *mStage, this);
            pipeline.ExecuteFullPipeline(boneNameConverter);
            
            if (!pipeline.IsValid()) {
                ASSIMP_LOG_WARN("USDZExporter: Mesh conversion pipeline validation failed");
            }
            
            // Note: Blend shapes are now handled directly by the pipeline in ExportMeshes
            // No cross-function storage needed
}

// ------------------------------------------------------------------------------------------------
// MeshConverterPipeline implementation methods
void USDZExporter::MeshConverterPipeline::ExecuteVertexConversion() {
    std::vector<tinyusdz::value::point3f> points;
    points.reserve(mMesh->mNumVertices);
    
    for (uint32_t i = 0; i < mMesh->mNumVertices; ++i) {
        const aiVector3D& v = mMesh->mVertices[i];
        float x = v.x, y = v.y, z = v.z;
        if (!std::isfinite(x)) x = 0.0f;
        if (!std::isfinite(y)) y = 0.0f;
        if (!std::isfinite(z)) z = 0.0f;
        points.push_back({x, y, z});
    }
    
    mUsdMesh.points.set_value(std::move(points));
}

void USDZExporter::MeshConverterPipeline::ExecuteFaceConversion() {
    std::vector<int> faceVertexCounts;
    std::vector<int> faceVertexIndices;
    
    faceVertexCounts.reserve(mMesh->mNumFaces);
    faceVertexIndices.reserve(mMesh->mNumFaces * 3); // Assume mostly triangles
    
    for (uint32_t i = 0; i < mMesh->mNumFaces; ++i) {
        const aiFace& face = mMesh->mFaces[i];
        faceVertexCounts.push_back(static_cast<int>(face.mNumIndices));
        
        for (uint32_t j = 0; j < face.mNumIndices; ++j) {
            faceVertexIndices.push_back(static_cast<int>(face.mIndices[j]));
        }
    }
    
    mUsdMesh.faceVertexCounts.set_value(std::move(faceVertexCounts));
    mUsdMesh.faceVertexIndices.set_value(std::move(faceVertexIndices));
}

void USDZExporter::MeshConverterPipeline::ExecuteNormalConversion() {
    std::vector<tinyusdz::value::normal3f> normals;
    
    if (mMesh->mNormals) {
        // Use existing normals from mesh
        normals.reserve(mMesh->mNumVertices);
        for (uint32_t i = 0; i < mMesh->mNumVertices; ++i) {
            const aiVector3D& n = mMesh->mNormals[i];
            normals.push_back({n.x, n.y, n.z});
        }
        
        tinyusdz::Attribute normalAttr;
        normalAttr.set_value(std::move(normals));
        normalAttr.set_type_name("normal3f[]");
        
        tinyusdz::AttrMeta normalMeta;
        normalMeta.interpolation = tinyusdz::Interpolation::Vertex;
        normalAttr.metas() = normalMeta;
        
        tinyusdz::Property normalProp(normalAttr, false);
        mUsdMesh.props["normals"] = normalProp;
        
        ASSIMP_LOG_DEBUG("USDZExporter: Added " + std::to_string(normals.size()) + " vertex normals");
    } else {
        // Generate face-varying normals for triangular faces
        if (mMesh->mNumFaces > 0) {
            // Calculate face normals and expand to face-varying
            std::vector<tinyusdz::value::normal3f> faceVaryingNormals;
            faceVaryingNormals.reserve(mMesh->mNumFaces * 3); // Assuming triangular faces
            
            for (uint32_t faceIdx = 0; faceIdx < mMesh->mNumFaces; ++faceIdx) {
                const aiFace& face = mMesh->mFaces[faceIdx];
                if (face.mNumIndices >= 3) {
                    // Calculate face normal from first 3 vertices
                    const aiVector3D& v0 = mMesh->mVertices[face.mIndices[0]];
                    const aiVector3D& v1 = mMesh->mVertices[face.mIndices[1]];
                    const aiVector3D& v2 = mMesh->mVertices[face.mIndices[2]];
                    
                    aiVector3D edge1 = v1 - v0;
                    aiVector3D edge2 = v2 - v0;
                    aiVector3D faceNormal = edge1 ^ edge2; // Cross product
                    faceNormal.Normalize();
                    
                    // Add the same normal for each vertex in the face
                    for (uint32_t i = 0; i < face.mNumIndices; ++i) {
                        faceVaryingNormals.push_back({faceNormal.x, faceNormal.y, faceNormal.z});
                    }
                }
            }
            
            if (!faceVaryingNormals.empty()) {
                tinyusdz::Attribute normalAttr;
                normalAttr.set_value(std::move(faceVaryingNormals));
                normalAttr.set_type_name("normal3f[]");
                
                tinyusdz::AttrMeta normalMeta;
                normalMeta.interpolation = tinyusdz::Interpolation::FaceVarying;
                normalAttr.metas() = normalMeta;
                
                tinyusdz::Property normalProp(normalAttr, false);
                mUsdMesh.props["normals"] = normalProp;
                
                ASSIMP_LOG_DEBUG("USDZExporter: Generated " + std::to_string(faceVaryingNormals.size()) + " face-varying normals");
            }
        }
    }
}

void USDZExporter::MeshConverterPipeline::ExecuteUVConversion() {
    for (uint32_t uvIndex = 0; uvIndex < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++uvIndex) {
        if (!mMesh->mTextureCoords[uvIndex]) continue;
        
        std::vector<tinyusdz::value::float2> uvs;
        uvs.reserve(mMesh->mNumVertices);
        
        for (uint32_t i = 0; i < mMesh->mNumVertices; ++i) {
            const aiVector3D& uv = mMesh->mTextureCoords[uvIndex][i];
            // Note: USD uses V flipped compared to many formats
            uvs.emplace_back(tinyusdz::value::float2{uv.x, 1.0f - uv.y});
        }
        
        // Add as primvar
        std::string primvarName = (uvIndex == 0) ? "primvars:st" : ("primvars:st" + std::to_string(uvIndex));
        
        tinyusdz::Attribute uvAttr;
        uvAttr.set_value(uvs);
        uvAttr.set_type_name("texCoord2f[]");
        
        tinyusdz::AttrMeta uvMeta;
        uvMeta.interpolation = tinyusdz::Interpolation::Vertex;
        uvAttr.metas() = uvMeta;
        
        tinyusdz::Property uvProp(uvAttr, false);
        mUsdMesh.props[primvarName] = uvProp;
    }
}

void USDZExporter::MeshConverterPipeline::ExecuteVertexColorConversion() {
    if (!mMesh->mColors[0]) return; // Check if vertex colors exist
    
    // Only process the first color set
        std::vector<tinyusdz::value::color3f> colors;
    colors.reserve(mMesh->mNumVertices);
    
    for (uint32_t i = 0; i < mMesh->mNumVertices; ++i) {
        const aiColor4D& c = mMesh->mColors[0][i];
        colors.push_back({c.r, c.g, c.b});
    }
        
        tinyusdz::Attribute colorAttr;
    colorAttr.set_value(std::move(colors));
    colorAttr.set_type_name("color3f[]");
        
        tinyusdz::AttrMeta colorMeta;
        colorMeta.interpolation = tinyusdz::Interpolation::Vertex;
        colorAttr.metas() = colorMeta;
        
    std::string primvarName = "primvars:displayColor";
        tinyusdz::Property colorProp(colorAttr, false);
    mUsdMesh.props[primvarName] = colorProp;
}

void USDZExporter::MeshConverterPipeline::ExecuteTangentConversion() {
    if (!mMesh->mTangents) return; // Check if tangents exist
    
    std::vector<tinyusdz::value::vector3f> tangents;
    tangents.reserve(mMesh->mNumVertices);
    
    for (uint32_t i = 0; i < mMesh->mNumVertices; ++i) {
        const aiVector3D& t = mMesh->mTangents[i];
        tangents.push_back({t.x, t.y, t.z});
    }
    
    tinyusdz::Attribute tangentAttr;
    tangentAttr.set_value(std::move(tangents));
    tangentAttr.set_type_name("vector3f[]");
    
    tinyusdz::AttrMeta tangentMeta;
    tangentMeta.interpolation = tinyusdz::Interpolation::Vertex;
    tangentAttr.metas() = tangentMeta;
    
    tinyusdz::Property tangentProp(tangentAttr, false);
    mUsdMesh.props["primvars:tangents"] = tangentProp;
}

void USDZExporter::MeshConverterPipeline::ExecuteSkinningConversion(BoneNameConverter boneNameConverter) {
    // Only convert skinning if bones exist
    if (mMesh && mMesh->mNumBones > 0) {
        ASSIMP_LOG_DEBUG("USDZExporter: Adding skinning primvars for " + std::to_string(mMesh->mNumBones) + " bones");
        
        // Use global bone-to-skeleton-index mapping if available (set by ExportSkeletons),
        // otherwise fall back to local mesh bone ordering
        std::map<std::string, uint32_t> boneToSkeletonIndex;
        
        if (!mExporter->mGlobalBoneToSkeletonIndex.empty()) {
            // Use the global mapping for correct multi-skeleton joint index remapping
            boneToSkeletonIndex = mExporter->mGlobalBoneToSkeletonIndex;
        } else {
            // Fallback: local mesh bone ordering (single-skeleton case)
            for (uint32_t i = 0; i < mMesh->mNumBones; ++i) {
                boneToSkeletonIndex[mMesh->mBones[i]->mName.C_Str()] = i;
            }
        }
        
        // Determine maximum weights per vertex
        uint32_t maxWeightsPerVertex = 4;
        
        // Prepare joint indices and weights arrays
        std::vector<int> jointIndices(mMesh->mNumVertices * maxWeightsPerVertex, 0);
        std::vector<float> jointWeights(mMesh->mNumVertices * maxWeightsPerVertex, 0.0f);
        
        // Collect joint name tokens for this mesh's bones
        std::vector<tinyusdz::value::token> jointTokens;
        for (uint32_t i = 0; i < mMesh->mNumBones; ++i) {
            jointTokens.push_back(tinyusdz::value::token(mMesh->mBones[i]->mName.C_Str()));
        }
        
        // Process bone weights and fill joint data arrays using skeleton indices
        for (uint32_t boneIdx = 0; boneIdx < mMesh->mNumBones; ++boneIdx) {
            const aiBone* bone = mMesh->mBones[boneIdx];
            std::string boneName = bone->mName.C_Str();
            
            // ✅ FIX: Use skeleton joint index instead of original bone index
            auto skeletonIndexIt = boneToSkeletonIndex.find(boneName);
            if (skeletonIndexIt == boneToSkeletonIndex.end()) {
                ASSIMP_LOG_WARN("USDZExporter: Bone '" + boneName + "' not found in skeleton joint mapping, skipping");
                continue;
            }
            uint32_t skeletonJointIndex = skeletonIndexIt->second;
            
            for (uint32_t weightIdx = 0; weightIdx < bone->mNumWeights; ++weightIdx) {
                const aiVertexWeight& weight = bone->mWeights[weightIdx];
                uint32_t vertexIdx = weight.mVertexId;
                
                if (vertexIdx >= mMesh->mNumVertices) {
                    continue;
                }
                
                // Find an empty slot for this vertex
                bool added = false;
                for (uint32_t slotIdx = 0; slotIdx < maxWeightsPerVertex; ++slotIdx) {
                    uint32_t arrayIdx = vertexIdx * maxWeightsPerVertex + slotIdx;
                    
                    if (jointWeights[arrayIdx] == 0.0f) {
                        jointIndices[arrayIdx] = static_cast<int>(skeletonJointIndex);  // ✅ Use skeleton index
                        jointWeights[arrayIdx] = weight.mWeight;
                        added = true;
                        break;
                    }
                }
                
                if (!added) {
                    // Replace the smallest weight if this is larger
                    uint32_t minSlot = 0;
                    float minWeight = jointWeights[vertexIdx * maxWeightsPerVertex];
                    
                    for (uint32_t slotIdx = 1; slotIdx < maxWeightsPerVertex; ++slotIdx) {
                        uint32_t arrayIdx = vertexIdx * maxWeightsPerVertex + slotIdx;
                        if (jointWeights[arrayIdx] < minWeight) {
                            minWeight = jointWeights[arrayIdx];
                            minSlot = slotIdx;
                        }
                    }
                    
                    if (weight.mWeight > minWeight) {
                        uint32_t arrayIdx = vertexIdx * maxWeightsPerVertex + minSlot;
                        jointIndices[arrayIdx] = static_cast<int>(skeletonJointIndex);  // ✅ Use skeleton index
                        jointWeights[arrayIdx] = weight.mWeight;
                    }
                }
            }
        }
        
        // Normalize weights per vertex
        for (uint32_t vertexIdx = 0; vertexIdx < mMesh->mNumVertices; ++vertexIdx) {
            float totalWeight = 0.0f;
            uint32_t baseIdx = vertexIdx * maxWeightsPerVertex;
            
            for (uint32_t slotIdx = 0; slotIdx < maxWeightsPerVertex; ++slotIdx) {
                totalWeight += jointWeights[baseIdx + slotIdx];
            }
            
            if (totalWeight > 0.0f) {
                for (uint32_t slotIdx = 0; slotIdx < maxWeightsPerVertex; ++slotIdx) {
                    jointWeights[baseIdx + slotIdx] /= totalWeight;
                }
            }
        }
        
        // Create skel:jointWeights primvar using proper tinyusdz APIs
        tinyusdz::GeomPrimvar weightsPrimvar;
        weightsPrimvar.set_name("skel:jointWeights");
        weightsPrimvar.set_value(jointWeights);
        weightsPrimvar.set_elementSize(maxWeightsPerVertex);
        weightsPrimvar.set_interpolation(tinyusdz::Interpolation::Vertex);
        mUsdMesh.set_primvar(weightsPrimvar);
        
        // Create skel:jointIndices primvar using proper tinyusdz APIs  
        tinyusdz::GeomPrimvar indicesPrimvar;
        indicesPrimvar.set_name("skel:jointIndices");
        indicesPrimvar.set_value(jointIndices);
        indicesPrimvar.set_elementSize(maxWeightsPerVertex);
        indicesPrimvar.set_interpolation(tinyusdz::Interpolation::Vertex);
        mUsdMesh.set_primvar(indicesPrimvar);
        
        // Create skel:geomBindTransform primvar (identity matrix for now)
        tinyusdz::value::matrix4d geomBindTransform;
        tinyusdz::Identity(&geomBindTransform);
        tinyusdz::GeomPrimvar geomBindPrimvar;
        geomBindPrimvar.set_name("skel:geomBindTransform");
        geomBindPrimvar.set_value(geomBindTransform);
        mUsdMesh.set_primvar(geomBindPrimvar);
        
        // ✅ FIX: Add SkelBindingAPI to mesh with skeletal properties (USD schema requirement)
        // Meshes with skel: properties MUST have SkelBindingAPI applied
        // Note: This should be set on the final mesh prim, not here on the GeomMesh object
        // The calling code must apply this to the final Prim
        
        // skel:joints and skel:skeleton relationships are handled at the SkelRoot level
        
        ASSIMP_LOG_DEBUG("USDZExporter: Added skinning primvars to USD mesh: " + 
                         std::to_string(jointTokens.size()) + " joints, " +
                         std::to_string(maxWeightsPerVertex) + " weights per vertex");
    }
}

void USDZExporter::MeshConverterPipeline::ExecuteBlendShapeConversion() {
    if (!mMesh || mMesh->mNumAnimMeshes == 0) {
        return;
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Converting " + std::to_string(mMesh->mNumAnimMeshes) + " blend shapes");
    
    // Store created BlendShape prims for later reference
    std::vector<tinyusdz::Prim> blendShapePrims;
    // Clear any previous blend shape data
    mBlendShapeNames.clear();
    mBlendShapePrims.clear();
    
    // Process each animation mesh as a blend shape target
    for (uint32_t animMeshIdx = 0; animMeshIdx < mMesh->mNumAnimMeshes; ++animMeshIdx) {
        const aiAnimMesh* animMesh = mMesh->mAnimMeshes[animMeshIdx];
        if (!animMesh) continue;
        
        // Create BlendShape prim using proper tinyusdz APIs
        tinyusdz::BlendShape blendShape;
        
        std::string blendShapeName;
        if (animMesh->mName.length > 0) {
            blendShapeName = NameRegistry::Sanitize(animMesh->mName.C_Str());
        } else {
            blendShapeName = "target_" + std::to_string(animMeshIdx);
        }
        blendShape.name = blendShapeName;
        
        // Calculate offsets and collect affected vertices
        std::vector<tinyusdz::value::vector3f> offsets;
        std::vector<tinyusdz::value::vector3f> normalOffsets;
        std::vector<int> pointIndices;
        
        // Compare with base mesh to find vertex offsets
        if (animMesh->mVertices && mMesh->mVertices && mMesh->mNumVertices == animMesh->mNumVertices) {
            for (uint32_t vertIdx = 0; vertIdx < mMesh->mNumVertices; ++vertIdx) {
                const aiVector3D& baseVertex = mMesh->mVertices[vertIdx];
                const aiVector3D& animVertex = animMesh->mVertices[vertIdx];
                
                // Calculate vertex offset
                aiVector3D offset = animVertex - baseVertex;
                
                // Only include vertices with significant offsets
                const float threshold = 1e-6f;
                if (offset.SquareLength() > threshold) {
                    pointIndices.push_back(static_cast<int>(vertIdx));
                    
                    tinyusdz::value::vector3f usdOffset;
                    usdOffset[0] = offset.x;
                    usdOffset[1] = offset.y;
                    usdOffset[2] = offset.z;
                    offsets.push_back(usdOffset);
                }
            }
        }
        
        // Calculate normal offsets if available
        if (animMesh->mNormals && mMesh->mNormals && offsets.size() > 0) {
            normalOffsets.resize(pointIndices.size());
            
            for (size_t i = 0; i < pointIndices.size(); ++i) {
                uint32_t vertIdx = static_cast<uint32_t>(pointIndices[i]);
                if (vertIdx < mMesh->mNumVertices && vertIdx < animMesh->mNumVertices) {
                    const aiVector3D& baseNormal = mMesh->mNormals[vertIdx];
                    const aiVector3D& animNormal = animMesh->mNormals[vertIdx];
                    
                    aiVector3D normalOffset = animNormal - baseNormal;
                    
                    tinyusdz::value::vector3f usdNormalOffset;
                    usdNormalOffset[0] = normalOffset.x;
                    usdNormalOffset[1] = normalOffset.y;
                    usdNormalOffset[2] = normalOffset.z;
                    normalOffsets[i] = usdNormalOffset;
                }
            }
        }
        
        // Set BlendShape attributes using proper tinyusdz APIs
        if (!pointIndices.empty()) {
            blendShape.pointIndices.set_value(pointIndices);
            ASSIMP_LOG_DEBUG("USDZExporter: BlendShape " + blendShapeName + " point indices: " + std::to_string(pointIndices.size()));
        }
        
        if (!offsets.empty()) {
            blendShape.offsets.set_value(offsets);
            ASSIMP_LOG_DEBUG("USDZExporter: BlendShape " + blendShapeName + " offsets: " + std::to_string(offsets.size()));
        }
        
        if (!normalOffsets.empty()) {
            blendShape.normalOffsets.set_value(normalOffsets);
            ASSIMP_LOG_DEBUG("USDZExporter: BlendShape " + blendShapeName + " normal offsets: " + std::to_string(normalOffsets.size()));
        }
        
        // Store BlendShape prim to be added as mesh child later
        if (!pointIndices.empty() && !offsets.empty()) {
            auto blendShapePrim = std::make_unique<tinyusdz::Prim>(blendShape);
            mBlendShapePrims.emplace_back(std::move(blendShapePrim));
            
            // Store the actual created name for reference building
            mBlendShapeNames.push_back(blendShapeName);
            
            ASSIMP_LOG_DEBUG("USDZExporter: Created BlendShape prim: " + blendShapeName + " to be added as mesh child");
        }
    }
    
    // After creating all BlendShape prims, add skel:blendShapes property to mesh using stored names
    if (!mBlendShapeNames.empty()) {
        std::vector<tinyusdz::value::token> blendShapePathTokens;
        
        // Use the actual created names we stored during creation
        // BlendShapes will be children of the mesh, so use relative paths
        for (const std::string& blendShapeName : mBlendShapeNames) {
            blendShapePathTokens.push_back(tinyusdz::value::token(blendShapeName));
        }
    
        // Set skel:blendShapes property on the mesh with USD paths
        if (!blendShapePathTokens.empty()) {
            tinyusdz::Attribute blendShapesAttr;
            blendShapesAttr.set_value(blendShapePathTokens);
            blendShapesAttr.set_type_name("token[]");
            blendShapesAttr.variability() = tinyusdz::Variability::Uniform;
            
            tinyusdz::Property blendShapesProp(blendShapesAttr, false);
            mUsdMesh.props["skel:blendShapes"] = blendShapesProp;
            
            ASSIMP_LOG_DEBUG("USDZExporter: Added skel:blendShapes property with " + std::to_string(blendShapePathTokens.size()) + " USD path references");
        }
        
        // Add skel:blendShapeTargets relationships (absolute paths to blend shapes)
        std::vector<tinyusdz::Path> blendShapeTargetPaths;
        for (const std::string& blendShapeName : mBlendShapeNames) {
            // BlendShapes are children of this mesh - use relative path without "./"
            // USD expects just the child name for direct children
            blendShapeTargetPaths.emplace_back(blendShapeName, "");
        }
        
        tinyusdz::Relationship blendShapeTargetsRel;
        blendShapeTargetsRel.set(blendShapeTargetPaths);
        tinyusdz::Property blendShapeTargetsProp(blendShapeTargetsRel);
        mUsdMesh.props["skel:blendShapeTargets"] = blendShapeTargetsProp;
        
        // Add required skeletal binding properties for blend shapes
        // Add dummy joint indices (all vertices bound to joint 0)
        std::vector<int> jointIndices(mMesh->mNumVertices, 0);
        tinyusdz::Attribute jointIndicesAttr;
        jointIndicesAttr.set_value(jointIndices);
        jointIndicesAttr.set_type_name("int[]");
        
        tinyusdz::AttrMeta jointIndicesMeta;
        jointIndicesMeta.interpolation = tinyusdz::Interpolation::Vertex;
        jointIndicesMeta.elementSize = 1;
        jointIndicesAttr.metas() = jointIndicesMeta;
        
        tinyusdz::Property jointIndicesProp(jointIndicesAttr, false);
        mUsdMesh.props["primvars:skel:jointIndices"] = jointIndicesProp;
        
        // Add dummy joint weights (all vertices have weight 1.0)
        std::vector<float> jointWeights(mMesh->mNumVertices, 1.0f);
        tinyusdz::Attribute jointWeightsAttr;
        jointWeightsAttr.set_value(jointWeights);
        jointWeightsAttr.set_type_name("float[]");
        
        tinyusdz::AttrMeta jointWeightsMeta;
        jointWeightsMeta.interpolation = tinyusdz::Interpolation::Vertex;
        jointWeightsMeta.elementSize = 1;
        jointWeightsAttr.metas() = jointWeightsMeta;
        
        tinyusdz::Property jointWeightsProp(jointWeightsAttr, false);
        mUsdMesh.props["primvars:skel:jointWeights"] = jointWeightsProp;
        
        // Note: skel:skeleton relationship will be set later in CreateSkelRootForMesh 
        // where we have access to the full USD hierarchy context
        
        ASSIMP_LOG_DEBUG("USDZExporter: Added skeletal properties for blend shapes");
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Blend shape conversion completed");
}

// ------------------------------------------------------------------------------------------------
// Get complete mesh prim with blend shapes and skeletal properties
USDZExporter::BlendShapeResult USDZExporter::MeshConverterPipeline::GetCompleteMeshWithBlendShapes(tinyusdz::Prim&& baseMeshPrim) {
    // Since blend shapes are now created as separate root-level prims,
    // we just return the base mesh as-is. The skel:blendShapes property
    // has already been set during ExecuteBlendShapeConversion()
    return BlendShapeResult(std::move(baseMeshPrim), {});
}

void USDZExporter::MeshConverterPipeline::ExecuteAttributeConversion() {
    // Standard USD mesh attributes - these are required for proper USD mesh operation
    
    // 1. Set doubleSided attribute based on mesh properties
    bool doubleSided = false;
    
    // Check material for double-sided property if available
    if (mScene && mMesh->mMaterialIndex < mScene->mNumMaterials) {
        const aiMaterial* material = mScene->mMaterials[mMesh->mMaterialIndex];
        if (material) {
            int twoSided = 0;
            if (material->Get(AI_MATKEY_TWOSIDED, twoSided) == aiReturn_SUCCESS) {
                doubleSided = (twoSided != 0);
            }
        }
    }
    
    mUsdMesh.doubleSided.set_value(doubleSided);
    
    // 2. Set subdivisionScheme = "none" (standard for triangle meshes) - must be uniform
    tinyusdz::Attribute subdivisionSchemeAttr = tinyusdz::Attribute::Uniform(tinyusdz::value::token("none"));
    subdivisionSchemeAttr.set_type_name("token");
    tinyusdz::Property subdivisionSchemeProp(subdivisionSchemeAttr, false);
    mUsdMesh.props["subdivisionScheme"] = subdivisionSchemeProp;
    
    // 3. Set triangleSubdivisionRule = "none" - must be uniform  
    tinyusdz::Attribute triangleSubdivisionRuleAttr = tinyusdz::Attribute::Uniform(tinyusdz::value::token("none"));
    triangleSubdivisionRuleAttr.set_type_name("token");
    tinyusdz::Property triangleSubdivisionRuleProp(triangleSubdivisionRuleAttr, false);
    mUsdMesh.props["triangleSubdivisionRule"] = triangleSubdivisionRuleProp;
    
    // 4. Calculate and set extent (bounding box) - required for USD meshes
    if (mMesh->mNumVertices > 0) {
        auto clampF = [](float v) -> float { return std::isfinite(v) ? v : 0.0f; };
        aiVector3D v0 = mMesh->mVertices[0];
        aiVector3D minBounds(clampF(v0.x), clampF(v0.y), clampF(v0.z));
        aiVector3D maxBounds = minBounds;
        
        for (uint32_t i = 1; i < mMesh->mNumVertices; ++i) {
            const aiVector3D& vertex = mMesh->mVertices[i];
            float vx = clampF(vertex.x), vy = clampF(vertex.y), vz = clampF(vertex.z);
            minBounds.x = std::min(minBounds.x, vx);
            minBounds.y = std::min(minBounds.y, vy);
            minBounds.z = std::min(minBounds.z, vz);
            maxBounds.x = std::max(maxBounds.x, vx);
            maxBounds.y = std::max(maxBounds.y, vy);
            maxBounds.z = std::max(maxBounds.z, vz);
        }
        
        // Create extent array [min, max]
        std::vector<tinyusdz::value::float3> extent = {
            tinyusdz::value::float3{minBounds.x, minBounds.y, minBounds.z},
            tinyusdz::value::float3{maxBounds.x, maxBounds.y, maxBounds.z}
        };
        
        tinyusdz::Attribute extentAttr;
        extentAttr.set_value(extent);
        extentAttr.set_type_name("float3[]");
        tinyusdz::Property extentProp(extentAttr, false);
        mUsdMesh.props["extent"] = extentProp;
        
        ASSIMP_LOG_DEBUG("USDZExporter: Added extent property: min(" + 
                        std::to_string(minBounds.x) + ", " + std::to_string(minBounds.y) + ", " + std::to_string(minBounds.z) + 
                        ") max(" + std::to_string(maxBounds.x) + ", " + std::to_string(maxBounds.y) + ", " + std::to_string(maxBounds.z) + ")");
    }
}

// ------------------------------------------------------------------------------------------------
// Utility method to convert uint to string
std::string USDZExporter::ai_to_string(uint32_t value) const {
    return std::to_string(value);
}

// ------------------------------------------------------------------------------------------------
// Utility method to get file extension
std::string USDZExporter::GetFileExtension(const std::string& filename) const {
    size_t lastDot = filename.find_last_of('.');
    if (lastDot != std::string::npos && lastDot < filename.length() - 1) {
        return filename.substr(lastDot + 1);
    }
    return "";
}

// ------------------------------------------------------------------------------------------------
// Convert material
void USDZExporter::ConvertMaterial(const aiMaterial* mat, tinyusdz::Material& usdMaterial) {
    if (!mat) return;
    
    // Create UsdPreviewSurface shader
    tinyusdz::UsdPreviewSurface surface;
    CreatePreviewSurface(mat, surface);
    
    // Create shader prim
    tinyusdz::Shader shader;
    shader.value = surface;
    std::string shaderName = "PreviewSurface";
    shader.name = shaderName;
    
    // Set info:id to indicate this is a UsdPreviewSurface
    tinyusdz::Attribute infoIdAttr;
    infoIdAttr.set_value(std::string("UsdPreviewSurface"));
    tinyusdz::Property infoIdProp(infoIdAttr, false);
    shader.props["info:id"] = infoIdProp;
    
    // TODO: Add shader as child to material and set up connections
    // The tinyusdz Material API doesn't directly expose children() method
    // This may require a different approach to connect shaders to materials
    
    ASSIMP_LOG_DEBUG("USDZExporter: Material converted successfully");
}

// ------------------------------------------------------------------------------------------------
// Create preview surface
void USDZExporter::CreatePreviewSurface(const aiMaterial* mat, tinyusdz::UsdPreviewSurface& surface) {
    if (!mat) return;
    
    MapPBRProperties(mat, surface);
    
    if (mExportClearcoat) {
        MapClearcoatProperties(mat, surface);
    }
    
    MapTextureProperties(mat, surface);
}

// ------------------------------------------------------------------------------------------------
// Map PBR properties
void USDZExporter::MapPBRProperties(const aiMaterial* mat, tinyusdz::UsdPreviewSurface& surface) {
    // Detect unlit materials (KHR_materials_unlit)
    int shadingModel = 0;
    bool isUnlit = (mat->Get(AI_MATKEY_SHADING_MODEL, shadingModel) == AI_SUCCESS &&
                    shadingModel == aiShadingMode_Unlit);
    
    // Base color / Diffuse color
    aiColor3D baseColor(0.8f, 0.8f, 0.8f);
    if (mat->Get(AI_MATKEY_BASE_COLOR, baseColor) == AI_SUCCESS ||
        mat->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor) == AI_SUCCESS) {
        tinyusdz::value::color3f color{baseColor.r, baseColor.g, baseColor.b};
        surface.diffuseColor.set_value(color);
        
        if (isUnlit) {
            surface.emissiveColor.set_value(color);
            surface.metallic.set_value(0.0f);
            surface.roughness.set_value(1.0f);
            ASSIMP_LOG_DEBUG("USDZExporter: Unlit material - mapped baseColor to emissiveColor");
        }
    }
    
    // Specular workflow detection - check for specular factor, glossiness factor, or specular color
    float specularFactor = 1.0f;
    float glossinessFactor = 1.0f;
    aiColor3D specularColor(1.0f, 1.0f, 1.0f);
    
    bool hasSpecularFactor = mat->Get(AI_MATKEY_SPECULAR_FACTOR, specularFactor) == AI_SUCCESS;
    bool hasGlossinessFactor = mat->Get(AI_MATKEY_GLOSSINESS_FACTOR, glossinessFactor) == AI_SUCCESS;
    bool hasSpecularColor = mat->Get(AI_MATKEY_COLOR_SPECULAR, specularColor) == AI_SUCCESS;
    
    // Use specular workflow if any specular-related properties are present
    if (hasSpecularFactor || hasGlossinessFactor || hasSpecularColor) {
        surface.useSpecularWorkflow.set_value(1);
        
        auto clamp01 = [](float v) { return std::min(1.0f, std::max(0.0f, v)); };
        if (hasSpecularColor) {
            float r = specularColor.r, g = specularColor.g, b = specularColor.b;
            if (hasSpecularFactor) {
                r *= specularFactor; g *= specularFactor; b *= specularFactor;
            }
            tinyusdz::value::color3f clamped{clamp01(r), clamp01(g), clamp01(b)};
            surface.specularColor.set_value(clamped);
        } else if (hasSpecularFactor) {
            float v = clamp01(specularFactor);
            tinyusdz::value::color3f grayscaleSpecular{v, v, v};
            surface.specularColor.set_value(grayscaleSpecular);
        }
        
        // Convert glossiness to roughness if available (roughness = 1 - glossiness)
        if (hasGlossinessFactor) {
            float roughnessFromGlossiness = 1.0f - glossinessFactor;
            surface.roughness.set_value(roughnessFromGlossiness);
            ASSIMP_LOG_DEBUG("USDZExporter: Converted glossiness to roughness: " + std::to_string(roughnessFromGlossiness));
        }
        
        ASSIMP_LOG_DEBUG("USDZExporter: Set specular workflow - specularFactor: " + std::to_string(specularFactor) + 
                        ", glossinessFactor: " + std::to_string(glossinessFactor));
    }
    
    // Metallic factor
    float metallic = 0.0f;
    if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS) {
        surface.metallic.set_value(metallic);
    }
    
    // Roughness factor
    float roughness = 1.0f;
    if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS) {
        surface.roughness.set_value(roughness);
    } else {
        // Try to derive from shininess
        float shininess;
        if (mat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS) {
            // Convert shininess to roughness (approximate)
            surface.roughness.set_value(std::sqrt(2.0f / (shininess + 2.0f)));
        }
    }
    
    // Emissive color with intensity support
    aiColor3D emissive(0.0f, 0.0f, 0.0f);
    float emissiveIntensity = 1.0f;
    
    bool hasEmissiveColor = mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS;
    bool hasEmissiveIntensity = mat->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissiveIntensity) == AI_SUCCESS;
    
    if (hasEmissiveColor || hasEmissiveIntensity) {
        // Apply intensity scaling to emissive color
        if (hasEmissiveIntensity && emissiveIntensity != 1.0f) {
            emissive.r *= emissiveIntensity;
            emissive.g *= emissiveIntensity;
            emissive.b *= emissiveIntensity;
            ASSIMP_LOG_DEBUG("USDZExporter: Applied emissive intensity: " + std::to_string(emissiveIntensity));
        }
        
        tinyusdz::value::color3f color{emissive.r, emissive.g, emissive.b};
        surface.emissiveColor.set_value(color);
    }
    
    // Opacity
    float opacity = 1.0f;
    if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
        surface.opacity.set_value(opacity);
    }

    // Map glTF KHR_materials_transmission to UsdPreviewSurface opacity
    bool hasTransmission = false;
    float transmission = 0.0f;
    if (mat->Get(AI_MATKEY_TRANSMISSION_FACTOR, transmission) == AI_SUCCESS && transmission > 0.0f) {
        hasTransmission = true;
        float transmissionOpacity = 1.0f - transmission;
        // UsdPreviewSurface has no true refraction -- clamp minimum opacity
        // so glass surfaces are always visible as semi-transparent
        transmissionOpacity = std::max(transmissionOpacity, 0.1f);
        if (transmissionOpacity < opacity) {
            opacity = transmissionOpacity;
            surface.opacity.set_value(opacity);
        }
    }

    // Handle glTF alphaMode for proper transparency
    aiString alphaMode;
    if (mat->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS) {
        std::string mode(alphaMode.C_Str());
        if (mode == "MASK") {
            float alphaCutoff = 0.5f;
            mat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, alphaCutoff);
            surface.opacityThreshold.set_value(alphaCutoff);
        } else if (mode == "BLEND") {
            if (opacity >= 1.0f) {
                surface.opacity.set_value(0.99f);
                opacity = 0.99f;
            }
        }
    }

    // Opacity threshold (for masked transparency)
    float opacityThreshold = 0.0f;
    if (mat->Get(AI_MATKEY_TRANSPARENCYFACTOR, opacityThreshold) == AI_SUCCESS && opacityThreshold > 0.0f) {
        surface.opacityThreshold.set_value(opacityThreshold);
    }

    // Set opacityMode: "transparent" for glass/transmission/blend, threshold handles MASK
    if (opacity < 1.0f || hasTransmission) {
        surface.opacityMode.set_value(tinyusdz::UsdPreviewSurface::OpacityMode::Transparent);
    }
    
    // IOR (Index of Refraction)
    float ior = 1.5f;
    if (mat->Get(AI_MATKEY_REFRACTI, ior) == AI_SUCCESS) {
        surface.ior.set_value(ior);
    }
    
    // Note: useSpecularWorkflow is now set dynamically based on material properties above
    // Default metallic workflow (useSpecularWorkflow = 0) unless specular properties are detected
}

// ------------------------------------------------------------------------------------------------
// Map clearcoat properties
void USDZExporter::MapClearcoatProperties(const aiMaterial* mat, tinyusdz::UsdPreviewSurface& surface) {
    // Clearcoat factor
    float clearcoat = 0.0f;
    if (mat->Get(AI_MATKEY_CLEARCOAT_FACTOR, clearcoat) == AI_SUCCESS) {
        surface.clearcoat.set_value(clearcoat);
    }
    
    // Clearcoat roughness
    float clearcoatRoughness = 0.0f;
    if (mat->Get(AI_MATKEY_CLEARCOAT_ROUGHNESS_FACTOR, clearcoatRoughness) == AI_SUCCESS) {
        surface.clearcoatRoughness.set_value(clearcoatRoughness);
    }
}

// ------------------------------------------------------------------------------------------------
// Map texture properties with proper tinyusdz connections (using template system)
void USDZExporter::MapTextureProperties(const aiMaterial* mat, tinyusdz::UsdPreviewSurface& surface) {
    if (!mat) return;
    
    int shadingModel = 0;
    bool isUnlit = (mat->Get(AI_MATKEY_SHADING_MODEL, shadingModel) == AI_SUCCESS &&
                    shadingModel == aiShadingMode_Unlit);
    
    // Store texture shaders to be added as children later
    mCurrentMaterialTextureShaders.clear();
    mCurrentMaterialTextureTransforms.clear();
    mCurrentMaterialTextureUVIndices.clear();
    
    // Define texture configurations for all supported texture types
    // Using initializer list for optimal performance and readability
    std::vector<std::pair<TextureConfig, std::function<void()>>> textureConfigs = {
    // Base color / Diffuse texture (includes Maya support)
        {TextureConfig("diffuseColor", "rgb"), [&]() {
            TextureConfig config("diffuseColor", "rgb");
            config.fallbackTypes = {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE, aiTextureType_MAYA_BASE};
            aiColor3D baseColor(1.0f, 1.0f, 1.0f);
            if (mat->Get(AI_MATKEY_BASE_COLOR, baseColor) == AI_SUCCESS ||
                mat->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor) == AI_SUCCESS) {
                config.scale = {baseColor.r, baseColor.g, baseColor.b, 1.0f};
            }
            ProcessTextureProperty(mat, config, surface.diffuseColor, surface);
        }},
        
        // Normal texture (with special bias/scale for 8-bit normal maps)
        {TextureConfig("normal", "rgb"), [&]() {
            TextureConfig config("normal", "rgb");
            config.fallbackTypes = {aiTextureType_NORMALS, aiTextureType_HEIGHT};
            config.bias = {-1.0f, -1.0f, -1.0f, 0.0f};
            config.scale = {2.0f, 2.0f, 2.0f, 1.0f};
            ProcessTextureProperty(mat, config, surface.normal, surface);
        }},
        
        // Metallic texture (requires non-zero metallic factor)
        {TextureConfig("metallic", "b"), [&]() {
            TextureConfig config("metallic", "b");
            config.fallbackTypes = {aiTextureType_METALNESS, aiTextureType_GLTF_METALLIC_ROUGHNESS};
            config.requiresNonZeroFactor = true;
            config.factorKey = AI_MATKEY_METALLIC_FACTOR;
            ProcessTextureProperty(mat, config, surface.metallic, surface);
        }},
        
        // Roughness texture (with shininess fallback that needs inversion)
        {TextureConfig("roughness", "g"), [&]() {
            TextureConfig config("roughness", "g");
            config.fallbackTypes = {aiTextureType_DIFFUSE_ROUGHNESS, aiTextureType_MAYA_SPECULAR_ROUGHNESS, aiTextureType_SHININESS, aiTextureType_GLTF_METALLIC_ROUGHNESS};
            
            // Check if using shininess (needs inversion: roughness = 1 - glossiness)
            aiString shininessPath;
            if (mat->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &shininessPath) != AI_SUCCESS &&
                mat->GetTexture(aiTextureType_SHININESS, 0, &shininessPath) == AI_SUCCESS) {
                config.scale = {-1.0f, -1.0f, -1.0f, 1.0f};
                config.bias = {1.0f, 1.0f, 1.0f, 0.0f};
            }
            ProcessTextureProperty(mat, config, surface.roughness, surface);
        }},
        
        // Emissive texture (for unlit materials, also check diffuse textures)
        {TextureConfig("emissiveColor", "rgb"), [&]() {
            TextureConfig config("emissiveColor", "rgb");
            config.fallbackTypes = {aiTextureType_EMISSIVE, aiTextureType_EMISSION_COLOR};
            if (isUnlit) {
                config.fallbackTypes.push_back(aiTextureType_BASE_COLOR);
                config.fallbackTypes.push_back(aiTextureType_DIFFUSE);
            }
            aiColor3D emissive(1.0f, 1.0f, 1.0f);
            if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
                if (emissive.r != 0.0f || emissive.g != 0.0f || emissive.b != 0.0f) {
                    config.scale = {emissive.r, emissive.g, emissive.b, 1.0f};
                }
            }
            ProcessTextureProperty(mat, config, surface.emissiveColor, surface);
        }},
        
        // Occlusion texture intentionally skipped: Hydra Storm renders the entire
        // material as black when inputs:occlusion is connected to a UsdUVTexture.
        // The UsdPreviewSurface default (1.0 = fully lit) is acceptable.
        
        // Opacity texture
        {TextureConfig("opacity", "a"), [&]() {
            TextureConfig config("opacity", "a");
            config.fallbackTypes = {aiTextureType_OPACITY};
            ProcessTextureProperty(mat, config, surface.opacity, surface);
        }},
        
        // Displacement texture
        {TextureConfig("displacement", "r"), [&]() {
            TextureConfig config("displacement", "r");
            config.fallbackTypes = {aiTextureType_DISPLACEMENT, aiTextureType_HEIGHT};
            ProcessTextureProperty(mat, config, surface.displacement, surface);
        }},
        
        // Specular texture (triggers specular workflow when present)
        {TextureConfig("specularColor", "rgb"), [&]() {
            TextureConfig config("specularColor", "rgb");
            config.fallbackTypes = {aiTextureType_SPECULAR, aiTextureType_MAYA_SPECULAR, aiTextureType_MAYA_SPECULAR_COLOR};
            if (ProcessTextureProperty(mat, config, surface.specularColor, surface)) {
                // Switch to specular workflow when specular texture is present
                surface.useSpecularWorkflow.set_value(1);
                ASSIMP_LOG_DEBUG("USDZExporter: Switched to specular workflow due to specular texture");
            }
        }},
        
        // Clearcoat texture (requires clearcoat export enabled)
        {TextureConfig("clearcoat", "r"), [&]() {
            if (!mExportClearcoat) return;
            
            aiString clearcoatTexPath;
            if (mat->GetTexture(AI_MATKEY_CLEARCOAT_TEXTURE, &clearcoatTexPath) == AI_SUCCESS) {
                tinyusdz::UsdUVTexture clearcoatTexture = CreateUVTexture(clearcoatTexPath.C_Str(), "clearcoat", mat, aiTextureType_CLEARCOAT, 0);
                std::string texShaderPath = mCurrentMaterialPath + "/clearcoat";
                tinyusdz::Path connPath(std::move(texShaderPath), "outputs:r");
                surface.clearcoat.set_connection(std::move(connPath));
                surface.clearcoat.set_value_empty();
                
                aiUVTransform uvTransform;
                if (mat->Get(AI_MATKEY_UVTRANSFORM(aiTextureType_CLEARCOAT, 0), uvTransform) == AI_SUCCESS) {
                    if (uvTransform.mScaling.x != 1.0f || uvTransform.mScaling.y != 1.0f ||
                        uvTransform.mTranslation.x != 0.0f || uvTransform.mTranslation.y != 0.0f ||
                        uvTransform.mRotation != 0.0f) {
                        mCurrentMaterialTextureTransforms["clearcoat"] = uvTransform;
                    }
                }
                int uvIndex = 0;
                mat->Get(AI_MATKEY_UVWSRC(aiTextureType_CLEARCOAT, 0), uvIndex);
                mCurrentMaterialTextureUVIndices["clearcoat"] = uvIndex;
                
                mCurrentMaterialTextureShaders.emplace_back("clearcoat", std::move(clearcoatTexture));
                ASSIMP_LOG_DEBUG("USDZExporter: Connected clearcoat texture: " + std::string(clearcoatTexPath.C_Str()) + " (UV" + std::to_string(uvIndex) + ")");
            }
        }},
        
        // Clearcoat roughness texture (requires clearcoat export enabled)
        {TextureConfig("clearcoatRoughness", "g"), [&]() {
            if (!mExportClearcoat) return;
            
            aiString clearcoatRoughnessTexPath;
            if (mat->GetTexture(AI_MATKEY_CLEARCOAT_ROUGHNESS_TEXTURE, &clearcoatRoughnessTexPath) == AI_SUCCESS) {
                tinyusdz::UsdUVTexture clearcoatRoughnessTexture = CreateUVTexture(clearcoatRoughnessTexPath.C_Str(), "clearcoatRoughness", mat, aiTextureType_CLEARCOAT, 1);
                std::string texShaderPath = mCurrentMaterialPath + "/clearcoatRoughness";
                tinyusdz::Path connPath(std::move(texShaderPath), "outputs:g");
                surface.clearcoatRoughness.set_connection(std::move(connPath));
                surface.clearcoatRoughness.set_value_empty();
                
                aiUVTransform uvTransform;
                if (mat->Get(AI_MATKEY_UVTRANSFORM(aiTextureType_CLEARCOAT, 1), uvTransform) == AI_SUCCESS) {
                    if (uvTransform.mScaling.x != 1.0f || uvTransform.mScaling.y != 1.0f ||
                        uvTransform.mTranslation.x != 0.0f || uvTransform.mTranslation.y != 0.0f ||
                        uvTransform.mRotation != 0.0f) {
                        mCurrentMaterialTextureTransforms["clearcoatRoughness"] = uvTransform;
                    }
                }
                int uvIndex = 0;
                mat->Get(AI_MATKEY_UVWSRC(aiTextureType_CLEARCOAT, 1), uvIndex);
                mCurrentMaterialTextureUVIndices["clearcoatRoughness"] = uvIndex;
                
                mCurrentMaterialTextureShaders.emplace_back("clearcoatRoughness", std::move(clearcoatRoughnessTexture));
                ASSIMP_LOG_DEBUG("USDZExporter: Connected clearcoat roughness texture: " + std::string(clearcoatRoughnessTexPath.C_Str()) + " (UV" + std::to_string(uvIndex) + ")");
            }
        }}
    };
    
    
    // Execute all texture processing with exception safety
    for (const auto& [config, processor] : textureConfigs) {
        try {
            processor();
        } catch (const std::exception& e) {
            ASSIMP_LOG_WARN("USDZExporter: Failed to process " + config.paramName + " texture: " + e.what());
        }
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: All texture properties processed with template system");
}

// ------------------------------------------------------------------------------------------------
// ShaderBuilder template implementation (needs to be in CPP due to incomplete types in header)
template<typename ShaderType>
tinyusdz::Shader USDZExporter::ShaderBuilder::CreateShader(const std::string& name, const std::string& infoId, ShaderType&& shaderValue) {
    tinyusdz::Shader shader;
    shader.name = name;
    shader.info_id = infoId;
    shader.value = std::forward<ShaderType>(shaderValue);
    return shader;
}

// ------------------------------------------------------------------------------------------------
// ShaderBuilder implementation for consistent shader creation
tinyusdz::Shader USDZExporter::ShaderBuilder::CreateSurfaceShader(tinyusdz::UsdPreviewSurface&& surface) {
    tinyusdz::Shader surfaceShader;
    surfaceShader.name = "UsdPreviewSurface";
    surfaceShader.info_id = tinyusdz::kUsdPreviewSurface;
    
    // Set outputs using move semantics for performance
    surface.outputsSurface.set_authored(true);
    surface.outputsDisplacement.set_authored(true);
    surfaceShader.value = std::move(surface);
    
    return surfaceShader;
}

tinyusdz::Material USDZExporter::ShaderBuilder::CreateMaterial(const std::string& name, const std::string& surfaceShaderName) {
    tinyusdz::Material usdMaterial;
    usdMaterial.name = name;
    
    // Create material surface and displacement connections using proper tinyusdz Path API
    std::string shaderPath = mMaterialPath + "/" + surfaceShaderName;
    usdMaterial.surface.set(tinyusdz::Path(shaderPath, "outputs:surface"));
    usdMaterial.displacement.set(tinyusdz::Path(shaderPath, "outputs:displacement"));
    
    return usdMaterial;
}

// ------------------------------------------------------------------------------------------------
// PrimFactory template implementation (needs to be in CPP due to incomplete types in header)
template<typename PrimType>
tinyusdz::Prim USDZExporter::PrimFactory::CreatePrim(PrimType&& primData) {
    return tinyusdz::Prim(std::forward<PrimType>(primData));
}

template<typename PrimType>
void USDZExporter::PrimFactory::AddChildPrim(tinyusdz::Prim& parent, PrimType&& primData) {
    parent.children().emplace_back(CreatePrim(std::forward<PrimType>(primData)));
}

template<typename... PrimTypes>
void USDZExporter::PrimFactory::AddChildren(tinyusdz::Prim& parent, PrimTypes&&... prims) {
    (parent.children().emplace_back(CreatePrim(std::forward<PrimTypes>(prims))), ...);
}

// ------------------------------------------------------------------------------------------------
// PrimFactory static method implementations
tinyusdz::Prim USDZExporter::PrimFactory::CreateScope(const std::string& name) {
    tinyusdz::Scope scope;
    scope.name = name;
    return tinyusdz::Prim(std::move(scope));
}

tinyusdz::Prim USDZExporter::PrimFactory::CreateXform(const std::string& name) {
    tinyusdz::Xform xform;
    xform.name = name;
    return tinyusdz::Prim(std::move(xform));
}

// ------------------------------------------------------------------------------------------------
// Create UV texture shader using tinyusdz APIs
tinyusdz::UsdUVTexture USDZExporter::CreateUVTexture(const std::string& filePath, const std::string& paramName, 
                                                   const aiMaterial* mat, aiTextureType textureType, unsigned int texSlot) {
    tinyusdz::UsdUVTexture uvTexture;
    
    // Set name for the texture shader
    uvTexture.name = paramName; // Use paramName directly (e.g., "clearcoat", "diffuseColor")
    
    // Handle texture file path using canonical Assimp pattern
    const aiTexture* embeddedTexture = mScene->GetEmbeddedTexture(filePath.c_str());
    if (embeddedTexture != nullptr) {
        // Texture is embedded in memory - write it directly
        HandleEmbeddedTexture(filePath, uvTexture);
    } else {
        // External texture reference - use path as-is
        HandleExternalTexture(filePath, uvTexture);
    }
    
    // Add source color space
    AddSourceColorSpace(uvTexture, paramName);
    
    // Set wrap modes
    
    auto ConvertAssimpWrapToUSD = [](aiTextureMapMode assimpWrap) -> tinyusdz::UsdUVTexture::Wrap {
        switch (assimpWrap) {
            case aiTextureMapMode_Clamp:
                return tinyusdz::UsdUVTexture::Wrap::Clamp;
            case aiTextureMapMode_Mirror:
                return tinyusdz::UsdUVTexture::Wrap::Mirror;
            case aiTextureMapMode_Wrap:
            case aiTextureMapMode_Decal:
            default:
                return tinyusdz::UsdUVTexture::Wrap::Repeat;
        }
    };
    
    // Read wrap modes from material properties (set by importers like glTF)
    aiTextureMapMode wrapU = aiTextureMapMode_Wrap; // Default fallback
    aiTextureMapMode wrapV = aiTextureMapMode_Wrap; // Default fallback
    
    if (mat) {
        mat->Get(AI_MATKEY_MAPPINGMODE_U(textureType, texSlot), (int&)wrapU);
        mat->Get(AI_MATKEY_MAPPINGMODE_V(textureType, texSlot), (int&)wrapV);
    }
    
    // Convert to USD wrap modes
    tinyusdz::UsdUVTexture::Wrap usdWrapS = ConvertAssimpWrapToUSD(wrapU);
    tinyusdz::UsdUVTexture::Wrap usdWrapT = ConvertAssimpWrapToUSD(wrapV);
    
    ASSIMP_LOG_DEBUG("USDZExporter: Programmatic wrap modes - U: " + std::to_string((int)wrapU) + 
                    " -> " + (usdWrapS == tinyusdz::UsdUVTexture::Wrap::Clamp ? "clamp" : 
                             usdWrapS == tinyusdz::UsdUVTexture::Wrap::Mirror ? "mirror" : "repeat") +
                    ", V: " + std::to_string((int)wrapV) + 
                    " -> " + (usdWrapT == tinyusdz::UsdUVTexture::Wrap::Clamp ? "clamp" : 
                             usdWrapT == tinyusdz::UsdUVTexture::Wrap::Mirror ? "mirror" : "repeat"));
    
    uvTexture.wrapS.set_value(usdWrapS);
    uvTexture.wrapT.set_value(usdWrapT);
    
    // Add appropriate outputs based on texture type
    AddTextureOutputs(uvTexture, paramName);
    
    return uvTexture;
}

// ------------------------------------------------------------------------------------------------
// Template-based texture processing system for eliminating duplication
template<typename SurfacePropertyType>
bool USDZExporter::ProcessTextureProperty(const aiMaterial* mat, const TextureConfig& config, 
                                          SurfacePropertyType& surfaceProperty, tinyusdz::UsdPreviewSurface& surface) {
    if (!mat) return false;
    
    // Check material factor if required
    if (config.requiresNonZeroFactor && !config.factorKey.empty()) {
        float factor = 0.0f;
        if (mat->Get(config.factorKey.c_str(), 0, 0, factor) == AI_SUCCESS && factor <= 0.0f) {
            return false; // Skip if factor is zero or negative
        }
    }
    
    // Try each fallback type in order until we find a texture
    aiString texturePath;
    aiTextureType foundType = aiTextureType_NONE;
    for (aiTextureType type : config.fallbackTypes) {
        if (mat->GetTexture(type, 0, &texturePath) == AI_SUCCESS) {
            foundType = type;
            break;
        }
    }
    
    if (foundType == aiTextureType_NONE) {
        return false; // No texture found
    }
    
    // Create UV texture with optimized construction (programmatic wrap mode support)
    tinyusdz::UsdUVTexture uvTexture = CreateUVTexture(texturePath.C_Str(), config.paramName, mat, foundType, 0);
    
    // Read texture transform data from material using proper matrix-based coordinate conversion
    aiUVTransform uvTransform;
    if (mat->Get(AI_MATKEY_UVTRANSFORM(foundType, 0), uvTransform) == AI_SUCCESS) {
        // Check if transform is non-identity
        if (uvTransform.mScaling.x != 1.0f || uvTransform.mScaling.y != 1.0f ||
            uvTransform.mTranslation.x != 0.0f || uvTransform.mTranslation.y != 0.0f ||
            uvTransform.mRotation != 0.0f) {
            mCurrentMaterialTextureTransforms[config.paramName] = uvTransform;
            ASSIMP_LOG_DEBUG("USDZExporter: Stored non-identity texture transform for " + config.paramName);
        } else {
            ASSIMP_LOG_DEBUG("USDZExporter: Skipping identity texture transform for " + config.paramName);
        }
    } else {
        ASSIMP_LOG_DEBUG("USDZExporter: No texture transform found for " + config.paramName);
    }
    
    // Apply texture-specific transformations
    if (config.bias[0] != 0.0f || config.bias[1] != 0.0f || config.bias[2] != 0.0f || config.bias[3] != 0.0f) {
        uvTexture.bias.set_value(tinyusdz::value::float4{config.bias[0], config.bias[1], config.bias[2], config.bias[3]});
    }
    if (config.scale[0] != 1.0f || config.scale[1] != 1.0f || config.scale[2] != 1.0f || config.scale[3] != 1.0f) {
        uvTexture.scale.set_value(tinyusdz::value::float4{config.scale[0], config.scale[1], config.scale[2], config.scale[3]});
    }
    
    // Create connection path using move semantics for performance
    std::string texShaderPath = mCurrentMaterialPath + "/" + config.paramName;
    tinyusdz::Path connPath(std::move(texShaderPath), "outputs:" + config.outputChannel);
    
    // Connect to surface property and clear default value
    surfaceProperty.set_connection(std::move(connPath));
    surfaceProperty.set_value_empty();
    
    // Read UV channel index (glTF texCoord override)
    int uvIndex = 0;
    mat->Get(AI_MATKEY_UVWSRC(foundType, 0), uvIndex);
    mCurrentMaterialTextureUVIndices[config.paramName] = uvIndex;
    
    // Store texture shader for later addition
    mCurrentMaterialTextureShaders.emplace_back(config.paramName, std::move(uvTexture));
    
    ASSIMP_LOG_DEBUG("USDZExporter: Connected " + config.paramName + " texture: " + texturePath.C_Str() + " (UV" + std::to_string(uvIndex) + ")");
    return true;
}

// ------------------------------------------------------------------------------------------------
// Handle embedded texture using tinyusdz APIs
void USDZExporter::HandleEmbeddedTexture(const std::string& texPath, tinyusdz::UsdUVTexture& uvTexture) {
    if (texPath.empty() || texPath[0] != '*') {
        ASSIMP_LOG_WARN("USDZExporter: Invalid embedded texture path: " + texPath);
        return;
    }
    
    try {
        int textureIndex = std::stoi(texPath.substr(1));
        if (textureIndex < 0 || static_cast<uint32_t>(textureIndex) >= mScene->mNumTextures) {
            ASSIMP_LOG_WARN("USDZExporter: Embedded texture index out of range: " + ai_to_string(textureIndex));
            return;
        }
        
        std::string textureName = GetEmbeddedTextureFilename(textureIndex);
        std::string texturePath = "./textures/" + textureName;
        tinyusdz::value::AssetPath assetPath(texturePath);
        uvTexture.file.set_value(assetPath);
        
        ASSIMP_LOG_DEBUG("USDZExporter: Prepared embedded texture for USDZ: " + texPath + " -> " + textureName);
        
    } catch (const std::exception& e) {
        ASSIMP_LOG_ERROR("USDZExporter: Error processing embedded texture " + texPath + ": " + e.what());
    }
}

// ------------------------------------------------------------------------------------------------
// Decode percent-encoded URI sequences (e.g. %20 -> space)
static std::string UrlDecode(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            auto fromHex = [](char ch) -> unsigned char {
                if (ch >= '0' && ch <= '9') return static_cast<unsigned char>(ch - '0');
                if (ch >= 'a' && ch <= 'f') return static_cast<unsigned char>(ch - 'a' + 10);
                if (ch >= 'A' && ch <= 'F') return static_cast<unsigned char>(ch - 'A' + 10);
                return 0;
            };
            unsigned char hi = fromHex(str[i + 1]);
            unsigned char lo = fromHex(str[i + 2]);
            result += static_cast<char>((hi << 4) | lo);
            i += 2;
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

// ------------------------------------------------------------------------------------------------
// Handle external texture using tinyusdz APIs
void USDZExporter::HandleExternalTexture(const std::string& texPath, tinyusdz::UsdUVTexture& uvTexture) {
    // glTF stores URIs with percent-encoding (e.g. %20 for spaces); decode before use
    std::string decodedPath = UrlDecode(texPath);

    std::string filename = decodedPath;
    size_t lastSlash = decodedPath.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        filename = decodedPath.substr(lastSlash + 1);
    }
    
    std::string sanitizedFilename = SanitizeFilename(filename);
    std::string texturePath = "./textures/" + sanitizedFilename;
    tinyusdz::value::AssetPath assetPath(texturePath);
    uvTexture.file.set_value(assetPath);
    
    // Store decoded path for file I/O (the actual filename on disk)
    std::string usdzKey = "textures/" + sanitizedFilename;
    mExternalTexturePaths[usdzKey] = decodedPath;
    
    ASSIMP_LOG_DEBUG("USDZExporter: Prepared external texture: " + texPath + " -> " + sanitizedFilename);
}

// ------------------------------------------------------------------------------------------------
// Create centralized SkelAnimation (data-driven approach)
void USDZExporter::CreateCentralizedSkelAnimation() {
    if (mScene->mNumAnimations == 0) {
        ASSIMP_LOG_DEBUG("USDZExporter: No animations to process for SkelAnimation");
        return;
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Creating centralized SkelAnimation inside Skeleton prim");
    
    // Find the Skeleton prim inside SkelRoot to add SkelAnimation as its child
    tinyusdz::Prim* skeletonPrimPtr = nullptr;
    tinyusdz::Prim* parentPrim = mSkeletonParentPrim;
    if (!parentPrim) parentPrim = FindSkeletonParentPrim();
    if (parentPrim) {
        for (auto& child : parentPrim->children()) {
            if (child.as<tinyusdz::SkelRoot>()) {
                for (auto& skelChild : child.children()) {
                    if (skelChild.as<tinyusdz::Skeleton>()) {
                        skeletonPrimPtr = &skelChild;
                        break;
                    }
                }
                break;
            }
        }
    }
    
    if (!skeletonPrimPtr) {
        ASSIMP_LOG_ERROR("USDZExporter: Could not find Skeleton prim inside SkelRoot for animation placement");
        return;
    }
    
    // PHASE 2: Collect bone data (reuse logic from ExportSkeletons)
    std::set<std::string> allBoneNames;
    std::map<std::string, const aiBone*> boneDataMap;
    std::map<std::string, uint32_t> boneIndexMap;
    
    for (uint32_t meshIdx = 0; meshIdx < mScene->mNumMeshes; ++meshIdx) {
        const aiMesh* mesh = mScene->mMeshes[meshIdx];
        if (mesh->HasBones()) {
            for (uint32_t boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
                const aiBone* bone = mesh->mBones[boneIdx];
                std::string boneName = bone->mName.C_Str();
                allBoneNames.insert(boneName);
                boneDataMap[boneName] = bone;
            }
        }
    }
    
    // Build scene node hierarchy first (same approach as skeleton export)
    NodeHierarchyMapping nodeMapping = BuildSceneNodeHierarchy();
    
    // Build joint paths using skeleton bone ordering (same as skeleton export)
    JointPathMapping jointPaths = BuildJointPathsFromNodeHierarchy(nodeMapping, allBoneNames, boneDataMap);
    
    // Create bone index mapping based on skeleton joint order (for consistent alignment)
    for (size_t i = 0; i < jointPaths.skeletonJointNames.size(); ++i) {
        boneIndexMap[jointPaths.skeletonJointNames[i]] = i;
    }
    
    // PHASE 3: Process first animation to create SkelAnimation
    const aiAnimation* anim = mScene->mAnimations[0];
    std::string animationName = "Animation";
    if (anim->mName.length > 0) {
        animationName = SanitizeName(anim->mName.C_Str());
    }
    
    // Convert string animation joint paths to tokens
    std::vector<tinyusdz::value::token> animationJointTokens;
    for (const std::string& jointPath : jointPaths.animationJoints) {
        animationJointTokens.push_back(tinyusdz::value::token(jointPath));
    }
    
    tinyusdz::SkelAnimation skelAnim;
    skelAnim.name = animationName;
    skelAnim.joints.set_value(animationJointTokens);  // Use hierarchical animation joint paths
    
    // PHASE 4: Generate per-frame time samples with interpolation
    double ticksPerSecond = anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 24.0;
    double usdFrameRate = 24.0;
    
    // Compute total frame count from animation duration
    double durationInSeconds = anim->mDuration / ticksPerSecond;
    int totalFrames = std::max(1, static_cast<int>(std::round(durationInSeconds * usdFrameRate)));
    
    // Generate one time sample per frame for smooth interpolation
    std::set<double> allTimeKeys;
    for (int f = 1; f <= totalFrames; ++f) {
        allTimeKeys.insert(static_cast<double>(f));
    }
    
    // PHASE 5: Create time-sampled animation data aligned with animation joint ordering
    std::map<double, std::vector<tinyusdz::value::quatf>> rotationsByTime;
    std::map<double, std::vector<tinyusdz::value::float3>> translationsByTime;
    std::map<double, std::vector<tinyusdz::value::half3>> scalesByTime;
    
    // Create mapping from bone name to animation joint index 
    // Since animation joints use identical ordering as skeleton joints, map bones to their skeleton positions
    ASSIMP_LOG_DEBUG("USDZExporter: Creating bone to animation joint mapping for " + std::to_string(jointPaths.animationJoints.size()) + " animation joints");
    
    std::map<std::string, uint32_t> boneToAnimJointIndex;
    
    // Build mapping: since animation and skeleton have identical ordering, find bones' positions in animationJoints
    for (size_t animIdx = 0; animIdx < jointPaths.animationJoints.size(); ++animIdx) {
        const std::string& animJointPath = jointPaths.animationJoints[animIdx];
        
        // Find the bone name that corresponds to this animation joint path
        for (const auto& pair : jointPaths.boneToJointPath) {
            if (pair.second == animJointPath) {
                boneToAnimJointIndex[pair.first] = static_cast<uint32_t>(animIdx);
                ASSIMP_LOG_DEBUG("USDZExporter: Bone '" + pair.first + "' -> animation joint index " + std::to_string(animIdx) + " (path: '" + animJointPath + "')");
                break;
            }
        }
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: ANIM_CHECK: Processing " + std::to_string(anim->mNumChannels) + " animation channels for " + std::to_string(allTimeKeys.size()) + " frame keys");
    
    for (double frameNumber : allTimeKeys) {
        double timeInSeconds = frameNumber / usdFrameRate;
        std::vector<tinyusdz::value::quatf> rotations(jointPaths.animationJoints.size());
        std::vector<tinyusdz::value::float3> translations(jointPaths.animationJoints.size());
        std::vector<tinyusdz::value::half3> scales(jointPaths.animationJoints.size());
        
        // Initialize with identity values
        for (size_t i = 0; i < jointPaths.animationJoints.size(); ++i) {
            rotations[i].real = 1.0f;
            rotations[i].imag = {0.0f, 0.0f, 0.0f}; // Identity quaternion
            translations[i] = {0.0f, 0.0f, 0.0f};
            scales[i] = {
                tinyusdz::value::float_to_half_full(1.0f),
                tinyusdz::value::float_to_half_full(1.0f),
                tinyusdz::value::float_to_half_full(1.0f)
            };
        }
        
        // Fill in actual animation data using linear interpolation between keyframes
        double animTimeTicks = timeInSeconds * ticksPerSecond;
        
        for (uint32_t chanIdx = 0; chanIdx < anim->mNumChannels; ++chanIdx) {
            const aiNodeAnim* nodeAnim = anim->mChannels[chanIdx];
            if (!nodeAnim) continue;
            std::string boneName = nodeAnim->mNodeName.C_Str();
            
            auto animJointIt = boneToAnimJointIndex.find(boneName);
            if (animJointIt != boneToAnimJointIndex.end()) {
                uint32_t boneIdx = animJointIt->second;
                
                // Interpolate rotations (slerp)
                if (nodeAnim->mNumRotationKeys > 0) {
                    if (nodeAnim->mNumRotationKeys == 1 || animTimeTicks <= nodeAnim->mRotationKeys[0].mTime) {
                        const auto& q = nodeAnim->mRotationKeys[0].mValue;
                        rotations[boneIdx].real = q.w;
                        rotations[boneIdx].imag = {q.x, q.y, q.z};
                    } else if (animTimeTicks >= nodeAnim->mRotationKeys[nodeAnim->mNumRotationKeys - 1].mTime) {
                        const auto& q = nodeAnim->mRotationKeys[nodeAnim->mNumRotationKeys - 1].mValue;
                        rotations[boneIdx].real = q.w;
                        rotations[boneIdx].imag = {q.x, q.y, q.z};
                    } else {
                        // Find bracketing keyframes and interpolate
                        for (uint32_t k = 0; k < nodeAnim->mNumRotationKeys - 1; ++k) {
                            if (animTimeTicks >= nodeAnim->mRotationKeys[k].mTime && 
                                animTimeTicks < nodeAnim->mRotationKeys[k + 1].mTime) {
                                double dt = nodeAnim->mRotationKeys[k + 1].mTime - nodeAnim->mRotationKeys[k].mTime;
                                float t = (dt > 0) ? static_cast<float>((animTimeTicks - nodeAnim->mRotationKeys[k].mTime) / dt) : 0.0f;
                                
                                aiQuaternion result;
                                aiQuaternion::Interpolate(result, nodeAnim->mRotationKeys[k].mValue, 
                                                          nodeAnim->mRotationKeys[k + 1].mValue, t);
                                result.Normalize();
                                rotations[boneIdx].real = result.w;
                                rotations[boneIdx].imag = {result.x, result.y, result.z};
                                break;
                            }
                        }
                    }
                }
                
                // Interpolate translations (lerp)
                if (nodeAnim->mNumPositionKeys > 0) {
                    if (nodeAnim->mNumPositionKeys == 1 || animTimeTicks <= nodeAnim->mPositionKeys[0].mTime) {
                        const auto& p = nodeAnim->mPositionKeys[0].mValue;
                        translations[boneIdx] = {p.x, p.y, p.z};
                    } else if (animTimeTicks >= nodeAnim->mPositionKeys[nodeAnim->mNumPositionKeys - 1].mTime) {
                        const auto& p = nodeAnim->mPositionKeys[nodeAnim->mNumPositionKeys - 1].mValue;
                        translations[boneIdx] = {p.x, p.y, p.z};
                    } else {
                        for (uint32_t k = 0; k < nodeAnim->mNumPositionKeys - 1; ++k) {
                            if (animTimeTicks >= nodeAnim->mPositionKeys[k].mTime && 
                                animTimeTicks < nodeAnim->mPositionKeys[k + 1].mTime) {
                                double dt = nodeAnim->mPositionKeys[k + 1].mTime - nodeAnim->mPositionKeys[k].mTime;
                                float t = (dt > 0) ? static_cast<float>((animTimeTicks - nodeAnim->mPositionKeys[k].mTime) / dt) : 0.0f;
                                const auto& p0 = nodeAnim->mPositionKeys[k].mValue;
                                const auto& p1 = nodeAnim->mPositionKeys[k + 1].mValue;
                                translations[boneIdx] = {p0.x + t * (p1.x - p0.x), p0.y + t * (p1.y - p0.y), p0.z + t * (p1.z - p0.z)};
                                break;
                            }
                        }
                    }
                }
                
                // Interpolate scales (lerp)
                if (nodeAnim->mNumScalingKeys > 0) {
                    aiVector3D scaledVal;
                    if (nodeAnim->mNumScalingKeys == 1 || animTimeTicks <= nodeAnim->mScalingKeys[0].mTime) {
                        scaledVal = nodeAnim->mScalingKeys[0].mValue;
                    } else if (animTimeTicks >= nodeAnim->mScalingKeys[nodeAnim->mNumScalingKeys - 1].mTime) {
                        scaledVal = nodeAnim->mScalingKeys[nodeAnim->mNumScalingKeys - 1].mValue;
                    } else {
                        scaledVal = nodeAnim->mScalingKeys[0].mValue;
                        for (uint32_t k = 0; k < nodeAnim->mNumScalingKeys - 1; ++k) {
                            if (animTimeTicks >= nodeAnim->mScalingKeys[k].mTime && 
                                animTimeTicks < nodeAnim->mScalingKeys[k + 1].mTime) {
                                double dt = nodeAnim->mScalingKeys[k + 1].mTime - nodeAnim->mScalingKeys[k].mTime;
                                float t = (dt > 0) ? static_cast<float>((animTimeTicks - nodeAnim->mScalingKeys[k].mTime) / dt) : 0.0f;
                                const auto& s0 = nodeAnim->mScalingKeys[k].mValue;
                                const auto& s1 = nodeAnim->mScalingKeys[k + 1].mValue;
                                scaledVal = {s0.x + t * (s1.x - s0.x), s0.y + t * (s1.y - s0.y), s0.z + t * (s1.z - s0.z)};
                                break;
                            }
                        }
                    }
                    
                    scales[boneIdx] = {
                        tinyusdz::value::float_to_half_full(scaledVal.x),
                        tinyusdz::value::float_to_half_full(scaledVal.y),
                        tinyusdz::value::float_to_half_full(scaledVal.z)
                    };
                }
            }
        }
        
        rotationsByTime[frameNumber] = rotations;
        translationsByTime[frameNumber] = translations;
        scalesByTime[frameNumber] = scales;
    }
    
    // PHASE 6: Set time-sampled attributes on SkelAnimation
    tinyusdz::Animatable<std::vector<tinyusdz::value::quatf>> animRotations;
    for (const auto& timePair : rotationsByTime) {
        ASSIMP_LOG_DEBUG("USDZExporter: ANIM_FINAL: Adding time sample " + std::to_string(timePair.first) + 
                         " with " + std::to_string(timePair.second.size()) + " rotations");
        
        // Log first few non-identity rotations for verification
        for (size_t i = 0; i < std::min(size_t(5), timePair.second.size()); ++i) {
            const auto& rot = timePair.second[i];
            if (rot.real != 1.0f || rot.imag[0] != 0.0f || rot.imag[1] != 0.0f || rot.imag[2] != 0.0f) {
                ASSIMP_LOG_DEBUG("USDZExporter: ANIM_FINAL_QUAT[" + std::to_string(i) + "]: (" + 
                                 std::to_string(rot.real) + ", " + std::to_string(rot.imag[0]) + ", " + 
                                 std::to_string(rot.imag[1]) + ", " + std::to_string(rot.imag[2]) + ")");
            }
        }
        
        animRotations.add_sample(timePair.first, timePair.second);
    }
    skelAnim.rotations = animRotations;
    
    tinyusdz::Animatable<std::vector<tinyusdz::value::float3>> animTranslations;
    for (const auto& timePair : translationsByTime) {
        animTranslations.add_sample(timePair.first, timePair.second);
    }
    skelAnim.translations = animTranslations;
    
    tinyusdz::Animatable<std::vector<tinyusdz::value::half3>> animScales;
    for (const auto& timePair : scalesByTime) {
        animScales.add_sample(timePair.first, timePair.second);
    }
    skelAnim.scales = animScales;
    
    // Add SkelAnimation as child of Skeleton prim (inside SkelRoot)
    tinyusdz::Prim skelAnimPrim(skelAnim);
    skeletonPrimPtr->children().emplace_back(std::move(skelAnimPrim));
    
    ASSIMP_LOG_DEBUG("USDZExporter: Created centralized SkelAnimation '" + animationName + "' with " +
                     std::to_string(allBoneNames.size()) + " joints and " + 
                     std::to_string(allTimeKeys.size()) + " time samples");
}

// ------------------------------------------------------------------------------------------------
// Recursively find a prim by element name in a prim's subtree
tinyusdz::Prim* USDZExporter::FindPrimByNameRecursive(tinyusdz::Prim& prim, const std::string& name) {
    if (prim.element_name() == name) return &prim;
    for (auto& child : prim.children()) {
        if (auto* result = FindPrimByNameRecursive(child, name)) return result;
    }
    return nullptr;
}

// ------------------------------------------------------------------------------------------------
// Find a prim anywhere in the stage hierarchy by element name
tinyusdz::Prim* USDZExporter::FindPrimByName(const std::string& name) {
    for (auto& rootPrim : mStage->root_prims()) {
        if (auto* result = FindPrimByNameRecursive(rootPrim, name)) return result;
    }
    return nullptr;
}

// ------------------------------------------------------------------------------------------------
// Convert animation using tinyusdz XformOp time sampling APIs
void USDZExporter::ConvertAnimation(const aiAnimation* anim) {
    if (!anim) return;
    
    ASSIMP_LOG_DEBUG("USDZExporter: Converting animation: " + std::string(anim->mName.C_Str()));
    
    // Convert ticks to USD frame numbers (matching stage timeCodesPerSecond = 24)
    double ticksPerSecond = anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 24.0;
    double frameRate = 24.0;
    double timeScale = frameRate / ticksPerSecond;
    
    // Process node animation channels (transform animations)
    for (uint32_t i = 0; i < anim->mNumChannels; ++i) {
        const aiNodeAnim* nodeAnim = anim->mChannels[i];
        if (!nodeAnim) continue;
        
        std::string rawNodeName = nodeAnim->mNodeName.C_Str();
        
        // Find the aiNode to look up its USD prim name
        const aiNode* targetNode = FindNodeByName(mScene->mRootNode, rawNodeName);
        if (!targetNode) {
            ASSIMP_LOG_WARN("USDZExporter: Could not find node '" + rawNodeName + "' for animation channel");
            continue;
        }
        
        // Get the actual USD prim name from our node-to-name mapping
        auto nodeIt = mNodeIdMap.find(targetNode);
        if (nodeIt == mNodeIdMap.end()) {
            ASSIMP_LOG_WARN("USDZExporter: Node '" + rawNodeName + "' was not exported as a prim (possibly a bone node)");
            continue;
        }
        const std::string& usdPrimName = nodeIt->second;
        
        // Find the existing prim in the stage hierarchy
        tinyusdz::Prim* targetPrim = FindPrimByName(usdPrimName);
        if (!targetPrim) {
            ASSIMP_LOG_WARN("USDZExporter: Could not find prim '" + usdPrimName + "' in stage for animation channel '" + rawNodeName + "'");
            continue;
        }
        
        auto* xformData = targetPrim->as<tinyusdz::Xform>();
        if (!xformData) {
            ASSIMP_LOG_WARN("USDZExporter: Prim '" + usdPrimName + "' is not an Xform, cannot apply animation");
            continue;
        }
        
        // Build animated XformOps
        std::vector<tinyusdz::XformOp> xformOps;
        
        if (nodeAnim->mNumPositionKeys > 0) {
            tinyusdz::XformOp translateOp;
            translateOp.op_type = tinyusdz::XformOp::OpType::Translate;
            
            for (uint32_t j = 0; j < nodeAnim->mNumPositionKeys; ++j) {
                const aiVectorKey& key = nodeAnim->mPositionKeys[j];
                double time = key.mTime * timeScale;
                tinyusdz::value::double3 position;
                position[0] = key.mValue.x;
                position[1] = key.mValue.y;
                position[2] = key.mValue.z;
                translateOp.set_timesample(time, position);
            }
            xformOps.push_back(translateOp);
        }
        
        if (nodeAnim->mNumRotationKeys > 0) {
            tinyusdz::XformOp rotateOp;
            rotateOp.op_type = tinyusdz::XformOp::OpType::Orient;
            
            for (uint32_t j = 0; j < nodeAnim->mNumRotationKeys; ++j) {
                const aiQuatKey& key = nodeAnim->mRotationKeys[j];
                double time = key.mTime * timeScale;
                tinyusdz::value::quatf rotation;
                rotation[0] = key.mValue.x;
                rotation[1] = key.mValue.y;
                rotation[2] = key.mValue.z;
                rotation[3] = key.mValue.w;
                rotateOp.set_timesample(time, rotation);
            }
            xformOps.push_back(rotateOp);
        }
        
        if (nodeAnim->mNumScalingKeys > 0) {
            tinyusdz::XformOp scaleOp;
            scaleOp.op_type = tinyusdz::XformOp::OpType::Scale;
            
            for (uint32_t j = 0; j < nodeAnim->mNumScalingKeys; ++j) {
                const aiVectorKey& key = nodeAnim->mScalingKeys[j];
                double time = key.mTime * timeScale;
                tinyusdz::value::double3 scale;
                scale[0] = key.mValue.x;
                scale[1] = key.mValue.y;
                scale[2] = key.mValue.z;
                scaleOp.set_timesample(time, scale);
            }
            xformOps.push_back(scaleOp);
        }
        
        // Replace the existing prim's static XformOps with animated ones
        const_cast<tinyusdz::Xform*>(xformData)->xformOps = xformOps;
        
        ASSIMP_LOG_DEBUG("USDZExporter: Applied animation to existing prim '" + usdPrimName + "' (" +
                         ai_to_string(nodeAnim->mNumPositionKeys) + " pos, " +
                         ai_to_string(nodeAnim->mNumRotationKeys) + " rot, " +
                         ai_to_string(nodeAnim->mNumScalingKeys) + " scale keyframes)");
    }
    
    // Process morph mesh animation channels (blend shape weight animations)
    for (uint32_t i = 0; i < anim->mNumMorphMeshChannels; ++i) {
        const aiMeshMorphAnim* morphAnim = anim->mMorphMeshChannels[i];
        if (!morphAnim) continue;
        
        std::string targetName = SanitizeName(morphAnim->mName.C_Str());
        ASSIMP_LOG_DEBUG("USDZExporter: Converting morph animation for target: " + targetName);
        
        // Resolve target name to actual mesh name using node-to-mesh mapping
        std::string actualMeshName;
        
        // Check if target is directly a mesh name
        for (const auto& meshEntry : mMeshIdMap) {
            if (meshEntry.second == targetName) {
                actualMeshName = targetName;
                break;
            }
        }
        
        // If not found, check if target is a node name that contains a mesh
        if (actualMeshName.empty()) {
            for (const auto& nodeEntry : mNodeIdMap) {
                if (nodeEntry.second == targetName) {
                    // Found the target node, now find the mesh it contains
                    const aiNode* targetNode = nodeEntry.first;
                    if (targetNode && targetNode->mNumMeshes > 0 && targetNode->mMeshes[0] < mScene->mNumMeshes) {
                        const aiMesh* mesh = mScene->mMeshes[targetNode->mMeshes[0]];
                        if (mesh) {
                            // Find the USD mesh name from our mapping
                            auto meshIt = mMeshIdMap.find(mesh);
                            if (meshIt != mMeshIdMap.end()) {
                                actualMeshName = meshIt->second;
                                ASSIMP_LOG_DEBUG("USDZExporter: Resolved target node '" + targetName + "' to mesh '" + actualMeshName + "'");
                                break;
                            }
                        }
                    }
                }
            }
        }
        
        if (actualMeshName.empty()) {
            ASSIMP_LOG_WARN("USDZExporter: Could not resolve morph animation target '" + targetName + "' to any mesh");
            continue;
        }
        
        ASSIMP_LOG_DEBUG("USDZExporter: Processing morph animation for mesh: " + actualMeshName);
        
        // Update the existing SkelAnimation in the skeleton with morph animation data
        UpdateSkelAnimationWithMorphData(morphAnim, actualMeshName, timeScale, anim->mName.C_Str());
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Animation conversion completed for " + ai_to_string(anim->mNumChannels) + " node channels and " + ai_to_string(anim->mNumMorphMeshChannels) + " morph channels");
}

// ------------------------------------------------------------------------------------------------
// Export property animations (KHR_animation_pointer: material color, visibility, UV transforms)
void USDZExporter::ExportPropertyAnimations(const aiAnimation* anim) {
    if (!anim || anim->mNumPropertyChannels == 0) return;
    
    double ticksPerSecond = anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 1000.0;
    double frameRate = 24.0;
    double timeScale = frameRate / ticksPerSecond;
    
    ASSIMP_LOG_DEBUG("USDZExporter: Processing " + ai_to_string(anim->mNumPropertyChannels) + " property animation channels");
    
    for (uint32_t i = 0; i < anim->mNumPropertyChannels; ++i) {
        const aiPropertyAnim* propAnim = anim->mPropertyChannels[i];
        if (!propAnim || propAnim->mNumKeys == 0) continue;
        
        std::string targetPath = propAnim->mTargetPath.C_Str();
        std::string targetProp = propAnim->mTargetProperty.C_Str();
        
        if (propAnim->mTargetType == aiPropertyAnimTarget_MATERIAL_COLOR) {
            // Find the target material's shader prim in the stage
            uint32_t matIdx = propAnim->mTargetIndex;
            if (matIdx >= mScene->mNumMaterials) {
                ASSIMP_LOG_WARN("USDZExporter: Property animation targets material index " + ai_to_string(matIdx) + " but only " + ai_to_string(mScene->mNumMaterials) + " materials exist");
                continue;
            }
            
            const aiMaterial* mat = mScene->mMaterials[matIdx];
            auto matIt = mMaterialIdMap.find(mat);
            if (matIt == mMaterialIdMap.end()) {
                ASSIMP_LOG_WARN("USDZExporter: Could not find USD material for index " + ai_to_string(matIdx));
                continue;
            }
            
            std::string matName = matIt->second;
            std::string shaderPrimName = "UsdPreviewSurface";
            
            // Find the shader prim: /SceneName/Materials/MatName/UsdPreviewSurface
            tinyusdz::Prim* shaderPrim = nullptr;
            tinyusdz::Prim* matPrim = FindPrimByName(matName);
            if (matPrim) {
                for (auto& child : matPrim->children()) {
                    if (child.element_name() == shaderPrimName) {
                        shaderPrim = &child;
                        break;
                    }
                }
            }
            
            if (!shaderPrim) {
                ASSIMP_LOG_WARN("USDZExporter: Could not find shader prim for material '" + matName + "'");
                continue;
            }
            
            auto* shaderData = shaderPrim->as<tinyusdz::Shader>();
            if (!shaderData) {
                ASSIMP_LOG_WARN("USDZExporter: Prim for material '" + matName + "' is not a Shader");
                continue;
            }
            
            // Get the UsdPreviewSurface from the shader's value
            auto* surfacePtr = shaderData->value.as<tinyusdz::UsdPreviewSurface>();
            if (!surfacePtr) {
                ASSIMP_LOG_WARN("USDZExporter: Shader for material '" + matName + "' is not a UsdPreviewSurface");
                continue;
            }
            
            tinyusdz::UsdPreviewSurface* surface = const_cast<tinyusdz::UsdPreviewSurface*>(surfacePtr);
            
            if (targetProp == "baseColorFactor" || targetProp == "diffuseColor") {
                tinyusdz::Animatable<tinyusdz::value::color3f> animColor;
                for (uint32_t k = 0; k < propAnim->mNumKeys; ++k) {
                    double timeCode = propAnim->mKeys[k].mTime * timeScale;
                    tinyusdz::value::color3f color{
                        propAnim->mKeys[k].mValues[0],
                        propAnim->mKeys[k].mValues[1],
                        propAnim->mKeys[k].mValues[2]
                    };
                    if (k == 0) animColor.set_default(color);
                    animColor.add_sample(timeCode, color);
                }
                surface->diffuseColor.set_value(animColor);
                ASSIMP_LOG_DEBUG("USDZExporter: Applied diffuseColor animation to material '" + matName + "' with " + ai_to_string(propAnim->mNumKeys) + " keyframes");
            } else if (targetProp == "emissiveFactor" || targetProp == "emissiveColor") {
                tinyusdz::Animatable<tinyusdz::value::color3f> animColor;
                for (uint32_t k = 0; k < propAnim->mNumKeys; ++k) {
                    double timeCode = propAnim->mKeys[k].mTime * timeScale;
                    tinyusdz::value::color3f color{
                        propAnim->mKeys[k].mValues[0],
                        propAnim->mKeys[k].mValues[1],
                        propAnim->mKeys[k].mValues[2]
                    };
                    if (k == 0) animColor.set_default(color);
                    animColor.add_sample(timeCode, color);
                }
                surface->emissiveColor.set_value(animColor);
                ASSIMP_LOG_DEBUG("USDZExporter: Applied emissiveColor animation to material '" + matName + "' with " + ai_to_string(propAnim->mNumKeys) + " keyframes");
            }
        } else if (propAnim->mTargetType == aiPropertyAnimTarget_VISIBILITY) {
            // Find the target node's prim
            uint32_t nodeIdx = propAnim->mTargetIndex;
            const aiNode* targetNode = nullptr;
            
            // Walk the scene tree to find node by glTF index
            std::function<const aiNode*(const aiNode*, uint32_t&)> findByIndex = [&](const aiNode* node, uint32_t& currentIdx) -> const aiNode* {
                if (currentIdx == nodeIdx) return node;
                ++currentIdx;
                for (uint32_t c = 0; c < node->mNumChildren; ++c) {
                    auto* result = findByIndex(node->mChildren[c], currentIdx);
                    if (result) return result;
                }
                return nullptr;
            };
            uint32_t idx = 0;
            targetNode = findByIndex(mScene->mRootNode, idx);
            
            if (!targetNode) {
                ASSIMP_LOG_WARN("USDZExporter: Could not find node at index " + ai_to_string(nodeIdx) + " for visibility animation");
                continue;
            }
            
            auto nodeIt = mNodeIdMap.find(targetNode);
            if (nodeIt == mNodeIdMap.end()) continue;
            
            tinyusdz::Prim* targetPrim = FindPrimByName(nodeIt->second);
            if (!targetPrim) continue;
            
            auto* xformData = targetPrim->as<tinyusdz::Xform>();
            if (!xformData) continue;
            
            tinyusdz::Animatable<tinyusdz::Visibility> animVis;
            for (uint32_t k = 0; k < propAnim->mNumKeys; ++k) {
                double timeCode = propAnim->mKeys[k].mTime * timeScale;
                auto vis = propAnim->mKeys[k].mValues[0] > 0.5f ? tinyusdz::Visibility::Inherited : tinyusdz::Visibility::Invisible;
                if (k == 0) animVis.set_default(vis);
                animVis.add_sample(timeCode, vis);
            }
            const_cast<tinyusdz::Xform*>(xformData)->visibility.set_value(animVis);
            ASSIMP_LOG_DEBUG("USDZExporter: Applied visibility animation to node '" + nodeIt->second + "' with " + ai_to_string(propAnim->mNumKeys) + " keyframes");
        } else if (propAnim->mTargetType == aiPropertyAnimTarget_TEXTURE_TRANSFORM) {
            ASSIMP_LOG_DEBUG("USDZExporter: Texture transform animation for material " + ai_to_string(propAnim->mTargetIndex) + " property '" + targetProp + "' (support pending)");
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Convert skeletal animation using tinyusdz SkelAnimation APIs
void USDZExporter::ConvertSkeletalAnimation(const aiAnimation* anim) {
    if (!anim) return;
    
    ASSIMP_LOG_DEBUG("USDZExporter: Converting skeletal animation: " + std::string(anim->mName.C_Str()));
    
    // Create SkelRoot as container
    tinyusdz::SkelRoot skelRoot;
    skelRoot.name = SanitizeName(anim->mName.C_Str()) + "_SkelRoot";
    
    // Create Skeleton
    tinyusdz::Skeleton skeleton;
    skeleton.name = SanitizeName(anim->mName.C_Str()) + "_Skeleton";
    
    // Create SkelAnimation
    tinyusdz::SkelAnimation skelAnim;
    skelAnim.name = SanitizeName(anim->mName.C_Str()) + "_SkelAnim";
    
    // Collect joint information
    std::vector<std::string> jointNames;
    std::vector<tinyusdz::value::token> jointTokens;
    std::vector<tinyusdz::value::matrix4d> bindTransforms;
    std::vector<tinyusdz::value::matrix4d> restTransforms;
    
    // Process each animation channel as a joint
    for (uint32_t i = 0; i < anim->mNumChannels; ++i) {
        const aiNodeAnim* nodeAnim = anim->mChannels[i];
        std::string jointName = SanitizeName(nodeAnim->mNodeName.C_Str());
        
        jointNames.push_back(jointName);
        jointTokens.push_back(tinyusdz::value::token(jointName));
        
        // Create identity bind and rest transforms (should be computed from scene hierarchy)
        tinyusdz::value::matrix4d bindTransform;
        tinyusdz::value::matrix4d restTransform;
        tinyusdz::Identity(&bindTransform);
        tinyusdz::Identity(&restTransform);
        
        bindTransforms.push_back(bindTransform);
        restTransforms.push_back(restTransform);
    }
    
    // Set skeleton attributes
    skeleton.joints.set_value(jointTokens);
    skeleton.jointNames.set_value(jointTokens);
    skeleton.bindTransforms.set_value(bindTransforms);
    skeleton.restTransforms.set_value(restTransforms);
    
    // Prepare animation data arrays
    std::vector<std::vector<tinyusdz::value::quatf>> allRotations(jointNames.size());
    std::vector<std::vector<tinyusdz::value::float3>> allTranslations(jointNames.size());  
    std::vector<std::vector<tinyusdz::value::float3>> allScales(jointNames.size());
    
    // Convert time scale
    double timeScale = 1.0 / (anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 24.0);
    
    // Process animation keyframes
    for (uint32_t i = 0; i < anim->mNumChannels; ++i) {
        const aiNodeAnim* nodeAnim = anim->mChannels[i];
        
        // Process rotation keyframes
        if (nodeAnim->mNumRotationKeys > 0) {
            for (uint32_t j = 0; j < nodeAnim->mNumRotationKeys; ++j) {
                const aiQuatKey& key = nodeAnim->mRotationKeys[j];
                tinyusdz::value::quatf rotation;
                rotation[0] = key.mValue.x;
                rotation[1] = key.mValue.y;
                rotation[2] = key.mValue.z;
                rotation[3] = key.mValue.w;
                allRotations[i].push_back(rotation);
            }
        }
        
        // Process translation keyframes
        if (nodeAnim->mNumPositionKeys > 0) {
            for (uint32_t j = 0; j < nodeAnim->mNumPositionKeys; ++j) {
                const aiVectorKey& key = nodeAnim->mPositionKeys[j];
                tinyusdz::value::float3 translation;
                translation[0] = key.mValue.x;
                translation[1] = key.mValue.y;
                translation[2] = key.mValue.z;
                allTranslations[i].push_back(translation);
            }
        }
        
        // Process scale keyframes
        if (nodeAnim->mNumScalingKeys > 0) {
            for (uint32_t j = 0; j < nodeAnim->mNumScalingKeys; ++j) {
                const aiVectorKey& key = nodeAnim->mScalingKeys[j];
                tinyusdz::value::float3 scale;
                scale[0] = key.mValue.x;
                scale[1] = key.mValue.y;
                scale[2] = key.mValue.z;
                allScales[i].push_back(scale);
            }
        }
    }
    
    // Set SkelAnimation attributes
    skelAnim.joints.set_value(jointTokens);
    
    // Create time-sampled attributes
    if (!allRotations.empty() && !allRotations[0].empty()) {
        tinyusdz::Animatable<std::vector<tinyusdz::value::quatf>> animRotations;
        
        // Set default value (first keyframe)
        std::vector<tinyusdz::value::quatf> firstRotFrame;
        for (const auto& jointRots : allRotations) {
            if (!jointRots.empty()) {
                firstRotFrame.push_back(jointRots[0]);
            }
        }
        animRotations.set_default(firstRotFrame);
        
        // Add time samples for each keyframe
        for (uint32_t i = 0; i < anim->mNumChannels; ++i) {
            const aiNodeAnim* nodeAnim = anim->mChannels[i];
            for (uint32_t j = 0; j < nodeAnim->mNumRotationKeys; ++j) {
                const aiQuatKey& key = nodeAnim->mRotationKeys[j];
                double time = key.mTime * timeScale;
                
                // Create full rotation frame for all joints at this time
                std::vector<tinyusdz::value::quatf> rotFrame(jointNames.size());
                for (size_t k = 0; k < jointNames.size(); ++k) {
                    if (k == i && !allRotations[k].empty()) {
                        rotFrame[k] = allRotations[k][j % allRotations[k].size()];
                    } else {
                        rotFrame[k] = firstRotFrame[k]; // Use default for other joints
                    }
                }
                animRotations.add_sample(time, rotFrame);
            }
        }
        
        skelAnim.rotations.set_value(animRotations);
    }
    
    if (!allTranslations.empty() && !allTranslations[0].empty()) {
        tinyusdz::Animatable<std::vector<tinyusdz::value::float3>> animTranslations;
        
        // Set default value (first keyframe) 
        std::vector<tinyusdz::value::float3> firstTransFrame;
        for (const auto& jointTrans : allTranslations) {
            if (!jointTrans.empty()) {
                firstTransFrame.push_back(jointTrans[0]);
            }
        }
        animTranslations.set_default(firstTransFrame);
        
        // Add time samples for each keyframe
        for (uint32_t i = 0; i < anim->mNumChannels; ++i) {
            const aiNodeAnim* nodeAnim = anim->mChannels[i];
            for (uint32_t j = 0; j < nodeAnim->mNumPositionKeys; ++j) {
                const aiVectorKey& key = nodeAnim->mPositionKeys[j];
                double time = key.mTime * timeScale;
                
                // Create full translation frame for all joints at this time
                std::vector<tinyusdz::value::float3> transFrame(jointNames.size());
                for (size_t k = 0; k < jointNames.size(); ++k) {
                    if (k == i && !allTranslations[k].empty()) {
                        transFrame[k] = allTranslations[k][j % allTranslations[k].size()];
                    } else {
                        transFrame[k] = firstTransFrame[k]; // Use default for other joints
                    }
                }
                animTranslations.add_sample(time, transFrame);
            }
        }
        
        skelAnim.translations.set_value(animTranslations);
    }
    
    // Create Prims and establish hierarchy
    tinyusdz::Prim skelRootPrim(skelRoot);
    tinyusdz::Prim skeletonPrim(skeleton);
    tinyusdz::Prim skelAnimPrim(skelAnim);
    
    // Add skeleton and animation as children of SkelRoot
    skelRootPrim.children().emplace_back(std::move(skeletonPrim));
    skelRootPrim.children().emplace_back(std::move(skelAnimPrim));
    
    // Add SkelRoot under skeleton parent prim
    tinyusdz::Prim* skelParentPrim = mSkeletonParentPrim;
    if (!skelParentPrim) skelParentPrim = FindSkeletonParentPrim();
    if (skelParentPrim) {
        skelParentPrim->children().emplace_back(std::move(skelRootPrim));
        ASSIMP_LOG_DEBUG("USDZExporter: Added SkelRoot under skeleton parent prim");
    } else {
        ASSIMP_LOG_ERROR("USDZExporter: Could not find parent prim for SkelRoot placement");
        return;
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Skeletal animation conversion completed for " + 
                     ai_to_string(jointNames.size()) + " joints");
}

// ------------------------------------------------------------------------------------------------
// Update existing SkelAnimation with morph target animation data
void USDZExporter::UpdateSkelAnimationWithMorphData(const aiMeshMorphAnim* morphAnim, 
                                                    const std::string& meshName, 
                                                    double timeScale, 
                                                    const char* animationName) {
    if (!morphAnim || morphAnim->mNumKeys == 0) {
        return;
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Updating SkelAnimation with morph data for mesh: " + meshName);
    
    // Find the SkelRoot that contains this mesh and update its SkelAnimation
    std::function<bool(tinyusdz::Prim&)> findAndUpdateSkelAnimation = [&](tinyusdz::Prim& prim) -> bool {
        // Look for SkelRoot containing our mesh
        if (prim.prim_type_name() == "SkelRoot") {
            // Check if this SkelRoot contains our mesh
            bool containsMesh = false;
            std::function<bool(const tinyusdz::Prim&)> checkForMesh = [&](const tinyusdz::Prim& p) -> bool {
                if (p.element_name() == meshName && p.prim_type_name() == "Mesh") {
                    return true;
                }
                for (const auto& child : p.children()) {
                    if (checkForMesh(child)) {
                        return true;
                    }
                }
                return false;
            };
            
            containsMesh = checkForMesh(prim);
            
            if (containsMesh) {
                // Find the SkelAnimation within this SkelRoot
                std::function<bool(tinyusdz::Prim&)> updateSkelAnim = [&](tinyusdz::Prim& skelPrim) -> bool {
                    if (skelPrim.prim_type_name() == "SkelAnimation") {
                        // Update this SkelAnimation with time samples
                        if (auto* skelAnimData = skelPrim.as<tinyusdz::SkelAnimation>()) {
                            // Build time-to-weights mapping
                            std::map<double, std::vector<float>> timeToWeights;
                            
                            for (uint32_t k = 0; k < morphAnim->mNumKeys; ++k) {
                                const aiMeshMorphKey& key = morphAnim->mKeys[k];
                                double time = key.mTime * timeScale;
                                
                                std::vector<float> weights;
                                weights.reserve(key.mNumValuesAndWeights);
                                
                                for (uint32_t w = 0; w < key.mNumValuesAndWeights; ++w) {
                                    weights.push_back(static_cast<float>(key.mWeights[w]));
                                }
                                
                                timeToWeights[time] = weights;
                            }
                            
                            // Create time-sampled blendShapeWeights
                            if (!timeToWeights.empty()) {
                                tinyusdz::Animatable<std::vector<float>> animatedWeights;
                                
                                // Add all time samples
                                for (const auto& timeWeightPair : timeToWeights) {
                                    animatedWeights.add_sample(timeWeightPair.first, timeWeightPair.second);
                                }
                                
                                // Update the SkelAnimation's blendShapeWeights
                                const_cast<tinyusdz::SkelAnimation*>(skelAnimData)->blendShapeWeights.set_value(animatedWeights);
                                
                                ASSIMP_LOG_DEBUG("USDZExporter: Updated SkelAnimation with " + std::to_string(timeToWeights.size()) + " time samples");
                            }
                            
                            return true;
                        }
                    }
                    
                    // Search recursively in children
                    for (auto& child : skelPrim.children()) {
                        if (updateSkelAnim(child)) {
                            return true;
                        }
                    }
                    return false;
                };
                
                return updateSkelAnim(prim);
            }
        }
        
        // Search recursively in children
        for (auto& child : prim.children()) {
            if (findAndUpdateSkelAnimation(child)) {
                return true;
            }
        }
        return false;
    };
    
    // Search all root prims for the SkelRoot containing our mesh
    for (auto& rootPrim : mStage->root_prims()) {
        if (findAndUpdateSkelAnimation(rootPrim)) {
            break;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Convert blend shapes (placeholder)
void USDZExporter::ConvertBlendShapes(const aiMesh* mesh) {
    ASSIMP_LOG_DEBUG("USDZExporter: Blend shape conversion not yet implemented");
}


// ------------------------------------------------------------------------------------------------
// Convert camera using tinyusdz GeomCamera API
void USDZExporter::ConvertCamera(const aiCamera* camera) {
    if (!camera) return;
    
    // Create USD Camera using proper tinyusdz API
    tinyusdz::GeomCamera usdCamera;
    
    // Generate unique camera name
    std::string cameraName = SanitizeName(camera->mName.C_Str());
    if (cameraName.empty()) {
        cameraName = "Camera";
    }
    cameraName = GenerateUniqueName(cameraName);
    usdCamera.name = cameraName;
    
    // Set camera projection type - perspective by default
    if (camera->mOrthographicWidth > 0.0f) {
        // Orthographic camera
        usdCamera.projection.set_value(tinyusdz::GeomCamera::Projection::Orthographic);
        
        // Set orthographic parameters
        usdCamera.horizontalAperture.set_value(camera->mOrthographicWidth * 10.0f); // Convert to mm
        usdCamera.verticalAperture.set_value(camera->mOrthographicWidth / camera->mAspect * 10.0f);
    } else {
        // Perspective camera
        usdCamera.projection.set_value(tinyusdz::GeomCamera::Projection::Perspective);
        
        // Convert FOV to focal length and aperture
        float verticalAperture = 15.2908f; // Default USD vertical aperture in mm
        float focalLength = verticalAperture / (2.0f * tan(camera->mHorizontalFOV * 0.5f * camera->mAspect));
        
        usdCamera.focalLength.set_value(focalLength);
        usdCamera.verticalAperture.set_value(verticalAperture);
        usdCamera.horizontalAperture.set_value(verticalAperture / camera->mAspect);
    }
    
    // Set clipping range
    usdCamera.clippingRange.set_value(tinyusdz::value::float2({camera->mClipPlaneNear, camera->mClipPlaneFar}));
    
    // Convert to Prim and add to stage
    tinyusdz::Prim cameraPrim(usdCamera);
    mStage->root_prims().emplace_back(std::move(cameraPrim));
    
    ASSIMP_LOG_DEBUG("USDZExporter: Camera " + cameraName + " exported successfully");
}

// ------------------------------------------------------------------------------------------------
// Convert light using tinyusdz light APIs
void USDZExporter::ConvertLight(const aiLight* light) {
    if (!light) return;
    
    // Generate unique light name
    std::string lightName = SanitizeName(light->mName.C_Str());
    if (lightName.empty()) {
        lightName = "Light";
    }
    lightName = GenerateUniqueName(lightName);
    
    // Convert Assimp aiColor3D to tinyusdz color3f
    tinyusdz::value::color3f lightColor{light->mColorDiffuse.r, light->mColorDiffuse.g, light->mColorDiffuse.b};
    
    // Calculate light intensity and attenuation parameters using proper USD approach
    float lightIntensity = 1.0f;
    float lightExposure = 0.0f;
    
    // Apply Assimp light attenuation to USD intensity
    // Assimp uses: attenuation = 1 / (constant + linear * d + quadratic * d^2)
    // USD lights use intensity and exposure for brightness control
    if (light->mAttenuationConstant > 0.0f && light->mAttenuationConstant != 1.0f) {
        // Convert constant attenuation to effective intensity
        lightIntensity = 1.0f / light->mAttenuationConstant;
        ASSIMP_LOG_DEBUG("USDZExporter: Applied constant attenuation " + ai_to_string(light->mAttenuationConstant) + 
                        " -> intensity " + ai_to_string(lightIntensity));
    }
    
    // For linear and quadratic attenuation, we'll use exposure adjustment
    // This provides a logarithmic brightness control which is more physically meaningful
    if (light->mAttenuationLinear > 0.0f || light->mAttenuationQuadratic > 0.0f) {
        // Convert to logarithmic exposure values (EV stops)
        float attenuationFactor = 1.0f;
        
        if (light->mAttenuationLinear > 0.0f) {
            attenuationFactor *= (1.0f + light->mAttenuationLinear);
        }
        
        if (light->mAttenuationQuadratic > 0.0f) {
            attenuationFactor *= (1.0f + light->mAttenuationQuadratic * 10.0f); // Scale for reasonable values
        }
        
        if (attenuationFactor > 1.0f) {
            lightExposure = -std::log2(attenuationFactor); // Negative exposure reduces brightness
            ASSIMP_LOG_DEBUG("USDZExporter: Applied attenuation factors -> exposure " + ai_to_string(lightExposure));
        }
    }
    
    // Find the node associated with this light to get its world transform
    aiMatrix4x4 worldXform;
    const aiNode* lightNode = mScene->mRootNode ? mScene->mRootNode->FindNode(light->mName) : nullptr;
    if (lightNode) {
        worldXform = lightNode->mTransformation;
        const aiNode* parent = lightNode->mParent;
        while (parent) {
            worldXform = parent->mTransformation * worldXform;
            parent = parent->mParent;
        }
    }

    // Build XformOp from the light's node world transform
    tinyusdz::XformOp xformOp;
    bool hasXform = false;
    if (lightNode) {
        tinyusdz::value::matrix4d mat;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                mat.m[r][c] = worldXform[r][c];
        xformOp.op_type = tinyusdz::XformOp::OpType::Transform;
        xformOp.set_value(mat);
        hasXform = true;
    }

    switch (light->mType) {
        case aiLightSource_DIRECTIONAL: {
            tinyusdz::DistantLight distantLight;
            distantLight.name = lightName;
            distantLight.color.set_value(lightColor);
            distantLight.intensity.set_value(lightIntensity);
            distantLight.exposure.set_value(lightExposure);
            distantLight.angle.set_value(0.53f);
            if (hasXform) distantLight.xformOps.push_back(xformOp);

            mStage->root_prims().emplace_back(tinyusdz::Prim(distantLight));
            break;
        }
        case aiLightSource_POINT: {
            tinyusdz::SphereLight sphereLight;
            sphereLight.name = lightName;
            sphereLight.color.set_value(lightColor);
            sphereLight.intensity.set_value(lightIntensity);
            sphereLight.exposure.set_value(lightExposure);
            sphereLight.radius.set_value(0.1f);
            if (hasXform) sphereLight.xformOps.push_back(xformOp);

            mStage->root_prims().emplace_back(tinyusdz::Prim(sphereLight));
            break;
        }
        case aiLightSource_SPOT: {
            tinyusdz::SphereLight spotLight;
            spotLight.name = lightName;
            spotLight.color.set_value(lightColor);
            spotLight.intensity.set_value(lightIntensity);
            spotLight.exposure.set_value(lightExposure);
            spotLight.radius.set_value(0.1f);
            if (hasXform) spotLight.xformOps.push_back(xformOp);

            mStage->root_prims().emplace_back(tinyusdz::Prim(spotLight));
            break;
        }
        case aiLightSource_AREA: {
            tinyusdz::RectLight rectLight;
            rectLight.name = lightName;
            rectLight.color.set_value(lightColor);
            rectLight.intensity.set_value(lightIntensity);
            rectLight.exposure.set_value(lightExposure);
            rectLight.width.set_value(light->mSize.x > 0 ? light->mSize.x : 1.0f);
            rectLight.height.set_value(light->mSize.y > 0 ? light->mSize.y : 1.0f);
            if (hasXform) rectLight.xformOps.push_back(xformOp);

            mStage->root_prims().emplace_back(tinyusdz::Prim(rectLight));
            break;
        }
        default: {
            tinyusdz::SphereLight defaultLight;
            defaultLight.name = lightName;
            defaultLight.color.set_value(lightColor);
            defaultLight.intensity.set_value(lightIntensity);
            defaultLight.exposure.set_value(lightExposure);
            defaultLight.radius.set_value(0.5f);
            if (hasXform) defaultLight.xformOps.push_back(xformOp);

            mStage->root_prims().emplace_back(tinyusdz::Prim(defaultLight));
            break;
        }
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Light " + lightName + " exported successfully");
}

// ------------------------------------------------------------------------------------------------
// Convert node
tinyusdz::Xform* USDZExporter::ConvertNode(const aiNode* node, tinyusdz::Prim* parentPrim) {
    if (!node) return nullptr;
    
    auto xform = std::make_unique<tinyusdz::Xform>();
    
    std::string nodeName = SanitizeName(node->mName.C_Str());
    if (nodeName.empty()) {
        nodeName = "node";
    }
    nodeName = GenerateUniqueName(nodeName);
    
    xform->name = nodeName;
    mNodeIdMap[node] = nodeName;
    
    SetupNodeTransform(node, *xform);
    
    // Convert to Prim
    tinyusdz::Prim xformPrim(*xform);
    
    // Add to parent or root depending on hierarchy
    if (parentPrim) {
        // Add as child to parent prim
        parentPrim->children().emplace_back(std::move(xformPrim));
    } else {
        // This is a root node, add to stage root prims
        mStage->root_prims().emplace_back(std::move(xformPrim));
    }
    
    return xform.release(); // Return raw pointer, ownership transferred to stage
}

// ------------------------------------------------------------------------------------------------
// Setup node transform using the node's actual transformation matrix
void USDZExporter::SetupNodeTransform(const aiNode* node, tinyusdz::Xform& xform) {
    if (!node || node->mTransformation.IsIdentity()) {
        return;
    }

    tinyusdz::XformOp transformOp;
    transformOp.op_type = tinyusdz::XformOp::OpType::Transform;
    tinyusdz::value::matrix4d matrix;
    const aiMatrix4x4& m = node->mTransformation;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            matrix.m[row][col] = static_cast<double>(m[col][row]); // Assimp col-major to USD row-major
        }
    }
    transformOp.set_value(matrix);
    xform.xformOps.push_back(transformOp);
}

// ------------------------------------------------------------------------------------------------
// Find node by name in the scene hierarchy
const aiNode* USDZExporter::FindNodeByName(const aiNode* node, const std::string& name) const {
    if (!node) {
        return nullptr;
    }
    
    // Check if current node matches
    if (std::string(node->mName.C_Str()) == name) {
        return node;
    }
    
    // Recursively search children
    for (uint32_t i = 0; i < node->mNumChildren; ++i) {
        const aiNode* found = FindNodeByName(node->mChildren[i], name);
        if (found) {
            return found;
        }
    }
    
    return nullptr;
}

// ------------------------------------------------------------------------------------------------
// Get world transform of a node (following FBX exporter pattern)
aiMatrix4x4 USDZExporter::GetWorldTransform(const aiNode* node) const {
    std::vector<const aiNode*> nodeChain;
    while (node != mScene->mRootNode && node != nullptr) {
        nodeChain.push_back(node);
        node = node->mParent;
    }
    aiMatrix4x4 transform;
    for (auto n = nodeChain.rbegin(); n != nodeChain.rend(); ++n) {
        transform *= (*n)->mTransformation; // Uses processed node transforms!
    }
    return transform;
}

// ------------------------------------------------------------------------------------------------
// Sanitize name for USD
std::string USDZExporter::SanitizeName(const std::string& name) const {
    return NameRegistry::Sanitize(name);
}

// ------------------------------------------------------------------------------------------------
// Sanitize filename for texture files (preserves file extension)
std::string USDZExporter::SanitizeFilename(const std::string& filename) const {
    if (filename.empty()) return "";
    
    // Find the last dot to separate filename from extension
    size_t lastDot = filename.find_last_of('.');
    
    if (lastDot == std::string::npos) {
        // No extension, sanitize entire filename
        return SanitizeName(filename);
    }
    
    // Split into name and extension
    std::string namepart = filename.substr(0, lastDot);
    std::string extension = filename.substr(lastDot); // includes the dot
    
    // Sanitize only the namepart, preserve extension as-is
    std::string sanitizedName = SanitizeName(namepart);
    
    // Ensure we have a valid namepart
    if (sanitizedName.empty()) {
        sanitizedName = "texture";
    }
    
    return sanitizedName + extension;
}

// ------------------------------------------------------------------------------------------------
// Generate unique name (delegated to NameRegistry for performance)
std::string USDZExporter::GenerateUniqueName(const std::string& baseName) const {
    return mNameRegistry.GenerateUnique(baseName);
}

// ------------------------------------------------------------------------------------------------
// Get scene name for root prim
std::string USDZExporter::GetSceneName() const {
    // Extract base name from filename
    std::string baseName = mFilename;
    
    // Remove path and extension
    size_t lastSlash = baseName.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        baseName = baseName.substr(lastSlash + 1);
    }
    
    size_t lastDot = baseName.find_last_of('.');
    if (lastDot != std::string::npos) {
        baseName = baseName.substr(0, lastDot);
    }
    
    // Sanitize and ensure valid USD name
    std::string sceneName = SanitizeName(baseName);
    if (sceneName.empty()) {
        sceneName = "Scene";
    }
    
    return sceneName;
}

// ------------------------------------------------------------------------------------------------
// Build full hierarchy path from scene root to target node
std::string USDZExporter::BuildFullHierarchyPath(const aiNode* node, const std::string& rootPrimName) const {
    if (!node) return "/" + rootPrimName;
    
    // Build path components by traversing up to the root, using mNodeIdMap
    // for actual USD prim names (which may differ from aiNode names due to
    // deduplication by GenerateUniqueName)
    std::vector<std::string> pathComponents;
    const aiNode* current = node;
    
    while (current && current != mScene->mRootNode) {
        auto nodeIt = mNodeIdMap.find(current);
        std::string nodeName = (nodeIt != mNodeIdMap.end())
            ? nodeIt->second
            : SanitizeName(current->mName.C_Str());
        if (nodeName.empty()) {
            nodeName = "UnnamedNode";
        }
        pathComponents.push_back(nodeName);
        current = current->mParent;
    }
    
    if (current == mScene->mRootNode) {
        auto rootIt = mNodeIdMap.find(mScene->mRootNode);
        std::string rootNodeName = (rootIt != mNodeIdMap.end())
            ? rootIt->second
            : SanitizeName(mScene->mRootNode->mName.C_Str());
        if (!rootNodeName.empty()) {
            pathComponents.push_back(rootNodeName);
        }
    }
    
    std::string fullPath = "/" + rootPrimName;
    
    for (auto it = pathComponents.rbegin(); it != pathComponents.rend(); ++it) {
        fullPath += "/" + *it;
    }
    
    return fullPath;
}

// ------------------------------------------------------------------------------------------------
// Create UV coordinate reader shader
tinyusdz::Shader USDZExporter::CreateTexCoordReader(const std::string& varName) {
    tinyusdz::Shader texCoordReader;
    texCoordReader.name = "texCoordReader";
    texCoordReader.info_id = "UsdPrimvarReader_float2";
    
    tinyusdz::UsdPrimvarReader_float2 primvarReader;
    
    if (varName.empty() || varName == "st") {
        // Default: connect to material's stPrimvarName input for backward compat
        std::string varNamePath = mCurrentMaterialPath + ".inputs:stPrimvarName";
        tinyusdz::Path varNameConnection(varNamePath, "");
        primvarReader.varname.set_connection(varNameConnection);
    } else {
        // Specific UV channel: set varname directly
        primvarReader.varname.set_value(varName);
    }
    
    primvarReader.result.set_authored(true);
    
    texCoordReader.value = primvarReader;
    
    return texCoordReader;
}

// ------------------------------------------------------------------------------------------------
// Create texture coordinate transform shader
tinyusdz::Shader USDZExporter::CreateStTransform(const std::string& inputConnection, 
                                                 const aiUVTransform* uvTransform, bool flipY) {
    tinyusdz::Shader stTransform;
    stTransform.name = "stTransform";
    stTransform.info_id = "UsdTransform2d";
    
    // Create UsdTransform2d to handle texture coordinate transformations
    tinyusdz::UsdTransform2d transform2d;
    
    // Connect input to the specified connection (usually texCoordReader.outputs:result)
    tinyusdz::Path inputPath(inputConnection, "");
    transform2d.in.set_connection(inputPath);
    // Clear any default value since we're using a connection
    transform2d.in.set_value_empty();
    
    // Apply texture transforms from material if available
    if (uvTransform) {
        ASSIMP_LOG_DEBUG("USDZExporter: CreateStTransform input: scale(" + ai_to_string(uvTransform->mScaling.x) + "," + ai_to_string(uvTransform->mScaling.y) + 
                       ") trans(" + ai_to_string(uvTransform->mTranslation.x) + "," + ai_to_string(uvTransform->mTranslation.y) + 
                       ") rot(" + ai_to_string(uvTransform->mRotation) + ")");
        
        // To reverse the glTF conversion, we need to work backwards:
        // 1. The original glTF transformation was: Translation * Rotation * Scale  
        // 2. glTF importer converted this to Assimp coordinate space
        // 3. For USD, we need simple values for the transform
        
        // Apply scaling with coordinate conversion for USD  
        float scaleX = uvTransform->mScaling.x;
        float scaleY = uvTransform->mScaling.y;
        if (flipY) {
            scaleY = -scaleY;  // Y-flip in scale
        }
        transform2d.scale.set_value(tinyusdz::value::float2{scaleX, scaleY});
        
        // Apply proper matrix-based coordinate conversion for Assimp→USD space
        float transX = uvTransform->mTranslation.x;
        float transY = uvTransform->mTranslation.y;
        
        if (flipY) {
            // Reverse the glTF2 importer's coordinate conversion to recover
            // original glTF offsets, then convert to USD coordinate space.
            float rcos = std::cos(-uvTransform->mRotation);
            float rsin = std::sin(-uvTransform->mRotation);
            
            float originalOffsetY = 1.0f - uvTransform->mScaling.y - uvTransform->mTranslation.y + 
                                   (0.5f * uvTransform->mScaling.y) * (rsin + rcos - 1.0f);
            float originalOffsetX = uvTransform->mTranslation.x - 
                                   (0.5f * uvTransform->mScaling.x) * (-rcos + rsin + 1.0f);
            
            transX = originalOffsetX;
            transY = 1.0f - originalOffsetY;
        }
        
        transform2d.translation.set_value(tinyusdz::value::float2{transX, transY});
        
        // Apply rotation if present (convert from radians to degrees)
        // The Assimp glTF2 importer stores mRotation = -theta_gltf (negated).
        // USD UsdTransform2d uses Convention A rotation (standard CCW in V-up space),
        // while glTF uses Convention B (CCW in V-down space). Since scale.y is negated
        // to flip V, the rotation operates in V-up space after scaling. Convention A
        // in V-up produces the same visual rotation as Convention B in V-down for the
        // same angle, so we need theta_gltf = -mRotation.
        if (uvTransform->mRotation != 0.0f) {
            float rotationDegrees = -uvTransform->mRotation * 180.0f / M_PI;
            transform2d.rotation.set_value(rotationDegrees);
        }
        
        ASSIMP_LOG_DEBUG("USDZExporter: Refined result: scale(" + ai_to_string(scaleX) + "," + ai_to_string(scaleY) + 
                       ") trans(" + ai_to_string(transX) + "," + ai_to_string(transY) + 
                       ") rot(" + ai_to_string(-uvTransform->mRotation * 180.0f / M_PI) + ")");
    } else {
        // Default Y-flip transformation (common for textures)
        if (flipY) {
            transform2d.scale.set_value(tinyusdz::value::float2{1.0f, -1.0f});
            transform2d.translation.set_value(tinyusdz::value::float2{0.0f, 1.0f});
        } else {
            transform2d.scale.set_value(tinyusdz::value::float2{1.0f, 1.0f});
            transform2d.translation.set_value(tinyusdz::value::float2{0.0f, 0.0f});
        }
    }
    
    // Set output
    transform2d.result.set_authored(true);
    
    stTransform.value = transform2d;
    
    return stTransform;
}

// ------------------------------------------------------------------------------------------------
// Add source color space to texture
void USDZExporter::AddSourceColorSpace(tinyusdz::UsdUVTexture& uvTexture, const std::string& textureType) {
    // Set source color space based on texture type
    tinyusdz::Animatable<tinyusdz::UsdUVTexture::SourceColorSpace> sourceColorSpace;
    if (textureType == "diffuseColor" || textureType == "emissiveColor" || textureType == "opacity") {
        // Color and opacity textures use sRGB (opacity textures are often RGBA images)
        sourceColorSpace.set_default(tinyusdz::UsdUVTexture::SourceColorSpace::SRGB);
    } else {
        // Data textures (normal, roughness, metallic, etc.) use raw
        sourceColorSpace.set_default(tinyusdz::UsdUVTexture::SourceColorSpace::Raw);
    }
    uvTexture.sourceColorSpace.set_value(sourceColorSpace);
}

// ------------------------------------------------------------------------------------------------
// Add texture outputs
void USDZExporter::AddTextureOutputs(tinyusdz::UsdUVTexture& uvTexture, const std::string& textureType) {
    // Set appropriate outputs based on texture type
    if (textureType == "diffuseColor" || textureType == "emissiveColor") {
        // Color textures output RGB + alpha
        uvTexture.outputsRGB.set_authored(true);
        uvTexture.outputsA.set_authored(true);
    } else if (textureType == "opacity") {
        // Opacity textures primarily output alpha channel
        uvTexture.outputsA.set_authored(true);
        uvTexture.outputsRGB.set_authored(true); // Also enable RGB in case needed
    } else if (textureType == "normal") {
        // Normal maps output RGB
        uvTexture.outputsRGB.set_authored(true);
    } else if (textureType == "roughness" || textureType == "clearcoatRoughness") {
        // Roughness textures output green channel (matches reference file)
        uvTexture.outputsG.set_authored(true);
    } else if (textureType == "metallic") {
        // Metallic textures output blue channel (matches reference file)
        uvTexture.outputsB.set_authored(true);
    } else {
        // Other scalar textures (occlusion, clearcoat, etc.) output red channel
        uvTexture.outputsR.set_authored(true);
    }
}

// ------------------------------------------------------------------------------------------------
// Check if texture is embedded
bool USDZExporter::IsEmbeddedTexture(const std::string& texPath) const {
    return !texPath.empty() && texPath[0] == '*';
}

// ------------------------------------------------------------------------------------------------
// Save as USDA
void USDZExporter::SaveAsUSDA(const std::string& filename) {
    std::string usdaContent = mStage->ExportToString();
    if (usdaContent.empty()) {
        throw DeadlyExportError("Failed to generate USDA content");
    }
    
    if (mIOSystem) {
        std::unique_ptr<IOStream> file(mIOSystem->Open(filename.c_str(), "wt"));
        if (!file) {
            throw DeadlyExportError("Failed to open file for writing: " + filename);
        }
        file->Write(usdaContent.data(), 1, usdaContent.size());
        file->Flush();
    } else {
        std::string warn, err;
        bool success = tinyusdz::usda::SaveAsUSDA(filename, *mStage, &warn, &err);
        if (!success) {
            throw DeadlyExportError("Failed to save USDA file: " + err);
        }
    }
    
    ASSIMP_LOG_INFO("USDZExporter: Successfully exported USDA");
    
    try {
        ExtractTexturesForUSDA(filename);
    } catch (const std::exception& e) {
        ReportWarning("Texture extraction failed: " + std::string(e.what()));
    }
}

// Note: Texture file writing is now handled by tinyusdz::usdz::SaveAsUSDZ()

// ------------------------------------------------------------------------------------------------
// Save as USDC
void USDZExporter::SaveAsUSDC(const std::string& filename) {
    std::string warn, err;
    bool success = tinyusdz::usdc::SaveAsUSDCToFile(filename, *mStage, &warn, &err);
    
    if (!warn.empty()) {
        ReportWarning("USDC export warning: " + warn);
    }
    
    if (!err.empty()) {
        ReportError("USDC export error: " + err);
        throw DeadlyImportError("Failed to save USDC file: " + err);
    }
    
    if (!success) {
        throw DeadlyImportError("Failed to save USDC file: Unknown error");
    }
    
    ASSIMP_LOG_INFO("USDZExporter: Successfully exported USDC");
}

// ------------------------------------------------------------------------------------------------
// Save as USDZ using zip.c + zip.patch approach  
void USDZExporter::SaveAsUSDZ(const std::string& filename) {
    try {
        std::string usdContent = GenerateUSDContent();
        if (usdContent.empty()) {
            throw DeadlyExportError("Generated USD content is empty");
        }
        
        std::map<std::string, std::vector<uint8_t>> textureDataMap;
        CollectTextureDataForUSDZ(textureDataMap);
        
        std::string warn, err;
        
        if (mIOSystem) {
            // Build USDZ in memory, then write through IOSystem
            std::vector<uint8_t> usdzData;
            bool success = tinyusdz::usdz::SaveAsUSDZToMemory(usdContent, textureDataMap, usdzData, &warn, &err);
            
            if (!warn.empty()) ReportWarning("USDZ export warning: " + warn);
            if (!success || !err.empty()) {
                throw DeadlyExportError("Failed to build USDZ archive: " + err);
            }
            
            std::unique_ptr<IOStream> file(mIOSystem->Open(filename.c_str(), "wb"));
            if (!file) {
                throw DeadlyExportError("Failed to open file for writing: " + filename);
            }
            file->Write(usdzData.data(), 1, usdzData.size());
            file->Flush();
        } else {
            bool success = tinyusdz::usdz::SaveAsUSDZWithTextures(filename, usdContent, textureDataMap, &warn, &err);
            if (!warn.empty()) ReportWarning("USDZ export warning: " + warn);
            if (!success || !err.empty()) {
                throw DeadlyExportError("Failed to save USDZ file: " + err);
            }
        }
        
        ASSIMP_LOG_INFO("USDZExporter: Successfully exported USDZ with ", textureDataMap.size(), " embedded textures");
        
    } catch (const std::exception& e) {
        ReportError(std::string("USDZ export failed: ") + e.what());
        throw;
    }
}

// ------------------------------------------------------------------------------------------------
// Generate USD content in memory instead of writing to file
std::string USDZExporter::GenerateUSDContent() {
    if (!mStage) {
        ReportError("USD Stage is not initialized");
        return "";
    }
    
    // Use Stage's ExportToString method to get USDA content
    try {
        std::string usdaContent = mStage->ExportToString();
        
        if (usdaContent.empty()) {
            ReportError("Generated USD content is empty");
            return "";
        }
        
        return usdaContent;
        
    } catch (const std::exception& e) {
        ReportError("Failed to generate USDA content: " + std::string(e.what()));
        return "";
    }
}

// ------------------------------------------------------------------------------------------------
// Shared texture processing with template handler for different output strategies
template<typename TextureHandler>
uint32_t USDZExporter::ProcessEmbeddedTextures(TextureHandler handler, const std::string& pathPrefix) {
    uint32_t processedCount = 0;
    
    for (uint32_t i = 0; i < mScene->mNumTextures; ++i) {
        const aiTexture* tex = mScene->mTextures[i];
        if (!tex) continue;
        
        std::string uniqueFilename = GetEmbeddedTextureFilename(static_cast<int>(i));
        std::string texturePath = pathPrefix + uniqueFilename;
        
        // Extract texture data
        std::vector<uint8_t> textureData;
        if (tex->mHeight == 0 && tex->pcData && tex->mWidth > 0) {
            textureData.assign(
                reinterpret_cast<const uint8_t*>(tex->pcData),
                reinterpret_cast<const uint8_t*>(tex->pcData) + tex->mWidth
            );
        } else {
            std::vector<uint8_t> pngData;
            if (ConvertRawTextureToPNG(tex, pngData)) {
                textureData = std::move(pngData);
                if (texturePath.length() >= 4 && texturePath.substr(texturePath.length() - 4) != ".png") {
                    texturePath = texturePath.substr(0, texturePath.find_last_of('.')) + ".png";
                }
            } else {
                size_t dataSize = tex->mWidth * tex->mHeight * 4;
                textureData.assign(
                    reinterpret_cast<const uint8_t*>(tex->pcData),
                    reinterpret_cast<const uint8_t*>(tex->pcData) + dataSize
                );
            }
        }
        
        if (!textureData.empty()) {
            if (handler(texturePath, textureData)) {
                ++processedCount;
            }
        } else {
            ASSIMP_LOG_DEBUG("USDZExporter: Empty texture data for: " + texturePath);
        }
    }
    
    return processedCount;
}

// ------------------------------------------------------------------------------------------------
// Collect texture data for USDZ embedding
void USDZExporter::CollectTextureDataForUSDZ(std::map<std::string, std::vector<uint8_t>>& textureDataMap) {
    auto usdzCollector = [this, &textureDataMap](const std::string& texturePath, std::vector<uint8_t>& textureData) -> bool {
        textureDataMap[texturePath] = std::move(textureData);
        ASSIMP_LOG_DEBUG("USDZExporter: Collected embedded texture: " + texturePath + 
                        " (" + ai_to_string(textureDataMap[texturePath].size()) + " bytes)");
        return true;
    };
    
    uint32_t collected = ProcessEmbeddedTextures(usdzCollector, "textures/");
    
    // Also read and include external textures referenced during material export
    for (const auto& entry : mExternalTexturePaths) {
        if (textureDataMap.find(entry.first) != textureDataMap.end()) {
            continue; // Already collected as embedded
        }
        if (mIOSystem && mIOSystem->Exists(entry.second)) {
            std::unique_ptr<IOStream> stream(mIOSystem->Open(entry.second, "rb"));
            if (stream) {
                size_t fileSize = stream->FileSize();
                std::vector<uint8_t> data(fileSize);
                if (stream->Read(data.data(), 1, fileSize) == fileSize) {
                    textureDataMap[entry.first] = std::move(data);
                    ++collected;
                    ASSIMP_LOG_DEBUG("USDZExporter: Collected external texture: " + entry.second);
                }
            }
        } else {
            ASSIMP_LOG_WARN("USDZExporter: External texture not found: " + entry.second);
        }
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Collected " + ai_to_string(collected) + " textures for USDZ embedding");
}

// ------------------------------------------------------------------------------------------------
// Extract texture files for USDA export (writes files to filesystem)
void USDZExporter::ExtractTexturesForUSDA(const std::string& usdaFilePath) {
    if (!mScene) return;

    bool hasEmbedded = mScene->mNumTextures > 0;
    bool hasExternal = !mExternalTexturePaths.empty();
    if (!hasEmbedded && !hasExternal) {
        ASSIMP_LOG_DEBUG("USDZExporter: No textures to extract for USDA export");
        return;
    }

    std::string outputDir = usdaFilePath.substr(0, usdaFilePath.find_last_of("/\\"));
    if (outputDir.empty()) {
        outputDir = ".";
    }

    std::string texturesDir = outputDir + "/textures";

    if (!CreateTexturesDirectory(texturesDir)) {
        ReportError("Failed to create textures directory: " + texturesDir);
        return;
    }

    uint32_t extracted = 0;

    // Process embedded textures
    if (hasEmbedded) {
        auto fileWriter = [this, &texturesDir](const std::string& texturePath, std::vector<uint8_t>& textureData) -> bool {
            std::string textureFilePath = texturesDir + "/" + texturePath;
            if (texturePath.length() >= 4 && texturePath.substr(texturePath.length() - 4) == ".png" &&
                textureFilePath.length() >= 4 && textureFilePath.substr(textureFilePath.length() - 4) != ".png") {
                textureFilePath = textureFilePath.substr(0, textureFilePath.find_last_of('.')) + ".png";
            }
            if (WriteTextureFile(textureFilePath, textureData)) {
                ASSIMP_LOG_DEBUG("USDZExporter: Extracted embedded texture: " + textureFilePath);
                return true;
            }
            return false;
        };
        extracted += ProcessEmbeddedTextures(fileWriter, "");
    }

    // Copy external textures from their original locations
    for (const auto& entry : mExternalTexturePaths) {
        if (!mIOSystem) continue;
        if (!mIOSystem->Exists(entry.second.c_str())) {
            ASSIMP_LOG_WARN("USDZExporter: External texture not found: " + entry.second);
            continue;
        }
        std::unique_ptr<IOStream> stream(mIOSystem->Open(entry.second.c_str(), "rb"));
        if (!stream) continue;

        size_t fileSize = stream->FileSize();
        std::vector<uint8_t> data(fileSize);
        if (stream->Read(data.data(), 1, fileSize) != fileSize) continue;

        // entry.first is like "textures/UV.png" -- strip the leading "textures/" prefix
        std::string filename = entry.first;
        auto slashPos = filename.find('/');
        if (slashPos != std::string::npos) {
            filename = filename.substr(slashPos + 1);
        }

        if (WriteTextureFile(texturesDir + "/" + filename, data)) {
            ++extracted;
            ASSIMP_LOG_DEBUG("USDZExporter: Copied external texture: " + entry.second + " -> " + filename);
        }
    }

    ASSIMP_LOG_DEBUG("USDZExporter: Texture extraction completed for USDA export, " +
                    ai_to_string(extracted) + " textures processed");
}

// ------------------------------------------------------------------------------------------------
// Write texture data to file using IOSystem for cross-platform compatibility
bool USDZExporter::WriteTextureFile(const std::string& filePath, const std::vector<uint8_t>& textureData) {
    if (textureData.empty()) {
        return false;
    }
    
    if (mIOSystem) {
        std::unique_ptr<IOStream> file(mIOSystem->Open(filePath.c_str(), "wb"));
        if (!file) {
            ReportError("Failed to create texture file using IOSystem: " + filePath);
            return false;
        }
        
        size_t written = file->Write(textureData.data(), 1, textureData.size());
        file->Flush();
        
        if (written != textureData.size()) {
            ReportError("Failed to write complete texture data to file: " + filePath);
            return false;
        }
        
        // IOSystem manages closing through RAII
        return true;
    }
    
    throw DeadlyExportError("IOSystem is not available for texture file writing: " + filePath);
}

// ------------------------------------------------------------------------------------------------
// Helper method to convert raw RGBA texture data to PNG
bool USDZExporter::ConvertRawTextureToPNG(const aiTexture* texture, std::vector<uint8_t>& pngData) {
    if (!texture || texture->mHeight == 0) {
        return false;
    }
    
    // This is a simplified implementation - in a real scenario you'd use a library like stb_image_write
    // For now, we'll just report that this functionality needs to be implemented
    ReportWarning("Raw texture to PNG conversion not yet implemented for: " + std::string(texture->mFilename.C_Str()));
    return false;
}

// ------------------------------------------------------------------------------------------------
// Recursively deduplicate sibling prim names in the stage
void USDZExporter::DeduplicateSiblingPrimNames() {
    if (!mStage) return;
    
    int totalRenamed = 0;
    std::function<void(std::vector<tinyusdz::Prim>&)> dedup = [&](std::vector<tinyusdz::Prim>& prims) {
        std::map<std::string, int> nameCount;
        for (const auto& prim : prims) {
            nameCount[prim.element_name()]++;
        }
        
        std::map<std::string, int> nameIndex;
        for (auto& prim : prims) {
            std::string name = prim.element_name();
            if (nameCount[name] > 1) {
                int idx = nameIndex[name]++;
                if (idx > 0) {
                    std::string newName = name + "_dup" + std::to_string(idx);
                    prim.element_path() = tinyusdz::Path(newName, "");
                    totalRenamed++;
                    ASSIMP_LOG_DEBUG("USDZExporter: Dedup renamed \"" + name + "\" -> \"" + newName + "\"");
                }
            }
        }
        
        for (auto& prim : prims) {
            dedup(prim.children());
        }
    };
    
    auto& roots = mStage->root_prims();
    dedup(roots);
    if (totalRenamed > 0) {
        ASSIMP_LOG_INFO("USDZExporter: Deduplicated ", totalRenamed, " prim names");
    }
}

// ------------------------------------------------------------------------------------------------
// Report error
void USDZExporter::ReportError(const std::string& message) {
    mErrors.push_back(message);
    ASSIMP_LOG_ERROR("USDZExporter: " + message);
}

// ------------------------------------------------------------------------------------------------
// Report warning
void USDZExporter::ReportWarning(const std::string& message) {
    mWarnings.push_back(message);
    ASSIMP_LOG_WARN("USDZExporter: " + message);
}

// ------------------------------------------------------------------------------------------------  
// Create textures directory with proper cross-platform support
bool USDZExporter::CreateTexturesDirectory(const std::string& dirPath) {
#ifdef _WIN32
    int result = ::_mkdir(dirPath.c_str());
#else
    int result = ::mkdir(dirPath.c_str(), 0777);
#endif
    return (result == 0) || (errno == EEXIST);
}

// ------------------------------------------------------------------------------------------------  
// Generate descriptive texture names based on usage context and material analysis
std::string USDZExporter::GenerateDescriptiveTextureName(int textureIndex, const std::string& baseTextureName) {
    // If we already have a good filename, use it
    if (!baseTextureName.empty() && baseTextureName.find("embedded_texture_") != 0) {
        return baseTextureName;
    }
    
    std::map<int, std::string> textureUsageMap;
    
    if (mScene && mScene->mNumMaterials > 0) {
        for (unsigned int matIdx = 0; matIdx < mScene->mNumMaterials; ++matIdx) {
            const aiMaterial* material = mScene->mMaterials[matIdx];
            if (!material) continue;
            
            aiString texPath;
            
            // Check each texture type and map to descriptive names
            if (material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == aiReturn_SUCCESS ||
                material->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == aiReturn_SUCCESS) {
                std::string path(texPath.C_Str());
                if (path.length() > 0 && path[0] == '*') {
                    int idx = std::stoi(path.substr(1));
                    textureUsageMap[idx] = "Default_albedo";
                }
            }
            
            if (material->GetTexture(aiTextureType_NORMALS, 0, &texPath) == aiReturn_SUCCESS) {
                std::string path(texPath.C_Str());
                if (path.length() > 0 && path[0] == '*') {
                    int idx = std::stoi(path.substr(1));
                    textureUsageMap[idx] = "Default_normal";
                }
            }
            
            if (material->GetTexture(aiTextureType_METALNESS, 0, &texPath) == aiReturn_SUCCESS ||
                material->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &texPath) == aiReturn_SUCCESS) {
                std::string path(texPath.C_Str());
                if (path.length() > 0 && path[0] == '*') {
                    int idx = std::stoi(path.substr(1));
                    textureUsageMap[idx] = "Default_metalRoughness";
                }
            }
            
            if (material->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &texPath) == aiReturn_SUCCESS ||
                material->GetTexture(aiTextureType_LIGHTMAP, 0, &texPath) == aiReturn_SUCCESS) {
                std::string path(texPath.C_Str());
                if (path.length() > 0 && path[0] == '*') {
                    int idx = std::stoi(path.substr(1));
                    textureUsageMap[idx] = "Default_AO";
                }
            }
            
            if (material->GetTexture(aiTextureType_EMISSIVE, 0, &texPath) == aiReturn_SUCCESS) {
                std::string path(texPath.C_Str());
                if (path.length() > 0 && path[0] == '*') {
                    int idx = std::stoi(path.substr(1));
                    textureUsageMap[idx] = "Default_emissive";
                }
            }
        }
    }
    
    // Return descriptive name if we found usage, otherwise fallback to generic
    auto it = textureUsageMap.find(textureIndex);
    if (it != textureUsageMap.end()) {
        return it->second;
    }
    
    return "embedded_texture_" + ai_to_string(textureIndex);
}

// ------------------------------------------------------------------------------------------------
// Build a centralized mapping from texture index to unique filename.
// Handles duplicate names from GLB images (same name, different data) and
// duplicate descriptive names (multiple unnamed images used for the same type).
void USDZExporter::BuildEmbeddedTextureFilenameMap() {
    mEmbeddedTextureFilenames.clear();
    if (!mScene || mScene->mNumTextures == 0) return;

    // First pass: generate initial filenames for every embedded texture
    std::map<int, std::string> initialNames;
    for (uint32_t i = 0; i < mScene->mNumTextures; ++i) {
        const aiTexture* tex = mScene->mTextures[i];
        if (!tex) continue;

        std::string name;
        if (tex->mFilename.length > 0) {
            name = tex->mFilename.C_Str();
            size_t lastSlash = name.find_last_of("/\\");
            if (lastSlash != std::string::npos) {
                name = name.substr(lastSlash + 1);
            }
        }

        if (name.empty() || name.find("embedded_texture_") == 0) {
            name = GenerateDescriptiveTextureName(static_cast<int>(i), "");
        }

        std::string extension = ".png";
        if (tex->mHeight == 0 && tex->achFormatHint[0] != '\0') {
            std::string formatHint(tex->achFormatHint);
            if (formatHint.find('.') == std::string::npos) {
                extension = "." + formatHint;
            } else {
                extension = formatHint;
            }
        }
        if (name.find('.') == std::string::npos) {
            name += extension;
        }

        name = SanitizeFilename(name);
        initialNames[static_cast<int>(i)] = name;
    }

    // Second pass: detect duplicates and append texture index to disambiguate
    std::map<std::string, std::vector<int>> nameToIndices;
    for (const auto& entry : initialNames) {
        nameToIndices[entry.second].push_back(entry.first);
    }

    for (const auto& entry : nameToIndices) {
        const std::string& name = entry.first;
        const std::vector<int>& indices = entry.second;
        if (indices.size() == 1) {
            mEmbeddedTextureFilenames[indices[0]] = name;
        } else {
            for (int idx : indices) {
                std::string base = name;
                std::string ext;
                auto dotPos = base.find_last_of('.');
                if (dotPos != std::string::npos) {
                    ext = base.substr(dotPos);
                    base = base.substr(0, dotPos);
                }
                mEmbeddedTextureFilenames[idx] = base + "_" + ai_to_string(idx) + ext;
            }
            ASSIMP_LOG_DEBUG("USDZExporter: Disambiguated " + ai_to_string(static_cast<uint32_t>(indices.size())) +
                            " textures with name \"" + name + "\"");
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Look up the unique filename for an embedded texture index.
std::string USDZExporter::GetEmbeddedTextureFilename(int textureIndex) {
    auto it = mEmbeddedTextureFilenames.find(textureIndex);
    if (it != mEmbeddedTextureFilenames.end()) {
        return it->second;
    }
    return "embedded_texture_" + ai_to_string(textureIndex) + ".png";
}

// ------------------------------------------------------------------------------------------------
// Export functions
// USDZ export removed - not supported by current tinyusdz version

void Assimp::ExportSceneUSDA(const char* pFile, IOSystem* pIOSystem, const aiScene* pScene, const ExportProperties* pProperties) {
    if (!pScene) {
        ASSIMP_LOG_ERROR("USDA export failed: Scene is null");
        throw DeadlyExportError("USDA export failed: Scene is null");
    }
    
    if (!pFile) {
        ASSIMP_LOG_ERROR("USDA export failed: Output file path is null");
        throw DeadlyExportError("USDA export failed: Output file path is null");
    }
    
    try {
        USDZExporter exporter(pFile, pIOSystem, pScene, pProperties, false);
    } catch (const DeadlyExportError& e) {
        ASSIMP_LOG_ERROR("USDA export failed: " + std::string(e.what()));
        throw;
    }
    
    // USD file written with proper tinyusdz APIs - no post-processing needed
}

void Assimp::ExportSceneUSDZ(const char* pFile, IOSystem* pIOSystem, const aiScene* pScene, const ExportProperties* pProperties) {
    if (!pScene) {
        ASSIMP_LOG_ERROR("USDZ export failed: Scene is null");
        throw DeadlyExportError("USDZ export failed: Scene is null");
    }
    
    if (!pFile) {
        ASSIMP_LOG_ERROR("USDZ export failed: Output file path is null");
        throw DeadlyExportError("USDZ export failed: Output file path is null");
    }
    
    try {
        USDZExporter exporter(pFile, pIOSystem, pScene, pProperties, true);
    } catch (const DeadlyExportError& e) {
        ASSIMP_LOG_ERROR("USDZ export failed: " + std::string(e.what()));
        throw;
    }
    
    // USDZ file written with embedded textures in ZIP archive
}


// ------------------------------------------------------------------------------------------------
// Initialize bone discriminator for skeletal export (instance method for proper timing)
void USDZExporter::InitializeBoneDiscriminator() {
    mBoneNames.clear();
    
    for (uint32_t meshIdx = 0; meshIdx < mScene->mNumMeshes; ++meshIdx) {
        const aiMesh* mesh = mScene->mMeshes[meshIdx];
        if (mesh->HasBones()) {
            for (uint32_t boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
                mBoneNames.insert(mesh->mBones[boneIdx]->mName.C_Str());
            }
        }
    }
    
    mBoneNamesInitialized = true;
    ASSIMP_LOG_DEBUG("USDZExporter: Initialized bone discriminator set with " + std::to_string(mBoneNames.size()) + " bone names");
    
    // Debug: Log first few bone names to verify discriminator
    int count = 0;
    for (const auto& boneName : mBoneNames) {
        if (count < 5) {
            ASSIMP_LOG_DEBUG("USDZExporter: Bone discriminator contains: '" + boneName + "'");
            count++;
        } else {
            break;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Check if a node should be skipped during hierarchy export (because it's a bone node)
bool USDZExporter::ShouldSkipBoneNode(const std::string& nodeName) {
    // Only skip bone nodes when we have skeletal meshes 
    bool hasSkeletalMeshes = false;
    for (uint32_t i = 0; i < mScene->mNumMeshes; ++i) {
        if (mScene->mMeshes[i]->HasBones()) {
            hasSkeletalMeshes = true;
            break;
        }
    }
    
    if (!hasSkeletalMeshes) {
        return false; // No skeletal data, don't skip any nodes
    }
    
    // Initialize bone discriminator if needed
    if (!mBoneNamesInitialized) {
        InitializeBoneDiscriminator();
    }
    
    // Skip if this node is a bone (will be handled by centralized SkelAnimation)
    return mBoneNames.find(nodeName) != mBoneNames.end();
}

// ------------------------------------------------------------------------------------------------
// Pre-detect shared meshes before hierarchy export
// ------------------------------------------------------------------------------------------------
// Check if node only contains skeletal meshes
bool USDZExporter::NodeOnlyContainsSkeletalMeshes(const aiNode* node) {
    if (!node || node->mNumMeshes == 0) {
        return false;
    }
    
    for (uint32_t i = 0; i < node->mNumMeshes; ++i) {
        uint32_t meshIndex = node->mMeshes[i];
        if (meshIndex < mScene->mNumMeshes) {
            const aiMesh* mesh = mScene->mMeshes[meshIndex];
            if (!mesh->HasBones()) {
                return false;
            }
        }
    }
    
    return true;
}

// ------------------------------------------------------------------------------------------------
// Generate individual skeleton joint Xforms with hierarchical nesting and time samples
void USDZExporter::GenerateIndividualJointXforms(tinyusdz::Prim& armaturePrim, const std::set<std::string>& allBoneNames) {
    if (mScene->mNumAnimations == 0) {
        ASSIMP_LOG_DEBUG("USDZExporter: No animations available for joint Xform generation");
        return;
    }
    
    const aiAnimation* anim = mScene->mAnimations[0]; // Use first animation
    ASSIMP_LOG_DEBUG("USDZExporter: STARTING hierarchical joint generation for " + std::to_string(allBoneNames.size()) + " bones");
    
    // Create a map of bone name to aiNodeAnim for animation lookup
    std::map<std::string, const aiNodeAnim*> boneToNodeAnim;
    for (uint32_t i = 0; i < anim->mNumChannels; ++i) {
        const aiNodeAnim* nodeAnim = anim->mChannels[i];
        std::string boneName = nodeAnim->mNodeName.C_Str();
        boneToNodeAnim[boneName] = nodeAnim;
    }
    
    // Find skeleton root node from scene hierarchy to build proper tree structure
    std::function<const aiNode*(const aiNode*, const std::set<std::string>&)> findSkeletonRoot = 
        [&](const aiNode* node, const std::set<std::string>& boneNames) -> const aiNode* {
        std::string nodeName = node->mName.C_Str();
        if (boneNames.find(nodeName) != boneNames.end()) {
            return node; // This is a bone node
        }
        
        // Check children
        for (uint32_t i = 0; i < node->mNumChildren; ++i) {
            if (const aiNode* found = findSkeletonRoot(node->mChildren[i], boneNames)) {
                return found;
            }
        }
        return nullptr;
    };
    
    // Find the skeleton root in the scene hierarchy
    const aiNode* skeletonRoot = findSkeletonRoot(mScene->mRootNode, allBoneNames);
    if (!skeletonRoot) {
        ASSIMP_LOG_ERROR("USDZExporter: Could not find skeleton root node in scene hierarchy");
        return;
    }
    
    // Recursive function to build hierarchical joint structure with time samples
    std::function<void(const aiNode*, tinyusdz::Prim&)> buildJointHierarchy = 
        [&](const aiNode* node, tinyusdz::Prim& parentPrim) {
        std::string nodeName = node->mName.C_Str();
        
        // Only process nodes that are actual bones
        if (allBoneNames.find(nodeName) != allBoneNames.end()) {
            tinyusdz::Xform jointXform;
            jointXform.name = SanitizeName(nodeName);
            
            // Create base transform operations
            tinyusdz::XformOp translateOp;
            translateOp.op_type = tinyusdz::XformOp::OpType::Translate;
            
            tinyusdz::XformOp orientOp;
            orientOp.op_type = tinyusdz::XformOp::OpType::Orient;
            
            tinyusdz::XformOp scaleOp;
            scaleOp.op_type = tinyusdz::XformOp::OpType::Scale;
            
            // Add time-sampled animation data if available, otherwise set default values
            auto nodeAnimIt = boneToNodeAnim.find(nodeName);
            if (nodeAnimIt != boneToNodeAnim.end()) {
                const aiNodeAnim* nodeAnim = nodeAnimIt->second;
                
                // Add time samples using correct tinyusdz XformOp API - ONLY time samples for animated joints
                if (nodeAnim->mNumRotationKeys > 0) {
                    for (uint32_t keyIdx = 0; keyIdx < nodeAnim->mNumRotationKeys; ++keyIdx) {
                        const aiQuatKey& rotKey = nodeAnim->mRotationKeys[keyIdx];
                        double timeCode = rotKey.mTime / anim->mTicksPerSecond;
                        
                        tinyusdz::value::quatf quat;
                        quat.real = rotKey.mValue.w;
                        quat.imag = {rotKey.mValue.x, rotKey.mValue.y, rotKey.mValue.z};
                        
                        orientOp.set_timesample(timeCode, quat);
                    }
                    // Don't set default value for animated properties - time samples only
                } else {
                    // Set default identity quaternion if no animation data
                    tinyusdz::value::quatf identityQuat;
                    identityQuat.real = 1.0f;
                    identityQuat.imag = {0.0f, 0.0f, 0.0f};
                    orientOp.set_value(identityQuat);
                }
                
                if (nodeAnim->mNumPositionKeys > 0) {
                    for (uint32_t keyIdx = 0; keyIdx < nodeAnim->mNumPositionKeys; ++keyIdx) {
                        const aiVectorKey& posKey = nodeAnim->mPositionKeys[keyIdx];
                        double timeCode = posKey.mTime / anim->mTicksPerSecond;
                        
                        tinyusdz::value::double3 pos{posKey.mValue.x, posKey.mValue.y, posKey.mValue.z};
                        translateOp.set_timesample(timeCode, pos);
                    }
                    // Don't set default value for animated properties - time samples only  
                } else {
                    // Set default zero translation if no animation data
                    translateOp.set_value(tinyusdz::value::double3{0.0, 0.0, 0.0});
                }
                
                if (nodeAnim->mNumScalingKeys > 0) {
                    for (uint32_t keyIdx = 0; keyIdx < nodeAnim->mNumScalingKeys; ++keyIdx) {
                        const aiVectorKey& scaleKey = nodeAnim->mScalingKeys[keyIdx];
                        double timeCode = scaleKey.mTime / anim->mTicksPerSecond;
                        
                        tinyusdz::value::double3 scale{scaleKey.mValue.x, scaleKey.mValue.y, scaleKey.mValue.z};
                        scaleOp.set_timesample(timeCode, scale);
                    }
                    // Don't set default value for animated properties - time samples only
                } else {
                    // Set default unit scale if no animation data
                    scaleOp.set_value(tinyusdz::value::double3{1.0, 1.0, 1.0});
                }
                
                ASSIMP_LOG_DEBUG("USDZExporter: Added time samples for joint: " + nodeName);
            } else {
                // Set default values if no animation data for this bone
                translateOp.set_value(tinyusdz::value::double3{0.0, 0.0, 0.0});
                tinyusdz::value::quatf identityQuat;
                identityQuat.real = 1.0f;
                identityQuat.imag = {0.0f, 0.0f, 0.0f};
                orientOp.set_value(identityQuat);
                scaleOp.set_value(tinyusdz::value::double3{1.0, 1.0, 1.0});
            }
            
            // Add transform operations in USD order: translate, orient, scale (after time samples are set)
            jointXform.xformOps.push_back(translateOp);
            jointXform.xformOps.push_back(orientOp);
            jointXform.xformOps.push_back(scaleOp);
            
            // Create joint prim
            tinyusdz::Prim jointPrim(jointXform);
            
            // Recursively process children - this creates the hierarchical nesting
            for (uint32_t i = 0; i < node->mNumChildren; ++i) {
                buildJointHierarchy(node->mChildren[i], jointPrim);
            }
            
            // Add this joint to parent
            parentPrim.children().emplace_back(std::move(jointPrim));
            
            ASSIMP_LOG_DEBUG("USDZExporter: Created hierarchical joint: " + nodeName);
        } else {
            // Not a bone node, but continue checking children
            for (uint32_t i = 0; i < node->mNumChildren; ++i) {
                buildJointHierarchy(node->mChildren[i], parentPrim);
            }
        }
    };
    
    // Build the hierarchical joint structure starting from skeleton root
    buildJointHierarchy(skeletonRoot, armaturePrim);
    
    ASSIMP_LOG_DEBUG("USDZExporter: Generated hierarchical joint structure with time samples for " + std::to_string(allBoneNames.size()) + " bones");
}

// ------------------------------------------------------------------------------------------------
// Build scene node hierarchy mapping (exactly like gltfImport.cpp buildSkeletonNodeNames)
USDZExporter::NodeHierarchyMapping USDZExporter::BuildSceneNodeHierarchy() const {
    NodeHierarchyMapping mapping;
    int nextIndex = 0;
    
    // First pass: Assign indices to all nodes (like gltfImport.cpp node indices)
    std::function<void(const aiNode*)> assignIndices = [&](const aiNode* node) {
        mapping.nodeToIndex[node] = nextIndex++;
        for (uint32_t i = 0; i < node->mNumChildren; ++i) {
            assignIndices(node->mChildren[i]);
        }
    };
    
    // Second pass: Build hierarchical names (exactly like gltfImport.cpp buildSkeletonNodeNames)
    std::function<void(const aiNode*, int)> buildHierarchicalNames = 
        [&](const aiNode* node, int parentIndex) {
            int nodeIndex = mapping.nodeToIndex[node];
            std::string name = "n" + std::to_string(nodeIndex);
            std::string hierarchicalPath = parentIndex >= 0 ? 
                mapping.indexToHierarchicalPath[parentIndex] + "/" + name : name;
            
            mapping.indexToHierarchicalPath[nodeIndex] = hierarchicalPath;
            mapping.nodeNameToPath[node->mName.C_Str()] = hierarchicalPath;
            
            ASSIMP_LOG_DEBUG("USDZExporter: Node '" + std::string(node->mName.C_Str()) + 
                           "' (index " + std::to_string(nodeIndex) + ") → '" + hierarchicalPath + "'");
            
            for (uint32_t i = 0; i < node->mNumChildren; ++i) {
                buildHierarchicalNames(node->mChildren[i], nodeIndex);
            }
        };
    
    // Build the hierarchy mapping
    assignIndices(mScene->mRootNode);
    buildHierarchicalNames(mScene->mRootNode, -1);
    
    return mapping;
}

// ------------------------------------------------------------------------------------------------  
// Build joint paths using skeleton bone ordering (like gltfImport.cpp skin.joints)
USDZExporter::JointPathMapping USDZExporter::BuildJointPathsFromNodeHierarchy(
    const NodeHierarchyMapping& nodeMapping, const std::set<std::string>& allBoneNames,
    const std::map<std::string, const aiBone*>& boneDataMap) const {
    
    ASSIMP_LOG_DEBUG("USDZExporter: Building joint paths using skeleton bone ordering for " + 
                     std::to_string(allBoneNames.size()) + " bones");
    
    JointPathMapping mapping;
    
    // Find skeletons and use their bone ordering (like glTF skin.joints)
    for (uint32_t skelIdx = 0; skelIdx < mScene->mNumSkeletons; ++skelIdx) {
        const aiSkeleton* skeleton = mScene->mSkeletons[skelIdx];
        ASSIMP_LOG_DEBUG("USDZExporter: Processing skeleton '" + std::string(skeleton->mName.C_Str()) + 
                        "' with " + std::to_string(skeleton->mNumBones) + " bones");
        
        // Process bones in the order they appear in mBones (like gltf skin.joints order)
        for (uint32_t boneIdx = 0; boneIdx < skeleton->mNumBones; ++boneIdx) {
            const aiSkeletonBone* skelBone = skeleton->mBones[boneIdx];
            
                        // Get the actual scene node for this bone
                        const aiNode* boneNode = skelBone->mNode;  // This requires aiProcess_PopulateArmatureData
                        if (!boneNode) {
                            ASSIMP_LOG_WARN("USDZExporter: Skeleton bone " + std::to_string(boneIdx) + 
                                           " has no associated node (aiProcess_PopulateArmatureData not enabled?)");
                            continue;
                        }
            
            std::string boneName = boneNode->mName.C_Str();
            if (allBoneNames.find(boneName) == allBoneNames.end()) {
                continue; // Skip bones not in our bone set
            }
            
            // Get the hierarchical path for this bone's node
            auto pathIt = nodeMapping.nodeNameToPath.find(boneName);
            if (pathIt != nodeMapping.nodeNameToPath.end()) {
                mapping.skeletonJoints.push_back(pathIt->second);
                mapping.skeletonJointNames.push_back(boneName);
                
                // Find the corresponding aiBone* from mesh data (needed for bindTransforms)
                auto boneDataIt = boneDataMap.find(boneName);
                if (boneDataIt != boneDataMap.end()) {
                    mapping.skeletonBonePointers.push_back(boneDataIt->second);
                } else {
                    ASSIMP_LOG_WARN("USDZExporter: Could not find mesh bone data for skeleton bone '" + boneName + "'");
                    mapping.skeletonBonePointers.push_back(nullptr);
                }
                
                ASSIMP_LOG_DEBUG("USDZExporter: Skeleton joint[" + std::to_string(boneIdx) + 
                               "]: '" + boneName + "' → '" + pathIt->second + "'");
            } else {
                ASSIMP_LOG_WARN("USDZExporter: Could not find hierarchical path for bone '" + boneName + "'");
            }
        }
    }
    
    // Fallback: If no skeletons found, use mesh bone ordering (preserves original glTF skin.joints order)
    if (mapping.skeletonJoints.empty()) {
        ASSIMP_LOG_WARN("USDZExporter: No aiSkeleton found, falling back to mesh bone ordering");
        
        // Use mesh bone ordering - this preserves the original glTF skin.joints order
        std::set<std::string> processedBones;
        
        for (uint32_t meshIdx = 0; meshIdx < mScene->mNumMeshes; ++meshIdx) {
            const aiMesh* mesh = mScene->mMeshes[meshIdx];
            
            if (!mesh->HasBones()) {
                continue;
            }
            
            ASSIMP_LOG_DEBUG("USDZExporter: Processing mesh '" + std::string(mesh->mName.C_Str()) + 
                           "' with " + std::to_string(mesh->mNumBones) + " bones");
            
            // Process bones in the order they appear in mesh->mBones
            // This preserves the original glTF skin.joints ordering
            for (uint32_t boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
                const aiBone* bone = mesh->mBones[boneIdx];
                std::string boneName = bone->mName.C_Str();
                
                // Skip bones we've already processed (avoid duplicates across meshes)
                if (processedBones.find(boneName) != processedBones.end()) {
                    continue;
                }
                
                // Only process bones that are in our bone set
                if (allBoneNames.find(boneName) != allBoneNames.end()) {
                    auto pathIt = nodeMapping.nodeNameToPath.find(boneName);
                    if (pathIt != nodeMapping.nodeNameToPath.end()) {
                        mapping.skeletonJoints.push_back(pathIt->second);
                        mapping.skeletonJointNames.push_back(boneName);
                        mapping.skeletonBonePointers.push_back(bone); // ✅ CAPTURE CORRECT BONE POINTER
                        processedBones.insert(boneName);
                        
                        ASSIMP_LOG_DEBUG("USDZExporter: Added bone in mesh order: '" + boneName + 
                                       "' → '" + pathIt->second + "'");
                    }
                }
            }
        }
    }
    
    // Build animation joints (only bones that are actually animated)
    std::set<std::string> animatedBones;
    if (mScene->mNumAnimations > 0) {
        for (uint32_t animIdx = 0; animIdx < mScene->mNumAnimations; ++animIdx) {
            const aiAnimation* anim = mScene->mAnimations[animIdx];
            for (uint32_t chanIdx = 0; chanIdx < anim->mNumChannels; ++chanIdx) {
                const aiNodeAnim* nodeAnim = anim->mChannels[chanIdx];
                std::string nodeName = nodeAnim->mNodeName.C_Str();
                if (allBoneNames.find(nodeName) != allBoneNames.end()) {
                    animatedBones.insert(nodeName);
                }
            }
        }
    }
    
    // Use SAME ordering for animation joints as skeleton joints (revert previous change)
    // Animation joints are the subset of skeleton joints that are actually animated
    for (size_t i = 0; i < mapping.skeletonJoints.size(); ++i) {
        const std::string& boneName = mapping.skeletonJointNames[i];
        if (animatedBones.find(boneName) != animatedBones.end()) {
            mapping.animationJoints.push_back(mapping.skeletonJoints[i]);
            ASSIMP_LOG_DEBUG("USDZExporter: Animation joint (same order as skeleton): '" + boneName + 
                           "' → '" + mapping.skeletonJoints[i] + "'");
        }
    }
    
    // Build the bone name to joint path mapping
    for (size_t i = 0; i < mapping.skeletonJoints.size(); ++i) {
        mapping.boneToJointPath[mapping.skeletonJointNames[i]] = mapping.skeletonJoints[i];
        ASSIMP_LOG_DEBUG("USDZExporter: BONE_MAP: '" + mapping.skeletonJointNames[i] + "' -> '" + mapping.skeletonJoints[i] + "'");
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Built " + std::to_string(mapping.skeletonJoints.size()) + 
                     " skeleton joints and " + std::to_string(mapping.animationJoints.size()) + " animation joints");
    
    return mapping;
}

// ------------------------------------------------------------------------------------------------
// Complete SkelRoot with animation relationships and GeomScope after animations are created
void USDZExporter::CompleteSkelRootWithAnimation() {
    ASSIMP_LOG_DEBUG("USDZExporter: SkelRoot completion - Skeleton and Animation are already inside SkelRoot");
}

// ------------------------------------------------------------------------------------------------
// Helper function to find existing GeomScope in SkelRoot hierarchy for direct mesh placement
tinyusdz::Prim* USDZExporter::FindGeomScopeInSkelRoot() {
    tinyusdz::Prim* parentPrim = mSkeletonParentPrim;
    if (!parentPrim) parentPrim = FindSkeletonParentPrim();
    if (!parentPrim) {
        ASSIMP_LOG_WARN("USDZExporter: Could not find skeleton parent prim for GeomScope lookup");
        return nullptr;
    }
    
    for (auto& child : parentPrim->children()) {
        if (child.as<tinyusdz::SkelRoot>()) {
            for (auto& skelChild : child.children()) {
                if (skelChild.element_name() == "GeomScope") {
                    ASSIMP_LOG_DEBUG("USDZExporter: Found existing GeomScope in SkelRoot hierarchy");
                    return &skelChild;
                }
            }
            break;
        }
    }
    
    ASSIMP_LOG_WARN("USDZExporter: Could not find GeomScope in SkelRoot hierarchy");
    return nullptr;
}

#endif // !ASSIMP_BUILD_NO_USD_EXPORTER
#endif // ASSIMP_BUILD_NO_EXPORT
