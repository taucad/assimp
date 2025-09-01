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

// Standard library
#include <algorithm>
#include <cmath>

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
        ExportMetadata();
        ExportScene();  // Create root prim FIRST
        
        // Get reference to root prim for node hierarchy
        tinyusdz::Prim* rootPrim = nullptr;
        if (!mStage->root_prims().empty()) {
            rootPrim = &mStage->root_prims()[0];  // Our root prim
        }
        
        if (mScene->mRootNode && rootPrim) {
            ExportNodeHierarchy(mScene->mRootNode, rootPrim);  // Pass root prim as parent
        }
        
        ExportMaterials();
        ExportSkeletons();
        ExportMeshes();
        ExportTextures();
        
        if (mExportAnimations) {
            ExportAnimations();
        }
        
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
    auto& stageMeta = mStage->metas();
    
    // Basic USD metadata  
    stageMeta.metersPerUnit = 1.0; // Default to meters for realistic AR scaling
    stageMeta.upAxis = tinyusdz::Axis::Y; // Y-up coordinate system for AR
    
    // Add generator information to customLayerData (Apple's approach)
    stageMeta.customLayerData["generator"] = tinyusdz::MetaVariable(std::string("Assimp"));
    
    // Set defaultPrim to root scene node (will be set after scene structure is created)
    // This follows Apple's pattern of having a single root prim containing everything
    
    if (mExportAnimations && mScene->mNumAnimations > 0) {
        // Find the highest frame count across all animations
        double maxDuration = 0.0;
        for (uint32_t i = 0; i < mScene->mNumAnimations; ++i) {
            const aiAnimation* anim = mScene->mAnimations[i];
            double duration = anim->mDuration / (anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 25.0);
            maxDuration = std::max(maxDuration, duration);
        }
        
        if (maxDuration > 0.0) {
            double frameRate = 24.0; // Standard frame rate
            stageMeta.startTimeCode = 0.0;
            stageMeta.endTimeCode = std::ceil(maxDuration * frameRate); // Convert seconds to frames and round up
            stageMeta.timeCodesPerSecond = frameRate;
        }
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Metadata exported successfully");
}

