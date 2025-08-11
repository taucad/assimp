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
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

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
        if (!DefaultLogger::isNullLogger()) {
            LogDebug("Web-IFC model manager initialized");
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
    
#ifdef IFC_LOADER_DEBUG
    if (!DefaultLogger::isNullLogger()) {
        LogDebug("ExtractGeometry: IFC loader and geometry processor obtained successfully");
    }
#endif
    
    std::vector<aiMesh*> meshes;
    std::unordered_map<uint32_t, aiMesh*> expressIDToMesh;

    try {
        // Get all IFC element types that have geometry
        auto schemaManager = modelManager->GetSchemaManager();
        auto elementTypes = schemaManager.GetIfcElementList();

        for (auto elementType : elementTypes) {
            // Skip openings, spaces, and other non-geometric elements
            if (elementType == webifc::schema::IFCOPENINGELEMENT || 
                elementType == webifc::schema::IFCSPACE || 
                elementType == webifc::schema::IFCOPENINGSTANDARDCASE) {
                continue;
            }

            auto elements = loader->GetExpressIDsWithType(elementType);
            
            for (uint32_t expressID : elements) {
                try {
                    // Get the flat mesh from Web-IFC
                    webifc::geometry::IfcFlatMesh flatMesh = geomProcessor->GetFlatMesh(expressID);
                    
                    if (!flatMesh.geometries.empty()) {
#ifdef IFC_LOADER_DEBUG
                        if (!DefaultLogger::isNullLogger()) {
                            LogDebug("ExtractGeometry: Processing ", flatMesh.geometries.size(), " geometries for express ID: ", expressID);
                        }
#endif
                        // Convert each geometry in the flat mesh
                        for (size_t i = 0; i < flatMesh.geometries.size(); ++i) {
#ifdef IFC_LOADER_DEBUG
                            if (!DefaultLogger::isNullLogger()) {
                                LogDebug("ExtractGeometry: Processing geometry ", i + 1, " of ", flatMesh.geometries.size());
                            }
#endif
                            auto &placedGeom = flatMesh.geometries[i];
                            
#ifdef IFC_LOADER_DEBUG
                            if (!DefaultLogger::isNullLogger()) {
                                LogDebug("ExtractGeometry: Getting geometry for express ID: ", placedGeom.geometryExpressID);
                            }
#endif
                            // Prepare vertex data
                            auto &ifcGeom = geomProcessor->GetGeometry(placedGeom.geometryExpressID);
#ifdef IFC_LOADER_DEBUG
                            if (!DefaultLogger::isNullLogger()) {
                                LogDebug("ExtractGeometry: Calling GetVertexData()...");
                            }
#endif
                            ifcGeom.GetVertexData(); // Ensures data is available
#ifdef IFC_LOADER_DEBUG
                            if (!DefaultLogger::isNullLogger()) {
                                LogDebug("ExtractGeometry: GetVertexData() completed. Converting to Assimp mesh...");
                            }
#endif
                            
                            aiMesh* assimpMesh = ConvertWebIFCMesh(flatMesh, i);
#ifdef IFC_LOADER_DEBUG
                            if (!DefaultLogger::isNullLogger()) {
                                LogDebug("ExtractGeometry: ConvertWebIFCMesh completed. Result: ", (assimpMesh ? "SUCCESS" : "NULL"));
                            }
#endif
                            if (assimpMesh) {
                                // Set mesh name based on express ID
                                assimpMesh->mName = aiString("IFC_Element_" + std::to_string(expressID) + "_" + std::to_string(i));
                                meshes.push_back(assimpMesh);
                                expressIDToMesh[expressID] = assimpMesh;
                            }
                        }
                    }
                    
                    // Clear geometry to free memory
                    geomProcessor->Clear();
                    
                } catch (const std::exception &e) {
                    if (!DefaultLogger::isNullLogger()) {
                        LogWarn("Failed to extract geometry for element ", expressID, ": ", e.what());
                    }
                }
            }
        }

        // Set up meshes in scene
        if (!meshes.empty()) {
            pScene->mNumMeshes = static_cast<unsigned int>(meshes.size());
            pScene->mMeshes = new aiMesh*[meshes.size()];
            
            for (size_t i = 0; i < meshes.size(); ++i) {
                pScene->mMeshes[i] = meshes[i];
            }
            
            if (!DefaultLogger::isNullLogger()) {
                LogInfo("Extracted ", meshes.size(), " meshes from IFC file");
            }
        } else {
            // Fallback: create a simple mesh if no geometry found
            pScene->mNumMeshes = 1;
            pScene->mMeshes = new aiMesh*[1];
            
            aiMesh* fallbackMesh = new aiMesh();
            fallbackMesh->mName = aiString("IFC_NoGeometry");
            fallbackMesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
            fallbackMesh->mNumVertices = 3;
            fallbackMesh->mVertices = new aiVector3D[3];
            fallbackMesh->mVertices[0] = aiVector3D(0.0f, 0.0f, 0.0f);
            fallbackMesh->mVertices[1] = aiVector3D(1.0f, 0.0f, 0.0f);
            fallbackMesh->mVertices[2] = aiVector3D(0.5f, 1.0f, 0.0f);
            
            fallbackMesh->mNumFaces = 1;
            fallbackMesh->mFaces = new aiFace[1];
            fallbackMesh->mFaces[0].mNumIndices = 3;
            fallbackMesh->mFaces[0].mIndices = new unsigned int[3];
            fallbackMesh->mFaces[0].mIndices[0] = 0;
            fallbackMesh->mFaces[0].mIndices[1] = 1;
            fallbackMesh->mFaces[0].mIndices[2] = 2;
            fallbackMesh->mMaterialIndex = 0;
            
            pScene->mMeshes[0] = fallbackMesh;
            
            if (!DefaultLogger::isNullLogger()) {
                LogWarn("No geometry found in IFC file, created fallback mesh");
            }
        }

    } catch (const std::exception &e) {
        if (!DefaultLogger::isNullLogger()) {
            LogError("Failed to extract geometry: ", e.what());
        }
        throw;
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
    
#ifdef IFC_LOADER_DEBUG
    if (!DefaultLogger::isNullLogger()) {
        LogDebug("ConvertWebIFCMesh: Getting geometry processor and geometry data for express ID: ", placedGeom.geometryExpressID);
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
    mesh->mNormals = new aiVector3D[numVertices];
    
    for (size_t i = 0; i < numVertices; ++i) {
        size_t offset = i * VERTEX_FORMAT_SIZE;
        
        // Position
        mesh->mVertices[i] = aiVector3D(
            vertexData[offset + 0],
            vertexData[offset + 1], 
            vertexData[offset + 2]
        );
        
        // Normal
        mesh->mNormals[i] = aiVector3D(
            vertexData[offset + 3],
            vertexData[offset + 4],
            vertexData[offset + 5]
        );
    }
    
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
        aiVector3D &normal = mesh->mNormals[i];
        
        // Transform vertex position
        aiVector3D transformedVertex(
            static_cast<float>(transform[0][0] * vertex.x + transform[1][0] * vertex.y + transform[2][0] * vertex.z + transform[3][0]),
            static_cast<float>(transform[0][1] * vertex.x + transform[1][1] * vertex.y + transform[2][1] * vertex.z + transform[3][1]),
            static_cast<float>(transform[0][2] * vertex.x + transform[1][2] * vertex.y + transform[2][2] * vertex.z + transform[3][2])
        );
        vertex = transformedVertex;
        
        // Transform normal (only rotation part)
        aiVector3D transformedNormal(
            static_cast<float>(transform[0][0] * normal.x + transform[1][0] * normal.y + transform[2][0] * normal.z),
            static_cast<float>(transform[0][1] * normal.x + transform[1][1] * normal.y + transform[2][1] * normal.z),
            static_cast<float>(transform[0][2] * normal.x + transform[1][2] * normal.y + transform[2][2] * normal.z)
        );
        normal = transformedNormal;
        normal.Normalize();
    }
    
    // Set material index (will be set properly in ExtractMaterials)
    mesh->mMaterialIndex = 0;
    
    return mesh;
}

void IFCImporter::ExtractMaterials(uint32_t modelID, aiScene *pScene) {
    std::vector<aiMaterial*> materials;
    
    // Create default material
    aiMaterial* defaultMat = CreateMaterialFromColor(aiColor4D(0.8f, 0.8f, 0.8f, 1.0f), "IFC_Default");
    materials.push_back(defaultMat);
    
    // TODO: Extract actual materials from IFC data using Web-IFC
    // For now, create a few basic materials for different IFC element types
    
    // Wall material
    aiMaterial* wallMat = CreateMaterialFromColor(aiColor4D(0.9f, 0.9f, 0.8f, 1.0f), "IFC_Wall");
    materials.push_back(wallMat);
    
    // Slab material  
    aiMaterial* slabMat = CreateMaterialFromColor(aiColor4D(0.7f, 0.7f, 0.7f, 1.0f), "IFC_Slab");
    materials.push_back(slabMat);
    
    // Column material
    aiMaterial* columnMat = CreateMaterialFromColor(aiColor4D(0.6f, 0.6f, 0.6f, 1.0f), "IFC_Column");
    materials.push_back(columnMat);
    
    // Set up materials in scene
    pScene->mNumMaterials = static_cast<unsigned int>(materials.size());
    pScene->mMaterials = new aiMaterial*[materials.size()];
    
    for (size_t i = 0; i < materials.size(); ++i) {
        pScene->mMaterials[i] = materials[i];
    }
    
    if (!DefaultLogger::isNullLogger()) {
        LogInfo("Created ", materials.size(), " materials for IFC scene");
    }
}

aiMaterial* IFCImporter::CreateMaterialFromColor(const aiColor4D &color, const std::string &name) {
    aiMaterial* material = new aiMaterial();
    
    aiString materialName(name);
    material->AddProperty(&materialName, AI_MATKEY_NAME);
    
    material->AddProperty(&color, 1, AI_MATKEY_COLOR_DIFFUSE);
    
    aiColor4D specular(0.2f, 0.2f, 0.2f, 1.0f);
    material->AddProperty(&specular, 1, AI_MATKEY_COLOR_SPECULAR);
    
    float shininess = 32.0f;
    material->AddProperty(&shininess, 1, AI_MATKEY_SHININESS);
    
    return material;
}

void IFCImporter::BuildSceneGraph(uint32_t modelID, aiScene *pScene) {
    // For now, create a simple flat hierarchy
    // TODO: Build proper IFC spatial hierarchy (Site -> Building -> Storey -> Elements)
    
    if (pScene->mNumMeshes > 0) {
        // Link all meshes to root node
        pScene->mRootNode->mNumMeshes = pScene->mNumMeshes;
        pScene->mRootNode->mMeshes = new unsigned int[pScene->mNumMeshes];
        
        for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
            pScene->mRootNode->mMeshes[i] = i;
        }
    }
    
    if (!DefaultLogger::isNullLogger()) {
        LogInfo("Built scene graph with ", pScene->mRootNode->mNumMeshes, " meshes");
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

#endif // !! ASSIMP_BUILD_NO_IFC_IMPORTER

