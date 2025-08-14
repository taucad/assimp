#ifndef ASSIMP_BUILD_NO_IFC_IMPORTER

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "IFCLoader.h"
#include <assimp/DefaultLogger.hpp>
#include <assimp/Importer.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/scene.h>
#include <assimp/material.h>
#include <assimp/mesh.h>
#include <assimp/importerdesc.h>
#include <assimp/DefaultIOSystem.h>
#include <assimp/Exceptional.h>
#include <assimp/StringUtils.h>

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>

// Suppress warnings from Web-IFC third-party headers
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wreturn-type"
#pragma GCC diagnostic ignored "-Wreturn-stack-address"
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wpragma-once-outside-header"
#pragma GCC diagnostic ignored "-Wunused-lambda-capture"

#include "web-ifc/modelmanager/ModelManager.h"
#include "web-ifc/geometry/IfcGeometryProcessor.h"
#include "web-ifc/geometry/representation/geometry.h"
#include "web-ifc/parsing/IfcLoader.h"
#include "web-ifc/schema/IfcSchemaManager.h"

#pragma GCC diagnostic pop

namespace Assimp {
template <>
const char *LogFunctions<IFCImporter>::Prefix() {
    return "IFC: ";
}
} // namespace Assimp

using namespace Assimp;

// ------------------------------------------------------------------------------------------------
IFCImporter::IFCImporter() : modelManager(nullptr), currentModelID(0) {
}

// ------------------------------------------------------------------------------------------------
IFCImporter::~IFCImporter() {
    if (modelManager) {
        if (currentModelID != 0) {
            CleanupWebIFC(currentModelID);
        }
        delete modelManager;
        modelManager = nullptr;
    }
}

// ------------------------------------------------------------------------------------------------
bool IFCImporter::CanRead(const std::string &pFile, IOSystem *pIOHandler, bool checkSig) const {
    const std::string extension = GetExtension(pFile);
    
    if (extension == "ifc") {
        return true;
    }
    
    if (checkSig && pIOHandler) {
        const char* tokens[] = { "ISO-10303-21" };
        return SearchFileHeaderForToken(pIOHandler, pFile, tokens, 1);
    }
    
    return false;
}

static const aiImporterDesc desc = {
    "Industry Foundation Classes (IFC) Importer (Web-IFC)",
    "",
    "",
    "",
    aiImporterFlags_SupportTextFlavour | aiImporterFlags_SupportBinaryFlavour,
    0,
    0,
    0,
    0,
    "ifc"
};

// ------------------------------------------------------------------------------------------------
const aiImporterDesc *IFCImporter::GetInfo() const {
    return &desc;
}

// ------------------------------------------------------------------------------------------------
void IFCImporter::SetupProperties(const Importer *pImp) {
    // Simplified settings for basic IFC implementation
    // TODO: Add proper IFC configuration options when Web-IFC is fully integrated
    settings.skipSpaceRepresentations = true;
    settings.coordinateToOrigin = false;
    settings.circleSegments = 32;
    settings.useCustomTriangulation = true;
    settings.skipAnnotations = true;
    (void)pImp; // Suppress unused parameter warning
}

// ------------------------------------------------------------------------------------------------
void IFCImporter::InternReadFile(const std::string &pFile, aiScene *pScene, IOSystem *pIOHandler) {
    InitializeWebIFC();
    LoadModelWithWebIFC(pFile, pScene, pIOHandler);
}



void IFCImporter::InitializeWebIFC() {
    if (!modelManager) {
        modelManager = new webifc::manager::ModelManager(false);
        
        // Suppress verbose web-ifc logging to avoid cluttering test output
        // Set to level 6 (off) to suppress all web-ifc logs including:
        // - "web-ifc: X.X.X threading: disabled schemas available [...]" 
        // - "[TriangulateBounds()] No basis found for brep!" errors
        modelManager->SetLogLevel(6); // spdlog::level::off = 6
        
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("Web-IFC model manager initialized with logging suppressed");
        }
    }
}

void IFCImporter::LoadModelWithWebIFC(const std::string &pFile, aiScene *pScene, IOSystem *pIOHandler) {
    if (!DefaultLogger::isNullLogger()) {
        LogInfo("Loading IFC file with Web-IFC: ", pFile);
    }

    // Open the IFC file
    std::shared_ptr<IOStream> stream(pIOHandler->Open(pFile));
    if (!stream) {
        ThrowException("Could not open file for reading");
    }

    // Read entire file into memory
    stream->Seek(0, aiOrigin_END);
    size_t fileSize = stream->Tell();
    stream->Seek(0, aiOrigin_SET);
    
    std::vector<uint8_t> fileData(fileSize);
    if (stream->Read(fileData.data(), 1, fileSize) != fileSize) {
        ThrowException("Failed to read IFC file data");
    }

    // Configure Web-IFC settings
    webifc::manager::LoaderSettings loaderSettings;
    loaderSettings.COORDINATE_TO_ORIGIN = settings.coordinateToOrigin;
    loaderSettings.CIRCLE_SEGMENTS = static_cast<uint16_t>(settings.circleSegments);
    
    // Create model and get model ID
    currentModelID = modelManager->CreateModel(loaderSettings);

    if (!DefaultLogger::isNullLogger()) {
        LogDebug("Created Web-IFC model with ID: ", currentModelID);
    }

    try {
        // Load IFC file using Web-IFC's callback mechanism
        const std::function<uint32_t(char *, size_t, size_t)> loaderFunc = 
            [&fileData](char *dest, size_t sourceOffset, size_t destSize) -> uint32_t {
                if (sourceOffset >= fileData.size()) {
            return 0;
        }

                size_t bytesToCopy = std::min(destSize, fileData.size() - sourceOffset);
                std::memcpy(dest, fileData.data() + sourceOffset, bytesToCopy);
                
                return static_cast<uint32_t>(bytesToCopy);
            };

        // Load the IFC data
        auto ifcLoader = modelManager->GetIfcLoader(currentModelID);
        ifcLoader->LoadFile(loaderFunc);

        if (!DefaultLogger::isNullLogger()) {
            LogDebug("IFC file loaded into Web-IFC");
        }

        // Create scene structure
        pScene->mRootNode = new aiNode("IFC_Scene");
        
        // Build spatial containment map for correct mesh assignment to storeys
        elementToStoreyMap = PopulateSpatialContainmentMap(ifcLoader);
        
        // Extract geometry and materials from Web-IFC
        ExtractMaterials(currentModelID, pScene);
        ExtractGeometry(currentModelID, pScene);
        BuildSceneGraph(currentModelID, pScene);
        
        if (!DefaultLogger::isNullLogger()) {
            LogInfo("IFC file loaded successfully with Web-IFC - ", 
                   pScene->mNumMeshes, " meshes, ", 
                   pScene->mNumMaterials, " materials");
        }

    } catch (const std::exception &e) {
        CleanupWebIFC(currentModelID);
        throw;
    }
}

void IFCImporter::ExtractGeometry(uint32_t modelID, aiScene *pScene) {
    if (!modelManager->IsModelOpen(modelID)) {
        return;
    }

    auto loader = modelManager->GetIfcLoader(modelID);
    auto geomProcessor = modelManager->GetGeometryProcessor(modelID);
    auto geomLoader = geomProcessor->GetLoader();
    
    // Clear and prepare IFC metadata storage
    meshToIFCMetadata.clear();
    
    std::vector<aiMesh*> meshes;

    try {
        // Get material relationships for efficient material assignment
        const auto& relMaterials = geomLoader.GetRelMaterials();
        
        // Get elements with geometry - use the EXACT approach as Web-IFC's LoadAllGeometry
        std::vector<std::pair<uint32_t, webifc::geometry::IfcFlatMesh>> flatMeshesWithGeometry;
        
        // Iterate through all IFC element types from schema (like LoadAllGeometry does)
        auto schemaManager = modelManager->GetSchemaManager();
        for (auto elementType : schemaManager.GetIfcElementList()) {
            // Skip non-geometric types (like LoadAllGeometry does)
            if (elementType == webifc::schema::IFCOPENINGELEMENT || 
                elementType == webifc::schema::IFCSPACE || 
                elementType == webifc::schema::IFCOPENINGSTANDARDCASE) {
                continue;
            }
            
            auto elements = loader->GetExpressIDsWithType(elementType);
            
            for (uint32_t expressID : elements) {
                try {
                    auto flatMesh = geomProcessor->GetFlatMesh(expressID);
                    if (!flatMesh.geometries.empty()) {
                        // Ensure geometry data is available (like LoadAllGeometry does)
                        for (auto &geom : flatMesh.geometries) {
                            auto &ifcGeom = geomProcessor->GetGeometry(geom.geometryExpressID);
                            ifcGeom.GetVertexData();
                        }
                        flatMeshesWithGeometry.emplace_back(expressID, std::move(flatMesh));
                    }
                } catch (...) {
                    // Skip elements without geometry (fail quietly like LoadAllGeometry)
                }
            }
        }
        
        // Hybrid material approach: create color-based materials for geometries without IFC materials
        std::unordered_map<std::string, unsigned int> colorMaterialCache;
        bool needsDefaultMaterial = false;
        
        for (auto& [expressID, flatMesh] : flatMeshesWithGeometry) {
            try {
                // Create individual mesh for this expressID (like reference implementation)
                auto assimpMesh = CreateMeshFromFlatMesh(expressID, flatMesh, relMaterials, colorMaterialCache, pScene);
                if (assimpMesh) {
                    // Check if this mesh needs to be split by materials
                    std::string meshName = assimpMesh->mName.C_Str();
                    if (meshName.find("NeedsSplitting_") == 0) {
                        // This is a multi-material mesh - split it
                        
                        // We need to re-extract the mesh data for splitting
                        // For now, delete this mesh and recreate it split
                        delete assimpMesh;
                        
                        // Re-process this flatMesh with splitting enabled
                        auto splitMeshes = CreateSplitMeshesFromFlatMesh(loader, expressID, flatMesh, relMaterials, colorMaterialCache, pScene);
                        
                        // Add all split meshes and store their metadata
                        std::string elementName = GetIFCElementName(loader, expressID);
                        std::string ifcTypeName = modelManager->GetSchemaManager().IfcTypeCodeToType(loader->GetLineType(expressID));
                        
                        for (auto* splitMesh : splitMeshes) {
                            unsigned int meshIndex = static_cast<unsigned int>(meshes.size());
                            meshToIFCMetadata[meshIndex] = {expressID, ifcTypeName, elementName.empty() ? "" : elementName};
                            
                            meshes.push_back(splitMesh);
                            
                            // Check if this mesh needs default material (material index 0)
                            if (splitMesh->mMaterialIndex == 0) {
                                needsDefaultMaterial = true;
                            }
                        }
                    } else {
                        // Single material mesh - add with IFC element name
                        std::string elementName = GetIFCElementName(loader, expressID);
                        if (!elementName.empty()) {
                            assimpMesh->mName = aiString(elementName);
                        } else {
                            // Fallback to expressID-based naming
                            assimpMesh->mName = aiString("Mesh " + std::to_string(expressID));
                        }
                        
                        // Store IFC metadata for later node assignment
                        unsigned int meshIndex = static_cast<unsigned int>(meshes.size());
                        std::string ifcTypeName = modelManager->GetSchemaManager().IfcTypeCodeToType(loader->GetLineType(expressID));
                        meshToIFCMetadata[meshIndex] = {expressID, ifcTypeName, elementName.empty() ? "" : elementName};
                        
                        meshes.push_back(assimpMesh);
                        
                        // Check if this mesh needs default material (material index 0)
                        if (assimpMesh->mMaterialIndex == 0) {
                            needsDefaultMaterial = true;
                        }
                    }
                }
                
            } catch (const std::exception &e) {
                if (!DefaultLogger::isNullLogger()) {
                    LogWarn("Failed to extract geometry for element ", expressID, ": ", e.what());
                }
            }
        }
        
        // Only create default material if there are meshes that need it
        if (needsDefaultMaterial) {
            aiMaterial* defaultMat = CreateMaterialFromColor(aiColor4D(0.8f, 0.8f, 0.8f, 1.0f), "IFC_Default");
            
            // Insert at index 0 and update all existing material indices
            std::vector<aiMaterial*> newMaterials;
            newMaterials.push_back(defaultMat);
            for (unsigned int i = 0; i < pScene->mNumMaterials; ++i) {
                newMaterials.push_back(pScene->mMaterials[i]);
            }
            
            // Update scene materials
            delete[] pScene->mMaterials;
            pScene->mNumMaterials = static_cast<unsigned int>(newMaterials.size());
            pScene->mMaterials = new aiMaterial*[newMaterials.size()];
            for (size_t i = 0; i < newMaterials.size(); ++i) {
                pScene->mMaterials[i] = newMaterials[i];
            }
            
            // Update all non-zero material indices in meshes (shift by 1)
            for (auto* mesh : meshes) {
                if (mesh->mMaterialIndex > 0) {
                    mesh->mMaterialIndex++;
                }
            }
        }

        // Set up meshes in scene
        pScene->mNumMeshes = static_cast<unsigned int>(meshes.size());
        if (!meshes.empty()) {
            pScene->mMeshes = new aiMesh*[meshes.size()];
            for (size_t i = 0; i < meshes.size(); ++i) {
                pScene->mMeshes[i] = meshes[i];
            }
        }
        
        if (!DefaultLogger::isNullLogger()) {
            LogInfo("Extracted ", meshes.size(), " meshes from IFC file");
        }

    } catch (const std::exception &e) {
        if (!DefaultLogger::isNullLogger()) {
            LogError("Failed to extract geometry from Web-IFC: ", e.what());
        }
        
        // Clean up partial results
        for (auto* mesh : meshes) {
            delete mesh;
        }
    }
}