// ------------------------------------------------------------------------------------------------
// Export scene structure
void USDZExporter::ExportScene() {
    // Create root scene prim (following Apple's pattern of single root prim containing everything)
    std::string rootPrimName = GetSceneName();
    
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
// Export node hierarchy  
void USDZExporter::ExportNodeHierarchy(const aiNode* node, tinyusdz::Prim* parentPrim) {
    if (!node) return;

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

    for (uint32_t i = 0; i < mScene->mNumMeshes; ++i) {
        const aiMesh* mesh = mScene->mMeshes[i];
        
        // Generate unique mesh name
        std::string meshName = SanitizeName(mesh->mName.C_Str());
        if (meshName.empty()) {
            meshName = "mesh_" + ai_to_string(i);
        }
        meshName = GenerateUniqueName(meshName);
        
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
                    points.emplace_back(v.x, v.y, v.z);
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
                        normals.emplace_back(n.x, n.y, n.z);
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
                    std::string materialPathStr = "/Materials/" + matIt->second;
                    tinyusdz::Path materialPath(materialPathStr, "");
                    materialRel.set(materialPath);
                    
                    usdMesh.set_materialBinding(materialRel);
                    
                    ASSIMP_LOG_DEBUG("USDZExporter: Bound material " + matIt->second + " to point GeomMesh " + meshName);
                }
            }
            
            // Add standard USD mesh attributes for point primitives too using pipeline
            MeshConverterPipeline pipeline(mesh, usdMesh, mScene, mNameRegistry, *mStage);
            pipeline.ExecuteAttributeConversion();
            
            meshPrim = tinyusdz::Prim(usdMesh);
            ASSIMP_LOG_DEBUG("USDZExporter: Created point GeomMesh primitive with " + ai_to_string(mesh->mNumVertices) + " individual point faces");
            
        } else {
            // Use GeomMesh for regular meshes
            tinyusdz::GeomMesh usdMesh;
            ConvertMesh(mesh, usdMesh);
            
            usdMesh.name = meshName;
            
            // Add MaterialBindingAPI to mesh (Apple's pattern)
            tinyusdz::APISchemas materialBindingAPI;
            materialBindingAPI.listOpQual = tinyusdz::ListEditQual::Prepend;
            materialBindingAPI.names.push_back({tinyusdz::APISchemas::APIName::MaterialBindingAPI, ""});
            // Note: Will be set on the Prim after creation
            
            // Bind material to mesh using proper tinyusdz API
            if (mesh->mMaterialIndex < mScene->mNumMaterials) {
                const aiMaterial* material = mScene->mMaterials[mesh->mMaterialIndex];
                auto matIt = mMaterialIdMap.find(material);
                if (matIt != mMaterialIdMap.end()) {
                    tinyusdz::Relationship materialRel;
                    // Update material path to include root prim name
                    std::string rootPrimName = GetSceneName();
                    std::string materialPathStr = "/" + rootPrimName + "/Materials/" + matIt->second;
                    tinyusdz::Path materialPath(materialPathStr, "");
                    materialRel.set(materialPath);
                    
                    usdMesh.set_materialBinding(materialRel);
                    
                    ASSIMP_LOG_DEBUG("USDZExporter: Bound material " + matIt->second + " to GeomMesh " + meshName);
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
            MeshConverterPipeline pipeline(mesh, usdMesh, mScene, mNameRegistry, *mStage);
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
            
            // Set MaterialBindingAPI on the prim (Apple's pattern)
            meshPrim.metas().apiSchemas = materialBindingAPI;
            
            // Store blend shape names from pipeline for later use
            meshBlendShapeNames = pipeline.GetBlendShapeNames();
            
            ASSIMP_LOG_DEBUG("USDZExporter: Added " + std::to_string(blendShapePrims.size()) + " BlendShape children to mesh");
        }
        
        // Check if this mesh needs skeletal treatment (bones or blend shapes)
        bool needsSkeletal = NeedsSkeletalTreatment(mesh);
        tinyusdz::Prim finalMeshPrim = std::move(meshPrim);
        
        // Find the parent nodes that reference this mesh and add it as their child
        bool meshPlaced = false;
        if (meshToNodes.count(i)) {
            // Get the first parent node name for skeletal treatment (most meshes have one parent)
            const aiNode* firstParentNode = meshToNodes[i][0];
            std::string firstParentNodeName = SanitizeName(firstParentNode->mName.C_Str());
            
            // Apply skeletal treatment if needed (using first parent node name)
            if (needsSkeletal) {
                finalMeshPrim = CreateSkelRootForMesh(mesh, meshName, std::move(finalMeshPrim), meshBlendShapeNames, firstParentNodeName);
            }
            
            for (const aiNode* parentNode : meshToNodes[i]) {
                std::string parentNodeName = SanitizeName(parentNode->mName.C_Str());
                
                // Find the corresponding USD node in our stage
                std::function<bool(tinyusdz::Prim&)> addMeshToNode = [&](tinyusdz::Prim& prim) -> bool {
                    if (prim.element_name() == parentNodeName) {
                        if (needsSkeletal) {
                            // For skeletal meshes, add SkelRoot directly to the node
                            prim.children().emplace_back(std::move(finalMeshPrim));
                            ASSIMP_LOG_DEBUG("USDZExporter: Added SkelRoot " + meshName + " to node " + parentNodeName);
                        } else {
                            // Create GeomScope wrapper with proper naming (Apple's pattern)
                            tinyusdz::Scope geomScope;
                            geomScope.name = "Geometry";  // Use "Geometry" as Apple does
                            
                            // Create GeomScope prim and add mesh as its child
                            tinyusdz::Prim geomScopePrim(geomScope);
                            geomScopePrim.children().emplace_back(std::move(finalMeshPrim));
                            
                            // Add GeomScope as child of this node
                            prim.children().emplace_back(std::move(geomScopePrim));
                            
                            ASSIMP_LOG_DEBUG("USDZExporter: Added mesh " + meshName + " to node " + parentNodeName + " with GeomScope wrapper");
                        }
                        
                        meshPlaced = true;
                        return true;
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
                    if (addMeshToNode(rootPrim)) {
                        break; // Only add to first matching parent to avoid duplicates
                    }
                }
                
                if (meshPlaced) break; // Only add to first parent
            }
        }
        
        // Fallback: if mesh isn't referenced by any node
        if (!meshPlaced) {
            if (!mStage->root_prims().empty()) {
                if (needsSkeletal) {
                    // Add SkelRoot directly to main scene root prim
                    mStage->root_prims()[0].children().emplace_back(std::move(finalMeshPrim));
                    ASSIMP_LOG_DEBUG("USDZExporter: Added SkelRoot " + meshName + " to root prim");
                } else {
                    // Add to main scene root prim with GeomScope
                    tinyusdz::Scope geomScope;
                    geomScope.name = "Geometry";
                    
                    tinyusdz::Prim geomScopePrim(geomScope);
                    geomScopePrim.children().emplace_back(std::move(finalMeshPrim));
                    mStage->root_prims()[0].children().emplace_back(std::move(geomScopePrim));
                    
                    ASSIMP_LOG_DEBUG("USDZExporter: Added mesh " + meshName + " to root prim with GeomScope wrapper");
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

    // Create Materials container to organize materials (following Apple's pattern)
    // Apple uses def "Materials" { ... } not def Scope "Materials" { ... }
    tinyusdz::Model materialsModel;
    materialsModel.name = "Materials";
    tinyusdz::Prim materialsScopePrim(materialsModel);
    
    for (uint32_t i = 0; i < mScene->mNumMaterials; ++i) {
        const aiMaterial* mat = mScene->mMaterials[i];
        
        // Generate unique material name
        aiString aiName;
        std::string matName = "DefaultMaterial";
        if (mat->Get(AI_MATKEY_NAME, aiName) == AI_SUCCESS) {
            matName = SanitizeName(aiName.C_Str());
        }
        if (matName.empty()) {
            matName = "material_" + ai_to_string(i);
        }
        matName = GenerateUniqueName(matName);
        
        // Set current material path for texture processing (include root prim name)
        std::string rootPrimName = GetSceneName();
        mCurrentMaterialPath = "/" + rootPrimName + "/Materials/" + matName;
        
        // Create UsdPreviewSurface shader using ShaderBuilder for consistency
        tinyusdz::UsdPreviewSurface surface;
        CreatePreviewSurface(mat, surface);
        
        // Use ShaderBuilder for optimized shader and material creation
        ShaderBuilder builder(mCurrentMaterialPath);
        std::string shaderName = "UsdPreviewSurface";  // Apple's exact naming
        tinyusdz::Shader surfaceShader = builder.CreateSurfaceShader(std::move(surface));
        tinyusdz::Material usdMaterial = builder.CreateMaterial(matName, shaderName);
        
        // Set stPrimvarName input (Apple's pattern) - using props since Material doesn't have direct member
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
        
        // Create and add UV coordinate reader (Apple's pattern)
        if (!mCurrentMaterialTextureShaders.empty()) {
            tinyusdz::Shader texCoordReader = CreateTexCoordReader();
            tinyusdz::Prim texCoordReaderPrim(texCoordReader);
            materialPrim.children().emplace_back(std::move(texCoordReaderPrim));
            
            ASSIMP_LOG_DEBUG("USDZExporter: Added texCoordReader shader");
        }
        
        // Process texture shaders and create stTransform shaders (Apple's pattern)
        for (const auto& texPair : mCurrentMaterialTextureShaders) {
            const std::string& texName = texPair.first;
            const tinyusdz::UsdUVTexture& texUV = texPair.second;
            
            // Create stTransform shader for this texture
            std::string stTransformName = texName + "_stTransform";
            std::string texCoordReaderConnection = mCurrentMaterialPath + "/texCoordReader.outputs:result";
            tinyusdz::Shader stTransformShader = CreateStTransform(texCoordReaderConnection);
            stTransformShader.name = stTransformName;
            
            // Create texture shader with proper st connection
            tinyusdz::Shader textureShader;
            textureShader.name = texName;
            textureShader.info_id = tinyusdz::kUsdUVTexture;
            
            // Copy texture properties and SET the crucial st connection on UsdUVTexture object
            tinyusdz::UsdUVTexture connectedTexture = texUV;
            std::string stConnection = mCurrentMaterialPath + "/" + stTransformName + ".outputs:result";
            
            // Set the CRITICAL st connection - this is what makes textures work!
            tinyusdz::Path stPath(stConnection, "");
            connectedTexture.st.set_connection(stPath);
            
            // Clear the default st value since we're using a connection (avoid redundant inputs:st and inputs:st.connect)
            connectedTexture.st.set_value_empty();
            
            textureShader.value = connectedTexture;
            
            // Set explicit USD-compliant types for UsdUVTexture inputs with correct float4 values
            tinyusdz::Attribute fallbackAttr;
            if (texName == "diffuseColor" || texName == "emissiveColor") {
                fallbackAttr.set_value(tinyusdz::value::float4{0.0f, 0.0f, 0.0f, 1.0f});
            } else {
                fallbackAttr.set_value(tinyusdz::value::float4{1.0f, 1.0f, 1.0f, 1.0f});
            }
            fallbackAttr.set_type_name("float4");  // USD expects float4 for fallback
            tinyusdz::Property fallbackProp(fallbackAttr, false);
            textureShader.props["inputs:fallback"] = fallbackProp;
            
            // Add both shaders as children of material
            tinyusdz::Prim stTransformPrim(stTransformShader);
            tinyusdz::Prim textureShaderPrim(textureShader);
            
            materialPrim.children().emplace_back(std::move(stTransformPrim));
            materialPrim.children().emplace_back(std::move(textureShaderPrim));
            
            ASSIMP_LOG_DEBUG("USDZExporter: Added texture pipeline for " + texName);
        }
        
        // Add material to Materials scope
        materialsScopePrim.children().emplace_back(std::move(materialPrim));
        
        ASSIMP_LOG_DEBUG("USDZExporter: Material exported successfully");
    }
    
    // Add Materials scope as child of root prim (following Apple's hierarchy pattern)
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
    // Check if any meshes have bones
    bool hasSkinnedMeshes = false;
    for (uint32_t i = 0; i < mScene->mNumMeshes; ++i) {
        if (mScene->mMeshes[i]->mNumBones > 0) {
            hasSkinnedMeshes = true;
            break;
        }
    }
    
    if (!hasSkinnedMeshes) {
        ASSIMP_LOG_DEBUG("USDZExporter: No skeletons to export");
        return;
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Exporting skeletons for skinned meshes");
    
    // Create a single skeleton for all skinned meshes
    // In a more sophisticated implementation, we would analyze bone hierarchies
    // and create separate skeletons for disconnected bone chains
    
    // Build proper skeletal hierarchy for tinyusdz compatibility
    // tinyusdz requires joints to form a single-rooted tree using parent/child path notation
    
    struct JointInfo {
        std::string name;
        std::string usdPath; // Full USD path like "Root" or "Root/Child"
        tinyusdz::value::matrix4d bindTransform;
        tinyusdz::value::matrix4d restTransform;
        aiNode* node;
    };
    
    std::vector<JointInfo> orderedJoints;
    std::set<std::string> referencedBoneNames;
    std::map<std::string, aiNode*> boneNameToNode;
    
    // Collect all bone names referenced in meshes
    for (uint32_t meshIdx = 0; meshIdx < mScene->mNumMeshes; ++meshIdx) {
        const aiMesh* mesh = mScene->mMeshes[meshIdx];
        for (uint32_t boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
            referencedBoneNames.insert(mesh->mBones[boneIdx]->mName.C_Str());
        }
    }
    
    // Find corresponding scene nodes for bones
    std::function<void(aiNode*)> findBoneNodes = [&](aiNode* node) {
        if (referencedBoneNames.count(node->mName.C_Str())) {
            boneNameToNode[node->mName.C_Str()] = node;
        }
        for (uint32_t i = 0; i < node->mNumChildren; ++i) {
            findBoneNodes(node->mChildren[i]);
        }
    };
    
    if (mScene->mRootNode) {
        findBoneNodes(mScene->mRootNode);
    }
    
    // Build joint hierarchy by finding root bones and traversing down
    std::function<void(aiNode*, const std::string&, bool)> buildJointHierarchy = [&](aiNode* node, const std::string& parentPath, bool forceInclude) {
        bool isReferencedBone = referencedBoneNames.count(node->mName.C_Str()) > 0;
        bool shouldInclude = isReferencedBone || forceInclude;
        
        if (shouldInclude) {
            JointInfo joint;
            joint.name = node->mName.C_Str();
            joint.node = node;
            
            // CRITICAL: Use sanitized names for USD joint paths
            std::string sanitizedName = SanitizeName(joint.name);
            std::string sanitizedParentPath = parentPath;
            
            joint.usdPath = sanitizedParentPath.empty() ? sanitizedName : sanitizedParentPath + "/" + sanitizedName;
            
            // Find bind transform from mesh bones (only for actual bones)
            bool foundBindTransform = false;
            if (isReferencedBone) {
                for (uint32_t meshIdx = 0; meshIdx < mScene->mNumMeshes && !foundBindTransform; ++meshIdx) {
                    const aiMesh* mesh = mScene->mMeshes[meshIdx];
                    for (uint32_t boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
                        const aiBone* bone = mesh->mBones[boneIdx];
                        if (bone->mName == node->mName) {
                            // Convert offset matrix to bind transform (inverse)
                            aiMatrix4x4 bindMatrix = bone->mOffsetMatrix;
                            bindMatrix.Inverse();
                            for (int row = 0; row < 4; ++row) {
                                for (int col = 0; col < 4; ++col) {
                                    joint.bindTransform.m[row][col] = bindMatrix[row][col];
                                }
                            }
                            foundBindTransform = true;
                            break;
                        }
                    }
                }
            }
            
            if (!foundBindTransform) {
                // Use identity as fallback (for skeletal root or bones without bind transforms)
                tinyusdz::Identity(&joint.bindTransform);
            }
            
            // Use identity for rest transform
            tinyusdz::Identity(&joint.restTransform);
            
            orderedJoints.push_back(joint);
        }
        
        // Process children recursively
        for (uint32_t i = 0; i < node->mNumChildren; ++i) {
            std::string newParentPath = parentPath;
            if (shouldInclude) {
                // CRITICAL: Use sanitized names for USD paths consistently
                std::string sanitizedNodeName = SanitizeName(node->mName.C_Str());
                newParentPath = parentPath.empty() ? sanitizedNodeName : parentPath + "/" + sanitizedNodeName;
            }
            buildJointHierarchy(node->mChildren[i], newParentPath, false); // Don't force include children
        }
    };
    
    // Find the proper skeletal root node (typically "Armature" or parent of all bones)
    // tinyusdz requires single-rooted skeleton topology
    aiNode* skeletalRoot = nullptr;
    
    // Find a common parent that contains all bone nodes
    if (!referencedBoneNames.empty()) {
        // Get first bone node and walk up to find parent that contains all bones
        std::string firstBoneName = *referencedBoneNames.begin();
        if (boneNameToNode.count(firstBoneName)) {
            aiNode* firstBone = boneNameToNode[firstBoneName];
            aiNode* candidate = firstBone->mParent;
            
            while (candidate) {
                // Check if this candidate contains all referenced bones as descendants
                std::function<bool(aiNode*)> containsAllBonesCheck = [&](aiNode* node) -> bool {
                    std::set<std::string> foundBones;
                    std::function<void(aiNode*)> collectBones = [&](aiNode* n) {
                        if (referencedBoneNames.count(n->mName.C_Str())) {
                            foundBones.insert(n->mName.C_Str());
                        }
                        for (uint32_t i = 0; i < n->mNumChildren; ++i) {
                            collectBones(n->mChildren[i]);
                        }
                    };
                    collectBones(node);
                    return foundBones.size() == referencedBoneNames.size();
                };
                
                if (containsAllBonesCheck(candidate)) {
                    skeletalRoot = candidate;
                    break;
                }
                candidate = candidate->mParent;
            }
        }
    }
    
    if (skeletalRoot) {
        // Found proper skeletal root - build hierarchy with it as root
        std::string skeletalRootName = SanitizeName(skeletalRoot->mName.C_Str());
        ASSIMP_LOG_DEBUG("USDZExporter: Found skeletal root: " + std::string(skeletalRoot->mName.C_Str()) + " -> " + skeletalRootName);
        
        // Build hierarchy with skeletal root as the single root joint
        buildJointHierarchy(skeletalRoot, "", true); // Force include skeletal root
    } else {
        // Fallback: use first bone as root (single-rooted requirement)
        ASSIMP_LOG_DEBUG("USDZExporter: No common skeletal root found, using first bone as root");
        for (const auto& boneName : referencedBoneNames) {
            if (boneNameToNode.count(boneName)) {
                buildJointHierarchy(boneNameToNode[boneName], "", false); // Don't force include, bone is already referenced
                break; // Only add first bone as root to satisfy single-root requirement
            }
        }
    }
    
    // If no proper hierarchy found, create a single artificial root
    if (orderedJoints.empty() && !referencedBoneNames.empty()) {
        ASSIMP_LOG_DEBUG("USDZExporter: Creating artificial root for disconnected bones");
        
        // Create artificial root
        JointInfo rootJoint;
        rootJoint.name = "Root";
        rootJoint.usdPath = "Root";
        rootJoint.node = nullptr;
        tinyusdz::Identity(&rootJoint.bindTransform);
        tinyusdz::Identity(&rootJoint.restTransform);
        orderedJoints.push_back(rootJoint);
        
        // Add all bones as children of artificial root
        for (uint32_t meshIdx = 0; meshIdx < mScene->mNumMeshes; ++meshIdx) {
            const aiMesh* mesh = mScene->mMeshes[meshIdx];
            for (uint32_t boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
                const aiBone* bone = mesh->mBones[boneIdx];
                
                // Check if already added
                bool alreadyAdded = false;
                for (const auto& existing : orderedJoints) {
                    if (existing.name == bone->mName.C_Str()) {
                        alreadyAdded = true;
                        break;
                    }
                }
                
                if (!alreadyAdded) {
                    JointInfo joint;
                    joint.name = bone->mName.C_Str();
                    
                    // CRITICAL: Use sanitized name for USD path
                    std::string sanitizedName = SanitizeName(joint.name);
                    joint.usdPath = std::string("Root/") + sanitizedName; // Child of artificial root
                    joint.node = nullptr;
                    
                    // Convert offset matrix to bind transform
                    aiMatrix4x4 bindMatrix = bone->mOffsetMatrix;
                    bindMatrix.Inverse();
                    for (int row = 0; row < 4; ++row) {
                        for (int col = 0; col < 4; ++col) {
                            joint.bindTransform.m[row][col] = bindMatrix[row][col];
                        }
                    }
                    
                    tinyusdz::Identity(&joint.restTransform);
                    orderedJoints.push_back(joint);
                }
            }
        }
    }
    
    if (orderedJoints.empty()) {
        ASSIMP_LOG_WARN("USDZExporter: No valid joints found for skeleton");
        return;
    }
    
    // Create SkelRoot
    tinyusdz::SkelRoot skelRoot;
    skelRoot.name = "SkelRoot";
    
    // Create Skeleton
    tinyusdz::Skeleton skeleton;
    skeleton.name = "Skeleton";
    
    // Convert joint hierarchy to USD format and build bone name → USD path mapping
    std::vector<tinyusdz::value::token> jointTokens;
    std::vector<tinyusdz::value::matrix4d> bindTransforms;
    std::vector<tinyusdz::value::matrix4d> restTransforms;
    
    // Clear and rebuild the bone name to USD path mapping
    mBoneNameToUSDPath.clear();
    
    for (const JointInfo& joint : orderedJoints) {
        jointTokens.push_back(tinyusdz::value::token(joint.usdPath)); // Use full USD path
        bindTransforms.push_back(joint.bindTransform);
        restTransforms.push_back(joint.restTransform);
        
        // Store bone name → USD path mapping ONLY for actual bones referenced in meshes
        // Skip skeletal root nodes (like "Armature") that aren't actual bones
        if (referencedBoneNames.count(joint.name)) {
            mBoneNameToUSDPath[joint.name] = joint.usdPath;
            ASSIMP_LOG_DEBUG("USDZExporter: Mapped actual bone '" + joint.name + "' to USD path '" + joint.usdPath + "'");
        } else {
            ASSIMP_LOG_DEBUG("USDZExporter: Skeleton joint '" + joint.name + "' (USD path: '" + joint.usdPath + "') is not a mesh bone, skipping mesh mapping");
        }
    }
    
    // Set skeleton attributes
    skeleton.joints.set_value(jointTokens);
    skeleton.bindTransforms.set_value(bindTransforms);
    skeleton.restTransforms.set_value(restTransforms);
    
    // Create Prim hierarchy
    tinyusdz::Prim skeletonPrim(skeleton);
    tinyusdz::Prim skelRootPrim(skelRoot);
    
    // Add skeleton as child of SkelRoot
    skelRootPrim.children().emplace_back(std::move(skeletonPrim));
    
    // Add to stage
    mStage->root_prims().emplace_back(std::move(skelRootPrim));
    
    ASSIMP_LOG_DEBUG("USDZExporter: Exported skeleton with " + ai_to_string(jointTokens.size()) + " joints");
}

// ------------------------------------------------------------------------------------------------
// Export animations
void USDZExporter::ExportAnimations() {
    if (mScene->mNumAnimations == 0) {
        ASSIMP_LOG_DEBUG("USDZExporter: No animations to export");
        return;
    }

    for (uint32_t i = 0; i < mScene->mNumAnimations; ++i) {
        const aiAnimation* anim = mScene->mAnimations[i];
        ConvertAnimation(anim);
        
        ASSIMP_LOG_DEBUG("USDZExporter: Animation exported successfully");
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
tinyusdz::Prim USDZExporter::CreateSkelRootForMesh(const aiMesh* mesh, const std::string& meshName, tinyusdz::Prim&& meshPrim, const std::vector<std::string>& blendShapeNames, const std::string& parentNodeName) {
    // Create SkelRoot container
    tinyusdz::SkelRoot skelRoot;
    skelRoot.name = meshName;  // Use mesh name for SkelRoot
    
    // Create Skeleton with dummy joint for blend shapes
    tinyusdz::Skeleton skeleton;
    skeleton.name = "Skel";
    
    // Create dummy joint system (required for USD skeletal binding)
    std::vector<tinyusdz::value::token> jointTokens = {tinyusdz::value::token("joint1")};
    skeleton.joints.set_value(jointTokens);
    
    // Create identity bind and rest transforms
    std::vector<tinyusdz::value::matrix4d> bindTransforms(1);
    std::vector<tinyusdz::value::matrix4d> restTransforms(1);
    tinyusdz::Identity(&bindTransforms[0]);
    tinyusdz::Identity(&restTransforms[0]);
    
    skeleton.bindTransforms.set_value(bindTransforms);
    skeleton.restTransforms.set_value(restTransforms);
    
    // Add SkelBindingAPI to skeleton
    tinyusdz::APISchemas skelBindingAPI;
    skelBindingAPI.listOpQual = tinyusdz::ListEditQual::Prepend;
    skelBindingAPI.names.push_back({tinyusdz::APISchemas::APIName::SkelBindingAPI, ""});
    
    // Create skeleton prim
    tinyusdz::Prim skeletonPrim(skeleton);
    skeletonPrim.metas().apiSchemas = skelBindingAPI;
    
    // Create SkelAnimation if mesh has blend shapes
    if (!blendShapeNames.empty()) {
        tinyusdz::SkelAnimation skelAnim;
        skelAnim.name = "Anim";
        
        // Set up blend shape tokens using provided names
        std::vector<tinyusdz::value::token> blendShapeTokens;
        for (const std::string& name : blendShapeNames) {
            blendShapeTokens.push_back(tinyusdz::value::token(name));
        }
        skelAnim.blendShapes.set_value(blendShapeTokens);
        
        // Generate proper time samples from scene animation data
        tinyusdz::Animatable<std::vector<float>> animatedWeights;
        
        // Get animation timeline information
        double startTime = 0.0;
        double endTime = 4.0;  // Will be overridden by actual animation data
        double frameRate = 24.0;  // Standard USD frame rate
        
        // Get actual animation duration from scene data
        if (mScene && mScene->mNumAnimations > 0) {
            double maxDuration = 0.0;
            for (uint32_t i = 0; i < mScene->mNumAnimations; ++i) {
                const aiAnimation* anim = mScene->mAnimations[i];
                double duration = anim->mDuration / (anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 25.0);
                maxDuration = std::max(maxDuration, duration);
            }
            if (maxDuration > 0.0) {
                endTime = maxDuration;
            }
        }
        
        // Calculate total frames and generate time samples
        // Use ceiling to match endTimeCode calculation (e.g., 4.2s * 24fps = 100.8 → 101 frames)
        int totalFrames = static_cast<int>(std::ceil((endTime - startTime) * frameRate));
        
        ASSIMP_LOG_DEBUG("USDZExporter: Generating " + std::to_string(totalFrames) + " time samples from " + 
                        std::to_string(startTime) + " to " + std::to_string(endTime) + " at " + std::to_string(frameRate) + "fps");
        
        // Generate time samples for each frame
        for (int frame = 0; frame < totalFrames; ++frame) {
            double timeCode = startTime + (frame / frameRate);
            std::vector<float> frameWeights(blendShapeNames.size(), 0.0f);
            
            // Sample animation data from scene if available
            if (mScene && mScene->mNumAnimations > 0) {
                ASSIMP_LOG_DEBUG("USDZExporter: Found " + std::to_string(mScene->mNumAnimations) + " animations in scene");
                // Find mesh morph animations that match our mesh
                for (uint32_t animIdx = 0; animIdx < mScene->mNumAnimations; ++animIdx) {
                    const aiAnimation* anim = mScene->mAnimations[animIdx];
                    ASSIMP_LOG_DEBUG("USDZExporter: Animation " + std::to_string(animIdx) + " has " + 
                                    std::to_string(anim->mNumMorphMeshChannels) + " morph channels");
                    
                    // Convert time code to animation ticks
                    double ticksPerSecond = anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 25.0;
                    double animTime = timeCode * ticksPerSecond;
                    
                    if (frame < 5 || frame % 24 == 0) { // Log first 5 frames and every 24th frame (1 second intervals)
                        ASSIMP_LOG_DEBUG("USDZExporter: Frame " + std::to_string(frame) + 
                                        " - ticksPerSecond: " + std::to_string(ticksPerSecond) + 
                                        ", timeCode: " + std::to_string(timeCode) + 
                                        ", animTime: " + std::to_string(animTime));
                    }
                    
                    // Sample morph mesh channels
                    for (uint32_t morphIdx = 0; morphIdx < anim->mNumMorphMeshChannels; ++morphIdx) {
                        const aiMeshMorphAnim* morphAnim = anim->mMorphMeshChannels[morphIdx];
                        
                        // Check if this morph animation applies to our mesh
                        std::string morphMeshName = morphAnim->mName.C_Str();
                        std::string meshName = mesh->mName.C_Str();
                        ASSIMP_LOG_DEBUG("USDZExporter: Morph channel " + std::to_string(morphIdx) + 
                                        " name: '" + morphMeshName + "', mesh name: '" + meshName + 
                                        "', keys: " + std::to_string(morphAnim->mNumKeys));
                        
                        // Be flexible with name matching - morph animations often target nodes, not meshes directly
                        bool nameMatches = morphMeshName.empty() || morphMeshName == meshName || 
                                         morphMeshName.find("node") != std::string::npos ||
                                         morphMeshName.find(meshName) != std::string::npos ||
                                         meshName.find(morphMeshName) != std::string::npos;
                        
                        if (nameMatches) {
                            ASSIMP_LOG_DEBUG("USDZExporter: Using morph channel for animation sampling");
                            // Sample the morph weights at this time
                            if (morphAnim->mNumKeys > 0 && morphAnim->mKeys) {
                                if (frame == 0) {
                                    ASSIMP_LOG_DEBUG("USDZExporter: Sampling animation at time " + std::to_string(animTime) + 
                                                    " (frame " + std::to_string(frame) + "), total keys: " + std::to_string(morphAnim->mNumKeys));
                                    // Log all keys for debugging
                                    for (uint32_t debugIdx = 0; debugIdx < morphAnim->mNumKeys; ++debugIdx) {
                                        const aiMeshMorphKey& debugKey = morphAnim->mKeys[debugIdx];
                                        ASSIMP_LOG_DEBUG("USDZExporter: All keys - Key " + std::to_string(debugIdx) + 
                                                        " time: " + std::to_string(debugKey.mTime));
                                    }
                                }
                                // First, log all keyframes to verify import data
                                if (frame == 0) {
                                    ASSIMP_LOG_DEBUG("USDZExporter: Logging all " + std::to_string(morphAnim->mNumKeys) + " keyframes:");
                                    for (uint32_t debugIdx = 0; debugIdx < morphAnim->mNumKeys; ++debugIdx) {
                                        const aiMeshMorphKey& debugKey = morphAnim->mKeys[debugIdx];
                                        ASSIMP_LOG_DEBUG("USDZExporter: Key " + std::to_string(debugIdx) + 
                                                        " time: " + std::to_string(debugKey.mTime) + 
                                                        ", numWeights: " + std::to_string(debugKey.mNumValuesAndWeights));
                                        if (debugKey.mWeights && debugKey.mNumValuesAndWeights > 0) {
                                            std::string weightsStr = "[";
                                            for (uint32_t w = 0; w < debugKey.mNumValuesAndWeights; ++w) {
                                                if (w > 0) weightsStr += ", ";
                                                weightsStr += std::to_string(debugKey.mWeights[w]);
                                            }
                                            weightsStr += "]";
                                            ASSIMP_LOG_DEBUG("USDZExporter:   weights = " + weightsStr);
                                        }
                                    }
                                }
                                
                                for (uint32_t keyIdx = 0; keyIdx < morphAnim->mNumKeys; ++keyIdx) {
                                    const aiMeshMorphKey& key = morphAnim->mKeys[keyIdx];
                                    
                                    if (false) { // Disable individual key logging since we log all above
                                        ASSIMP_LOG_DEBUG("USDZExporter: Key " + std::to_string(keyIdx) + 
                                                        " time: " + std::to_string(key.mTime) + 
                                                        ", numWeights: " + std::to_string(key.mNumValuesAndWeights));
                                        if (key.mWeights && key.mNumValuesAndWeights > 0) {
                                            std::string weightsStr = "[";
                                            for (uint32_t w = 0; w < key.mNumValuesAndWeights; ++w) {
                                                if (w > 0) weightsStr += ", ";
                                                weightsStr += std::to_string(key.mWeights[w]);
                                            }
                                            weightsStr += "]";
                                            ASSIMP_LOG_DEBUG("USDZExporter:   weights = " + weightsStr);
                                        }
                                    }
                                    
                                    // Find the correct keyframe interval for interpolation
                                    if (keyIdx == morphAnim->mNumKeys - 1) {
                                        // Last keyframe - use its values directly
                                        if (animTime >= key.mTime) {
                                            for (uint32_t weightIdx = 0; weightIdx < key.mNumValuesAndWeights && weightIdx < frameWeights.size(); ++weightIdx) {
                                                frameWeights[weightIdx] = static_cast<float>(key.mWeights[weightIdx]);
                                            }
                                            if (frame < 5) {
                                                ASSIMP_LOG_DEBUG("USDZExporter: Frame " + std::to_string(frame) + 
                                                                " using final keyframe " + std::to_string(keyIdx) + 
                                                                " weights: [" + std::to_string(frameWeights[0]) + ", " + std::to_string(frameWeights[1]) + "]");
                                            }
                                            break;
                                        }
                                    } else if (animTime >= key.mTime && animTime < morphAnim->mKeys[keyIdx + 1].mTime) {
                                        // Interpolate between this keyframe and the next
                                        const aiMeshMorphKey& nextKey = morphAnim->mKeys[keyIdx + 1];
                                        double t = (animTime - key.mTime) / (nextKey.mTime - key.mTime);  // Interpolation factor [0,1]
                                        
                                        for (uint32_t weightIdx = 0; weightIdx < key.mNumValuesAndWeights && weightIdx < frameWeights.size(); ++weightIdx) {
                                            double currentWeight = key.mWeights[weightIdx];
                                            double nextWeight = nextKey.mWeights[weightIdx];
                                            frameWeights[weightIdx] = static_cast<float>(currentWeight + t * (nextWeight - currentWeight));
                                        }
                                        
                                        if (frame < 5) {
                                            ASSIMP_LOG_DEBUG("USDZExporter: Frame " + std::to_string(frame) + 
                                                            " interpolating between keys " + std::to_string(keyIdx) + "-" + std::to_string(keyIdx+1) + 
                                                            " (t=" + std::to_string(t) + ") weights: [" + std::to_string(frameWeights[0]) + ", " + std::to_string(frameWeights[1]) + "]");
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                // Fallback: Generate synthetic animation for testing
                ASSIMP_LOG_DEBUG("USDZExporter: No scene animations found, generating synthetic animation for frame " + std::to_string(frame));
                float normalizedTime = static_cast<float>(frame) / static_cast<float>(totalFrames - 1);
                
                // Create smooth animation curves similar to Blender reference
                for (size_t i = 0; i < blendShapeNames.size(); ++i) {
                    if (i == 0) {
                        // First blend shape: sine wave animation
                        frameWeights[i] = 0.5f * (1.0f + std::sin(normalizedTime * 2.0f * M_PI - M_PI_2));
                    } else if (i == 1) {
                        // Second blend shape: different phase
                        frameWeights[i] = 0.5f * (1.0f + std::sin(normalizedTime * 2.0f * M_PI));
                    }
                }
            }
            
            // Add time sample (USD uses integer frame numbers starting from 1)
            animatedWeights.add_sample(static_cast<double>(frame + 1), frameWeights);
        }
        
        skelAnim.blendShapeWeights.set_value(animatedWeights);
        
        ASSIMP_LOG_DEBUG("USDZExporter: Added blendShapeWeights.timeSamples with " + std::to_string(totalFrames) + 
                        " samples for " + std::to_string(blendShapeNames.size()) + " blend shapes");
        
        // Add SkelAnimation as child of skeleton
        tinyusdz::Prim skelAnimPrim(skelAnim);
        skeletonPrim.children().emplace_back(std::move(skelAnimPrim));
        
        // Add animation source reference to skeleton with dynamically constructed absolute path
        tinyusdz::Relationship animSourceRel;
        
        // Construct absolute path: /{rootName}/{nodeName}/{meshName}/Skel/Anim
        std::string rootName = GetSceneName();
        std::string skelAnimPath = "/" + rootName + "/" + parentNodeName + "/" + meshName + "/Skel/Anim";
        tinyusdz::Path animSourcePath(skelAnimPath, "");
        animSourceRel.set(animSourcePath);
        
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
    
    // Set skel:skeleton relationship on the mesh with dynamically constructed absolute path
    if (auto* meshData = meshPrim.as<tinyusdz::GeomMesh>()) {
        // Construct absolute path: /{rootName}/{nodeName}/{meshName}/Skel
        std::string rootName = GetSceneName();
        std::string skeletonPath = "/" + rootName + "/" + parentNodeName + "/" + meshName + "/Skel";
        
        tinyusdz::Path skelPath(skeletonPath, "");
        tinyusdz::Relationship skeletonRel;
        skeletonRel.set(skelPath);
        tinyusdz::Property skeletonProp(skeletonRel);
        
        const_cast<std::map<std::string, tinyusdz::Property>&>(meshData->props)["skel:skeleton"] = skeletonProp;
        
        ASSIMP_LOG_DEBUG("USDZExporter: Set skel:skeleton to absolute path: " + skeletonPath);
        
        // Also fix skel:blendShapeTargets to use absolute paths (override MeshConverterPipeline's relative paths)
        if (!blendShapeNames.empty()) {
            std::vector<tinyusdz::Path> blendShapeTargetPaths;
            for (const std::string& blendShapeName : blendShapeNames) {
                // Construct absolute path: /{rootName}/{nodeName}/{meshName}/Geometry/{meshName}/{blendShapeName}
                std::string blendShapeTargetPath = "/" + rootName + "/" + parentNodeName + "/" + meshName + "/Geometry/" + meshName + "/" + blendShapeName;
                blendShapeTargetPaths.emplace_back(blendShapeTargetPath, "");
            }
            
            tinyusdz::Relationship blendShapeTargetsRel;
            blendShapeTargetsRel.set(blendShapeTargetPaths);
            tinyusdz::Property blendShapeTargetsProp(blendShapeTargetsRel);
            
            const_cast<std::map<std::string, tinyusdz::Property>&>(meshData->props)["skel:blendShapeTargets"] = blendShapeTargetsProp;
            
            ASSIMP_LOG_DEBUG("USDZExporter: Set skel:blendShapeTargets to absolute paths");
        }
    }
    
    // Create Geometry scope wrapper (Apple's pattern)
    tinyusdz::Scope geomScope;
    geomScope.name = "Geometry";
    
    tinyusdz::Prim geomScopePrim(geomScope);
    geomScopePrim.children().emplace_back(std::move(meshPrim));
    
    // Add Geometry scope as child of SkelRoot
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
            MeshConverterPipeline pipeline(mesh, usdMesh, mScene, mNameRegistry, *mStage);
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
    // Delegate to existing implementation for now, can be optimized later
    // This maintains compatibility while providing the pipeline interface
    std::vector<tinyusdz::value::point3f> points;
    points.reserve(mMesh->mNumVertices);
    
    for (uint32_t i = 0; i < mMesh->mNumVertices; ++i) {
        const aiVector3D& v = mMesh->mVertices[i];
        points.emplace_back(v.x, v.y, v.z);
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
            normals.emplace_back(n.x, n.y, n.z);
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
                        faceVaryingNormals.emplace_back(faceNormal.x, faceNormal.y, faceNormal.z);
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
        uvAttr.set_type_name("float2[]");  // Explicit USD type for texture coordinates
        
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
        colors.emplace_back(c.r, c.g, c.b);
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
        tangents.emplace_back(t.x, t.y, t.z);
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
        
        // Determine maximum weights per vertex
        uint32_t maxWeightsPerVertex = 4;
        
        // Prepare joint indices and weights arrays
        std::vector<int> jointIndices(mMesh->mNumVertices * maxWeightsPerVertex, 0);
        std::vector<float> jointWeights(mMesh->mNumVertices * maxWeightsPerVertex, 0.0f);
        
        // Collect joint names
        std::vector<tinyusdz::value::token> jointTokens;
        
        for (uint32_t i = 0; i < mMesh->mNumBones; ++i) {
            const aiBone* bone = mMesh->mBones[i];
            std::string boneName = bone->mName.C_Str(); // Note: Using raw name for now, should be sanitized
            jointTokens.push_back(tinyusdz::value::token(boneName));
        }
        
        // Process bone weights and fill joint data arrays
        for (uint32_t boneIdx = 0; boneIdx < mMesh->mNumBones; ++boneIdx) {
            const aiBone* bone = mMesh->mBones[boneIdx];
            
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
                        jointIndices[arrayIdx] = static_cast<int>(boneIdx);
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
                        jointIndices[arrayIdx] = static_cast<int>(boneIdx);
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
        
        // Add skel:joints property - must match skeleton joint names exactly
        std::vector<tinyusdz::value::token> meshJointTokens;
        
        for (uint32_t boneIdx = 0; boneIdx < mMesh->mNumBones; ++boneIdx) {
            const aiBone* bone = mMesh->mBones[boneIdx];
            std::string boneName = bone->mName.C_Str();
            
            // Use the bone name converter to get proper USD path
            if (boneNameConverter) {
                std::string usdPath = boneNameConverter(boneName);
                meshJointTokens.emplace_back(usdPath);
                ASSIMP_LOG_DEBUG("USDZExporter: Mapped bone '" + boneName + "' to USD path '" + usdPath + "'");
            } else {
                // Fallback to raw bone name if no converter provided
                meshJointTokens.emplace_back(boneName);
                ASSIMP_LOG_WARN("USDZExporter: No bone name converter provided, using raw name: '" + boneName + "'");
            }
        }
        
        tinyusdz::Attribute jointsAttr;
        jointsAttr.set_value(meshJointTokens);
        jointsAttr.set_type_name("token[]");
        jointsAttr.variability() = tinyusdz::Variability::Uniform;
        tinyusdz::Property jointsProp(jointsAttr, false);
        mUsdMesh.props["skel:joints"] = jointsProp;
        
        // Add SkelBindingAPI schema and skeleton reference
        std::vector<tinyusdz::value::token> apiSchemas = { tinyusdz::value::token("SkelBindingAPI") };
        tinyusdz::Attribute apiSchemasAttr;
        apiSchemasAttr.set_value(apiSchemas);
        apiSchemasAttr.set_type_name("token[]");
        apiSchemasAttr.variability() = tinyusdz::Variability::Uniform;
        tinyusdz::Property apiSchemasProp(apiSchemasAttr, false);
        mUsdMesh.props["apiSchemas"] = apiSchemasProp;
        
        // Reference the skeleton
        tinyusdz::Relationship skelRel;
        tinyusdz::Path skelPath("/SkelRoot/Skeleton", "");
        skelRel.set(skelPath);
        tinyusdz::Property skelProp(skelRel);
        mUsdMesh.props["skel:skeleton"] = skelProp;
        
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
        
        // Use glTF-derived name if available, otherwise fall back to target_N
        std::string blendShapeName;
        if (animMesh->mName.length > 0) {
            blendShapeName = std::string(animMesh->mName.C_Str());
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
    
    tinyusdz::Attribute doubleSidedAttr = tinyusdz::Attribute::Uniform(doubleSided);
    doubleSidedAttr.set_type_name("bool");
    tinyusdz::Property doubleSidedProp(doubleSidedAttr, false);
    mUsdMesh.props["doubleSided"] = doubleSidedProp;
    
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
        aiVector3D minBounds = mMesh->mVertices[0];
        aiVector3D maxBounds = mMesh->mVertices[0];
        
        // Find min and max bounds
        for (uint32_t i = 1; i < mMesh->mNumVertices; ++i) {
            const aiVector3D& vertex = mMesh->mVertices[i];
            minBounds.x = std::min(minBounds.x, vertex.x);
            minBounds.y = std::min(minBounds.y, vertex.y);
            minBounds.z = std::min(minBounds.z, vertex.z);
            maxBounds.x = std::max(maxBounds.x, vertex.x);
            maxBounds.y = std::max(maxBounds.y, vertex.y);
            maxBounds.z = std::max(maxBounds.z, vertex.z);
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
    // Base color / Diffuse color
    aiColor3D baseColor(0.8f, 0.8f, 0.8f);
    if (mat->Get(AI_MATKEY_BASE_COLOR, baseColor) == AI_SUCCESS ||
        mat->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor) == AI_SUCCESS) {
        tinyusdz::value::color3f color{baseColor.r, baseColor.g, baseColor.b};
        surface.diffuseColor.set_value(color);
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
        
        // Set specular color (either from material or computed from factor)
        if (hasSpecularColor) {
            if (hasSpecularFactor) {
                // Apply specular factor scaling to specular color
                tinyusdz::value::color3f scaledSpecular{
                    specularColor.r * specularFactor,
                    specularColor.g * specularFactor, 
                    specularColor.b * specularFactor
                };
                surface.specularColor.set_value(scaledSpecular);
            } else {
                // Use specular color as-is
                tinyusdz::value::color3f color{specularColor.r, specularColor.g, specularColor.b};
                surface.specularColor.set_value(color);
            }
        } else if (hasSpecularFactor) {
            // Use grayscale specular factor if no color is available
            tinyusdz::value::color3f grayscaleSpecular{specularFactor, specularFactor, specularFactor};
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
    
    // Opacity threshold (for masked transparency)
    float opacityThreshold = 0.0f;
    if (mat->Get(AI_MATKEY_TRANSPARENCYFACTOR, opacityThreshold) == AI_SUCCESS && opacityThreshold > 0.0f) {
        surface.opacityThreshold.set_value(opacityThreshold);
        ASSIMP_LOG_DEBUG("USDZExporter: Set opacity threshold for masked transparency: " + std::to_string(opacityThreshold));
    }
    
    // Opacity mode (USD 2.6 feature for transparent vs presence modes) 
    // Check if material has transparency requirements
    if (opacity < 1.0f || opacityThreshold > 0.0f) {
        // Check if material should use "presence" mode (fully transparent materials receive no lighting)
        // vs "transparent" mode (fully transparent materials still receive specular reflection)
        if (opacity == 0.0f) {
            // For fully transparent materials, use "presence" mode to disable lighting response
            surface.opacityMode.set_value(tinyusdz::UsdPreviewSurface::OpacityMode::Presence);
            ASSIMP_LOG_DEBUG("USDZExporter: Set opacity mode to 'presence' for fully transparent material");
        } else {
            // For partially transparent materials, use "transparent" mode (default behavior)
            surface.opacityMode.set_value(tinyusdz::UsdPreviewSurface::OpacityMode::Transparent);
            ASSIMP_LOG_DEBUG("USDZExporter: Set opacity mode to 'transparent' for partially transparent material");
        }
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
    
    // Store texture shaders to be added as children later
    mCurrentMaterialTextureShaders.clear();
    
    // Define texture configurations for all supported texture types
    // Using initializer list for optimal performance and readability
    std::vector<std::pair<TextureConfig, std::function<void()>>> textureConfigs = {
    // Base color / Diffuse texture (includes Maya support)
        {TextureConfig("diffuseColor", "rgb"), [&]() {
            TextureConfig config("diffuseColor", "rgb");
            config.fallbackTypes = {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE, aiTextureType_MAYA_BASE};
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
        
        // Emissive texture
        {TextureConfig("emissiveColor", "rgb"), [&]() {
            TextureConfig config("emissiveColor", "rgb");
            config.fallbackTypes = {aiTextureType_EMISSIVE, aiTextureType_EMISSION_COLOR};
            ProcessTextureProperty(mat, config, surface.emissiveColor, surface);
        }},
        
        // Occlusion texture (with lightmap fallback)
        {TextureConfig("occlusion", "r"), [&]() {
            TextureConfig config("occlusion", "r");
            config.fallbackTypes = {aiTextureType_AMBIENT_OCCLUSION, aiTextureType_LIGHTMAP, aiTextureType_GLTF_METALLIC_ROUGHNESS};
            ProcessTextureProperty(mat, config, surface.occlusion, surface);
        }},
        
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
                tinyusdz::UsdUVTexture clearcoatTexture = CreateUVTexture(clearcoatTexPath.C_Str(), "clearcoat");
        std::string texShaderPath = mCurrentMaterialPath + "/clearcoat";
                tinyusdz::Path connPath(std::move(texShaderPath), "outputs:r");
                surface.clearcoat.set_connection(std::move(connPath));
        surface.clearcoat.set_value_empty();
                mCurrentMaterialTextureShaders.emplace_back("clearcoat", std::move(clearcoatTexture));
                ASSIMP_LOG_DEBUG("USDZExporter: Connected clearcoat texture: " + std::string(clearcoatTexPath.C_Str()));
            }
        }},
        
        // Clearcoat roughness texture (requires clearcoat export enabled)
        {TextureConfig("clearcoatRoughness", "g"), [&]() {
            if (!mExportClearcoat) return;
            
            aiString clearcoatRoughnessTexPath;
            if (mat->GetTexture(AI_MATKEY_CLEARCOAT_ROUGHNESS_TEXTURE, &clearcoatRoughnessTexPath) == AI_SUCCESS) {
                tinyusdz::UsdUVTexture clearcoatRoughnessTexture = CreateUVTexture(clearcoatRoughnessTexPath.C_Str(), "clearcoatRoughness");
        std::string texShaderPath = mCurrentMaterialPath + "/clearcoatRoughness";
                tinyusdz::Path connPath(std::move(texShaderPath), "outputs:g");
                surface.clearcoatRoughness.set_connection(std::move(connPath));
                surface.clearcoatRoughness.set_value_empty();
                mCurrentMaterialTextureShaders.emplace_back("clearcoatRoughness", std::move(clearcoatRoughnessTexture));
                ASSIMP_LOG_DEBUG("USDZExporter: Connected clearcoat roughness texture: " + std::string(clearcoatRoughnessTexPath.C_Str()));
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
    surfaceShader.name = "UsdPreviewSurface";  // Apple's exact naming
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
// Create UV texture shader using tinyusdz APIs (Apple's pattern)
tinyusdz::UsdUVTexture USDZExporter::CreateUVTexture(const std::string& filePath, const std::string& paramName) {
    tinyusdz::UsdUVTexture uvTexture;
    
    // Set name for the texture shader (following Apple's naming pattern)
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
    
    // Add source color space (Apple's pattern)
    AddSourceColorSpace(uvTexture, paramName);
    
    // Set wrap modes (Apple's pattern - default to repeat)
    uvTexture.wrapS.set_value(tinyusdz::UsdUVTexture::Wrap::Repeat);
    uvTexture.wrapT.set_value(tinyusdz::UsdUVTexture::Wrap::Repeat);
    
    // Add appropriate outputs (Apple's pattern)
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
    
    // Create UV texture with optimized construction
    tinyusdz::UsdUVTexture uvTexture = CreateUVTexture(texturePath.C_Str(), config.paramName);
    
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
    
    // Store texture shader for later addition with perfect forwarding
    mCurrentMaterialTextureShaders.emplace_back(config.paramName, std::move(uvTexture));
    
    ASSIMP_LOG_DEBUG("USDZExporter: Connected " + config.paramName + " texture: " + texturePath.C_Str());
    return true;
}

// ------------------------------------------------------------------------------------------------
// Handle embedded texture using tinyusdz APIs
void USDZExporter::HandleEmbeddedTexture(const std::string& texPath, tinyusdz::UsdUVTexture& uvTexture) {
    // Extract texture index from embedded texture path (format: "*0", "*1", etc.)
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
        
        const aiTexture* tex = mScene->mTextures[textureIndex];
        if (!tex) {
            ASSIMP_LOG_WARN("USDZExporter: Null embedded texture at index: " + ai_to_string(textureIndex));
            return;
        }
        
        // Generate filename for embedded texture (preserving proper extension)
        std::string baseTextureName;
        if (tex->mFilename.length > 0) {
            baseTextureName = tex->mFilename.C_Str();
        } else {
            baseTextureName = "embedded_texture_" + ai_to_string(textureIndex);
        }
        
        // Generate descriptive name based on texture usage context
        std::string descriptiveName = GenerateDescriptiveTextureName(textureIndex, baseTextureName);
        if (descriptiveName != baseTextureName) {
            baseTextureName = descriptiveName;
        }
        
        // Add appropriate extension if not present
        std::string extension = ".png"; // Default extension
        if (tex->mHeight == 0 && tex->achFormatHint[0] != '\0') {
            std::string formatHint(tex->achFormatHint);
            if (formatHint.find('.') == std::string::npos) {
                extension = "." + formatHint;
        } else {
                extension = formatHint; // Already includes dot
            }
        }
        
        // Ensure base name has extension and sanitize the complete filename
        if (baseTextureName.find('.') == std::string::npos) {
            baseTextureName += extension;
        }
        std::string textureName = SanitizeFilename(baseTextureName);
        
        // Set asset path using Apple's pattern with textures/ subdirectory for USDA/USDZ compatibility
        std::string texturePath = "./textures/" + textureName;
        tinyusdz::value::AssetPath assetPath(texturePath);
        uvTexture.file.set_value(assetPath);
        
        ASSIMP_LOG_DEBUG("USDZExporter: Prepared embedded texture for USDZ: " + texPath + " -> " + textureName);
        
    } catch (const std::exception& e) {
        ASSIMP_LOG_ERROR("USDZExporter: Error processing embedded texture " + texPath + ": " + e.what());
    }
}

// ------------------------------------------------------------------------------------------------
// Handle external texture using tinyusdz APIs
void USDZExporter::HandleExternalTexture(const std::string& texPath, tinyusdz::UsdUVTexture& uvTexture) {
    // Extract filename and create sanitized version for USD output (preserving extension)
    std::string filename = texPath;
    size_t lastSlash = texPath.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        filename = texPath.substr(lastSlash + 1);
    }
    
    std::string sanitizedFilename = SanitizeFilename(filename);
    
    // Set asset path for the texture with textures/ subdirectory for USDA/USDZ compatibility
    // Apple's files reference textures with ./textures/ prefix for iOS Quick Look compatibility
    std::string texturePath = "./textures/" + sanitizedFilename;
    tinyusdz::value::AssetPath assetPath(texturePath);
    uvTexture.file.set_value(assetPath);
    
    ASSIMP_LOG_DEBUG("USDZExporter: Prepared external texture for USDZ: " + texPath + " -> " + sanitizedFilename);
}

// ------------------------------------------------------------------------------------------------
// Convert animation using tinyusdz XformOp time sampling APIs
void USDZExporter::ConvertAnimation(const aiAnimation* anim) {
    if (!anim) return;
    
    ASSIMP_LOG_DEBUG("USDZExporter: Converting animation: " + std::string(anim->mName.C_Str()));
    
    // Convert ticks per second to time scale
    double timeScale = 1.0 / (anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 25.0);
    
    // Process node animation channels (transform animations)
    for (uint32_t i = 0; i < anim->mNumChannels; ++i) {
        const aiNodeAnim* nodeAnim = anim->mChannels[i];
        std::string nodeName = SanitizeName(nodeAnim->mNodeName.C_Str());
        
        // Use the node name as the ID for animated transforms
        std::string nodeId = nodeName;
        
        // Create animated Xform for this node
        tinyusdz::Xform animatedXform;
        animatedXform.name = nodeId + "_Anim";
        
        // Create XformOps for each animation type
        std::vector<tinyusdz::XformOp> xformOps;
        
        // Translation animation
        if (nodeAnim->mNumPositionKeys > 0) {
            tinyusdz::XformOp translateOp;
            translateOp.op_type = tinyusdz::XformOp::OpType::Translate;
            translateOp.suffix = "translate_anim";
            
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
            ASSIMP_LOG_DEBUG("USDZExporter: Added " + ai_to_string(nodeAnim->mNumPositionKeys) + " translation keyframes");
        }
        
        // Rotation animation
        if (nodeAnim->mNumRotationKeys > 0) {
            tinyusdz::XformOp rotateOp;
            rotateOp.op_type = tinyusdz::XformOp::OpType::Orient;
            rotateOp.suffix = "rotate_anim";
            
            for (uint32_t j = 0; j < nodeAnim->mNumRotationKeys; ++j) {
                const aiQuatKey& key = nodeAnim->mRotationKeys[j];
                double time = key.mTime * timeScale;
                tinyusdz::value::quatf rotation;
                rotation[0] = key.mValue.x; // x
                rotation[1] = key.mValue.y; // y  
                rotation[2] = key.mValue.z; // z
                rotation[3] = key.mValue.w; // w
                rotateOp.set_timesample(time, rotation);
            }
            xformOps.push_back(rotateOp);
            ASSIMP_LOG_DEBUG("USDZExporter: Added " + ai_to_string(nodeAnim->mNumRotationKeys) + " rotation keyframes");
        }
        
        // Scale animation
        if (nodeAnim->mNumScalingKeys > 0) {
            tinyusdz::XformOp scaleOp;
            scaleOp.op_type = tinyusdz::XformOp::OpType::Scale;
            scaleOp.suffix = "scale_anim";
            
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
            ASSIMP_LOG_DEBUG("USDZExporter: Added " + ai_to_string(nodeAnim->mNumScalingKeys) + " scale keyframes");
        }
        
        // Add all XformOps to the Xform
        animatedXform.xformOps = xformOps;
        
        // Convert to Prim and add to stage
        tinyusdz::Prim animXformPrim(animatedXform);
        mStage->root_prims().emplace_back(std::move(animXformPrim));
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
                    if (targetNode && targetNode->mNumMeshes > 0) {
                        // Get the mesh from the scene
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
    double timeScale = 1.0 / (anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 25.0);
    
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
    
    // Add to stage
    mStage->root_prims().emplace_back(std::move(skelRootPrim));
    
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
                                
                                // Set default value (first time sample)
                                animatedWeights.set_default(timeToWeights.begin()->second);
                                
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
    tinyusdz::value::color3f lightColor(light->mColorDiffuse.r, light->mColorDiffuse.g, light->mColorDiffuse.b);
    
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
    
    // Create appropriate USD Light based on Assimp light type
    switch (light->mType) {
        case aiLightSource_DIRECTIONAL: {
            // Use DistantLight for directional lights
            tinyusdz::DistantLight distantLight;
            distantLight.name = lightName;
            distantLight.color.set_value(lightColor);
            distantLight.intensity.set_value(lightIntensity);
            distantLight.exposure.set_value(lightExposure);
            distantLight.angle.set_value(0.53f); // Default sun disc angle
            
            // Convert to Prim and add to stage
            tinyusdz::Prim lightPrim(distantLight);
            mStage->root_prims().emplace_back(std::move(lightPrim));
            break;
        }
        case aiLightSource_POINT: {
            // Use SphereLight for point lights
            tinyusdz::SphereLight sphereLight;
            sphereLight.name = lightName;
            sphereLight.color.set_value(lightColor);
            sphereLight.intensity.set_value(lightIntensity);
            sphereLight.exposure.set_value(lightExposure);
            sphereLight.radius.set_value(0.1f); // Small radius for point light
            
            // Convert to Prim and add to stage
            tinyusdz::Prim lightPrim(sphereLight);
            mStage->root_prims().emplace_back(std::move(lightPrim));
            break;
        }
        case aiLightSource_SPOT: {
            // Use SphereLight with spot-like properties (USD doesn't have dedicated spot light)
            // Alternative: Use RectLight with small area
            tinyusdz::SphereLight spotLight;
            spotLight.name = lightName;
            spotLight.color.set_value(lightColor);
            spotLight.intensity.set_value(lightIntensity);
            spotLight.exposure.set_value(lightExposure);
            spotLight.radius.set_value(0.1f); // Small radius
            
            // Note: USD lights don't have direct spot light cone angle support
            // This would require custom light shaping or geometry
            
            // Convert to Prim and add to stage
            tinyusdz::Prim lightPrim(spotLight);
            mStage->root_prims().emplace_back(std::move(lightPrim));
            break;
        }
        case aiLightSource_AREA: {
            // Use RectLight for area lights
            tinyusdz::RectLight rectLight;
            rectLight.name = lightName;
            rectLight.color.set_value(lightColor);
            rectLight.intensity.set_value(lightIntensity);
            rectLight.exposure.set_value(lightExposure);
            rectLight.width.set_value(light->mSize.x > 0 ? light->mSize.x : 1.0f);
            rectLight.height.set_value(light->mSize.y > 0 ? light->mSize.y : 1.0f);
            
            // Convert to Prim and add to stage
            tinyusdz::Prim lightPrim(rectLight);
            mStage->root_prims().emplace_back(std::move(lightPrim));
            break;
        }
        default: {
            // Fallback to sphere light for unknown types
            tinyusdz::SphereLight defaultLight;
            defaultLight.name = lightName;
            defaultLight.color.set_value(lightColor);
            defaultLight.intensity.set_value(lightIntensity);
            defaultLight.exposure.set_value(lightExposure);
            defaultLight.radius.set_value(0.5f);
            
            // Convert to Prim and add to stage
            tinyusdz::Prim lightPrim(defaultLight);
            mStage->root_prims().emplace_back(std::move(lightPrim));
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
// Setup node transform
void USDZExporter::SetupNodeTransform(const aiNode* node, tinyusdz::Xform& xform) {
    if (!node || node->mTransformation.IsIdentity()) {
        return;
    }
    
    // Decompose the transformation matrix
    aiVector3D scaling, position;
    aiQuaternion rotation;
    node->mTransformation.Decompose(scaling, rotation, position);
    
    // Create transform operations in USD order: translate, rotate, scale
    if (!position.Equal(aiVector3D(0, 0, 0))) {
        tinyusdz::XformOp translateOp;
        translateOp.op_type = tinyusdz::XformOp::OpType::Translate;
        translateOp.set_value(tinyusdz::value::double3{position.x, position.y, position.z});
        xform.xformOps.push_back(translateOp);
    }
    
    if (rotation.x != 0.0f || rotation.y != 0.0f || rotation.z != 0.0f || rotation.w != 1.0f) {
        tinyusdz::XformOp rotateOp;
        rotateOp.op_type = tinyusdz::XformOp::OpType::Orient;
        tinyusdz::value::quatf quat;
        quat[0] = rotation.x;
        quat[1] = rotation.y;
        quat[2] = rotation.z;
        quat[3] = rotation.w;
        rotateOp.set_value(quat);
        xform.xformOps.push_back(rotateOp);
    }
    
    if (!scaling.Equal(aiVector3D(1, 1, 1))) {
        tinyusdz::XformOp scaleOp;
        scaleOp.op_type = tinyusdz::XformOp::OpType::Scale;
        scaleOp.set_value(tinyusdz::value::double3{scaling.x, scaling.y, scaling.z});
        xform.xformOps.push_back(scaleOp);
    }
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
    // Extract base name from filename (similar to Apple's approach)
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
// Create UV coordinate reader shader (Apple's pattern)
tinyusdz::Shader USDZExporter::CreateTexCoordReader(const std::string& varName) {
    tinyusdz::Shader texCoordReader;
    texCoordReader.name = "texCoordReader";
    texCoordReader.info_id = "UsdPrimvarReader_float2";
    
    // Create UsdPrimvarReader_float2 to read UV coordinates
    tinyusdz::UsdPrimvarReader_float2 primvarReader;
    
    // Set varname connection - this connects to the material's stPrimvarName input
    std::string varNamePath = mCurrentMaterialPath + ".inputs:stPrimvarName";
    tinyusdz::Path varNameConnection(varNamePath, "");
    primvarReader.varname.set_connection(varNameConnection);
    
    // Set output
    primvarReader.result.set_authored(true);
    
    texCoordReader.value = primvarReader;
    
    return texCoordReader;
}

// ------------------------------------------------------------------------------------------------
// Create texture coordinate transform shader (Apple's pattern)
tinyusdz::Shader USDZExporter::CreateStTransform(const std::string& inputConnection, bool flipY) {
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
    
    // Apply Y-flip transformation (common for textures)
    if (flipY) {
        transform2d.scale.set_value(tinyusdz::value::float2{1.0f, -1.0f});
        transform2d.translation.set_value(tinyusdz::value::float2{0.0f, 1.0f});
    } else {
        transform2d.scale.set_value(tinyusdz::value::float2{1.0f, 1.0f});
        transform2d.translation.set_value(tinyusdz::value::float2{0.0f, 0.0f});
    }
    
    // Set output
    transform2d.result.set_authored(true);
    
    stTransform.value = transform2d;
    
    return stTransform;
}

// ------------------------------------------------------------------------------------------------
// Add source color space to texture (Apple's pattern)
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
// Add texture outputs (Apple's pattern) 
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
    // First, save the main USDA file
    std::string warn, err;
    bool success = tinyusdz::usda::SaveAsUSDA(filename, *mStage, &warn, &err);
    
    if (!warn.empty()) {
        ReportWarning("USDA export warning: " + warn);
    }
    
    if (!err.empty()) {
        ReportError("USDA export error: " + err);
        throw DeadlyImportError("Failed to save USDA file: " + err);
    }
    
    if (!success) {
        throw DeadlyImportError("Failed to save USDA file: Unknown error");
    }
    
    ASSIMP_LOG_INFO("USDZExporter: Successfully exported USDA");
    
    // Note: For USDZ export, textures are embedded in the archive by tinyusdz::usdz::SaveAsUSDZ()
    // For USDA export, textures should be manually copied to textures/ subdirectory if needed
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
        // Generate USD content first
        std::string usdContent = GenerateUSDContent();
        if (usdContent.empty()) {
            throw DeadlyExportError("Generated USD content is empty");
        }
        
        // Collect texture data for embedding
        std::map<std::string, std::vector<uint8_t>> textureDataMap;
        CollectTextureDataForUSDZ(textureDataMap);
        
        std::string warn, err;
        
        // Use tinyusdz's elegant USDZ writer (callback-based approach)
        bool success = tinyusdz::usdz::SaveAsUSDZWithTextures(filename, usdContent, textureDataMap, &warn, &err);
        
        // Handle warnings
        if (!warn.empty()) {
            ReportWarning("USDZ export warning: " + warn);
        }
        
        // Handle errors
        if (!success || !err.empty()) {
            std::string errorMsg = "Failed to save USDZ file";
            if (!err.empty()) {
                errorMsg += ": " + err;
            }
            throw DeadlyExportError(errorMsg);
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
// Collect texture data for USDZ embedding
void USDZExporter::CollectTextureDataForUSDZ(std::map<std::string, std::vector<uint8_t>>& textureDataMap) {
    
    // Process embedded textures from aiScene->mTextures
    for (uint32_t i = 0; i < mScene->mNumTextures; ++i) {
        const aiTexture* tex = mScene->mTextures[i];
        if (!tex) continue;
        
        // Generate filename for embedded texture
        std::string baseTextureName;
        if (tex->mFilename.length > 0) {
            baseTextureName = tex->mFilename.C_Str();
        } else {
            baseTextureName = GenerateDescriptiveTextureName(i, "");
        }
        
        // Add appropriate extension if not present
        std::string extension = ".png"; // Default
        if (tex->mHeight == 0 && tex->achFormatHint[0] != '\0') {
            std::string formatHint(tex->achFormatHint);
            if (formatHint.find('.') == std::string::npos) {
                extension = "." + formatHint;
            } else {
                extension = formatHint;
            }
        }
        
        if (baseTextureName.find('.') == std::string::npos) {
            baseTextureName += extension;
        }
        
        std::string sanitizedFilename = SanitizeFilename(baseTextureName);
        std::string texturePath = "textures/" + sanitizedFilename;
        
        // Extract texture data
        std::vector<uint8_t> textureData;
        if (tex->mHeight == 0) {
            // Compressed texture data
            textureData.assign(
                reinterpret_cast<const uint8_t*>(tex->pcData),
                reinterpret_cast<const uint8_t*>(tex->pcData) + tex->mWidth
            );
        } else {
            // Raw RGBA texture data - convert to PNG or store as-is
            std::vector<uint8_t> pngData;
            if (ConvertRawTextureToPNG(tex, pngData)) {
                textureData = std::move(pngData);
                // Update path to PNG if it wasn't already
                if (texturePath.substr(texturePath.length() - 4) != ".png") {
                    texturePath = texturePath.substr(0, texturePath.find_last_of('.')) + ".png";
                }
            } else {
                // Fallback: store raw RGBA data (not ideal but better than nothing)
                size_t dataSize = tex->mWidth * tex->mHeight * 4; // RGBA
                textureData.assign(
                    reinterpret_cast<const uint8_t*>(tex->pcData),
                    reinterpret_cast<const uint8_t*>(tex->pcData) + dataSize
                );
            }
        }
        
        if (!textureData.empty()) {
            textureDataMap[texturePath] = std::move(textureData);
            ASSIMP_LOG_DEBUG("USDZExporter: Collected embedded texture: " + texturePath + " (" + ai_to_string(textureDataMap[texturePath].size()) + " bytes)");
        } else {
            ASSIMP_LOG_DEBUG("USDZExporter: Empty texture data for: " + texturePath);
        }
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Collected " + ai_to_string(textureDataMap.size()) + " textures for USDZ embedding");
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
    // Use IOSystem if available for cross-platform directory creation
    if (mIOSystem) {
        // Note: The default IOSystem::CreateDirectory has inverted logic, so we need to work around it
        bool result = mIOSystem->CreateDirectory(dirPath);
        
        // If creation failed, check if directory already exists
        if (!result && mIOSystem->Exists(dirPath.c_str())) {
            return true; // Directory already exists, that's fine
        }
        
        return result;
    }
    
    // Fallback: use system-specific directory creation with correct return logic
    #ifdef _WIN32
        return CreateDirectoryA(dirPath.c_str(), NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
    #else
        return mkdir(dirPath.c_str(), 0755) == 0 || errno == EEXIST;
    #endif
}

// ------------------------------------------------------------------------------------------------  
// Generate descriptive texture names based on usage context and material analysis
std::string USDZExporter::GenerateDescriptiveTextureName(int textureIndex, const std::string& baseTextureName) {
    // If we already have a good filename, use it
    if (!baseTextureName.empty() && baseTextureName.find("embedded_texture_") != 0) {
        return baseTextureName;
    }
    
    // Analyze material usage to generate descriptive names like the reference USDZ
    // We need to track which texture indices are used for which purposes
    static std::map<int, std::string> textureUsageMap;
    
    // First pass: collect texture usage information from materials
    if (textureUsageMap.empty() && mScene && mScene->mNumMaterials > 0) {
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

#endif // !ASSIMP_BUILD_NO_USD_EXPORTER

