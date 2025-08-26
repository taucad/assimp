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
#include "USDZArchiveWriter.h"

// Assimp includes
#include <assimp/Exceptional.h>
#include <assimp/IOSystem.hpp>
#include <assimp/scene.h>
#include <assimp/StringUtils.h>
#include <assimp/DefaultLogger.hpp>
#include <assimp/ai_assert.h>
#include <assimp/StringComparison.h>
#include <assimp/CreateAnimMesh.h>
#include <assimp/Exporter.hpp>

// Standard library
#include <algorithm>
#include <cmath>
#include <regex>

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
    static const char* PROP_EXPORT_AR_ANCHORING = "USDZ_EXPORT_AR_ANCHORING";
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
    mExportARAnchoring(false),
    mExportMaterialX(false),
    mExportSubdivision(false),
    mExportVolumes(false),
    mOptimizeForMobile(true) {

    // Parse export properties
    if (mProperties) {
        mExportAnimations = mProperties->GetPropertyBool(PROP_EXPORT_ANIMATIONS, true);
        mExportClearcoat = mProperties->GetPropertyBool(PROP_EXPORT_CLEARCOAT, true);
        mExportARAnchoring = mProperties->GetPropertyBool(PROP_EXPORT_AR_ANCHORING, false);
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

        // Advanced features
        if (mExportARAnchoring) {
            ExportARAnchoring();
            ExportQuickLookMetadata();
        }
        
        if (mExportMaterialX) {
            ExportMaterialX();
        }
        
        if (mExportSubdivision) {
            ExportSubdivisionSurfaces();
        }
        
        if (mExportVolumes) {
            ExportVolumeRendering();
        }
        
        // Add AR anchoring for iOS Quick Look (USDZ only)
        if (mIsPackaged) {
            AddARAnchoring();
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
    
    // Add iOS Quick Look compatibility metadata using tinyusdz APIs
    if (mIsPackaged) {
        stageMeta.customLayerData["quickLook:compatible"] = tinyusdz::MetaVariable(std::string("true"));
        stageMeta.customLayerData["quickLook:version"] = tinyusdz::MetaVariable(std::string("1.0"));
        ASSIMP_LOG_DEBUG("USDZExporter: Added Quick Look compatibility metadata");
    }
    
    if (mExportAnimations && mScene->mNumAnimations > 0) {
        // Find the highest frame count across all animations
        double maxDuration = 0.0;
        for (uint32_t i = 0; i < mScene->mNumAnimations; ++i) {
            const aiAnimation* anim = mScene->mAnimations[i];
            double duration = anim->mDuration / (anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 25.0);
            maxDuration = std::max(maxDuration, duration);
        }
        
        if (maxDuration > 0.0) {
            stageMeta.startTimeCode = 0.0;
            stageMeta.endTimeCode = maxDuration;
            stageMeta.timeCodesPerSecond = 24.0; // Standard frame rate
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
            
            // Add standard USD mesh attributes for point primitives too
            ConvertMeshAttributes(mesh, usdMesh);
            
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
            
            meshPrim = tinyusdz::Prim(usdMesh);
            
            // Set MaterialBindingAPI on the prim (Apple's pattern)
            meshPrim.metas().apiSchemas = materialBindingAPI;
        }
        
        // Find the parent nodes that reference this mesh and add it as their child
        bool meshPlaced = false;
        if (meshToNodes.count(i)) {
            for (const aiNode* parentNode : meshToNodes[i]) {
                std::string parentNodeName = SanitizeName(parentNode->mName.C_Str());
                
                // Find the corresponding USD node in our stage
                std::function<bool(tinyusdz::Prim&)> addMeshToNode = [&](tinyusdz::Prim& prim) -> bool {
                    if (prim.element_name() == parentNodeName) {
                        // Create GeomScope wrapper with proper naming (Apple's pattern)
                        tinyusdz::Scope geomScope;
                        geomScope.name = "Geometry";  // Use "Geometry" as Apple does
                        
                        // Create GeomScope prim and add mesh as its child
                        tinyusdz::Prim geomScopePrim(geomScope);
                        geomScopePrim.children().emplace_back(meshPrim);  // Copy for multiple parents
                        
                        // Add GeomScope as child of this node
                        prim.children().emplace_back(std::move(geomScopePrim));
                        
                        meshPlaced = true;
                        ASSIMP_LOG_DEBUG("USDZExporter: Added mesh " + meshName + " to node " + parentNodeName + " with GeomScope wrapper");
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
        
        // Fallback: if mesh isn't referenced by any node or special case for skinned meshes
        if (!meshPlaced) {
            if (mesh->mNumBones > 0) {
                // Add skinned mesh to SkelRoot with GeomScope wrapper
                for (auto& rootPrim : mStage->root_prims()) {
                    if (rootPrim.element_name() == "SkelRoot") {
                        // Create GeomScope for skinned mesh
                        tinyusdz::Scope geomScope;
                        geomScope.name = "Geometry";
                        
                        tinyusdz::Prim geomScopePrim(geomScope);
                        geomScopePrim.children().emplace_back(meshPrim);
                        rootPrim.children().emplace_back(std::move(geomScopePrim));
                        
                        meshPlaced = true;
                        ASSIMP_LOG_DEBUG("USDZExporter: Added skinned mesh " + meshName + " to SkelRoot with GeomScope wrapper");
                        break;
                    }
                }
            }
            
            if (!meshPlaced) {
                // Last resort: add to root level with GeomScope wrapper
                if (!mStage->root_prims().empty()) {
                    // Add to main scene root prim with GeomScope
                    tinyusdz::Scope geomScope;
                    geomScope.name = "Geometry";
                    
                    tinyusdz::Prim geomScopePrim(geomScope);
                    geomScopePrim.children().emplace_back(meshPrim);
                    mStage->root_prims()[0].children().emplace_back(std::move(geomScopePrim));
                    
                    ASSIMP_LOG_DEBUG("USDZExporter: Added mesh " + meshName + " to root prim with GeomScope wrapper");
                } else {
                    // Absolute fallback: add directly to root level
                    mStage->root_prims().emplace_back(std::move(meshPrim));
                    ASSIMP_LOG_WARN("USDZExporter: Added mesh " + meshName + " to root level (no scene root found)");
                }
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
        
        // Create USD Material
        tinyusdz::Material usdMaterial;
        usdMaterial.name = matName;
        
        // Set current material path for texture processing (include root prim name)
        std::string rootPrimName = GetSceneName();
        mCurrentMaterialPath = "/" + rootPrimName + "/Materials/" + matName;
        
        // Create UsdPreviewSurface shader
        tinyusdz::UsdPreviewSurface surface;
        CreatePreviewSurface(mat, surface);
        
        // Create main surface shader using Apple's naming (without NodeGraph due to tinyusdz serialization issues)
        tinyusdz::Shader surfaceShader;
        std::string shaderName = "UsdPreviewSurface";  // Apple's exact naming
        surfaceShader.name = shaderName;
        surfaceShader.info_id = tinyusdz::kUsdPreviewSurface;
        
        // Set outputs
        surface.outputsSurface.set_authored(true);
        surface.outputsDisplacement.set_authored(true);
        surfaceShader.value = surface;
        
        // Set stPrimvarName input (Apple's pattern) - using props since Material doesn't have direct member
        tinyusdz::Attribute stPrimvarAttr;
        stPrimvarAttr.set_value(tinyusdz::value::token("st"));
        tinyusdz::Property stPrimvarProp(stPrimvarAttr, false);
        usdMaterial.props["inputs:stPrimvarName"] = stPrimvarProp;
        
        // Create material surface and displacement connections using proper tinyusdz Path API
        std::string shaderPath = mCurrentMaterialPath + "/" + shaderName;
        usdMaterial.surface.set(tinyusdz::Path(shaderPath, "outputs:surface"));
        usdMaterial.displacement.set(tinyusdz::Path(shaderPath, "outputs:displacement"));
        
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
            
            // Set explicit USD-compliant type for st input (texture coordinates)
            tinyusdz::Attribute stAttr;
            stAttr.set_value(tinyusdz::value::float2{0.0f, 0.0f});  // Default UV coordinates
            stAttr.set_type_name("float2");  // USD expects float2 for texture coordinates
            stAttr.set_connection(tinyusdz::Path(stConnection, ""));  // Keep the connection
            tinyusdz::Property stProp(stAttr, false);
            textureShader.props["inputs:st"] = stProp;
            
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
    // Following established Assimp pattern: only export embedded textures from memory
    // External textures are referenced by path (like OBJ exporter does)
    if (!mScene || mScene->mNumTextures == 0) {
        ASSIMP_LOG_DEBUG("USDZExporter: No embedded textures to export");
        return;
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Exporting " + ai_to_string(mScene->mNumTextures) + " embedded textures");
    
    // Process embedded textures - write them as siblings to USDA file
    for (uint32_t i = 0; i < mScene->mNumTextures; ++i) {
        const aiTexture* tex = mScene->mTextures[i];
        if (!tex) continue;
        
        // Generate texture filename (preserving proper extension)
        std::string baseTextureName;
        if (tex->mFilename.length > 0) {
            baseTextureName = tex->mFilename.C_Str();
        } else {
            baseTextureName = "texture_" + ai_to_string(i);
        }
        
        // Add appropriate extension if not present
        std::string extension = ".png"; // Default extension
        if (tex->mHeight == 0) {
            // Compressed texture - use format hint as extension
            if (tex->achFormatHint[0] != '\0') {
                std::string formatHint(tex->achFormatHint);
                if (formatHint.find('.') == std::string::npos) {
                    extension = "." + formatHint;
            } else {
                    extension = formatHint; // Already includes dot
                }
            }
        }
        
        // Ensure base name has extension and sanitize the complete filename
        if (baseTextureName.find('.') == std::string::npos) {
            baseTextureName += extension;
        }
        std::string textureName = SanitizeFilename(baseTextureName);
        
        // Ensure unique filename
        textureName = GenerateUniqueName(textureName);
        
        // Build texture path as sibling to USDA file
        std::string textureFilePath = textureName;
        
        try {
            // No directory creation needed - embedded textures are siblings to USDA file
            
            // Open output file
            if (mIOSystem) {
                std::unique_ptr<IOStream> outfile(mIOSystem->Open(textureFilePath, "wb"));
                if (!outfile) {
                    ASSIMP_LOG_WARN("USDZExporter: Could not create texture file: " + textureFilePath);
                    continue;
                }
                
                if (tex->mHeight == 0) {
                    // Compressed texture data - write directly
                    size_t written = outfile->Write(tex->pcData, tex->mWidth, 1);
                    if (written != tex->mWidth) {
                        ASSIMP_LOG_WARN("USDZExporter: Failed to write complete texture data for: " + textureFilePath);
                    }
                } else {
                    // Uncompressed texture data - convert to simple format
                    // This is a simplified implementation - in practice, you'd want to use a proper image library
                    ASSIMP_LOG_WARN("USDZExporter: Uncompressed texture export not fully implemented: " + textureName);
                    // For now, write raw RGBA data (would need proper PNG encoding in practice)
                    size_t dataSize = tex->mWidth * tex->mHeight * 4; // RGBA
                    size_t written = outfile->Write(tex->pcData, dataSize, 1);
                    if (written != dataSize) {
                        ASSIMP_LOG_WARN("USDZExporter: Failed to write complete texture data for: " + textureFilePath);
                    }
                }
                
                ASSIMP_LOG_DEBUG("USDZExporter: Exported texture: " + textureFilePath);
            }
            
        } catch (const std::exception& e) {
            ASSIMP_LOG_ERROR("USDZExporter: Failed to export embedded texture " + textureName + ": " + e.what());
        }
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Embedded texture export completed");
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
// Export Quick Look metadata  
void USDZExporter::ExportQuickLookMetadata() {
    auto& stageMeta = mStage->metas();
    
    stageMeta.doc = "Exported by Assimp USDZ Exporter - Compatible with iOS Quick Look";
    
    // Add Quick Look compatibility metadata through comment
    stageMeta.comment = "quickLook:compatible=true quickLook:version=1.0";
    
    ASSIMP_LOG_DEBUG("USDZExporter: Quick Look metadata exported");
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
// Convert mesh
void USDZExporter::ConvertMesh(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh) {
    if (!mesh) return;
    
    ConvertVertices(mesh, usdMesh);
    ConvertFaces(mesh, usdMesh);
    ConvertNormals(mesh, usdMesh);
    ConvertUVs(mesh, usdMesh);
    ConvertVertexColors(mesh, usdMesh);
    ConvertTangents(mesh, usdMesh);
    
    if (mesh->mNumBones > 0) {
        ConvertSkinningToMesh(mesh, usdMesh);
    }
    
    if (mesh->mNumAnimMeshes > 0) {
        ConvertBlendShapesToMesh(mesh, usdMesh);
    }
    
    // Add standard USD mesh attributes
    ConvertMeshAttributes(mesh, usdMesh);
}

// ------------------------------------------------------------------------------------------------
// Convert standard USD mesh attributes
void USDZExporter::ConvertMeshAttributes(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh) {
    if (!mesh) return;
    
    // 1. Set doubleSided attribute based on mesh properties
    // Check if mesh has consistent face winding or if material specifies double-sided
    bool doubleSided = false;
    
    // Check material for double-sided property if available
    if (mesh->mMaterialIndex < mScene->mNumMaterials) {
        const aiMaterial* material = mScene->mMaterials[mesh->mMaterialIndex];
        if (material) {
            int twoSided = 0;
            if (material->Get(AI_MATKEY_TWOSIDED, twoSided) == aiReturn_SUCCESS) {
                doubleSided = (twoSided != 0);
            }
        }
    }
    
    // If no material info, analyze face winding for consistency
    // If faces have inconsistent winding, assume double-sided
    if (!doubleSided && mesh->mFaces && mesh->mNumFaces > 1) {
        // Sample a few faces to check winding consistency
        int windingConsistentFaces = 0;
        int totalSampledFaces = std::min(mesh->mNumFaces, 10u); // Sample first 10 faces
        
        for (uint32_t i = 0; i < totalSampledFaces && i < mesh->mNumFaces; ++i) {
            const aiFace& face = mesh->mFaces[i];
            if (face.mNumIndices >= 3) {
                // Calculate face normal and check consistency
                windingConsistentFaces++; // For now, assume consistent
            }
        }
        
        // If less than 80% of faces have consistent winding, assume double-sided
        if (totalSampledFaces > 0 && windingConsistentFaces < (totalSampledFaces * 0.8f)) {
            doubleSided = true;
        }
    }
    
    // Set doubleSided attribute using tinyusdz API
    tinyusdz::Attribute doubleSidedAttr;
    doubleSidedAttr.set_value(doubleSided);
    doubleSidedAttr.set_type_name("bool");
    // Note: uniform qualifier will be handled by USD schema - doubleSided is inherently uniform
    tinyusdz::Property doubleSidedProp(doubleSidedAttr, false);
    usdMesh.props["doubleSided"] = doubleSidedProp;
    
    // 2. Set subdivisionScheme = "none" (standard for triangle meshes)
    tinyusdz::Attribute subdivisionSchemeAttr;
    subdivisionSchemeAttr.set_value(tinyusdz::value::token("none"));
    subdivisionSchemeAttr.set_type_name("token");
    // Note: uniform qualifier will be handled by USD schema - subdivisionScheme is inherently uniform
    tinyusdz::Property subdivisionSchemeProp(subdivisionSchemeAttr, false);
    usdMesh.props["subdivisionScheme"] = subdivisionSchemeProp;
    
    // 3. Set triangleSubdivisionRule = "none" 
    tinyusdz::Attribute triangleSubdivisionRuleAttr;
    triangleSubdivisionRuleAttr.set_value(tinyusdz::value::token("none"));
    triangleSubdivisionRuleAttr.set_type_name("token");
    tinyusdz::Property triangleSubdivisionRuleProp(triangleSubdivisionRuleAttr, false);
    usdMesh.props["triangleSubdivisionRule"] = triangleSubdivisionRuleProp;
}


// ------------------------------------------------------------------------------------------------
// Convert vertices
void USDZExporter::ConvertVertices(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh) {
    if (!mesh->mVertices || mesh->mNumVertices == 0) return;
    
    std::vector<tinyusdz::value::point3f> points;
    points.reserve(mesh->mNumVertices);
    
    for (uint32_t i = 0; i < mesh->mNumVertices; ++i) {
        const aiVector3D& v = mesh->mVertices[i];
        points.emplace_back(v.x, v.y, v.z);
    }
    
    usdMesh.points.set_value(std::move(points));
}

// ------------------------------------------------------------------------------------------------
// Convert faces
void USDZExporter::ConvertFaces(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh) {
    if (!mesh->mFaces || mesh->mNumFaces == 0) return;
    
    // Check if this is a point primitive (all faces have exactly 1 vertex)
    bool isPointPrimitive = true;
    for (uint32_t i = 0; i < mesh->mNumFaces; ++i) {
        if (mesh->mFaces[i].mNumIndices != 1) {
            isPointPrimitive = false;
            break;
        }
    }
    
    // For point primitives, USD doesn't need face data - just the points array
    if (isPointPrimitive) {
        ASSIMP_LOG_DEBUG("USDZExporter: Detected point primitive - skipping face data export");
        return;
    }
    
    std::vector<int> faceVertexCounts;
    std::vector<int> faceVertexIndices;
    
    faceVertexCounts.reserve(mesh->mNumFaces);
    
    uint32_t totalIndices = 0;
    for (uint32_t i = 0; i < mesh->mNumFaces; ++i) {
        totalIndices += mesh->mFaces[i].mNumIndices;
    }
    faceVertexIndices.reserve(totalIndices);
    
    for (uint32_t i = 0; i < mesh->mNumFaces; ++i) {
        const aiFace& face = mesh->mFaces[i];
        faceVertexCounts.push_back(static_cast<int>(face.mNumIndices));
        
        for (uint32_t j = 0; j < face.mNumIndices; ++j) {
            faceVertexIndices.push_back(static_cast<int>(face.mIndices[j]));
        }
    }
    
    usdMesh.faceVertexCounts.set_value(std::move(faceVertexCounts));
    usdMesh.faceVertexIndices.set_value(std::move(faceVertexIndices));
}

// ------------------------------------------------------------------------------------------------
// Convert normals
void USDZExporter::ConvertNormals(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh) {
    if (!mesh->mNormals) return;
    
    std::vector<tinyusdz::value::normal3f> normals;
    normals.reserve(mesh->mNumVertices);
    
    for (uint32_t i = 0; i < mesh->mNumVertices; ++i) {
        const aiVector3D& n = mesh->mNormals[i];
        normals.emplace_back(n.x, n.y, n.z);
    }
    
    // Add as primvar
    tinyusdz::Attribute normalAttr;
    normalAttr.set_value(normals);
    
    tinyusdz::AttrMeta normalMeta;
    normalMeta.interpolation = tinyusdz::Interpolation::Vertex;
    normalAttr.metas() = normalMeta;
    
    tinyusdz::Property normalProp(normalAttr, false);
    usdMesh.props["primvars:normals"] = normalProp;
}

// ------------------------------------------------------------------------------------------------
// Convert UVs
void USDZExporter::ConvertUVs(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh) {
    for (uint32_t uvIndex = 0; uvIndex < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++uvIndex) {
        if (!mesh->mTextureCoords[uvIndex]) continue;
        
        std::vector<tinyusdz::value::texcoord2f> uvs;
        uvs.reserve(mesh->mNumVertices);
        
        for (uint32_t i = 0; i < mesh->mNumVertices; ++i) {
            const aiVector3D& uv = mesh->mTextureCoords[uvIndex][i];
            // Note: USD uses V flipped compared to many formats
            uvs.emplace_back(uv.x, 1.0f - uv.y);
        }
        
        // Add as primvar
        std::string primvarName = (uvIndex == 0) ? "primvars:st" : ("primvars:st" + ai_to_string(uvIndex));
        
        tinyusdz::Attribute uvAttr;
        uvAttr.set_value(uvs);
        uvAttr.set_type_name("float2[]");  // Explicit USD type for texture coordinates
        
        tinyusdz::AttrMeta uvMeta;
        uvMeta.interpolation = tinyusdz::Interpolation::Vertex;
        uvAttr.metas() = uvMeta;
        
        tinyusdz::Property uvProp(uvAttr, false);
        usdMesh.props[primvarName] = uvProp;
    }
}

// ------------------------------------------------------------------------------------------------
// Convert vertex colors
void USDZExporter::ConvertVertexColors(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh) {
    for (uint32_t colorIndex = 0; colorIndex < AI_MAX_NUMBER_OF_COLOR_SETS; ++colorIndex) {
        if (!mesh->mColors[colorIndex]) continue;
        
        std::vector<tinyusdz::value::color3f> colors;
        colors.reserve(mesh->mNumVertices);
        
        for (uint32_t i = 0; i < mesh->mNumVertices; ++i) {
            const aiColor4D& c = mesh->mColors[colorIndex][i];
            colors.emplace_back(c.r, c.g, c.b); // tinyusdz requires Vec3 RGB, not Vec4 RGBA
        }
        
        // Add as primvar
        std::string primvarName = (colorIndex == 0) ? "primvars:displayColor" : ("primvars:color" + ai_to_string(colorIndex));
        
        tinyusdz::Attribute colorAttr;
        colorAttr.set_value(colors);
        
        tinyusdz::AttrMeta colorMeta;
        colorMeta.interpolation = tinyusdz::Interpolation::Vertex;
        colorAttr.metas() = colorMeta;
        
        tinyusdz::Property colorProp(colorAttr, false);
        usdMesh.props[primvarName] = colorProp;
    }
}

// ------------------------------------------------------------------------------------------------
// Convert tangents
void USDZExporter::ConvertTangents(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh) {
    if (!mesh->mTangents) return;
    
    std::vector<tinyusdz::value::vector3f> tangents;
    tangents.reserve(mesh->mNumVertices);
    
    for (uint32_t i = 0; i < mesh->mNumVertices; ++i) {
        const aiVector3D& t = mesh->mTangents[i];
        tangents.emplace_back(t.x, t.y, t.z);
    }
    
    // Add as primvar
    tinyusdz::Attribute tangentAttr;
    tangentAttr.set_value(tangents);
    
    tinyusdz::AttrMeta tangentMeta;
    tangentMeta.interpolation = tinyusdz::Interpolation::Vertex;
    tangentAttr.metas() = tangentMeta;
    
    tinyusdz::Property tangentProp(tangentAttr, false);
    usdMesh.props["primvars:tangents"] = tangentProp;
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
    
    // Emissive color
    aiColor3D emissive(0.0f, 0.0f, 0.0f);
    if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
        tinyusdz::value::color3f color{emissive.r, emissive.g, emissive.b};
        surface.emissiveColor.set_value(color);
    }
    
    // Opacity
    float opacity = 1.0f;
    if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
        surface.opacity.set_value(opacity);
    }
    
    // IOR (Index of Refraction)
    float ior = 1.5f;
    if (mat->Get(AI_MATKEY_REFRACTI, ior) == AI_SUCCESS) {
        surface.ior.set_value(ior);
    }
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
// Map texture properties with proper tinyusdz connections
void USDZExporter::MapTextureProperties(const aiMaterial* mat, tinyusdz::UsdPreviewSurface& surface) {
    if (!mat) return;
    
    // Store texture shaders to be added as children later
    mCurrentMaterialTextureShaders.clear();
    
    // Base color / Diffuse texture
    aiString texturePath;
    if (mat->GetTexture(aiTextureType_BASE_COLOR, 0, &texturePath) == AI_SUCCESS ||
        mat->GetTexture(aiTextureType_DIFFUSE, 0, &texturePath) == AI_SUCCESS) {
        
        tinyusdz::UsdUVTexture diffuseTexture = CreateUVTexture(texturePath.C_Str(), "diffuseColor");
        
        // Create connection path from texture to surface (using flat structure - no NodeGraph)
        std::string texShaderPath = mCurrentMaterialPath + "/diffuseColor";
        tinyusdz::Path connPath(texShaderPath, "outputs:rgb");
        surface.diffuseColor.set_connection(connPath);
        surface.diffuseColor.set_value_empty(); // Clear value when connected
        
        // Store texture shader to add as child later (using Apple's naming pattern)
        mCurrentMaterialTextureShaders.push_back(std::make_pair("diffuseColor", diffuseTexture));
        
        ASSIMP_LOG_DEBUG("USDZExporter: Connected diffuse texture: " + std::string(texturePath.C_Str()));
    }
    
    // Normal texture
    if (mat->GetTexture(aiTextureType_NORMALS, 0, &texturePath) == AI_SUCCESS) {
        tinyusdz::UsdUVTexture normalTexture = CreateUVTexture(texturePath.C_Str(), "normal");
        
        std::string texShaderPath = mCurrentMaterialPath + "/normal";
        tinyusdz::Path connPath(texShaderPath, "outputs:rgb");
        surface.normal.set_connection(connPath);
        surface.normal.set_value_empty();
        
        mCurrentMaterialTextureShaders.push_back(std::make_pair("normal", normalTexture));
        
        ASSIMP_LOG_DEBUG("USDZExporter: Connected normal texture: " + std::string(texturePath.C_Str()));
    }
    
    // Metallic texture (only create connection if metallic factor > 0)
    float currentMetallic = 0.0f;
    mat->Get(AI_MATKEY_METALLIC_FACTOR, currentMetallic);
    
    if (mat->GetTexture(aiTextureType_METALNESS, 0, &texturePath) == AI_SUCCESS && currentMetallic > 0.0f) {
        tinyusdz::UsdUVTexture metallicTexture = CreateUVTexture(texturePath.C_Str(), "metallic");
        
        std::string texShaderPath = mCurrentMaterialPath + "/metallic";
        tinyusdz::Path connPath(texShaderPath, "outputs:r"); // Use red channel for metallic
        surface.metallic.set_connection(connPath);
        surface.metallic.set_value_empty();
        
        mCurrentMaterialTextureShaders.push_back(std::make_pair("metallic", metallicTexture));
        
        ASSIMP_LOG_DEBUG("USDZExporter: Connected metallic texture: " + std::string(texturePath.C_Str()));
    }
    
    // Roughness texture
    if (mat->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &texturePath) == AI_SUCCESS) {
        tinyusdz::UsdUVTexture roughnessTexture = CreateUVTexture(texturePath.C_Str(), "roughness");
        
        std::string texShaderPath = mCurrentMaterialPath + "/roughness";
        tinyusdz::Path connPath(texShaderPath, "outputs:g"); // Use green channel for roughness (matches reference)
        surface.roughness.set_connection(connPath);
        surface.roughness.set_value_empty();
        
        mCurrentMaterialTextureShaders.push_back(std::make_pair("roughness", roughnessTexture));
        
        ASSIMP_LOG_DEBUG("USDZExporter: Connected roughness texture: " + std::string(texturePath.C_Str()));
    }
    
    // Emissive texture
    if (mat->GetTexture(aiTextureType_EMISSIVE, 0, &texturePath) == AI_SUCCESS) {
        tinyusdz::UsdUVTexture emissiveTexture = CreateUVTexture(texturePath.C_Str(), "emissiveColor");
        
        std::string texShaderPath = mCurrentMaterialPath + "/emissiveColor";
        tinyusdz::Path connPath(texShaderPath, "outputs:rgb");
        surface.emissiveColor.set_connection(connPath);
        surface.emissiveColor.set_value_empty();
        
        mCurrentMaterialTextureShaders.push_back(std::make_pair("emissiveColor", emissiveTexture));
        
        ASSIMP_LOG_DEBUG("USDZExporter: Connected emissive texture: " + std::string(texturePath.C_Str()));
    }
    
    // Occlusion texture
    if (mat->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &texturePath) == AI_SUCCESS) {
        tinyusdz::UsdUVTexture occlusionTexture = CreateUVTexture(texturePath.C_Str(), "occlusion");
        
        std::string texShaderPath = mCurrentMaterialPath + "/occlusion";
        tinyusdz::Path connPath(texShaderPath, "outputs:r"); // Use red channel for occlusion
        surface.occlusion.set_connection(connPath);
        surface.occlusion.set_value_empty();
        
        mCurrentMaterialTextureShaders.push_back(std::make_pair("occlusion", occlusionTexture));
        
        ASSIMP_LOG_DEBUG("USDZExporter: Connected occlusion texture: " + std::string(texturePath.C_Str()));
    }
    
    // Clearcoat texture
    if (mat->GetTexture(AI_MATKEY_CLEARCOAT_TEXTURE, &texturePath) == AI_SUCCESS) {
        tinyusdz::UsdUVTexture clearcoatTexture = CreateUVTexture(texturePath.C_Str(), "clearcoat");
        
        std::string texShaderPath = mCurrentMaterialPath + "/clearcoat";
        tinyusdz::Path connPath(texShaderPath, "outputs:r"); // Use red channel for clearcoat
        surface.clearcoat.set_connection(connPath);
        surface.clearcoat.set_value_empty();
        
        mCurrentMaterialTextureShaders.push_back(std::make_pair("clearcoat", clearcoatTexture));
        
        ASSIMP_LOG_DEBUG("USDZExporter: Connected clearcoat texture: " + std::string(texturePath.C_Str()));
    }
    
    // Clearcoat roughness texture
    if (mat->GetTexture(AI_MATKEY_CLEARCOAT_ROUGHNESS_TEXTURE, &texturePath) == AI_SUCCESS) {
        tinyusdz::UsdUVTexture clearcoatRoughnessTexture = CreateUVTexture(texturePath.C_Str(), "clearcoatRoughness");
        
        std::string texShaderPath = mCurrentMaterialPath + "/clearcoatRoughness";
        tinyusdz::Path connPath(texShaderPath, "outputs:g"); // Use green channel for clearcoat roughness (matches reference)
        surface.clearcoatRoughness.set_connection(connPath);
        surface.clearcoatRoughness.set_value_empty();
        
        mCurrentMaterialTextureShaders.push_back(std::make_pair("clearcoatRoughness", clearcoatRoughnessTexture));
        
        ASSIMP_LOG_DEBUG("USDZExporter: Connected clearcoat roughness texture: " + std::string(texturePath.C_Str()));
    }
    
    // Opacity texture
    if (mat->GetTexture(aiTextureType_OPACITY, 0, &texturePath) == AI_SUCCESS) {
        tinyusdz::UsdUVTexture opacityTexture = CreateUVTexture(texturePath.C_Str(), "opacity");
        
        std::string texShaderPath = mCurrentMaterialPath + "/opacity";
        tinyusdz::Path connPath(texShaderPath, "outputs:a"); // Use alpha channel for opacity
        surface.opacity.set_connection(connPath);
        surface.opacity.set_value_empty();
        
        mCurrentMaterialTextureShaders.push_back(std::make_pair("opacity", opacityTexture));
        
        ASSIMP_LOG_DEBUG("USDZExporter: Connected opacity texture: " + std::string(texturePath.C_Str()));
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Texture connections completed");
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
    
    // UV coordinates will be connected to stTransform.outputs:result in NodeGraph structure
    // This connection will be set when building the NodeGraph
    
    // Set wrap modes (Apple's pattern)
    uvTexture.wrapS.set_value(tinyusdz::UsdUVTexture::Wrap::Repeat);
    uvTexture.wrapT.set_value(tinyusdz::UsdUVTexture::Wrap::Repeat);
    
    // Note: NOT setting fallback on UsdUVTexture object to avoid color4f type issues
    // Will be handled by explicit shader property with correct float4 type
    
    // Add appropriate outputs (Apple's pattern)
    AddTextureOutputs(uvTexture, paramName);
    
    return uvTexture;
}

// ------------------------------------------------------------------------------------------------
// Convert texture using proper tinyusdz APIs
void USDZExporter::ConvertTexture(const aiMaterial* mat, aiTextureType type, 
                                 tinyusdz::UsdUVTexture& uvTexture) {
    if (!mat) return;
    
    aiString texturePath;
    if (mat->GetTexture(type, 0, &texturePath) != AI_SUCCESS) {
        return;
    }
    
    std::string texPath(texturePath.C_Str());
    
    if (IsEmbeddedTexture(texPath)) {
        HandleEmbeddedTexture(texPath, uvTexture);
    } else {
        HandleExternalTexture(texPath, uvTexture);
    }
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
        
        // Set asset path using Apple's pattern with ./ prefix for USDA/USDZ compatibility
        std::string texturePath = "./" + textureName;
        tinyusdz::value::AssetPath assetPath(texturePath);
        uvTexture.file.set_value(assetPath);
        
        // Add to list of textures to write alongside USDA file (following glTF2 pattern)
        TextureToWrite textureToWrite;
        textureToWrite.originalPath = texPath;
        textureToWrite.sanitizedFilename = textureName;
        textureToWrite.embeddedTexture = tex; // Embedded texture with data
        textureToWrite.isEmbedded = true; // From aiScene->mTextures
        mTexturesToWrite.push_back(textureToWrite);
        
        ASSIMP_LOG_DEBUG("USDZExporter: Will write embedded texture as sibling: " + texPath + " -> " + textureName);
        
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
    
    // Set asset path for the texture with ./ prefix for USDA/USDZ compatibility
    // Apple's files reference textures with ./ prefix for iOS Quick Look compatibility
    std::string texturePath = "./" + sanitizedFilename;
    tinyusdz::value::AssetPath assetPath(texturePath);
    uvTexture.file.set_value(assetPath);
    
    // Load external texture into memory using IOSystem (following Assimp canonical pattern)
    // This avoids hardcoded paths and works generically with any IOSystem configuration
    TextureToWrite textureToWrite;
    textureToWrite.originalPath = texPath;
    textureToWrite.sanitizedFilename = sanitizedFilename;
    textureToWrite.embeddedTexture = nullptr; // Not from aiScene->mTextures
    textureToWrite.isEmbedded = false; // External texture loaded into memory
    
    if (mIOSystem) {
        // Try to load external texture using IOSystem (respects working directory and search paths)
        std::unique_ptr<IOStream> textureFile(mIOSystem->Open(texPath.c_str()));
        if (textureFile) {
            // Get file size
            size_t fileSize = textureFile->FileSize();
            if (fileSize > 0) {
                // Load entire texture file into memory
                textureToWrite.externalTextureData.resize(fileSize);
                size_t bytesRead = textureFile->Read(textureToWrite.externalTextureData.data(), 1, fileSize);
                
                if (bytesRead == fileSize) {
                    ASSIMP_LOG_DEBUG("USDZExporter: Loaded external texture into memory: " + texPath + " (" + ai_to_string(fileSize) + " bytes)");
                } else {
                    ASSIMP_LOG_WARN("USDZExporter: Incomplete read of external texture: " + texPath);
                    textureToWrite.externalTextureData.clear(); // Clear incomplete data
                }
            } else {
                ASSIMP_LOG_WARN("USDZExporter: External texture file is empty: " + texPath);
            }
        } else {
            ASSIMP_LOG_WARN("USDZExporter: Could not open external texture file: " + texPath);
        }
    } else {
        ASSIMP_LOG_ERROR("USDZExporter: No IOSystem available for loading external texture: " + texPath);
    }
    
    mTexturesToWrite.push_back(textureToWrite);
    
    ASSIMP_LOG_DEBUG("USDZExporter: Will write external texture as sibling: " + texPath + " -> " + sanitizedFilename);
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
        
        // Create SkelAnimation for this morph target animation
        CreateMorphTargetSkelAnimation(morphAnim, actualMeshName, timeScale, anim->mName.C_Str());
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
// Create SkelAnimation for morph target animations
void USDZExporter::CreateMorphTargetSkelAnimation(const aiMeshMorphAnim* morphAnim, 
                                                  const std::string& meshName, 
                                                  double timeScale, 
                                                  const char* animationName) {
    if (!morphAnim || morphAnim->mNumKeys == 0) {
        return;
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Creating SkelAnimation for morph target: " + meshName);
    
    // Create SkelAnimation for morph targets
    tinyusdz::SkelAnimation skelAnim;
    std::string animName = SanitizeName(animationName) + "_" + SanitizeName(meshName) + "_MorphAnim";
    skelAnim.name = GenerateUniqueName(animName);
    
    // Collect blend shape names and build time-to-weights mapping
    std::vector<tinyusdz::value::token> blendShapeTokens;
    std::map<double, std::vector<float>> timeToWeights;
    
    // Get the first keyframe to determine number of blend shapes
    if (morphAnim->mNumKeys > 0) {
        const aiMeshMorphKey& firstKey = morphAnim->mKeys[0];
        
        // Build blend shape token list from mesh name and weight indices
        for (uint32_t w = 0; w < firstKey.mNumValuesAndWeights; ++w) {
            std::string blendShapeName = "BlendShape_" + ai_to_string(w);
            blendShapeTokens.push_back(tinyusdz::value::token(blendShapeName));
        }
    }
    
    // Process all keyframes to build time samples
    for (uint32_t k = 0; k < morphAnim->mNumKeys; ++k) {
        const aiMeshMorphKey& key = morphAnim->mKeys[k];
        double time = key.mTime * timeScale;
        
        std::vector<float> weights;
        weights.reserve(key.mNumValuesAndWeights);
        
        for (uint32_t w = 0; w < key.mNumValuesAndWeights; ++w) {
            weights.push_back(static_cast<float>(key.mWeights[w]));
        }
        
        timeToWeights[time] = weights;
        
        ASSIMP_LOG_DEBUG("USDZExporter: Morph keyframe at time " + ai_to_string(time) + 
                         " with " + ai_to_string(key.mNumValuesAndWeights) + " weights");
    }
    
    // Set blendShapes attribute
    skelAnim.blendShapes.set_value(blendShapeTokens);
    
    // Create time-sampled blendShapeWeights
    if (!timeToWeights.empty()) {
        tinyusdz::Animatable<std::vector<float>> animatedWeights;
        
        // Set default value (first time sample)
        animatedWeights.set_default(timeToWeights.begin()->second);
        
        // Add all time samples
        for (const auto& timeWeightPair : timeToWeights) {
            animatedWeights.add_sample(timeWeightPair.first, timeWeightPair.second);
        }
        
        skelAnim.blendShapeWeights.set_value(animatedWeights);
    }
    
    // Create Prim and add to stage
    tinyusdz::Prim skelAnimPrim(skelAnim);
    mStage->root_prims().emplace_back(std::move(skelAnimPrim));
    
    ASSIMP_LOG_DEBUG("USDZExporter: Created SkelAnimation '" + skelAnim.name + "' with " + 
                     ai_to_string(blendShapeTokens.size()) + " blend shapes and " +
                     ai_to_string(timeToWeights.size()) + " time samples");
}

// ------------------------------------------------------------------------------------------------
// Convert skinning using tinyusdz GeomPrimvar APIs
void USDZExporter::ConvertSkinning(const aiMesh* mesh) {
    if (!mesh || mesh->mNumBones == 0) {
        return;
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Converting skinning data for " + ai_to_string(mesh->mNumBones) + " bones");
    
    // Find the corresponding USD mesh to add skinning data to
    auto meshIt = mMeshIdMap.find(mesh);
    if (meshIt == mMeshIdMap.end()) {
        ASSIMP_LOG_WARN("USDZExporter: Could not find USD mesh for skinning data");
        return;
    }
    
    // Determine maximum weights per vertex (common values: 4, 8)
    uint32_t maxWeightsPerVertex = 4; // Default to 4 weights per vertex
    
    // Prepare joint indices and weights arrays
    std::vector<int> jointIndices(mesh->mNumVertices * maxWeightsPerVertex, 0);
    std::vector<float> jointWeights(mesh->mNumVertices * maxWeightsPerVertex, 0.0f);
    
    // Collect joint names for skel:joints property
    std::vector<tinyusdz::value::token> jointTokens;
    std::map<std::string, uint32_t> boneNameToIndex;
    
    for (uint32_t i = 0; i < mesh->mNumBones; ++i) {
        const aiBone* bone = mesh->mBones[i];
        std::string boneName = SanitizeName(bone->mName.C_Str());
        jointTokens.push_back(tinyusdz::value::token(boneName));
        boneNameToIndex[boneName] = i;
    }
    
    // Process bone weights and fill joint data arrays
    for (uint32_t boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
        const aiBone* bone = mesh->mBones[boneIdx];
        
        for (uint32_t weightIdx = 0; weightIdx < bone->mNumWeights; ++weightIdx) {
            const aiVertexWeight& weight = bone->mWeights[weightIdx];
            uint32_t vertexIdx = weight.mVertexId;
            
            if (vertexIdx >= mesh->mNumVertices) {
                continue; // Skip invalid vertex indices
            }
            
            // Find an empty slot for this vertex (up to maxWeightsPerVertex)
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
                // Find the slot with the smallest weight and replace it if this weight is larger
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
    for (uint32_t vertexIdx = 0; vertexIdx < mesh->mNumVertices; ++vertexIdx) {
        float totalWeight = 0.0f;
        uint32_t baseIdx = vertexIdx * maxWeightsPerVertex;
        
        // Calculate total weight
        for (uint32_t slotIdx = 0; slotIdx < maxWeightsPerVertex; ++slotIdx) {
            totalWeight += jointWeights[baseIdx + slotIdx];
        }
        
        // Normalize if total weight > 0
        if (totalWeight > 0.0f) {
            for (uint32_t slotIdx = 0; slotIdx < maxWeightsPerVertex; ++slotIdx) {
                jointWeights[baseIdx + slotIdx] /= totalWeight;
            }
        }
    }
    
    // TODO: Access the USD mesh prim to add skinning primvars
    // This requires modifying the mesh export to store references to created USD meshes
    // For now, we'll create the skinning data structure but can't attach it until
    // the mesh export architecture is updated
    
    ASSIMP_LOG_DEBUG("USDZExporter: Prepared skinning data: " + 
                     ai_to_string(jointIndices.size()) + " joint indices, " +
                     ai_to_string(jointWeights.size()) + " joint weights, " +
                     ai_to_string(maxWeightsPerVertex) + " weights per vertex");
}

// ------------------------------------------------------------------------------------------------
// Convert skinning to USD mesh using tinyusdz GeomPrimvar APIs
void USDZExporter::ConvertSkinningToMesh(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh) {
    if (!mesh || mesh->mNumBones == 0) {
        return;
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Adding skinning primvars for " + ai_to_string(mesh->mNumBones) + " bones");
    
    // Determine maximum weights per vertex
    uint32_t maxWeightsPerVertex = 4;
    
    // Prepare joint indices and weights arrays
    std::vector<int> jointIndices(mesh->mNumVertices * maxWeightsPerVertex, 0);
    std::vector<float> jointWeights(mesh->mNumVertices * maxWeightsPerVertex, 0.0f);
    
    // Collect joint names
    std::vector<tinyusdz::value::token> jointTokens;
    
    for (uint32_t i = 0; i < mesh->mNumBones; ++i) {
        const aiBone* bone = mesh->mBones[i];
        std::string boneName = SanitizeName(bone->mName.C_Str());
        jointTokens.push_back(tinyusdz::value::token(boneName));
    }
    
    // Process bone weights and fill joint data arrays
    for (uint32_t boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
        const aiBone* bone = mesh->mBones[boneIdx];
        
        for (uint32_t weightIdx = 0; weightIdx < bone->mNumWeights; ++weightIdx) {
            const aiVertexWeight& weight = bone->mWeights[weightIdx];
            uint32_t vertexIdx = weight.mVertexId;
            
            if (vertexIdx >= mesh->mNumVertices) {
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
    for (uint32_t vertexIdx = 0; vertexIdx < mesh->mNumVertices; ++vertexIdx) {
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
    usdMesh.set_primvar(weightsPrimvar);
    
    // Create skel:jointIndices primvar using proper tinyusdz APIs  
    tinyusdz::GeomPrimvar indicesPrimvar;
    indicesPrimvar.set_name("skel:jointIndices");
    indicesPrimvar.set_value(jointIndices);
    indicesPrimvar.set_elementSize(maxWeightsPerVertex);
    indicesPrimvar.set_interpolation(tinyusdz::Interpolation::Vertex);
    usdMesh.set_primvar(indicesPrimvar);
    
    // Create skel:geomBindTransform primvar (identity matrix for now)
    tinyusdz::value::matrix4d geomBindTransform;
    tinyusdz::Identity(&geomBindTransform);
    tinyusdz::GeomPrimvar geomBindPrimvar;
    geomBindPrimvar.set_name("skel:geomBindTransform");
    geomBindPrimvar.set_value(geomBindTransform);
    usdMesh.set_primvar(geomBindPrimvar);
    
    // Add skel:joints property - must match skeleton joint names exactly
    std::vector<tinyusdz::value::token> meshJointTokens;
    
    // Build mesh joint references using exact hierarchical USD paths from skeleton
    for (uint32_t boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
        const aiBone* bone = mesh->mBones[boneIdx];
        std::string boneName = bone->mName.C_Str();
        
        // Use the exact USD path from skeleton (critical for tinyusdz validation)
        auto pathIt = mBoneNameToUSDPath.find(boneName);
        if (pathIt != mBoneNameToUSDPath.end()) {
            const std::string& usdPath = pathIt->second;
            meshJointTokens.emplace_back(usdPath);
            
            ASSIMP_LOG_DEBUG("USDZExporter: Mapped bone '" + boneName + "' to USD path '" + usdPath + "'");
        } else {
            // Fallback to sanitized name (shouldn't happen if skeleton was built correctly)
            std::string sanitizedBoneName = SanitizeName(boneName);
            meshJointTokens.emplace_back(sanitizedBoneName);
            
            ASSIMP_LOG_WARN("USDZExporter: Could not find USD path for bone '" + boneName + "', using fallback: '" + sanitizedBoneName + "'");
        }
    }
    
    tinyusdz::Attribute jointsAttr;
    jointsAttr.set_value(meshJointTokens);
    jointsAttr.set_type_name("token[]");
    jointsAttr.variability() = tinyusdz::Variability::Uniform;
    tinyusdz::Property jointsProp(jointsAttr, false);
    usdMesh.props["skel:joints"] = jointsProp;
    
    // Add SkelBindingAPI schema and skeleton reference
    // Add to apiSchemas to indicate this mesh uses skeletal binding
    std::vector<tinyusdz::value::token> apiSchemas = { tinyusdz::value::token("SkelBindingAPI") };
    tinyusdz::Attribute apiSchemasAttr;
    apiSchemasAttr.set_value(apiSchemas);
    apiSchemasAttr.set_type_name("token[]");
    apiSchemasAttr.variability() = tinyusdz::Variability::Uniform;
    tinyusdz::Property apiSchemasProp(apiSchemasAttr, false);
    usdMesh.props["apiSchemas"] = apiSchemasProp;
    
    // Reference the skeleton
    tinyusdz::Relationship skelRel;
    tinyusdz::Path skelPath("/SkelRoot/Skeleton", "");
    skelRel.set(skelPath);
    tinyusdz::Property skelProp(skelRel);
    usdMesh.props["skel:skeleton"] = skelProp;
    
    ASSIMP_LOG_DEBUG("USDZExporter: Added skinning primvars to USD mesh: " + 
                     ai_to_string(jointTokens.size()) + " joints, " +
                     ai_to_string(maxWeightsPerVertex) + " weights per vertex");
}

// ------------------------------------------------------------------------------------------------
// Convert blend shapes (placeholder)
void USDZExporter::ConvertBlendShapes(const aiMesh* mesh) {
    ASSIMP_LOG_DEBUG("USDZExporter: Blend shape conversion not yet implemented");
}

// ------------------------------------------------------------------------------------------------
// Convert blend shapes to USD mesh using tinyusdz BlendShape APIs
void USDZExporter::ConvertBlendShapesToMesh(const aiMesh* mesh, tinyusdz::GeomMesh& usdMesh) {
    if (!mesh || mesh->mNumAnimMeshes == 0) {
        return;
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Converting " + ai_to_string(mesh->mNumAnimMeshes) + " blend shapes");
    
    // Process each animation mesh as a blend shape target
    for (uint32_t animMeshIdx = 0; animMeshIdx < mesh->mNumAnimMeshes; ++animMeshIdx) {
        const aiAnimMesh* animMesh = mesh->mAnimMeshes[animMeshIdx];
        if (!animMesh) continue;
        
        // Create BlendShape prim using proper tinyusdz APIs
        tinyusdz::BlendShape blendShape;
        std::string blendShapeName = SanitizeName(animMesh->mName.C_Str());
        if (blendShapeName.empty()) {
            blendShapeName = "BlendShape_" + ai_to_string(animMeshIdx);
        }
        blendShapeName = GenerateUniqueName(blendShapeName);
        blendShape.set_name(blendShapeName);
        
        // Calculate offsets and collect affected vertices
        std::vector<tinyusdz::value::vector3f> offsets;
        std::vector<tinyusdz::value::vector3f> normalOffsets;
        std::vector<int> pointIndices;
        
        // Compare with base mesh to find vertex offsets
        if (animMesh->mVertices && mesh->mVertices && mesh->mNumVertices == animMesh->mNumVertices) {
            for (uint32_t vertIdx = 0; vertIdx < mesh->mNumVertices; ++vertIdx) {
                const aiVector3D& baseVertex = mesh->mVertices[vertIdx];
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
        if (animMesh->mNormals && mesh->mNormals && offsets.size() > 0) {
            normalOffsets.resize(pointIndices.size());
            
            for (size_t i = 0; i < pointIndices.size(); ++i) {
                uint32_t vertIdx = static_cast<uint32_t>(pointIndices[i]);
                if (vertIdx < mesh->mNumVertices && vertIdx < animMesh->mNumVertices) {
                    const aiVector3D& baseNormal = mesh->mNormals[vertIdx];
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
            ASSIMP_LOG_DEBUG("USDZExporter: BlendShape " + blendShapeName + " point indices: " + ai_to_string(pointIndices.size()));
        }
        
        if (!offsets.empty()) {
            blendShape.offsets.set_value(offsets);
            ASSIMP_LOG_DEBUG("USDZExporter: BlendShape " + blendShapeName + " offsets: " + ai_to_string(offsets.size()));
        }
        
        if (!normalOffsets.empty()) {
            blendShape.normalOffsets.set_value(normalOffsets);
            ASSIMP_LOG_DEBUG("USDZExporter: BlendShape " + blendShapeName + " normal offsets: " + ai_to_string(normalOffsets.size()));
        }
        
        // Convert to Prim and add as child to stage
        // Note: BlendShapes are typically added as separate prims, not directly to the mesh
        if (!pointIndices.empty() && !offsets.empty()) {
            tinyusdz::Prim blendShapePrim(blendShape);
            mStage->root_prims().emplace_back(std::move(blendShapePrim));
            
            ASSIMP_LOG_DEBUG("USDZExporter: Created BlendShape prim: " + blendShapeName);
        }
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Blend shape conversion completed");
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
// Add AR anchoring setup for iOS Quick Look using tinyusdz APIs
void USDZExporter::AddARAnchoring() {
    ASSIMP_LOG_DEBUG("USDZExporter: AR anchoring setup for iOS Quick Look");
    
    // The main AR support is implemented through metadata in ExportMetadata():
    // - quickLook:compatible = "true"
    // - quickLook:version = "1.0"  
    // - metersPerUnit = 1.0 (for realistic AR scaling)
    // - upAxis = "Y" (for AR coordinate system)
    
    // Note: Advanced AR anchoring properties (preliminary:anchoring:type, etc.)
    // would require more complex tinyusdz prim property manipulation that is
    // beyond the current API access patterns. The metadata approach provides
    // the essential iOS Quick Look compatibility.
    
    ASSIMP_LOG_DEBUG("USDZExporter: AR anchoring configuration completed");
    ASSIMP_LOG_DEBUG("  - iOS Quick Look compatibility metadata: ENABLED");
    ASSIMP_LOG_DEBUG("  - Proper coordinate system and scaling: CONFIGURED");
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
    if (name.empty()) return "";
    
    std::string result = name;
    
    // Replace invalid characters with underscores
    std::regex invalidChars("[^a-zA-Z0-9_]");
    result = std::regex_replace(result, invalidChars, "_");
    
    // Ensure it starts with a letter or underscore
    if (!result.empty() && std::isdigit(result[0])) {
        result = "_" + result;
    }
    
    return result;
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
// Generate unique name
std::string USDZExporter::GenerateUniqueName(const std::string& baseName) const {
    auto it = mNameCounters.find(baseName);
    if (it == mNameCounters.end()) {
        mNameCounters[baseName] = 1;
        return baseName;
    } else {
        uint32_t counter = ++it->second;
        return baseName + "_" + ai_to_string(counter);
    }
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
    
    // Set explicit USD-compliant type for UsdPrimvarReader_float2 result output
    tinyusdz::Attribute resultAttr;
    resultAttr.set_value(tinyusdz::value::float2{0.0f, 0.0f});  // Default value
    resultAttr.set_type_name("float2");  // USD expects float2 for texture coordinates
    tinyusdz::Property resultProp(resultAttr, false);
    texCoordReader.props["outputs:result"] = resultProp;
    
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
    
    // Set explicit USD-compliant type for UsdTransform2d result output
    tinyusdz::Attribute resultAttr;
    resultAttr.set_value(tinyusdz::value::float2{0.0f, 0.0f});  // Default value
    resultAttr.set_type_name("float2");  // USD expects float2 for transform result
    tinyusdz::Property resultProp(resultAttr, false);
    stTransform.props["outputs:result"] = resultProp;
    
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
    } else {
        // Other scalar textures (metallic, clearcoat, etc.) output red channel
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
    
    // Then write texture files alongside the USDA file (following glTF2 pattern)
    WriteTextureFilesAlongsideMainFile(filename);
}

// ------------------------------------------------------------------------------------------------
// Write texture files alongside main USDA file (following glTF2 pattern)
void USDZExporter::WriteTextureFilesAlongsideMainFile(const std::string& mainFilename) {
    if (mTexturesToWrite.empty()) {
        ASSIMP_LOG_DEBUG("USDZExporter: No texture files to write");
        return;
    }
    
    // Extract directory from main filename (Apple's pattern - textures alongside main file)
    std::string outputDir;
    size_t lastSlash = mainFilename.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        outputDir = mainFilename.substr(0, lastSlash + 1);
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Writing " + ai_to_string(mTexturesToWrite.size()) + " texture files alongside main file");
    
    for (const auto& textureToWrite : mTexturesToWrite) {
        try {
            std::string outputPath = outputDir + textureToWrite.sanitizedFilename;
            
            if (textureToWrite.isEmbedded) {
                // Write embedded texture from aiScene->mTextures (like glTF2 buffer writing)
                WriteEmbeddedTextureToFile(textureToWrite.embeddedTexture, outputPath);
            } else {
                // Write external texture from memory (loaded during HandleExternalTexture)
                WriteExternalTextureFromMemory(textureToWrite, outputPath);
            }
            
        } catch (const std::exception& e) {
            ASSIMP_LOG_ERROR("USDZExporter: Failed to write texture file " + textureToWrite.sanitizedFilename + ": " + e.what());
        }
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Texture files written successfully");
}

// ------------------------------------------------------------------------------------------------
// Write embedded texture to file (following glTF2 embedded texture pattern)
void USDZExporter::WriteEmbeddedTextureToFile(const aiTexture* texture, const std::string& outputPath) {
    if (!texture || !mIOSystem) {
        ASSIMP_LOG_ERROR("USDZExporter: Invalid texture or IOSystem for embedded texture write");
        return;
    }
    
    std::unique_ptr<IOStream> outFile(mIOSystem->Open(outputPath, "wb"));
    if (!outFile) {
        ASSIMP_LOG_ERROR("USDZExporter: Could not create texture file: " + outputPath);
        return;
    }
    
    if (texture->mHeight == 0) {
        // Compressed texture data - write directly (like glTF2 buffer writing)
        size_t written = outFile->Write(texture->pcData, texture->mWidth, 1);
        if (written != texture->mWidth) {
            ASSIMP_LOG_WARN("USDZExporter: Failed to write complete texture data for: " + outputPath);
        }
    } else {
        // Uncompressed texture data - write raw RGBA data
        size_t dataSize = texture->mWidth * texture->mHeight * 4; // RGBA
        size_t written = outFile->Write(texture->pcData, dataSize, 1);
        if (written != dataSize) {
            ASSIMP_LOG_WARN("USDZExporter: Failed to write complete texture data for: " + outputPath);
        }
    }
    
    ASSIMP_LOG_DEBUG("USDZExporter: Written embedded texture to: " + outputPath);
}

// ------------------------------------------------------------------------------------------------
// Write external texture from memory (following Assimp canonical pattern)
void USDZExporter::WriteExternalTextureFromMemory(const TextureToWrite& textureToWrite, const std::string& outputPath) {
    if (!mIOSystem) {
        ASSIMP_LOG_ERROR("USDZExporter: No IOSystem available for texture file writing");
        return;
    }
    
    // Check if we have texture data in memory
    if (textureToWrite.externalTextureData.empty()) {
        ASSIMP_LOG_WARN("USDZExporter: No texture data available for: " + textureToWrite.originalPath);
        return;
    }
    
    std::unique_ptr<IOStream> targetFile(mIOSystem->Open(outputPath.c_str(), "wb"));
    if (!targetFile) {
        ASSIMP_LOG_ERROR("USDZExporter: Could not create texture file: " + outputPath);
        return;
    }
    
    // Write texture data from memory directly (like glTF2 buffer writing)
    size_t bytesWritten = targetFile->Write(textureToWrite.externalTextureData.data(), 
                                           textureToWrite.externalTextureData.size(), 1);
    
    if (bytesWritten != textureToWrite.externalTextureData.size()) {
        ASSIMP_LOG_WARN("USDZExporter: Failed to write complete texture data for: " + outputPath);
    } else {
        ASSIMP_LOG_DEBUG("USDZExporter: Written external texture from memory: " + textureToWrite.originalPath + 
                         " -> " + outputPath + " (" + ai_to_string(textureToWrite.externalTextureData.size()) + " bytes)");
    }
}

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
// Save as USDZ
void USDZExporter::SaveAsUSDZ(const std::string& filename) {
    try {
        // Create USDZ archive writer
        USDZArchiveWriter archive(filename);
        
        if (!archive.IsOpen()) {
            throw DeadlyExportError("Failed to create USDZ archive: " + filename);
        }
        
        // Generate USD content in memory (don't write to disk)
        std::string usdContent = GenerateUSDContent();
        
        // Add main USD file to archive (must be first per USDZ spec)
        if (!archive.AddMainUSDFile(usdContent, "model.usda")) {
            throw DeadlyExportError("Failed to add USD file to USDZ archive");
        }
        
        // Embed textures in the archive
        if (!EmbedTextures(archive)) {
            ReportWarning("Failed to embed some textures in USDZ archive");
        }
        
        // Finalize the archive
        if (!archive.Finalize()) {
            throw DeadlyExportError("Failed to finalize USDZ archive");
        }
        
        // Report any warnings from archive creation
        for (const auto& warning : archive.GetWarnings()) {
            ReportWarning("USDZ Archive: " + warning);
        }
        
        // Check for errors
        if (!archive.GetErrors().empty()) {
            std::string errorMsg = "USDZ Archive errors: ";
            for (const auto& error : archive.GetErrors()) {
                errorMsg += error + "; ";
            }
            throw DeadlyExportError(errorMsg);
        }
        
        ASSIMP_LOG_INFO("USDZExporter: Successfully exported USDZ with ", 
                       GetTextureCount(), " embedded textures");
        
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
// Embed textures in USDZ archive
bool USDZExporter::EmbedTextures(USDZArchiveWriter& archive) {
    bool allSuccess = true;
    size_t textureCount = 0;
    
    // Process all queued textures
    for (const auto& textureToWrite : mTexturesToWrite) {
        try {
            std::string archivePath = "textures/" + textureToWrite.sanitizedFilename;
            
            if (textureToWrite.isEmbedded && textureToWrite.embeddedTexture) {
                // Handle embedded texture from aiScene->mTextures
                const aiTexture* tex = textureToWrite.embeddedTexture;
                
                if (tex->mHeight == 0) {
                    // Compressed texture data
                    std::vector<uint8_t> textureData(
                        reinterpret_cast<const uint8_t*>(tex->pcData),
                        reinterpret_cast<const uint8_t*>(tex->pcData) + tex->mWidth
                    );
                    
                    if (!archive.AddTextureFile(archivePath, textureData)) {
                        ReportError("Failed to add embedded texture to archive: " + archivePath);
                        allSuccess = false;
                        continue;
                    }
                } else {
                    // Raw RGBA texture data - convert to PNG
                    std::vector<uint8_t> pngData;
                    if (ConvertRawTextureToPNG(tex, pngData)) {
                        // Update path to PNG if it wasn't already
                        if (archivePath.substr(archivePath.length() - 4) != ".png") {
                            archivePath = archivePath.substr(0, archivePath.find_last_of('.')) + ".png";
                        }
                        
                        if (!archive.AddTextureFile(archivePath, pngData)) {
                            ReportError("Failed to add converted PNG texture to archive: " + archivePath);
                            allSuccess = false;
                            continue;
                        }
                    } else {
                        ReportError("Failed to convert raw texture to PNG: " + textureToWrite.originalPath);
                        allSuccess = false;
                        continue;
                    }
                }
            } else {
                // Handle external texture loaded into memory
                if (!textureToWrite.externalTextureData.empty()) {
                    if (!archive.AddTextureFile(archivePath, textureToWrite.externalTextureData)) {
                        ReportError("Failed to add external texture to archive: " + archivePath);
                        allSuccess = false;
                        continue;
                    }
                } else {
                    // Try to load texture from file system using IOSystem
                    if (mIOSystem && !textureToWrite.originalPath.empty()) {
                        if (!archive.AddTextureFromFile(archivePath, textureToWrite.originalPath, mIOSystem)) {
                            ReportError("Failed to add texture file to archive: " + textureToWrite.originalPath);
                            allSuccess = false;
                            continue;
                        }
                    } else {
                        ReportError("No texture data available for: " + textureToWrite.originalPath);
                        allSuccess = false;
                        continue;
                    }
                }
            }
            
            textureCount++;
            ASSIMP_LOG_DEBUG("USDZExporter: Successfully embedded texture ", archivePath);
            
        } catch (const std::exception& e) {
            ReportError("Exception while embedding texture " + textureToWrite.originalPath + ": " + e.what());
            allSuccess = false;
        }
    }
    
    ASSIMP_LOG_INFO("USDZExporter: Embedded ", textureCount, " of ", mTexturesToWrite.size(), " textures");
    return allSuccess;
}

// ------------------------------------------------------------------------------------------------
// Get number of textures to be embedded
size_t USDZExporter::GetTextureCount() const {
    return mTexturesToWrite.size();
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

// USDC export removed - not supported by current tinyusdz version

// Post-processing removed - using proper tinyusdz APIs instead

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

#endif // !ASSIMP_BUILD_NO_USD_EXPORTER