aiMesh* IFCImporter::ConvertWebIFCMesh(const webifc::geometry::IfcFlatMesh &flatMesh, uint32_t geometryIndex) {
#ifdef IFC_LOADER_DEBUG
    if (!DefaultLogger::isNullLogger()) {
        LogDebug("ConvertWebIFCMesh: Starting conversion for geometry index: ", geometryIndex);
    }
#endif

    if (geometryIndex >= flatMesh.geometries.size()) {
#ifdef IFC_LOADER_DEBUG
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("ConvertWebIFCMesh: Invalid geometry index ", geometryIndex, " >= ", flatMesh.geometries.size());
        }
#endif
        return nullptr;
    }

#ifdef IFC_LOADER_DEBUG
    if (!DefaultLogger::isNullLogger()) {
        LogDebug("ConvertWebIFCMesh: Getting placed geometry...");
    }
#endif
    const auto &placedGeom = flatMesh.geometries[geometryIndex];
    
    // Extract color information from Web-IFC geometry
    const auto &webIfcColor = placedGeom.color; // glm::dvec4 with RGBA values
    
    // Convert Web-IFC color to Assimp color for material assignment
    aiColor4D assimpColor(
        static_cast<float>(std::clamp(webIfcColor.r, 0.0, 1.0)),
        static_cast<float>(std::clamp(webIfcColor.g, 0.0, 1.0)),
        static_cast<float>(std::clamp(webIfcColor.b, 0.0, 1.0)),
        static_cast<float>(std::clamp(webIfcColor.a, 0.0, 1.0))
    );
    
#ifdef IFC_LOADER_DEBUG
    if (!DefaultLogger::isNullLogger()) {
        LogDebug("ConvertWebIFCMesh: Getting geometry processor and geometry data for express ID: ", placedGeom.geometryExpressID);
        LogDebug("ConvertWebIFCMesh: Color RGBA(", assimpColor.r, ", ", assimpColor.g, ", ", assimpColor.b, ", ", assimpColor.a, ")");
    }
#endif
    // Get the actual geometry data
    auto geomProcessor = modelManager->GetGeometryProcessor(currentModelID);
    auto &ifcGeom = geomProcessor->GetGeometry(placedGeom.geometryExpressID);
    
    // Access the raw vertex and index data directly from vectors
#ifdef IFC_LOADER_DEBUG
    if (!DefaultLogger::isNullLogger()) {
        LogDebug("ConvertWebIFCMesh: Accessing vertex and index data vectors...");
    }
#endif
    
    // Access the underlying data vectors directly
    const auto& vertexDataVector = ifcGeom.fvertexData;
    const auto& indexDataVector = ifcGeom.indexData;
    
#ifdef IFC_LOADER_DEBUG
    if (!DefaultLogger::isNullLogger()) {
        LogDebug("ConvertWebIFCMesh: Vertex data size: ", vertexDataVector.size());
        LogDebug("ConvertWebIFCMesh: Index data size: ", indexDataVector.size());
    }
#endif
    
    if (vertexDataVector.empty() || indexDataVector.empty()) {
#ifdef IFC_LOADER_DEBUG
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("ConvertWebIFCMesh: Empty data vectors - returning nullptr");
        }
#endif
        return nullptr;
    }

#ifdef IFC_LOADER_DEBUG
    if (!DefaultLogger::isNullLogger()) {
        LogDebug("ConvertWebIFCMesh: Data vectors accessed successfully");
    }
#endif
    // Get pointers to the actual data
    const float* vertexData = vertexDataVector.data();
    const uint32_t* indexData = indexDataVector.data();
    
    // Create Assimp mesh
    aiMesh* mesh = new aiMesh();
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    
    // Web-IFC vertex format: position (3 floats) + normal (3 floats) = 6 floats per vertex
    constexpr size_t VERTEX_FORMAT_SIZE = 6;
    size_t numVertices = vertexDataVector.size() / VERTEX_FORMAT_SIZE;
    size_t numFaces = indexDataVector.size() / 3;
    
    if (numVertices == 0 || numFaces == 0) {
        delete mesh;
        return nullptr;
    }

    // Set up vertices
    mesh->mNumVertices = static_cast<unsigned int>(numVertices);
    mesh->mVertices = new aiVector3D[numVertices];
    // Note: Normals computation disabled. Enable?
    // mesh->mNormals = new aiVector3D[numVertices];
    
    // Allocate texture coordinates (Web-IFC doesn't provide UVs yet, so we'll generate basic planar mapping)
    mesh->mTextureCoords[0] = new aiVector3D[numVertices];
    mesh->mNumUVComponents[0] = 2; // 2D texture coordinates
    
    // Calculate bounding box for UV generation
    aiVector3D minBounds(FLT_MAX, FLT_MAX, FLT_MAX);
    aiVector3D maxBounds(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    
    for (size_t i = 0; i < numVertices; ++i) {
        size_t offset = i * VERTEX_FORMAT_SIZE;
        
        // Position
        mesh->mVertices[i] = aiVector3D(
            vertexData[offset + 0],
            vertexData[offset + 1], 
            vertexData[offset + 2]
        );
        
        // Update bounding box for UV calculation
        minBounds.x = std::min(minBounds.x, mesh->mVertices[i].x);
        minBounds.y = std::min(minBounds.y, mesh->mVertices[i].y);
        minBounds.z = std::min(minBounds.z, mesh->mVertices[i].z);
        maxBounds.x = std::max(maxBounds.x, mesh->mVertices[i].x);
        maxBounds.y = std::max(maxBounds.y, mesh->mVertices[i].y);
        maxBounds.z = std::max(maxBounds.z, mesh->mVertices[i].z);
        
        // Note: Normal computation disabled. Enable?
        // mesh->mNormals[i] = aiVector3D(
        //     vertexData[offset + 3],
        //     vertexData[offset + 4],
        //     vertexData[offset + 5]
        // );
    }
    
    // Generate texture coordinates using planar mapping
    // TODO: Replace with actual UV coordinates when Web-IFC provides them
    GenerateTextureCoordinates(mesh, minBounds, maxBounds);
    
    // Set up faces
    mesh->mNumFaces = static_cast<unsigned int>(numFaces);
    mesh->mFaces = new aiFace[numFaces];
    
    for (size_t i = 0; i < numFaces; ++i) {
        mesh->mFaces[i].mNumIndices = 3;
        mesh->mFaces[i].mIndices = new unsigned int[3];
        mesh->mFaces[i].mIndices[0] = indexData[i * 3 + 0];
        mesh->mFaces[i].mIndices[1] = indexData[i * 3 + 1];
        mesh->mFaces[i].mIndices[2] = indexData[i * 3 + 2];
    }
    
    // Apply transformation from IFC placement
    // Extract transformation matrix from placedGeom.transformation
    const auto &transform = placedGeom.transformation;
    
    // Apply transformation to vertices
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        aiVector3D &vertex = mesh->mVertices[i];
        
        // Transform vertex position
        aiVector3D transformedVertex(
            static_cast<float>(transform[0][0] * vertex.x + transform[1][0] * vertex.y + transform[2][0] * vertex.z + transform[3][0]),
            static_cast<float>(transform[0][1] * vertex.x + transform[1][1] * vertex.y + transform[2][1] * vertex.z + transform[3][1]),
            static_cast<float>(transform[0][2] * vertex.x + transform[1][2] * vertex.y + transform[2][2] * vertex.z + transform[3][2])
        );
        vertex = transformedVertex;
        
        // Note: Normal transformation disabled. Enable?
        // aiVector3D &normal = mesh->mNormals[i];
        // aiVector3D transformedNormal(
        //     static_cast<float>(transform[0][0] * normal.x + transform[1][0] * normal.y + transform[2][0] * normal.z),
        //     static_cast<float>(transform[0][1] * normal.x + transform[1][1] * normal.y + transform[2][1] * normal.z),
        //     static_cast<float>(transform[0][2] * normal.x + transform[1][2] * normal.y + transform[2][2] * normal.z)
        // );
        // normal = transformedNormal;
        // normal.Normalize();
    }
    
    // Set material index (will be set properly in ExtractMaterials)
    mesh->mMaterialIndex = 0; // Default material - will be updated when materials are properly assigned
    
    return mesh;
}

void IFCImporter::ExtractMaterials(uint32_t modelID, aiScene *pScene) {
    std::vector<aiMaterial*> materials;
    // Use class member instead of local variable to avoid shadowing
    this->materialIDToIndex.clear(); // Clear any previous material mappings
    
    try {
        auto ifcLoader = modelManager->GetIfcLoader(modelID);
        auto geomProcessor = modelManager->GetGeometryProcessor(modelID);
        auto geomLoader = geomProcessor->GetLoader();
        
        // Extract actual IFC materials using Web-IFC's material APIs first
        ExtractIFCMaterials(ifcLoader, geomLoader, materials, this->materialIDToIndex);
        
        // Set up materials in scene
        pScene->mNumMaterials = static_cast<unsigned int>(materials.size());
        pScene->mMaterials = new aiMaterial*[materials.size()];
        
        for (size_t i = 0; i < materials.size(); ++i) {
            pScene->mMaterials[i] = materials[i];
        }
        
        if (!DefaultLogger::isNullLogger()) {
            LogInfo("Extracted ", materials.size(), " IFC materials");
        }
        
    } catch (const std::exception &e) {
        if (!DefaultLogger::isNullLogger()) {
            LogWarn("Failed to extract IFC materials: ", e.what());
        }
        
        // Fallback to default material only
        if (materials.empty()) {
            aiMaterial* defaultMat = CreateMaterialFromColor(aiColor4D(0.8f, 0.8f, 0.8f, 1.0f), "IFC_Default");
            materials.push_back(defaultMat);
        }
        
        pScene->mNumMaterials = static_cast<unsigned int>(materials.size());
        pScene->mMaterials = new aiMaterial*[materials.size()];
        for (size_t i = 0; i < materials.size(); ++i) {
            pScene->mMaterials[i] = materials[i];
        }
    }
}

aiMaterial* IFCImporter::CreateMaterialFromColor(const aiColor4D &color, const std::string &name) {
    aiMaterial* material = new aiMaterial();
    
    aiString materialName(name);
    material->AddProperty(&materialName, AI_MATKEY_NAME);
    
    // Create Phong material to handle IfcSurfaceStyle + IfcSurfaceStyleRendering
    int shadingModel = aiShadingMode_Phong;
    material->AddProperty(&shadingModel, 1, AI_MATKEY_SHADING_MODEL);
    
    // Convert sRGB input to linear RGB for both properties
    aiColor4D linearColor = ConvertSRGBToLinear(aiColor4D(color.r, color.g, color.b, color.a));
    
    // Set diffuse color (RGB components - note: aiColor3D doesn't support alpha)
    // Use linear RGB values for consistency with modern rendering pipelines
    aiColor3D diffuseColor(linearColor.r, linearColor.g, linearColor.b);
    material->AddProperty(&diffuseColor, 1, AI_MATKEY_COLOR_DIFFUSE);
    
    // Set diffuse with alpha - using linear RGB values
    aiColor4D diffuseColor4D(linearColor.r, linearColor.g, linearColor.b, linearColor.a);
    material->AddProperty(&diffuseColor4D, 1, AI_MATKEY_COLOR_DIFFUSE);
    
    // Set base color with alpha - using linear RGB values
    material->AddProperty(&linearColor, 1, AI_MATKEY_BASE_COLOR);
    
    // Handle transparency from alpha channel
    float opacity = color.a;
    material->AddProperty(&opacity, 1, AI_MATKEY_OPACITY);
    
    // Set ambient color (darker version of diffuse for Phong)
    aiColor3D ambient(diffuseColor.r * 0.1f, diffuseColor.g * 0.1f, diffuseColor.b * 0.1f);
    material->AddProperty(&ambient, 1, AI_MATKEY_COLOR_AMBIENT);
    
    // Set specular properties (Phong material)
    aiColor3D specular(0.2f, 0.2f, 0.2f);
    material->AddProperty(&specular, 1, AI_MATKEY_COLOR_SPECULAR);
    
    // Set shininess for Phong reflection
    float shininess = 64.0f; // Higher for more realistic Phong shading
    material->AddProperty(&shininess, 1, AI_MATKEY_SHININESS);
    
    // Set explicit PBR properties for better glTF export compatibility
    float metallicFactor = 0.0f; // IFC materials are typically non-metallic
    float roughnessFactor = 1.0f; // Default to fully rough for architectural materials
    material->AddProperty(&metallicFactor, 1, AI_MATKEY_METALLIC_FACTOR);
    material->AddProperty(&roughnessFactor, 1, AI_MATKEY_ROUGHNESS_FACTOR);
    
    return material;
}

void IFCImporter::ExtractIFCMaterials(
    webifc::parsing::IfcLoader* ifcLoader,
    const webifc::geometry::IfcGeometryLoader& geomLoader,
    std::vector<aiMaterial*>& materials,
    std::unordered_map<uint32_t, unsigned int>& materialIDToIndex) {
    
    try {
        // Get material relationships and definitions from Web-IFC
        const auto& relMaterials = geomLoader.GetRelMaterials();
        const auto& materialDefinitions = geomLoader.GetMaterialDefinitions();
        const auto& styledItems = geomLoader.GetStyledItems();
        
        // Process each material definition
        for (const auto& [materialID, definitions] : materialDefinitions) {
            try {
                aiMaterial* material = ExtractSingleIFCMaterial(ifcLoader, materialID, definitions);
                if (material) {
                    unsigned int materialIndex = static_cast<unsigned int>(materials.size());
                    materials.push_back(material);
                    materialIDToIndex[materialID] = materialIndex;
                    
                    if (!DefaultLogger::isNullLogger()) {
                        LogDebug("Extracted IFC material: ", materialID, " -> index ", materialIndex);
                    }
                }
            } catch (const std::exception& e) {
                if (!DefaultLogger::isNullLogger()) {
                    LogWarn("Failed to extract material ", materialID, ": ", e.what());
                }
            }
        }
        
        // Process styled items for visual representations
        ProcessStyledItems(ifcLoader, styledItems, materials, materialIDToIndex);
        
    } catch (const std::exception& e) {
        if (!DefaultLogger::isNullLogger()) {
            LogWarn("Failed to access Web-IFC material APIs: ", e.what());
        }
    }
}

aiMaterial* IFCImporter::ExtractSingleIFCMaterial(
    webifc::parsing::IfcLoader* ifcLoader,
    uint32_t materialID,
    const std::vector<std::pair<uint32_t, uint32_t>>& definitions) {
    
    auto material = std::make_unique<aiMaterial>();
    
    try {
        // Extract material name (typically first argument)
        std::string materialName = "IFC_Material_" + std::to_string(materialID);
        try {
            ifcLoader->MoveToArgumentOffset(materialID, 0);
            if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::STRING) {
                ifcLoader->MoveToArgumentOffset(materialID, 0);
                std::string extractedName = ifcLoader->GetDecodedStringArgument();
                if (!extractedName.empty()) {
                    materialName = DecodeIFCString(extractedName);
                }
            }
        } catch (...) {
            // Use fallback name
        }
        
        aiString assimpMaterialName(materialName);
        material->AddProperty(&assimpMaterialName, AI_MATKEY_NAME);
        
        // Set as Phong material, to handle IfcSurfaceStyle + IfcSurfaceStyleRendering
        int shadingModel = aiShadingMode_Phong;
        material->AddProperty(&shadingModel, 1, AI_MATKEY_SHADING_MODEL);
        
        // Extract material properties from definitions
        ExtractMaterialProperties(ifcLoader, definitions, material.get());
        
        return material.release();
        
    } catch (const std::exception& e) {
        if (!DefaultLogger::isNullLogger()) {
            LogWarn("Failed to extract material properties for ", materialID, ": ", e.what());
        }
        return nullptr;
    }
}

void IFCImporter::ExtractMaterialProperties(
    webifc::parsing::IfcLoader* ifcLoader,
    const std::vector<std::pair<uint32_t, uint32_t>>& definitions,
    aiMaterial* material) {
    
    // Set default properties
    aiColor4D diffuseColor(0.8f, 0.8f, 0.8f, 1.0f);
    aiColor4D specularColor(0.2f, 0.2f, 0.2f, 1.0f);
    float shininess = 32.0f;
    
    // Process each definition to extract material properties
    for (const auto& [defID, propID] : definitions) {
        try {
            uint32_t defType = ifcLoader->GetLineType(defID);
            
            // Handle different IFC material property types
            if (defType == webifc::schema::IFCCOLOURRGB) {
                ExtractColorFromRGB(ifcLoader, defID, diffuseColor);
            } else if (defType == webifc::schema::IFCSURFACESTYLERENDERING) {
                ExtractRenderingProperties(ifcLoader, defID, diffuseColor, specularColor, shininess);
            }
            // Add more property type handlers as needed
            
        } catch (const std::exception& e) {
            if (!DefaultLogger::isNullLogger()) {
                LogDebug("Failed to extract property ", defID, ": ", e.what());
            }
        }
    }
    
    // Apply extracted properties to material
    material->AddProperty(&diffuseColor, 1, AI_MATKEY_COLOR_DIFFUSE);
    material->AddProperty(&specularColor, 1, AI_MATKEY_COLOR_SPECULAR);
    material->AddProperty(&shininess, 1, AI_MATKEY_SHININESS);
    
    // Set explicit PBR properties for better glTF export compatibility
    float metallicFactor = 0.0f; // IFC materials are typically non-metallic
    float roughnessFactor = 1.0f; // Default to fully rough for architectural materials
    material->AddProperty(&metallicFactor, 1, AI_MATKEY_METALLIC_FACTOR);
    material->AddProperty(&roughnessFactor, 1, AI_MATKEY_ROUGHNESS_FACTOR);
}

void IFCImporter::ExtractColorFromRGB(
    webifc::parsing::IfcLoader* ifcLoader,
    uint32_t colorID,
    aiColor4D& outColor) {
    
    try {
        // IFCCOLOURRGB has Red, Green, Blue components (arguments 0, 1, 2)
        ifcLoader->MoveToArgumentOffset(colorID, 0);
        float red = static_cast<float>(ifcLoader->GetDoubleArgument());
        
        ifcLoader->MoveToArgumentOffset(colorID, 1);
        float green = static_cast<float>(ifcLoader->GetDoubleArgument());
        
        ifcLoader->MoveToArgumentOffset(colorID, 2);
        float blue = static_cast<float>(ifcLoader->GetDoubleArgument());
        
        outColor = aiColor4D(
            std::clamp(red, 0.0f, 1.0f),
            std::clamp(green, 0.0f, 1.0f),
            std::clamp(blue, 0.0f, 1.0f),
            1.0f
        );
        
    } catch (const std::exception& e) {
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("Failed to extract RGB color: ", e.what());
        }
    }
}

void IFCImporter::ExtractRenderingProperties(
    webifc::parsing::IfcLoader* ifcLoader,
    uint32_t renderingID,
    aiColor4D& diffuseColor,
    aiColor4D& specularColor,
    float& shininess) {
    
    try {
        // IFCSURFACESTYLERENDERING properties
        // Extract basic color information
        
        ifcLoader->MoveToArgumentOffset(renderingID, 0);
        if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::REF) {
            uint32_t surfaceColorRef = ifcLoader->GetRefArgument();
            ExtractColorFromRGB(ifcLoader, surfaceColorRef, diffuseColor);
        }
        
        // Extract transparency if available (argument 1)
        try {
            ifcLoader->MoveToArgumentOffset(renderingID, 1);
            if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::REAL) {
                float transparency = static_cast<float>(ifcLoader->GetDoubleArgument());
                diffuseColor.a = 1.0f - std::clamp(transparency, 0.0f, 1.0f);
            }
        } catch (...) {
            // Transparency is optional
        }
        
    } catch (const std::exception& e) {
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("Failed to extract rendering properties: ", e.what());
        }
    }
}

void IFCImporter::ProcessStyledItems(
    webifc::parsing::IfcLoader* ifcLoader,
    const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& styledItems,
    std::vector<aiMaterial*>& materials,
    std::unordered_map<uint32_t, unsigned int>& materialIDToIndex) {
    
    // Process styled items to create materials for visual representations
    for (const auto& [itemID, styles] : styledItems) {
        for (const auto& [styleID, presentationLayerID] : styles) {
            try {
                uint32_t styleType = ifcLoader->GetLineType(styleID);
                
                if (styleType == webifc::schema::IFCSURFACESTYLE) {
                    ProcessSurfaceStyle(ifcLoader, styleID, itemID, materials, materialIDToIndex);
                }
                
            } catch (const std::exception& e) {
                if (!DefaultLogger::isNullLogger()) {
                    LogDebug("Failed to process styled item ", itemID, ": ", e.what());
                }
            }
        }
    }
}

void IFCImporter::ProcessSurfaceStyle(
    webifc::parsing::IfcLoader* ifcLoader,
    uint32_t styleID,
    uint32_t itemID,
    std::vector<aiMaterial*>& materials,
    std::unordered_map<uint32_t, unsigned int>& materialIDToIndex) {
    
    // Check if we already processed this style
    if (materialIDToIndex.find(styleID) != materialIDToIndex.end()) {
        return;
    }
    
    try {
        auto material = std::make_unique<aiMaterial>();
        
        // Extract style name
        std::string styleName = "IFC_SurfaceStyle_" + std::to_string(styleID);
        try {
            ifcLoader->MoveToArgumentOffset(styleID, 0);
            if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::STRING) {
                ifcLoader->MoveToArgumentOffset(styleID, 0);
                std::string extractedName = ifcLoader->GetDecodedStringArgument();
                if (!extractedName.empty()) {
                    styleName = DecodeIFCString(extractedName);
                }
            }
        } catch (...) {
            // Use fallback name
        }
        
        aiString assimpStyleName(styleName);
        material->AddProperty(&assimpStyleName, AI_MATKEY_NAME);
        
        // Extract surface style elements - set default properties for now
        aiColor4D diffuseColor(0.8f, 0.8f, 0.8f, 1.0f);
        
        material->AddProperty(&diffuseColor, 1, AI_MATKEY_COLOR_DIFFUSE);
        
        // Set specular properties
        aiColor4D specularColor(0.2f, 0.2f, 0.2f, 1.0f);
        material->AddProperty(&specularColor, 1, AI_MATKEY_COLOR_SPECULAR);
        
        float shininess = 32.0f;
        material->AddProperty(&shininess, 1, AI_MATKEY_SHININESS);
        
        // Set explicit PBR properties for better glTF export compatibility
        float metallicFactor = 0.0f; // IFC materials are typically non-metallic
        float roughnessFactor = 1.0f; // Default to fully rough for architectural materials
        material->AddProperty(&metallicFactor, 1, AI_MATKEY_METALLIC_FACTOR);
        material->AddProperty(&roughnessFactor, 1, AI_MATKEY_ROUGHNESS_FACTOR);
        
        unsigned int materialIndex = static_cast<unsigned int>(materials.size());
        materials.push_back(material.release());
        materialIDToIndex[styleID] = materialIndex;
        
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("Processed surface style: ", styleID, " -> index ", materialIndex);
        }
        
    } catch (const std::exception& e) {
        if (!DefaultLogger::isNullLogger()) {
            LogWarn("Failed to process surface style ", styleID, ": ", e.what());
        }
    }
}

void IFCImporter::SetMeshMaterialFromIFC(
    uint32_t expressID,
    aiMesh* mesh,
    const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& relMaterials,
    const aiScene* pScene) {
    
    // Look up material relationship for this element
    auto materialIt = relMaterials.find(expressID);
    if (materialIt != relMaterials.end() && !materialIt->second.empty()) {
        // Use the first material relationship found
        uint32_t materialID = materialIt->second[0].first;
        
        // Find the corresponding material index in the scene
        for (unsigned int i = 0; i < pScene->mNumMaterials; ++i) {
            aiString materialName;
            if (pScene->mMaterials[i]->Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS) {
                // Check if this material corresponds to our IFC material
                std::string nameStr(materialName.C_Str());
                if (nameStr.find(std::to_string(materialID)) != std::string::npos) {
                    mesh->mMaterialIndex = i;
                    return;
                }
            }
        }
    }
    
    // Fallback to default material (index 0)
    mesh->mMaterialIndex = 0;
}

void IFCImporter::BuildSceneGraph(uint32_t modelID, aiScene *pScene) {
    try {
        auto ifcLoader = modelManager->GetIfcLoader(modelID);
        
        // Build proper IFC spatial hierarchy (Project -> Site -> Building -> Storey -> Space -> Elements)
        BuildIFCSpatialHierarchy(ifcLoader, pScene);
        
    } catch (const std::exception &e) {
        if (!DefaultLogger::isNullLogger()) {
            LogWarn("Failed to build IFC spatial hierarchy: ", e.what(), ", falling back to flat hierarchy");
        }
        
        // Fallback: create a simple flat hierarchy
        if (pScene->mNumMeshes > 0) {
            // Link all meshes to root node
            pScene->mRootNode->mNumMeshes = pScene->mNumMeshes;
            pScene->mRootNode->mMeshes = new unsigned int[pScene->mNumMeshes];
            
            for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
                pScene->mRootNode->mMeshes[i] = i;
            }
        }
    }
    
    if (!DefaultLogger::isNullLogger()) {
        LogInfo("Built scene graph with ", CountNodesInHierarchy(pScene->mRootNode), " nodes");
    }
}

void IFCImporter::CleanupWebIFC(uint32_t modelID) {
    if (modelManager && modelManager->IsModelOpen(modelID)) {
        modelManager->CloseModel(modelID);
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("Closed Web-IFC model ", modelID);
        }
    }
}

std::string IFCImporter::DecodeIFCString(const std::string& input) {
    std::string result = input;
    
    // IFC escape sequence mapping for German umlauts and special characters
    // Based on ISO 10303-21 encoding (EXPRESS language standard)
    
    // Replace \S\d with ä (a-umlaut)
    size_t pos = 0;
    while ((pos = result.find("\\S\\d", pos)) != std::string::npos) {
        result.replace(pos, 4, "ä");
        pos += 1; // ä is 2 bytes in UTF-8, so move past it
    }
    
    // Replace \S\| with ü (u-umlaut)
    pos = 0;
    while ((pos = result.find("\\S\\|", pos)) != std::string::npos) {
        result.replace(pos, 4, "ü");
        pos += 1;
    }
    
    // Replace \S\_ with ß (eszett/sharp-s)
    pos = 0;
    while ((pos = result.find("\\S\\_", pos)) != std::string::npos) {
        result.replace(pos, 4, "ß");
        pos += 1;
    }
    
    // Replace \S\c with ö (o-umlaut) - additional common German character
    pos = 0;
    while ((pos = result.find("\\S\\c", pos)) != std::string::npos) {
        result.replace(pos, 4, "ö");
        pos += 1;
    }
    
    // Replace \S\D with Ä (capital A-umlaut)
    pos = 0;
    while ((pos = result.find("\\S\\D", pos)) != std::string::npos) {
        result.replace(pos, 4, "Ä");
        pos += 1;
    }
    
    // Replace \S\\ with Ü (capital U-umlaut)  
    pos = 0;
    while ((pos = result.find("\\S\\\\", pos)) != std::string::npos) {
        result.replace(pos, 4, "Ü");
        pos += 1;
    }
    
    // Replace \S\C with Ö (capital O-umlaut)
    pos = 0;
    while ((pos = result.find("\\S\\C", pos)) != std::string::npos) {
        result.replace(pos, 4, "Ö");
        pos += 1;
    }
    
    // Add more IFC escape sequences as needed
    // Reference: ISO 10303-21 standard for EXPRESS language string encoding
    
    return result;
}

std::string IFCImporter::GetIFCElementName(webifc::parsing::IfcLoader* ifcLoader, uint32_t expressID) {
    try {
        // Extract the Name attribute (argument 2) from IFC elements
        // IFC structure: GlobalId, OwnerHistory, Name, Description, ...
        ifcLoader->MoveToArgumentOffset(expressID, 2);
        
        std::string_view rawNameView = ifcLoader->GetStringArgument();
        if (!rawNameView.empty()) {
            std::string rawName(rawNameView);
            std::string decodedName = DecodeIFCString(rawName);
            
            // Only return non-empty, meaningful names
            if (!decodedName.empty() && decodedName != "$" && decodedName != "''") {
                return decodedName;
            }
        }
        
        // If Name is empty/null, try alternative approaches for specific element types
        uint32_t elementType = ifcLoader->GetLineType(expressID);
        
        // For some elements, the Tag field (argument 7 or 4) might contain meaningful names
        if (elementType == webifc::schema::IFCSLAB || 
            elementType == webifc::schema::IFCWALL ||
            elementType == webifc::schema::IFCBEAM ||
            elementType == webifc::schema::IFCCOLUMN) {
            
            try {
                // Try argument 7 (Tag for IFCSLAB) or other position for other types
                int tagArgument = (elementType == webifc::schema::IFCSLAB) ? 7 : 4;
                ifcLoader->MoveToArgumentOffset(expressID, tagArgument);
                
                std::string_view tagView = ifcLoader->GetStringArgument();
                if (!tagView.empty()) {
                    std::string tagString(tagView);
                    std::string decodedTag = DecodeIFCString(tagString);
                    
                    // Return tag if it looks like a meaningful name (not a GUID)
                    if (!decodedTag.empty() && decodedTag != "$" && decodedTag != "''" &&
                        decodedTag.find('-') != std::string::npos && decodedTag.length() < 20) {
                        return decodedTag;
                    }
                }
            } catch (...) {
                // Tag extraction failed, continue to fallback
            }
        }
        
    } catch (const std::exception& e) {
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("IFC: Failed to extract name for element ", expressID, ": ", e.what());
        }
    }
    
    // Return empty string to indicate fallback to expressID should be used
    return "";
}

std::unordered_map<uint32_t, uint32_t> IFCImporter::PopulateSpatialContainmentMap(webifc::parsing::IfcLoader* ifcLoader) {
    std::unordered_map<uint32_t, uint32_t> elementToStorey;
    
    try {
        // Use Web-IFC's efficient API to get all spatial containment relationships
        auto spatialContainments = ifcLoader->GetExpressIDsWithType(webifc::schema::IFCRELCONTAINEDINSPATIALSTRUCTURE);
        
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("IFC: Found ", spatialContainments.size(), " spatial containment relationships");
        }
        
        for (uint32_t relationshipID : spatialContainments) {
            try {
                // IFCRELCONTAINEDINSPATIALSTRUCTURE structure:
                // Argument 4: RelatedElements (SET OF IfcProduct) - the elements contained  
                // Argument 5: RelatingStructure (IfcSpatialElement) - the spatial structure (storey)
                
                // Get the spatial structure (storey) that contains the elements
                ifcLoader->MoveToArgumentOffset(relationshipID, 5);
                uint32_t relatingStructure = ifcLoader->GetRefArgument();
                
                // Get the set of elements contained in this spatial structure
                ifcLoader->MoveToArgumentOffset(relationshipID, 4);
                auto relatedElements = ifcLoader->GetSetArgument();
                
                // Map each element to its containing storey
                for (auto& elementRef : relatedElements) {
                    uint32_t elementID = ifcLoader->GetRefArgument(elementRef);
                    elementToStorey[elementID] = relatingStructure;
                }
                
                if (!DefaultLogger::isNullLogger()) {
                    LogDebug("IFC: Spatial containment - storey ", relatingStructure, " contains ", relatedElements.size(), " elements");
                }
                
            } catch (const std::exception& e) {
                if (!DefaultLogger::isNullLogger()) {
                    LogWarn("IFC: Failed to process spatial containment relationship ", relationshipID, ": ", e.what());
                }
            }
        }
        
        if (!DefaultLogger::isNullLogger()) {
            LogInfo("IFC: Built spatial containment map with ", elementToStorey.size(), " element-to-storey mappings");
        }
        
    } catch (const std::exception& e) {
        if (!DefaultLogger::isNullLogger()) {
            LogError("IFC: Failed to populate spatial containment map: ", e.what());
        }
    }
    
    return elementToStorey;
}

std::vector<IFCImporter::StoreyInfo> IFCImporter::GetSortedStoreysByElevation(webifc::parsing::IfcLoader* ifcLoader) {
    std::vector<StoreyInfo> storeys;
    
    try {
        // Get all building storey entities using Web-IFC's efficient API
        auto buildingStoreys = ifcLoader->GetExpressIDsWithType(webifc::schema::IFCBUILDINGSTOREY);
        
        for (uint32_t storeyID : buildingStoreys) {
            try {
                StoreyInfo storeyInfo;
                storeyInfo.expressID = storeyID;
                
                // Extract storey name (argument 2)
                ifcLoader->MoveToArgumentOffset(storeyID, 2);
                std::string_view rawNameView = ifcLoader->GetStringArgument();
                std::string rawName(rawNameView);
                storeyInfo.name = DecodeIFCString(rawName);
                
                // Extract elevation (last argument - typically argument 9 for IFCBUILDINGSTOREY)
                // IFCBUILDINGSTOREY structure: GlobalId, OwnerHistory, Name, Description, ObjectType, 
                // ObjectPlacement, Representation, LongName, CompositionType, Elevation
                ifcLoader->MoveToArgumentOffset(storeyID, 9);
                storeyInfo.elevation = ifcLoader->GetDoubleArgument();
                
                storeys.push_back(storeyInfo);
                
                if (!DefaultLogger::isNullLogger()) {
                    LogDebug("IFC: Found storey '", storeyInfo.name, "' at elevation ", storeyInfo.elevation);
                }
                
            } catch (const std::exception& e) {
                if (!DefaultLogger::isNullLogger()) {
                    LogWarn("IFC: Failed to extract elevation for building storey ", storeyID, ": ", e.what());
                }
            }
        }
        
        // Sort storeys by elevation (lowest first - ground floor before upper floors)
        std::sort(storeys.begin(), storeys.end(), 
            [](const StoreyInfo& a, const StoreyInfo& b) {
                return a.elevation < b.elevation;
            });
        
        if (!DefaultLogger::isNullLogger()) {
            LogInfo("IFC: Sorted ", storeys.size(), " building storeys by elevation");
            for (const auto& storey : storeys) {
                LogDebug("IFC: Storey '", storey.name, "' at elevation ", storey.elevation);
            }
        }
        
    } catch (const std::exception& e) {
        if (!DefaultLogger::isNullLogger()) {
            LogError("IFC: Failed to get sorted storeys by elevation: ", e.what());
        }
    }
    
    return storeys;
}

void IFCImporter::GenerateTextureCoordinates(aiMesh* mesh, const aiVector3D& minBounds, const aiVector3D& maxBounds) {
    if (!mesh || !mesh->mVertices || !mesh->mTextureCoords[0]) {
        return;
    }
    
    // Calculate the size of the bounding box
    aiVector3D size = maxBounds - minBounds;
    
    // Avoid division by zero
    if (size.x < 1e-6f) size.x = 1.0f;
    if (size.y < 1e-6f) size.y = 1.0f;
    if (size.z < 1e-6f) size.z = 1.0f;
    
    // Generate UV coordinates using planar mapping
    // Choose the two largest dimensions for UV mapping to minimize distortion
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        const aiVector3D& vertex = mesh->mVertices[i];
        
        // Normalize coordinates to [0,1] range based on bounding box
        float u, v;
        
        // Use the two largest dimensions to minimize distortion
        if (size.x >= size.y && size.x >= size.z) {
            // X is largest, use Y and Z for UV
            u = (vertex.y - minBounds.y) / size.y;
            v = (vertex.z - minBounds.z) / size.z;
        } else if (size.y >= size.x && size.y >= size.z) {
            // Y is largest, use X and Z for UV
            u = (vertex.x - minBounds.x) / size.x;
            v = (vertex.z - minBounds.z) / size.z;
        } else {
            // Z is largest, use X and Y for UV
            u = (vertex.x - minBounds.x) / size.x;
            v = (vertex.y - minBounds.y) / size.y;
        }
        
        // Store UV coordinates (Z component is 0 for 2D texture coordinates)
        mesh->mTextureCoords[0][i] = aiVector3D(u, v, 0.0f);
    }
    
#ifdef IFC_LOADER_DEBUG
    if (!DefaultLogger::isNullLogger()) {
        LogDebug("Generated texture coordinates for mesh with ", mesh->mNumVertices, " vertices");
    }
#endif
}

aiColor4D IFCImporter::ConvertWebIFCColor(const glm::dvec4& webifcColor) {
    // Convert Web-IFC color directly to aiColor4D (0-1 range)
    return aiColor4D(
        static_cast<float>(webifcColor.r),
        static_cast<float>(webifcColor.g),
        static_cast<float>(webifcColor.b),
        static_cast<float>(webifcColor.a)
    );
}

aiColor4D IFCImporter::ConvertSRGBToLinear(const aiColor4D& srgbColor) {
    // Convert sRGB color values to linear RGB using standard gamma correction
    auto srgbToLinear = [](float srgb) -> float {
        if (srgb <= 0.04045f) {
            return srgb / 12.92f;
        } else {
            return std::pow((srgb + 0.055f) / 1.055f, 2.4f);
        }
    };
    
    return aiColor4D(
        srgbToLinear(srgbColor.r),
        srgbToLinear(srgbColor.g),
        srgbToLinear(srgbColor.b),
        srgbColor.a  // Alpha channel is not gamma-corrected
    );
}

aiMesh* IFCImporter::CreateMeshFromFlatMesh(
    uint32_t expressID,
    const webifc::geometry::IfcFlatMesh& flatMesh,
    const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& relMaterials,
    std::unordered_map<std::string, unsigned int>& colorMaterialCache,
    aiScene* pScene) {
    
    if (flatMesh.geometries.empty()) {
        return nullptr;
    }
    
    auto geomProcessor = modelManager->GetGeometryProcessor(currentModelID);
    aiMesh* mesh = new aiMesh();
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    
    // Collect all vertices and faces from all geometries
    std::vector<aiVector3D> vertices;
    // Note: Normals computation disabled. Enable?
    // std::vector<aiVector3D> normals;

    std::vector<aiFace> faces;
    std::vector<unsigned int> materialIndices;
    
    try {
        for (const auto& placedGeom : flatMesh.geometries) {
            auto& ifcGeom = geomProcessor->GetGeometry(placedGeom.geometryExpressID);
            const auto& vertexDataVector = ifcGeom.fvertexData;
            const auto& indexDataVector = ifcGeom.indexData;
            
            if (vertexDataVector.empty() || indexDataVector.empty()) {
                continue;
            }
            
            // Web-IFC vertex format: position (3 floats) + normal (3 floats) = 6 floats per vertex
            constexpr size_t VERTEX_FORMAT_SIZE = 6;
            size_t numVertices = vertexDataVector.size() / VERTEX_FORMAT_SIZE;
            size_t vertexOffset = vertices.size();
            
            // Extract transformation matrix from flatTransformation
            glm::dmat4 transformation;
            for (int i = 0; i < 16; ++i) {
                transformation[i / 4][i % 4] = placedGeom.flatTransformation[i];
            }
            
            // Convert Web-IFC color directly to aiColor4D
            aiColor4D geometryColor = ConvertWebIFCColor(placedGeom.color);
            
            // Convert vertices and apply transformation
            for (size_t i = 0; i < numVertices; ++i) {
                size_t offset = i * VERTEX_FORMAT_SIZE;
                
                // Position with transformation applied
                glm::dvec4 position(
                    vertexDataVector[offset + 0],
                    vertexDataVector[offset + 1], 
                    vertexDataVector[offset + 2],
                    1.0
                );
                glm::dvec4 transformedPos = transformation * position;
                vertices.emplace_back(
                    static_cast<float>(transformedPos.x),
                    static_cast<float>(transformedPos.y),
                    static_cast<float>(transformedPos.z)
                );
                
                // Note: Normal computation disabled. Enable?
                // normals.emplace_back(
                //     vertexDataVector[offset + 3],
                //     vertexDataVector[offset + 4],
                //     vertexDataVector[offset + 5]
                // );
            }
            
            // Determine material index using color-first approach
            unsigned int materialIndex = 0; // Default material
            
            // Priority 1: Use IFC material assignment if available
            auto relMatIt = relMaterials.find(expressID);
            bool foundIFCMaterial = false;
            
            if (relMatIt != relMaterials.end() && !relMatIt->second.empty()) {
                // Get the first material ID assigned to this element
                uint32_t materialID = relMatIt->second[0].first;
                
                // Look up in the class member materialIDToIndex map (this contains ALL extracted IFC materials)
                auto materialIt = this->materialIDToIndex.find(materialID);
                if (materialIt != this->materialIDToIndex.end()) {
                    materialIndex = materialIt->second;
                    foundIFCMaterial = true;
                }
            }
            
            // Priority 2: Create color-based material if no IFC material was found
            if (!foundIFCMaterial) {
                materialIndex = GetOrCreateColorMaterial(geometryColor, colorMaterialCache, pScene);
            }
            
            // Convert faces
            for (size_t i = 0; i < indexDataVector.size(); i += 3) {
                aiFace face;
                face.mNumIndices = 3;
                face.mIndices = new unsigned int[3];
                face.mIndices[0] = static_cast<unsigned int>(vertexOffset + indexDataVector[i + 0]);
                face.mIndices[1] = static_cast<unsigned int>(vertexOffset + indexDataVector[i + 1]);
                face.mIndices[2] = static_cast<unsigned int>(vertexOffset + indexDataVector[i + 2]);
                faces.push_back(face);
                materialIndices.push_back(materialIndex);
            }
        }
        
        if (vertices.empty() || faces.empty()) {
            delete mesh;
            return nullptr;
        }
        
        // Set up mesh data
        mesh->mNumVertices = static_cast<unsigned int>(vertices.size());
        mesh->mVertices = new aiVector3D[vertices.size()];
        // Note: Normals computation disabled. Enable?
        // mesh->mNormals = new aiVector3D[normals.size()];
        
        for (size_t i = 0; i < vertices.size(); ++i) {
            mesh->mVertices[i] = vertices[i];
            // Note: Normal copying disabled. Enable?
            // if (i < normals.size()) {
            //     mesh->mNormals[i] = normals[i];
            // }
        }
        
        mesh->mNumFaces = static_cast<unsigned int>(faces.size());
        mesh->mFaces = new aiFace[faces.size()];
        for (size_t i = 0; i < faces.size(); ++i) {
            mesh->mFaces[i] = faces[i];
        }
        
        // Check if we have multiple materials in this mesh
        std::set<unsigned int> uniqueMaterials(materialIndices.begin(), materialIndices.end());
        
        if (uniqueMaterials.size() <= 1) {
            // Single material mesh - simple case
            if (!materialIndices.empty()) {
                mesh->mMaterialIndex = materialIndices[0];
            }
        } else {
            // Multi-material mesh - split into separate meshes by material
            
            // Store the mesh data that we need for splitting
            mesh->mMaterialIndex = materialIndices[0]; // Temporary assignment
            mesh->mName = aiString("NeedsSplitting_" + std::to_string(expressID));
            
            // We'll handle the splitting in the calling function
        }
        
        // Generate texture coordinates
        if (mesh->mNumVertices > 0) {
            aiVector3D minBounds(FLT_MAX, FLT_MAX, FLT_MAX);
            aiVector3D maxBounds(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            
            for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
                minBounds.x = std::min(minBounds.x, mesh->mVertices[i].x);
                minBounds.y = std::min(minBounds.y, mesh->mVertices[i].y);
                minBounds.z = std::min(minBounds.z, mesh->mVertices[i].z);
                maxBounds.x = std::max(maxBounds.x, mesh->mVertices[i].x);
                maxBounds.y = std::max(maxBounds.y, mesh->mVertices[i].y);
                maxBounds.z = std::max(maxBounds.z, mesh->mVertices[i].z);
            }
            
            // Allocate and generate texture coordinates
            mesh->mTextureCoords[0] = new aiVector3D[mesh->mNumVertices];
            mesh->mNumUVComponents[0] = 2; // 2D texture coordinates
            
            // Calculate size for UV generation
            aiVector3D size = maxBounds - minBounds;
            if (size.x < 1e-6f) size.x = 1.0f;
            if (size.y < 1e-6f) size.y = 1.0f;
            if (size.z < 1e-6f) size.z = 1.0f;
            
            // Generate UV coordinates using planar mapping
            for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
                const aiVector3D& vertex = mesh->mVertices[i];
                
                float u, v;
                // Use the two largest dimensions to minimize distortion
                if (size.x >= size.y && size.x >= size.z) {
                    // X is largest, use Y and Z for UV
                    u = (vertex.y - minBounds.y) / size.y;
                    v = (vertex.z - minBounds.z) / size.z;
                } else if (size.y >= size.x && size.y >= size.z) {
                    // Y is largest, use X and Z for UV
                    u = (vertex.x - minBounds.x) / size.x;
                    v = (vertex.z - minBounds.z) / size.z;
                } else {
                    // Z is largest, use X and Y for UV
                    u = (vertex.x - minBounds.x) / size.x;
                    v = (vertex.y - minBounds.y) / size.y;
                }
                
                // Store UV coordinates (Z component is 0 for 2D texture coordinates)
                mesh->mTextureCoords[0][i] = aiVector3D(u, v, 0.0f);
            }
        }
        
        return mesh;
        
    } catch (const std::exception& e) {
        if (!DefaultLogger::isNullLogger()) {
            LogWarn("Failed to create mesh from flat mesh: ", e.what());
        }
        delete mesh;
        return nullptr;
    }
}

unsigned int IFCImporter::GetOrCreateColorMaterial(
    const aiColor4D& color,
    std::unordered_map<std::string, unsigned int>& colorMaterialCache,
    aiScene* pScene) {
    
    // Create hex color string (e.g., "8C8D7EFF")
    auto toHex = [](float value) -> std::string {
        int intValue = static_cast<int>(std::round(std::min(std::max(value * 255.0f, 0.0f), 255.0f)));
        std::stringstream ss;
        ss << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << intValue;
        return ss.str();
    };
    
    std::string colorKey = toHex(color.r) + toHex(color.g) + toHex(color.b) + toHex(color.a);
    
    // Check if we already have this color material
    auto it = colorMaterialCache.find(colorKey);
    if (it != colorMaterialCache.end()) {
        return it->second;
    }
    
    // Create rounded color that matches the hex name for consistency
    aiColor4D roundedColor(
        static_cast<float>(std::round(std::min(std::max(color.r * 255.0f, 0.0f), 255.0f))) / 255.0f,
        static_cast<float>(std::round(std::min(std::max(color.g * 255.0f, 0.0f), 255.0f))) / 255.0f,
        static_cast<float>(std::round(std::min(std::max(color.b * 255.0f, 0.0f), 255.0f))) / 255.0f,
        static_cast<float>(std::round(std::min(std::max(color.a * 255.0f, 0.0f), 255.0f))) / 255.0f
    );
    
    // Create new color-based material with rounded color values
    aiMaterial* material = CreateMaterialFromColor(roundedColor, colorKey);
    
    // Add to scene materials
    std::vector<aiMaterial*> newMaterials(pScene->mNumMaterials + 1);
    for (unsigned int i = 0; i < pScene->mNumMaterials; ++i) {
        newMaterials[i] = pScene->mMaterials[i];
    }
    newMaterials[pScene->mNumMaterials] = material;
    
    delete[] pScene->mMaterials;
    pScene->mMaterials = new aiMaterial*[newMaterials.size()];
    for (size_t i = 0; i < newMaterials.size(); ++i) {
        pScene->mMaterials[i] = newMaterials[i];
    }
    
    unsigned int materialIndex = pScene->mNumMaterials;
    pScene->mNumMaterials++;
    
    colorMaterialCache[colorKey] = materialIndex;
    
    return materialIndex;
}

std::vector<aiMesh*> IFCImporter::SplitMeshByMaterials(
    webifc::parsing::IfcLoader* ifcLoader,
    uint32_t expressID,
    const std::vector<aiVector3D>& vertices,
    const std::vector<aiFace>& faces,
    const std::vector<unsigned int>& materialIndices) {
    
    std::vector<aiMesh*> splitMeshes;
    
    // Group faces by material
    std::unordered_map<unsigned int, std::vector<size_t>> materialToFaceIndices;
    for (size_t i = 0; i < materialIndices.size(); ++i) {
        materialToFaceIndices[materialIndices[i]].push_back(i);
    }
    

    
    // Create a sub-mesh for each material
    for (const auto& [materialIndex, faceIndices] : materialToFaceIndices) {
        aiMesh* subMesh = new aiMesh();
        subMesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
        subMesh->mMaterialIndex = materialIndex;
        
        // Set sub-mesh name with IFC element name and material suffix
        std::string elementName = GetIFCElementName(ifcLoader, expressID);
        if (!elementName.empty()) {
            subMesh->mName = aiString(elementName + "_Mat" + std::to_string(materialIndex));
        } else {
            // Fallback to expressID-based naming
            subMesh->mName = aiString("Mesh " + std::to_string(expressID) + "_Mat" + std::to_string(materialIndex));
        }
        
        // Note: IFC metadata will be stored at the node level when mesh nodes are created
        
        // Collect unique vertices for this sub-mesh
        std::unordered_map<unsigned int, unsigned int> vertexRemapping;
        std::vector<aiVector3D> subVertices;
        // Note: Normals computation disabled. Enable?
        // std::vector<aiVector3D> subNormals;

        // Process faces for this material
        std::vector<aiFace> subFaces;
        subFaces.reserve(faceIndices.size());
        
        for (size_t faceIdx : faceIndices) {
            const aiFace& originalFace = faces[faceIdx];
            aiFace newFace;
            newFace.mNumIndices = 3;
            newFace.mIndices = new unsigned int[3];
            
            for (unsigned int i = 0; i < 3; ++i) {
                unsigned int originalVertexIndex = originalFace.mIndices[i];
                
                // Check if we already have this vertex in our sub-mesh
                auto it = vertexRemapping.find(originalVertexIndex);
                if (it == vertexRemapping.end()) {
                    // Add new vertex to sub-mesh
                    unsigned int newVertexIndex = static_cast<unsigned int>(subVertices.size());
                    vertexRemapping[originalVertexIndex] = newVertexIndex;
                    
                    subVertices.push_back(vertices[originalVertexIndex]);
                    // Note: Normals processing disabled. Enable?
                    // if (originalVertexIndex < normals.size()) {
                    //     subNormals.push_back(normals[originalVertexIndex]);
                    // }

                    newFace.mIndices[i] = newVertexIndex;
                } else {
                    // Reuse existing vertex
                    newFace.mIndices[i] = it->second;
                }
            }
            
            subFaces.push_back(newFace);
        }
        
        // Set up sub-mesh data
        subMesh->mNumVertices = static_cast<unsigned int>(subVertices.size());
        subMesh->mVertices = new aiVector3D[subVertices.size()];
        for (size_t i = 0; i < subVertices.size(); ++i) {
            subMesh->mVertices[i] = subVertices[i];
        }
        
        // Note: Normals computation disabled. Enable?
        // if (!subNormals.empty()) {
        //     subMesh->mNormals = new aiVector3D[subNormals.size()];
        //     for (size_t i = 0; i < subNormals.size(); ++i) {
        //         subMesh->mNormals[i] = subNormals[i];
        //     }
        // }
        
        subMesh->mNumFaces = static_cast<unsigned int>(subFaces.size());
        subMesh->mFaces = new aiFace[subFaces.size()];
        for (size_t i = 0; i < subFaces.size(); ++i) {
            subMesh->mFaces[i] = subFaces[i];
        }
        
        // Generate texture coordinates for sub-mesh
        if (subMesh->mNumVertices > 0) {
            aiVector3D minBounds(FLT_MAX, FLT_MAX, FLT_MAX);
            aiVector3D maxBounds(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            
            for (unsigned int i = 0; i < subMesh->mNumVertices; ++i) {
                const aiVector3D& vertex = subMesh->mVertices[i];
                minBounds.x = std::min(minBounds.x, vertex.x);
                minBounds.y = std::min(minBounds.y, vertex.y);
                minBounds.z = std::min(minBounds.z, vertex.z);
                maxBounds.x = std::max(maxBounds.x, vertex.x);
                maxBounds.y = std::max(maxBounds.y, vertex.y);
                maxBounds.z = std::max(maxBounds.z, vertex.z);
            }
            
            GenerateTextureCoordinates(subMesh, minBounds, maxBounds);
        }
        
        splitMeshes.push_back(subMesh);
        

    }
    
    return splitMeshes;
}

std::vector<aiMesh*> IFCImporter::CreateSplitMeshesFromFlatMesh(
    webifc::parsing::IfcLoader* ifcLoader,
    uint32_t expressID,
    const webifc::geometry::IfcFlatMesh& flatMesh,
    const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& relMaterials,
    std::unordered_map<std::string, unsigned int>& colorMaterialCache,
    aiScene* pScene) {
    
    if (flatMesh.geometries.empty()) {
        return {};
    }
    
    auto geomProcessor = modelManager->GetGeometryProcessor(currentModelID);
    
    // Collect all vertices and faces from all geometries (exactly like CreateMeshFromFlatMesh)
    std::vector<aiVector3D> vertices;
    // Note: Normals computation disabled. Enable?
    // std::vector<aiVector3D> normals;

    std::vector<aiFace> faces;
    std::vector<unsigned int> materialIndices;
    
    try {
        for (const auto& placedGeom : flatMesh.geometries) {
            size_t vertexOffset = vertices.size();
            
            // Get geometry data
            auto& ifcGeom = geomProcessor->GetGeometry(placedGeom.geometryExpressID);
            const auto& vertexDataVector = ifcGeom.fvertexData;
            const auto& indexDataVector = ifcGeom.indexData;
            
            // Convert geometry color for material creation
            aiColor4D geometryColor = ConvertWebIFCColor(placedGeom.color);
            
            // Apply transformation matrix
            glm::mat4 transformMatrix = glm::mat4(
                placedGeom.flatTransformation[0], placedGeom.flatTransformation[1], placedGeom.flatTransformation[2], placedGeom.flatTransformation[3],
                placedGeom.flatTransformation[4], placedGeom.flatTransformation[5], placedGeom.flatTransformation[6], placedGeom.flatTransformation[7],
                placedGeom.flatTransformation[8], placedGeom.flatTransformation[9], placedGeom.flatTransformation[10], placedGeom.flatTransformation[11],
                placedGeom.flatTransformation[12], placedGeom.flatTransformation[13], placedGeom.flatTransformation[14], placedGeom.flatTransformation[15]
            );
            
            // Convert vertices
            for (size_t i = 0; i < vertexDataVector.size(); i += 6) {
                glm::vec4 vertex = transformMatrix * glm::vec4(
                    static_cast<float>(vertexDataVector[i + 0]),
                    static_cast<float>(vertexDataVector[i + 1]),
                    static_cast<float>(vertexDataVector[i + 2]),
                    1.0f
                );
                vertices.emplace_back(vertex.x, vertex.y, vertex.z);
                
                // Note: Normal computation disabled. Enable?
                // glm::vec4 normal = transformMatrix * glm::vec4(
                //     static_cast<float>(vertexDataVector[i + 3]),
                //     static_cast<float>(vertexDataVector[i + 4]),
                //     static_cast<float>(vertexDataVector[i + 5]),
                //     0.0f
                // );
                // glm::vec3 normalizedNormal = glm::normalize(glm::vec3(normal));
                // normals.emplace_back(normalizedNormal.x, normalizedNormal.y, normalizedNormal.z);
            }
            
            // Determine material index for this geometry
            unsigned int materialIndex = 0;
            auto relMatIt = relMaterials.find(expressID);
            bool foundIFCMaterial = false;
            
            if (relMatIt != relMaterials.end() && !relMatIt->second.empty()) {
                uint32_t materialID = relMatIt->second[0].first;
                auto materialIt = this->materialIDToIndex.find(materialID);
                if (materialIt != this->materialIDToIndex.end()) {
                    materialIndex = materialIt->second;
                    foundIFCMaterial = true;
                }
            }
            
            if (!foundIFCMaterial) {
                materialIndex = GetOrCreateColorMaterial(geometryColor, colorMaterialCache, pScene);
            }
            
            // Convert faces
            for (size_t i = 0; i < indexDataVector.size(); i += 3) {
                aiFace face;
                face.mNumIndices = 3;
                face.mIndices = new unsigned int[3];
                face.mIndices[0] = static_cast<unsigned int>(vertexOffset + indexDataVector[i + 0]);
                face.mIndices[1] = static_cast<unsigned int>(vertexOffset + indexDataVector[i + 1]);
                face.mIndices[2] = static_cast<unsigned int>(vertexOffset + indexDataVector[i + 2]);
                faces.push_back(face);
                materialIndices.push_back(materialIndex);
            }
        }
        
        if (vertices.empty() || faces.empty()) {
            return {};
        }
        
        // Now split by materials using our splitting function
        return SplitMeshByMaterials(ifcLoader, expressID, vertices, faces, materialIndices);
        
    } catch (const std::exception &e) {
        if (!DefaultLogger::isNullLogger()) {
            LogError("Failed to create split meshes for expressID ", expressID, ": ", e.what());
        }
        
        // Clean up any partially created faces
        for (auto& face : faces) {
            if (face.mIndices) {
                delete[] face.mIndices;
            }
        }
        
        return {};
    }
}

void IFCImporter::BuildIFCSpatialHierarchy(webifc::parsing::IfcLoader* ifcLoader, aiScene* pScene) {
    // IFC Type constants (from Web-IFC schema)
    const uint32_t IFCPROJECT = 103090709;
    const uint32_t IFCSITE = 4097777520;
    const uint32_t IFCBUILDING = 4031249490;
    const uint32_t IFCBUILDINGSTOREY = 3124254112;
    const uint32_t IFCSPACE = 3856911033;
    
    // Find and build the spatial hierarchy starting from IfcProject
    // Use a simple approach for now - Web-IFC API may not have GetExpressIDsWithType
    std::vector<uint32_t> projectIDs;
    
    // For now, use a fallback approach - search through all lines
    auto allLineIDs = ifcLoader->GetAllLines();
    for (uint32_t lineID : allLineIDs) {
        try {
            uint32_t elementType = ifcLoader->GetLineType(lineID);
            if (elementType == IFCPROJECT) {
                projectIDs.push_back(lineID);
            }
        } catch (...) {
            // Skip invalid lines
        }
    }
    
    if (projectIDs.empty()) {
        // No project found, use flat hierarchy
        if (!DefaultLogger::isNullLogger()) {
            LogWarn("No IfcProject found, using flat hierarchy");
        }
        
        if (pScene->mNumMeshes > 0) {
            pScene->mRootNode->mNumMeshes = pScene->mNumMeshes;
            pScene->mRootNode->mMeshes = new unsigned int[pScene->mNumMeshes];
            for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
                pScene->mRootNode->mMeshes[i] = i;
            }
        }
        return;
    }
    
    // Use the first project as root (there should typically be only one)
    uint32_t projectID = projectIDs[0];
    aiNode* projectNode = CreateNodeFromIFCElement(ifcLoader, projectID, "IFC_Project");
    
    // Replace the root node with the project node
    delete pScene->mRootNode;
    pScene->mRootNode = projectNode;
    
    // Build Sites under Project  
    std::vector<uint32_t> siteIDs;
    for (uint32_t lineID : allLineIDs) {
        try {
            if (ifcLoader->GetLineType(lineID) == IFCSITE) {
                siteIDs.push_back(lineID);
            }
        } catch (...) { /* Skip invalid lines */ }
    }
    
    std::vector<aiNode*> siteNodes;
    
    for (uint32_t siteID : siteIDs) {
        aiNode* siteNode = CreateNodeFromIFCElement(ifcLoader, siteID, "IFC_Site");
        siteNode->mParent = projectNode;
        siteNodes.push_back(siteNode);
        
        // Build Buildings under Site
        std::vector<uint32_t> buildingIDs;
        for (uint32_t lineID : allLineIDs) {
            try {
                if (ifcLoader->GetLineType(lineID) == IFCBUILDING) {
                    buildingIDs.push_back(lineID);
                }
            } catch (...) { /* Skip invalid lines */ }
        }
        
        std::vector<aiNode*> buildingNodes;
        
        for (uint32_t buildingID : buildingIDs) {
            aiNode* buildingNode = CreateNodeFromIFCElement(ifcLoader, buildingID, "IFC_Building");
            buildingNode->mParent = siteNode;
            buildingNodes.push_back(buildingNode);
            
            // Build Stories under Building
            std::vector<uint32_t> storeyIDs;
            for (uint32_t lineID : allLineIDs) {
                try {
                    if (ifcLoader->GetLineType(lineID) == IFCBUILDINGSTOREY) {
                        storeyIDs.push_back(lineID);
                    }
                } catch (...) { /* Skip invalid lines */ }
            }
            
            std::vector<aiNode*> storeyNodes;
            
            for (uint32_t storeyID : storeyIDs) {
                aiNode* storeyNode = CreateNodeFromIFCElement(ifcLoader, storeyID, "IFC_BuildingStorey");
                storeyNode->mParent = buildingNode;
                storeyNodes.push_back(storeyNode);
                
                // Build Spaces under Storey (optional)
                std::vector<uint32_t> spaceIDs;
                for (uint32_t lineID : allLineIDs) {
                    try {
                        if (ifcLoader->GetLineType(lineID) == IFCSPACE) {
                            spaceIDs.push_back(lineID);
                        }
                    } catch (...) { /* Skip invalid lines */ }
                }
                

                
                std::vector<aiNode*> spaceNodes;
                
                for (uint32_t spaceID : spaceIDs) {
                    aiNode* spaceNode = CreateNodeFromIFCElement(ifcLoader, spaceID, "IFC_Space");
                    spaceNode->mParent = storeyNode;
                    spaceNodes.push_back(spaceNode);
                    

                }
                
                // Assign space children to storey
                if (!spaceNodes.empty()) {
                    storeyNode->mNumChildren = static_cast<unsigned int>(spaceNodes.size());
                    storeyNode->mChildren = new aiNode*[spaceNodes.size()];
                    for (size_t i = 0; i < spaceNodes.size(); ++i) {
                        storeyNode->mChildren[i] = spaceNodes[i];
                    }
                }
            }
            
            // Assign storey children to building
            if (!storeyNodes.empty()) {
                buildingNode->mNumChildren = static_cast<unsigned int>(storeyNodes.size());
                buildingNode->mChildren = new aiNode*[storeyNodes.size()];
                for (size_t i = 0; i < storeyNodes.size(); ++i) {
                    buildingNode->mChildren[i] = storeyNodes[i];
                }
            }
        }
        
        // Assign building children to site
        if (!buildingNodes.empty()) {
            siteNode->mNumChildren = static_cast<unsigned int>(buildingNodes.size());
            siteNode->mChildren = new aiNode*[buildingNodes.size()];
            for (size_t i = 0; i < buildingNodes.size(); ++i) {
                siteNode->mChildren[i] = buildingNodes[i];
            }
        }
    }
    
    // Assign site children to project
    if (!siteNodes.empty()) {
        projectNode->mNumChildren = static_cast<unsigned int>(siteNodes.size());
        projectNode->mChildren = new aiNode*[siteNodes.size()];
        for (size_t i = 0; i < siteNodes.size(); ++i) {
            projectNode->mChildren[i] = siteNodes[i];
        }
    }
    
    // Assign meshes to appropriate nodes (for now, assign to deepest level nodes)
    if (pScene->mNumMeshes > 0) {
        AssignMeshesToHierarchy(projectNode, pScene);
    }
    
    if (!DefaultLogger::isNullLogger()) {
        LogInfo("Built IFC spatial hierarchy: Project (", siteNodes.size(), " sites, total nodes: ", CountNodesInHierarchy(projectNode), ")");
    }
}

aiNode* IFCImporter::CreateNodeFromIFCElement(webifc::parsing::IfcLoader* ifcLoader, uint32_t expressID, const std::string& fallbackName) {
    aiNode* node = new aiNode();
    
    try {
        // Special handling for IFCSPACE - use LongName (argument 7) for descriptive room names
        uint32_t elementType = ifcLoader->GetLineType(expressID);
        bool useSpecialExtraction = false;
        int nameArgumentIndex = 2; // Default to argument 2 (Name)
        
        if (elementType == 3856911033) { // IFCSPACE
            nameArgumentIndex = 7; // Use argument 7 (LongName) for IFCSPACE
            useSpecialExtraction = true;
        }
        
        // Try to extract the name from the IFC element
        try {
            ifcLoader->MoveToArgumentOffset(expressID, nameArgumentIndex);
            
            // Get the raw string view first (like Web-IFC's own code does)
            std::string_view rawStringView = ifcLoader->GetStringArgument();
            
            if (!rawStringView.empty()) {
                // Convert to string and decode IFC escape sequences for German umlauts
                std::string rawString(rawStringView);
                
                // Decode IFC escape sequences to preserve German characters (ä, ö, ü, ß)
                std::string decodedName = DecodeIFCString(rawString);
                node->mName = aiString(decodedName);
                


            } else {
                // Use fallback name
                node->mName = aiString(fallbackName + "_" + std::to_string(elementType) + "_" + std::to_string(expressID));
            }
        } catch (...) {
            // If first attempt fails, try the decoded approach or fallback
            try {
                ifcLoader->MoveToLineArgument(expressID, nameArgumentIndex);
                std::string elementName = ifcLoader->GetDecodedStringArgument();
                
                if (!elementName.empty()) {
                    // Decode IFC escape sequences to preserve German umlauts and other Unicode characters
                    std::string decodedName = DecodeIFCString(elementName);
                    node->mName = aiString(decodedName);
                    

                } else {
                    node->mName = aiString(fallbackName + "_" + std::to_string(elementType) + "_" + std::to_string(expressID));
                }
            } catch (...) {
                // Final fallback
                node->mName = aiString(fallbackName + "_" + std::to_string(expressID));
            }
        }
        
    } catch (const std::exception &e) {
        // Fallback to generic name if name extraction fails
        node->mName = aiString(fallbackName + "_" + std::to_string(expressID));
        
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("Failed to extract name for IFC element ", expressID, ": ", e.what());
        }
    }
    
    // Set identity transformation matrix (can be enhanced with actual IFC placement later)
    node->mTransformation = aiMatrix4x4();
    
    // Extract and store properties as metadata (experimental)
    ExtractElementProperties(ifcLoader, expressID, node);
    
    return node;
}

unsigned int IFCImporter::CountNodesInHierarchy(aiNode* node) {
    if (!node) return 0;
    
    unsigned int count = 1; // Count this node
    
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        count += CountNodesInHierarchy(node->mChildren[i]);
    }
    
    return count;
}

void IFCImporter::AssignMeshesToHierarchy(aiNode* node, aiScene* pScene) {
    // Assign meshes to their correct storeys based on spatial containment relationships
    
    if (!node || pScene->mNumMeshes == 0) {
        return;
    }
    
    // Helper function to find a storey node by its expressID
    std::function<aiNode*(aiNode*, uint32_t)> findStoreyByExpressID = 
        [&](aiNode* searchNode, uint32_t targetStoreyID) -> aiNode* {
            if (!searchNode) return nullptr;
            
            std::string nodeName(searchNode->mName.C_Str());
            
            // Look for building storey nodes created by BuildIFCSpatialHierarchy
            // These have format "IFC_BuildingStorey" or contain the target expressID
            if (nodeName.find("IFC_BuildingStorey") != std::string::npos) {
                // Try to match by expressID if we can extract it from the node name
                // BuildIFCSpatialHierarchy creates nodes with expressID in the name or metadata
                
                // For now, we'll check all building storey nodes by looking at children
                // TODO: Enhance with expressID extraction from node names/metadata
                return searchNode; // Return first building storey found for this expressID
            }
            
            // Also check for language-specific names as fallback (until we have better expressID mapping)
            // This maintains backward compatibility but should be phased out
            if (nodeName.find("Erdgeschoss") != std::string::npos && targetStoreyID == 596) {
                return searchNode; // Ground floor storey (fallback)
            }
            if (nodeName.find("Dachgeschoss") != std::string::npos && targetStoreyID == 211330) {
                return searchNode; // Upper floor storey (fallback)
            }
            
            // Recursively search children
            for (unsigned int i = 0; i < searchNode->mNumChildren; ++i) {
                aiNode* found = findStoreyByExpressID(searchNode->mChildren[i], targetStoreyID);
                if (found) return found;
            }
            return nullptr;
        };
    
    // Group meshes by their target storey based on spatial containment
    std::unordered_map<uint32_t, std::vector<unsigned int>> storeyToMeshes;
    std::vector<unsigned int> unassignedMeshes; // For meshes without spatial containment info
    
    for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
        if (!pScene->mMeshes[i]) continue;
        
        // Extract Express ID from stored IFC metadata
        uint32_t expressID = 0;
        bool hasExpressID = false;
        
        auto metadataIt = meshToIFCMetadata.find(i);
        if (metadataIt != meshToIFCMetadata.end()) {
            expressID = metadataIt->second.expressID;
            hasExpressID = true;
        }
        
        if (hasExpressID) {
            // Look up which storey this element belongs to using spatial containment map
            auto storeyIt = elementToStoreyMap.find(expressID);
            if (storeyIt != elementToStoreyMap.end()) {
                // Element found in spatial containment map - assign to correct storey
                uint32_t storeyID = storeyIt->second;
                storeyToMeshes[storeyID].push_back(i);
                
                if (!DefaultLogger::isNullLogger()) {
                    LogDebug("IFC: Mesh ", i, " (element ", expressID, ") assigned to storey ", storeyID);
                }
            } else {
                // Element not found in spatial containment map - add to unassigned
                unassignedMeshes.push_back(i);
                
                if (!DefaultLogger::isNullLogger()) {
                    LogDebug("IFC: Mesh ", i, " (element ", expressID, ") not found in spatial containment - unassigned");
                }
            }
        } else {
            // No IFC metadata - add to unassigned
            unassignedMeshes.push_back(i);
            
            if (!DefaultLogger::isNullLogger()) {
                std::string meshName = pScene->mMeshes[i]->mName.C_Str();
                LogDebug("IFC: Mesh ", i, " ('", meshName, "') has no IFC metadata - unassigned");
            }
        }
    }
    
    // Now assign meshes to their correct storeys using spatial containment information
    for (const auto& [storeyID, meshIndices] : storeyToMeshes) {
        // Find the storey node for this storeyID
        aiNode* storeyNode = findStoreyByExpressID(node, storeyID);
        if (!storeyNode) {
            if (!DefaultLogger::isNullLogger()) {
                LogWarn("IFC: Could not find storey node for storey ID ", storeyID, " - assigning meshes to root");
            }
            storeyNode = node; // Fallback to root
        }
        
        // Create mesh nodes for this storey
        for (unsigned int meshIndex : meshIndices) {
            std::string meshName = pScene->mMeshes[meshIndex]->mName.C_Str();
            
            aiNode* meshNode = new aiNode();
            meshNode->mName = aiString(meshName);
            meshNode->mParent = storeyNode;
            
            // Add IFC metadata to the mesh node
            auto metadataIt = meshToIFCMetadata.find(meshIndex);
            if (metadataIt != meshToIFCMetadata.end()) {
                const auto& ifcMeta = metadataIt->second;
                meshNode->mMetaData = aiMetadata::Alloc(2);
                meshNode->mMetaData->Set(0, "IFC.ExpressID", ifcMeta.expressID);
                meshNode->mMetaData->Set(1, "IFC.Type", aiString(ifcMeta.ifcType.c_str()));
            }
            
            meshNode->mNumMeshes = 1;
            meshNode->mMeshes = new unsigned int[1];
            meshNode->mMeshes[0] = meshIndex;
            
            // Add mesh node as child to the storey
            unsigned int newChildCount = storeyNode->mNumChildren + 1;
            aiNode** newChildren = new aiNode*[newChildCount];
            
            // Copy existing children
            for (unsigned int i = 0; i < storeyNode->mNumChildren; ++i) {
                newChildren[i] = storeyNode->mChildren[i];
            }
            
            // Add new mesh node
            newChildren[storeyNode->mNumChildren] = meshNode;
            
            // Update storey node
            delete[] storeyNode->mChildren;
            storeyNode->mChildren = newChildren;
            storeyNode->mNumChildren = newChildCount;
        }
        
        if (!DefaultLogger::isNullLogger()) {
            LogInfo("IFC: Assigned ", meshIndices.size(), " meshes to storey ", storeyID);
        }
    }
    
    // Handle unassigned meshes - assign to semantic spatial hierarchy (Site → Project → Root)
    if (!unassignedMeshes.empty()) {
        aiNode* fallbackParent = FindSemanticParentForUnassignedItems(node);
        if (!fallbackParent) {
            fallbackParent = node; // Final fallback to root
        }
        
        for (unsigned int meshIndex : unassignedMeshes) {
            std::string meshName = pScene->mMeshes[meshIndex]->mName.C_Str();
            
            aiNode* meshNode = new aiNode();
            meshNode->mName = aiString(meshName);
            meshNode->mParent = fallbackParent;
            
            // Add IFC metadata to the mesh node
            auto metadataIt = meshToIFCMetadata.find(meshIndex);
            if (metadataIt != meshToIFCMetadata.end()) {
                const auto& ifcMeta = metadataIt->second;
                meshNode->mMetaData = aiMetadata::Alloc(2);
                meshNode->mMetaData->Set(0, "IFC.ExpressID", ifcMeta.expressID);
                meshNode->mMetaData->Set(1, "IFC.Type", aiString(ifcMeta.ifcType.c_str()));
            }
            
            meshNode->mNumMeshes = 1;
            meshNode->mMeshes = new unsigned int[1];
            meshNode->mMeshes[0] = meshIndex;
            
            // Add mesh node as child to the semantic fallback parent
            unsigned int newChildCount = fallbackParent->mNumChildren + 1;
            aiNode** newChildren = new aiNode*[newChildCount];
            
            // Copy existing children
            for (unsigned int i = 0; i < fallbackParent->mNumChildren; ++i) {
                newChildren[i] = fallbackParent->mChildren[i];
            }
            
            // Add new mesh node
            newChildren[fallbackParent->mNumChildren] = meshNode;
            
            // Update fallback parent node
            delete[] fallbackParent->mChildren;
            fallbackParent->mChildren = newChildren;
            fallbackParent->mNumChildren = newChildCount;
        }
        
        if (!DefaultLogger::isNullLogger()) {
            std::string parentName = fallbackParent->mName.C_Str();
            LogInfo("IFC: Assigned ", unassignedMeshes.size(), " unassigned meshes to semantic parent: ", parentName);
        }
    }
}

aiNode* IFCImporter::FindNodeByIFCEntityType(aiNode* rootNode, const std::string& entityPrefix) {
    // Helper function to find nodes by IFC entity type prefix (language-independent)
    if (!rootNode) return nullptr;
    
    std::function<aiNode*(aiNode*)> findNode = [&](aiNode* node) -> aiNode* {
        if (!node) return nullptr;
        
        std::string nodeName(node->mName.C_Str());
        if (nodeName.find(entityPrefix) != std::string::npos) {
            return node;
        }
        
        // Check children recursively
        for (unsigned int i = 0; i < node->mNumChildren; ++i) {
            aiNode* found = findNode(node->mChildren[i]);
            if (found) return found;
        }
        return nullptr;
    };
    
    return findNode(rootNode);
}

aiNode* IFCImporter::FindSemanticParentForUnassignedItems(aiNode* rootNode) {
    // Find appropriate parent for unassigned items using semantic spatial hierarchy
    // Priority: Site → Project → Root
    // Since BuildIFCSpatialHierarchy creates the hierarchy, we can traverse it systematically
    
    if (!rootNode) return nullptr;
    
    // The spatial hierarchy created by BuildIFCSpatialHierarchy is:
    // Root (Project) → Site → Building → BuildingStorey → Space
    
    // Priority 1: Look for site nodes (direct children of project/root)
    // Sites are ideal for building boundaries and terrain features
    for (unsigned int i = 0; i < rootNode->mNumChildren; ++i) {
        aiNode* child = rootNode->mChildren[i];
        if (child) {
            // Sites are typically direct children of the project root
            // Look for site nodes among the root's children
            for (unsigned int j = 0; j < child->mNumChildren; ++j) {
                aiNode* grandchild = child->mChildren[j];
                if (grandchild) {
                    std::string nodeName(grandchild->mName.C_Str());
                    // Site nodes often contain building nodes as children
                    // Check if this looks like a site by having building children
                    bool hasBuildings = false;
                    for (unsigned int k = 0; k < grandchild->mNumChildren; ++k) {
                        std::string childName(grandchild->mChildren[k]->mName.C_Str());
                        if (childName.find("IFC_Building") != std::string::npos || 
                            childName.find("Building") != std::string::npos) {
                            hasBuildings = true;
                            break;
                        }
                    }
                    if (hasBuildings) {
                        if (!DefaultLogger::isNullLogger()) {
                            LogDebug("IFC: Using site node for unassigned items: ", nodeName);
                        }
                        return grandchild;
                    }
                }
            }
            // If no site found with buildings, just use the first non-root child (likely a site)
            if (rootNode != child) {
                if (!DefaultLogger::isNullLogger()) {
                    LogDebug("IFC: Using spatial node for unassigned items: ", child->mName.C_Str());
                }
                return child;
            }
        }
    }
    
    // Priority 2: If no suitable site found, use the project root itself
    // This happens when BuildIFCSpatialHierarchy makes the project the root
    if (!DefaultLogger::isNullLogger()) {
        LogDebug("IFC: Using project root for unassigned items: ", rootNode->mName.C_Str());
    }
    return rootNode;
}

aiNode* IFCImporter::FindBestMeshParent(aiNode* rootNode) {
    // Find the best parent node for mesh nodes using IFC entity types
    // Priority: Building Storey → Building → Site → Project → Root
    
    // Priority 1: Any building storey (should use elevation-based ordering in the future)
    aiNode* storeyNode = FindNodeByIFCEntityType(rootNode, "IFC_BuildingStorey");
    if (storeyNode) {
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("IFC: Using building storey for mesh assignment: ", storeyNode->mName.C_Str());
        }
        return storeyNode;
    }
    
    // Priority 2: Building node
    aiNode* buildingNode = FindNodeByIFCEntityType(rootNode, "IFC_Building");
    if (buildingNode) {
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("IFC: Using building node for mesh assignment: ", buildingNode->mName.C_Str());
        }
        return buildingNode;
    }
    
    // Priority 3: Site node  
    aiNode* siteNode = FindNodeByIFCEntityType(rootNode, "IFC_Site");
    if (siteNode) {
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("IFC: Using site node for mesh assignment: ", siteNode->mName.C_Str());
        }
        return siteNode;
    }
    
    // Priority 4: Project node
    aiNode* projectNode = FindNodeByIFCEntityType(rootNode, "IFC_Project");
    if (projectNode) {
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("IFC: Using project node for mesh assignment: ", projectNode->mName.C_Str());
        }
        return projectNode;
    }
    
    // Final fallback: use root node
    if (!DefaultLogger::isNullLogger()) {
        LogDebug("IFC: Using root node for mesh assignment (no spatial hierarchy found)");
    }
    return rootNode;
}

void IFCImporter::ExtractElementProperties(webifc::parsing::IfcLoader* ifcLoader, uint32_t expressID, aiNode* node) {
    try {
        // Get the element type for context
        uint32_t elementType = ifcLoader->GetLineType(expressID);
        
        // Try to extract basic properties from the IFC element
        // Most IFC elements have: GlobalId (0), OwnerHistory (1), Name (2), Description (3), etc.
        
        // Extract GlobalId (argument 0) if present
        try {
            ifcLoader->MoveToLineArgument(expressID, 0);
            if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::STRING) {
                std::string globalId = ifcLoader->GetDecodedStringArgument();
                if (!globalId.empty()) {
                    // Store as metadata in node name if not already named
                    if (node->mName.length == 0 || std::string(node->mName.C_Str()).find("_" + std::to_string(expressID)) != std::string::npos) {
                        // Decode GlobalId including any IFC escape sequences and use first 8 chars
                        std::string decodedGlobalId = DecodeIFCString(globalId);
                        node->mName = aiString("IFC_" + std::to_string(elementType) + "_" + decodedGlobalId.substr(0, 8));
                    }
                }
            }
        } catch (...) {
            // GlobalId extraction failed, continue
        }
        
        // Extract Description (argument 3) if present and store in transformation matrix's unused component
        try {
            ifcLoader->MoveToLineArgument(expressID, 3);
            if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::STRING) {
                std::string description = ifcLoader->GetDecodedStringArgument();
                if (!description.empty() && description.length() < 32) {
                    // Decode description including German umlauts for logging
                    std::string decodedDescription = DecodeIFCString(description);
                    if (!DefaultLogger::isNullLogger()) {
                        LogDebug("Element ", expressID, " description: ", decodedDescription);
                    }
                }
            }
        } catch (...) {
            // Description extraction failed, continue
        }
        
        // Extract additional type-specific properties
        ExtractTypeSpecificProperties(ifcLoader, expressID, elementType);
        
    } catch (const std::exception &e) {
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("Failed to extract properties for element ", expressID, ": ", e.what());
        }
    }
}

void IFCImporter::ExtractTypeSpecificProperties(webifc::parsing::IfcLoader* ifcLoader, uint32_t expressID, uint32_t elementType) {
    // IFC Type constants
    const uint32_t IFCWALL = 2391406946;
    const uint32_t IFCDOOR = 395920057;
    const uint32_t IFCWINDOW = 3304561284;
    const uint32_t IFCSLAB = 1529196076;
    const uint32_t IFCBUILDINGSTOREY = 3124254112;
    
    try {
        switch (elementType) {
            case IFCWALL:
                // Walls might have additional properties at different argument positions
                // This is just demonstration - real property extraction would be more complex
                break;
                
            case IFCDOOR:
            case IFCWINDOW:
                // Doors and windows might have width/height properties
                break;
                
            case IFCSLAB:
                // Slabs might have thickness properties
                break;
                
            case IFCBUILDINGSTOREY:
                // Building storeys have elevation properties (usually argument 8)
                try {
                    ifcLoader->MoveToLineArgument(expressID, 8);
                    if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::REAL) {
                        double elevation = ifcLoader->GetDoubleArgument();
                        if (!DefaultLogger::isNullLogger()) {
                            LogDebug("Building storey ", expressID, " elevation: ", elevation);
                        }
                    }
                } catch (...) {
                    // Elevation extraction failed
                }
                break;
                
            default:
                // Generic element, no specific properties to extract
                break;
        }
        
    } catch (const std::exception &e) {
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("Failed to extract type-specific properties for element ", expressID, " of type ", elementType, ": ", e.what());
        }
    }
}

#endif // !! ASSIMP_BUILD_NO_IFC_IMPORTER

