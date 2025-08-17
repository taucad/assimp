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
#include <mutex>

#include <cmath>
#include <cfloat>
#include <cassert>
#include <limits>

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
#include "web-ifc/schema/schema-names.h"
#include "web-ifc/schema/ifc-schema.h"

#pragma GCC diagnostic pop

namespace Assimp {
template <>
const char *LogFunctions<IFCImporter>::Prefix() {
    return "IFC: ";
}
} // namespace Assimp

using namespace Assimp;

// Named constants for better maintainability and readability
namespace IFCConstants {
    // Logging configuration
    constexpr int WEB_IFC_LOG_LEVEL_OFF = 6;  // spdlog::level::off = 6
    
    // Geometry settings
    constexpr int DEFAULT_CIRCLE_SEGMENTS = 32;
    constexpr size_t GEOMETRY_CONTAINER_INITIAL_CAPACITY = 512;
    
    // Default material colors
    constexpr float DEFAULT_MATERIAL_RED = 0.8f;
    constexpr float DEFAULT_MATERIAL_GREEN = 0.8f;
    constexpr float DEFAULT_MATERIAL_BLUE = 0.8f;
    constexpr float DEFAULT_MATERIAL_ALPHA = 1.0f;
}

// IFC Entity Argument Indices - Named constants for better maintainability
// Dynamic schema introspection using Web-IFC's schema APIs
// ------------------------------------------------------------------------------------------------
// SchemaArgumentCache implementation - moved to proper class design

int IFCImporter::SchemaArgumentCache::GetPropertyIndex(uint32_t elementType, const std::string& propertyName, 
                                                      webifc::parsing::IfcLoader* ifcLoader) {
            // Check cache first
            auto typeIt = typeToPropertyIndices.find(elementType);
            if (typeIt != typeToPropertyIndices.end()) {
                auto propIt = typeIt->second.find(propertyName);
                if (propIt != typeIt->second.end()) {
                    return propIt->second;
                }
            }
            
            // Cache miss - bulk load ALL properties for this element type for efficiency
            try {
                // Dynamically detect schema from the IFC file header
                IFC_SCHEMA schema = ifcLoader->GetSchema();
                
                uint32_t propertyCount = getPropertyCount(schema, elementType);
                
                // Bulk cache ALL properties for this element type in one pass
                // This avoids expensive repeated schema queries for the same element type
                auto& propertyMap = typeToPropertyIndices[elementType];
                propertyMap.clear(); // Clear any partial entries
                
                for (uint32_t i = 0; i < propertyCount; ++i) {
                    std::string propName = getPropertyName(schema, elementType, i);
                    if (!propName.empty()) {
                        propertyMap[propName] = static_cast<int>(i);
                    }
                }
                
                // Now look up the requested property in our freshly cached data
                auto propIt = propertyMap.find(propertyName);
                if (propIt != propertyMap.end()) {
                    return propIt->second;
                }
                
            } catch (const std::exception& e) {
                // Schema query failed - this is a serious error that should be addressed
                // Use efficient string building to avoid multiple temporary allocations
                std::string errorMsg;
                errorMsg.reserve(128);  // Reserve space for typical error message
                errorMsg = "Failed to query IFC schema for property '";
                errorMsg += propertyName;
                errorMsg += "' on element type ";
                errorMsg += std::to_string(elementType);
                errorMsg += ": ";
                errorMsg += e.what();
                throw std::runtime_error(errorMsg);
            }
            
            // Property not found in schema - this indicates a schema mismatch or invalid property name
            // Use efficient string building to avoid multiple temporary allocations
            std::string errorMsg;
            errorMsg.reserve(80);  // Reserve space for typical error message
            errorMsg = "Property '";
            errorMsg += propertyName;
            errorMsg += "' not found in IFC schema for element type ";
            errorMsg += std::to_string(elementType);
            throw std::runtime_error(errorMsg);
}

// ------------------------------------------------------------------------------------------------
IFCImporter::IFCImporter() : currentModelID(0) {
}

// ------------------------------------------------------------------------------------------------
IFCImporter::~IFCImporter() {
    std::lock_guard<std::recursive_mutex> lock(modelManagerMutex);
    if (modelManager) {
        if (currentModelID != 0) {
            // CleanupWebIFC will be called with the lock held
            if (modelManager->IsModelOpen(currentModelID)) {
                modelManager->CloseModel(currentModelID);
                LogDebug("Closed Web-IFC model ", currentModelID);
            }
        }
        // modelManager will be automatically destroyed by unique_ptr
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
    settings.circleSegments = IFCConstants::DEFAULT_CIRCLE_SEGMENTS;
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
    std::lock_guard<std::recursive_mutex> lock(modelManagerMutex);
    if (!modelManager) {
        modelManager = std::make_unique<webifc::manager::ModelManager>(false);
        
        // Suppress verbose web-ifc logging to avoid cluttering test output
        // Set to level 6 (off) to suppress all web-ifc logs including:
        // - "web-ifc: X.X.X threading: disabled schemas available [...]" 
        // - "[TriangulateBounds()] No basis found for brep!" errors
        modelManager->SetLogLevel(IFCConstants::WEB_IFC_LOG_LEVEL_OFF);
        
        LogDebug("Web-IFC model manager initialized with logging suppressed");
    }
}

void IFCImporter::LoadModelWithWebIFC(const std::string &pFile, aiScene *pScene, IOSystem *pIOHandler) {
    LogInfo("Loading IFC file with Web-IFC: ", pFile);

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
    
    // Lock modelManager for the entire loading operation
    std::lock_guard<std::recursive_mutex> lock(modelManagerMutex);
    
    // Create model and get model ID
    currentModelID = modelManager->CreateModel(loaderSettings);

    LogDebug("Created Web-IFC model with ID: ", currentModelID);

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

        LogDebug("IFC file loaded into Web-IFC");

        // Create scene structure
        pScene->mRootNode = new aiNode("IFC_Scene");
        
        // Build spatial containment map for correct mesh assignment to storeys
        elementToStoreyMap = PopulateSpatialContainmentMap(ifcLoader);
        
        // Extract geometry and materials from Web-IFC
        ExtractMaterials(currentModelID, pScene);
        ExtractGeometry(currentModelID, pScene);
        BuildSceneGraph(currentModelID, pScene);
        
        LogInfo("IFC file loaded successfully with Web-IFC - ", 
               pScene->mNumMeshes, " meshes, ", 
               pScene->mNumMaterials, " materials");

    } catch (const std::exception &e) {
        // Cleanup is already protected by the lock in this scope
                    if (modelManager && modelManager->IsModelOpen(currentModelID)) {
                modelManager->CloseModel(currentModelID);
                LogDebug("Closed Web-IFC model ", currentModelID, " due to exception");
            }
        currentModelID = 0;
        throw;
    }
}

// ------------------------------------------------------------------------------------------------
// ExtractGeometry helper methods for better maintainability and code organization

std::vector<uint32_t> IFCImporter::FilterGeometricElementTypes(const std::unordered_set<uint32_t>& allElementTypes) {
    // Pre-compute geometric element types (exclude non-geometric types)
    std::vector<uint32_t> geometricElementTypes;
    geometricElementTypes.reserve(allElementTypes.size());
    
    for (auto elementType : allElementTypes) {
        // Skip non-geometric types based on configurable settings for better performance and flexibility
        bool shouldSkip = false;
        
        if (settings.skipOpeningElements && elementType == webifc::schema::IFCOPENINGELEMENT) {
            shouldSkip = true;
        } else if (settings.skipSpaceGeometry && elementType == webifc::schema::IFCSPACE) {
            shouldSkip = true;
        } else if (settings.skipOpeningStandardCase && elementType == webifc::schema::IFCOPENINGSTANDARDCASE) {
            shouldSkip = true;
        }
        
        if (!shouldSkip) {
            geometricElementTypes.push_back(elementType);
        }
    }
    
    return geometricElementTypes;
}

std::vector<std::pair<uint32_t, webifc::geometry::IfcFlatMesh>> IFCImporter::CollectFlatMeshes(
    webifc::parsing::IfcLoader* loader,
    const std::vector<uint32_t>& geometricElementTypes) {
    
    // Get geometry processor
    auto geomProcessor = modelManager->GetGeometryProcessor(currentModelID);
    
    // Optimized bulk geometry loading approach
    std::vector<std::pair<uint32_t, webifc::geometry::IfcFlatMesh>> flatMeshesWithGeometry;
    flatMeshesWithGeometry.reserve(IFCConstants::GEOMETRY_CONTAINER_INITIAL_CAPACITY);
    
    // Batch process geometric elements by type for better cache locality
    for (uint32_t elementType : geometricElementTypes) {
        auto elements = loader->GetExpressIDsWithType(elementType);
        
        // Process all elements of this type in batch for better performance
        for (uint32_t i = 0; i < elements.size(); ++i) {
            uint32_t expressID = elements[i];
            try {
                auto flatMesh = geomProcessor->GetFlatMesh(expressID);
                if (!flatMesh.geometries.empty()) {
                    // Batch vertex data loading for this mesh
                    for (auto &geom : flatMesh.geometries) {
                        auto &ifcGeom = geomProcessor->GetGeometry(geom.geometryExpressID);
                        ifcGeom.GetVertexData(); // Ensure geometry data is loaded
                    }
                    flatMeshesWithGeometry.emplace_back(expressID, std::move(flatMesh));
                }
            } catch (const std::exception& e) {
                // Skip elements without geometry but log for debugging
                LogVerboseDebug("IFC: Element ", expressID, " has no geometry: ", e.what());
                continue;
            }
        }
    }
    
    return flatMeshesWithGeometry;
}

void IFCImporter::ProcessMeshesFromFlatMeshes(
    const std::vector<std::pair<uint32_t, webifc::geometry::IfcFlatMesh>>& flatMeshesWithGeometry,
    const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& relMaterials,
    webifc::parsing::IfcLoader* loader,
    std::vector<aiMesh*>& meshes,
    std::unordered_map<std::string, unsigned int>& colorMaterialCache,
    bool& needsDefaultMaterial,
    aiScene* pScene) {
    
    // Process each flat mesh to create Assimp meshes
    for (auto& [expressID, flatMesh] : flatMeshesWithGeometry) {
        try {
            // Create individual mesh for this expressID (like reference implementation)
            auto assimpMesh = CreateMeshFromFlatMesh(expressID, flatMesh, relMaterials, colorMaterialCache, pScene);
            if (assimpMesh) {
                // Check if this mesh needs to be split by materials
                std::string meshName = assimpMesh->mName.C_Str();
                if (meshName == "IFC_MultiMaterial_Element") {
                    // This is a multi-material mesh - split it
                    
                    // Use RAII pattern for exception safety: create replacement before deleting original
                    std::unique_ptr<aiMesh> originalMesh(assimpMesh); // Take ownership
                    assimpMesh = nullptr; // Clear original pointer
                    
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
                        // Use semantic fallback name - ExpressID stored in metadata
                        assimpMesh->mName = aiString("IFC_Element");
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
            LogWarn("IFC: Failed to extract geometry for element ", expressID, ": ", e.what());
        }
    }
}

void IFCImporter::SetupSceneMeshes(std::vector<aiMesh*>& meshes, bool needsDefaultMaterial, aiScene* pScene) {
    // Only create default material if there are meshes that need it
    if (needsDefaultMaterial) {
        aiMaterial* defaultMat = CreateMaterialFromColor(aiColor4D(IFCConstants::DEFAULT_MATERIAL_RED, IFCConstants::DEFAULT_MATERIAL_GREEN, 
                     IFCConstants::DEFAULT_MATERIAL_BLUE, IFCConstants::DEFAULT_MATERIAL_ALPHA), "IFC_Default");
        
        // Insert at index 0 and update all existing material indices
        std::vector<aiMaterial*> newMaterials;
        newMaterials.push_back(defaultMat);
        for (unsigned int i = 0; i < pScene->mNumMaterials; ++i) {
            newMaterials.push_back(pScene->mMaterials[i]);
        }
        
        // Update scene materials using exception-safe RAII pattern
        std::unique_ptr<aiMaterial*[]> newMaterialArray(new aiMaterial*[newMaterials.size()]);
        for (size_t i = 0; i < newMaterials.size(); ++i) {
            newMaterialArray[i] = newMaterials[i];
        }
        
        // Now safely replace (no exceptions can occur here)
        delete[] pScene->mMaterials;
        pScene->mNumMaterials = static_cast<unsigned int>(newMaterials.size());
        pScene->mMaterials = newMaterialArray.release();
        
        // Update all non-zero material indices in meshes (shift by 1)
        for (auto* mesh : meshes) {
            if (mesh->mMaterialIndex > 0) {
                mesh->mMaterialIndex++;
            }
        }
    }

    // Set up meshes in scene using exception-safe pattern
    if (!meshes.empty()) {
        std::unique_ptr<aiMesh*[]> newMeshArray(new aiMesh*[meshes.size()]);
        for (size_t i = 0; i < meshes.size(); ++i) {
            newMeshArray[i] = meshes[i];
        }
        
        pScene->mNumMeshes = static_cast<unsigned int>(meshes.size());
        pScene->mMeshes = newMeshArray.release();
    }
}

void IFCImporter::ExtractGeometry(uint32_t modelID, aiScene *pScene) {
    std::lock_guard<std::recursive_mutex> lock(modelManagerMutex);
    
    // Thread safety verification: Ensure Web-IFC operations are protected
    assert(modelManager && "Thread Safety Violation: modelManager accessed without proper initialization");
    
    if (!modelManager || !modelManager->IsModelOpen(modelID)) {
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
        
        // 1. Filter geometric element types
        auto schemaManager = modelManager->GetSchemaManager();
        const auto& allElementTypes = schemaManager.GetIfcElementList();
        auto geometricElementTypes = FilterGeometricElementTypes(allElementTypes);
        
        // 2. Collect flat meshes from geometry processor
        auto flatMeshesWithGeometry = CollectFlatMeshes(loader, geometricElementTypes);
        
        // 3. Process meshes and handle material assignments
        std::unordered_map<std::string, unsigned int> colorMaterialCache;
        bool needsDefaultMaterial = false;
        ProcessMeshesFromFlatMeshes(flatMeshesWithGeometry, relMaterials, loader, 
                                  meshes, colorMaterialCache, needsDefaultMaterial, pScene);
        
        // 4. Setup scene with processed meshes
        SetupSceneMeshes(meshes, needsDefaultMaterial, pScene);
        
        LogInfo("Extracted ", meshes.size(), " meshes from IFC file");

    } catch (const std::exception &e) {
        LogError("IFC: Failed to extract geometry from Web-IFC: ", e.what());
        
        // Clean up partial results
        for (auto* mesh : meshes) {
            delete mesh;
        }
    }
}

aiMesh* IFCImporter::ConvertWebIFCMesh(const webifc::geometry::IfcFlatMesh &flatMesh, uint32_t geometryIndex) {
    LogVerboseDebug("ConvertWebIFCMesh: Starting conversion for geometry index: ", geometryIndex);

    if (geometryIndex >= flatMesh.geometries.size()) {
        LogVerboseDebug("ConvertWebIFCMesh: Invalid geometry index ", geometryIndex, " >= ", flatMesh.geometries.size());
        return nullptr;
    }

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
    
    // Get the actual geometry data - optimized logging to reduce overhead
    auto geomProcessor = modelManager->GetGeometryProcessor(currentModelID);
    auto &ifcGeom = geomProcessor->GetGeometry(placedGeom.geometryExpressID);
    
    // Access the underlying data vectors directly
    const auto& vertexDataVector = ifcGeom.fvertexData;
    const auto& indexDataVector = ifcGeom.indexData;
    
    LogVerboseDebug("ConvertWebIFCMesh: Processing geometry ID ", placedGeom.geometryExpressID, " with ", vertexDataVector.size(), " vertex elements and ", indexDataVector.size(), " indices");
    
    if (vertexDataVector.empty() || indexDataVector.empty()) {
        LogVerboseDebug("ConvertWebIFCMesh: Empty data vectors - returning nullptr");
        return nullptr;
    }
    // Get pointers to the actual data
    const float* vertexData = vertexDataVector.data();
    const uint32_t* indexData = indexDataVector.data();
    
    // Create Assimp mesh with RAII protection
    std::unique_ptr<aiMesh> mesh(new aiMesh());
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    
    // Use Web-IFC's official vertex format constant
    constexpr size_t VERTEX_FORMAT_SIZE = webifc::geometry::VERTEX_FORMAT_SIZE_FLOATS;
    size_t numVertices = vertexDataVector.size() / VERTEX_FORMAT_SIZE;
    size_t numFaces = indexDataVector.size() / 3;
    
    if (numVertices == 0 || numFaces == 0) {
        return nullptr;
    }

    // Set up vertices
    mesh->mNumVertices = static_cast<unsigned int>(numVertices);
    std::unique_ptr<aiVector3D[]> vertices(new aiVector3D[numVertices]);
    mesh->mVertices = vertices.release();
    // Note: Normals computation disabled. Enable?
    // mesh->mNormals = new aiVector3D[numVertices];
    
    // Allocate texture coordinates (Web-IFC doesn't provide UVs yet, so we'll generate basic planar mapping)
    std::unique_ptr<aiVector3D[]> texCoords(new aiVector3D[numVertices]);
    mesh->mTextureCoords[0] = texCoords.release();
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
    GenerateTextureCoordinates(mesh.get(), minBounds, maxBounds);
    
    // Set up faces
    mesh->mNumFaces = static_cast<unsigned int>(numFaces);
    std::unique_ptr<aiFace[]> faces(new aiFace[numFaces]);
    mesh->mFaces = faces.release();
    
    for (size_t i = 0; i < numFaces; ++i) {
        mesh->mFaces[i].mNumIndices = 3;
        std::unique_ptr<unsigned int[]> faceIndices(new unsigned int[3]);
        faceIndices[0] = indexData[i * 3 + 0];
        faceIndices[1] = indexData[i * 3 + 1];
        faceIndices[2] = indexData[i * 3 + 2];
        mesh->mFaces[i].mIndices = faceIndices.release();
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
    
    return mesh.release();
}

void IFCImporter::ExtractMaterials(uint32_t modelID, aiScene *pScene) {
    std::lock_guard<std::recursive_mutex> lock(modelManagerMutex);
    
    // Thread safety verification: Ensure Web-IFC operations are protected
    assert(modelManager && "Thread Safety Violation: modelManager accessed without proper initialization");
    
    if (!modelManager) {
        return;
    }
    
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
        
        LogInfo("Extracted ", materials.size(), " IFC materials");
        
    } catch (const std::exception &e) {
        LogWarn("IFC: Failed to extract materials: ", e.what());
        
        // Fallback to default material only
        if (materials.empty()) {
            aiMaterial* defaultMat = CreateMaterialFromColor(aiColor4D(IFCConstants::DEFAULT_MATERIAL_RED, IFCConstants::DEFAULT_MATERIAL_GREEN, 
                         IFCConstants::DEFAULT_MATERIAL_BLUE, IFCConstants::DEFAULT_MATERIAL_ALPHA), "IFC_Default");
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
    std::unique_ptr<aiMaterial> material(new aiMaterial());
    
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
    
    return material.release();
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
                    
                    LogVerboseDebug("Extracted IFC material: ", materialID, " -> index ", materialIndex);
                }
            } catch (const std::exception& e) {
                LogWarn("IFC: Failed to extract material ", materialID, ": ", e.what());
            }
        }
        
        // Process styled items for visual representations
        ProcessStyledItems(ifcLoader, styledItems, materials, materialIDToIndex);
        
    } catch (const std::exception& e) {
        LogWarn("IFC: Failed to access Web-IFC material APIs: ", e.what());
    }
}

aiMaterial* IFCImporter::ExtractSingleIFCMaterial(
    webifc::parsing::IfcLoader* ifcLoader,
    uint32_t materialID,
    const std::vector<std::pair<uint32_t, uint32_t>>& definitions) {
    
    auto material = std::make_unique<aiMaterial>();
    
    try {
        // Extract material name (typically first argument) - use semantic naming
        std::string materialName = "IFC_Material";
        try {
            ifcLoader->MoveToArgumentOffset(materialID, 0);
            if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::STRING) {
                ifcLoader->MoveToArgumentOffset(materialID, 0);
                std::string extractedName = ifcLoader->GetDecodedStringArgument();
                if (!extractedName.empty()) {
                    materialName = extractedName; // GetDecodedStringArgument already handles IFC decoding
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
        LogWarn("IFC: Failed to extract material properties for ", materialID, ": ", e.what());
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
            } else if (defType == webifc::schema::IFCMATERIALLAYER) {
                ExtractMaterialLayerProperties(ifcLoader, defID, material);
            }
            
        } catch (const std::exception& e) {
                            LogDebug("Failed to extract property ", defID, ": ", e.what());
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
        LogDebug("Failed to extract RGB color: ", e.what());
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
        LogDebug("Failed to extract rendering properties: ", e.what());
    }
}

void IFCImporter::ExtractMaterialLayerProperties(
    webifc::parsing::IfcLoader* ifcLoader,
    uint32_t layerID,
    aiMaterial* material) {
    
    try {
        // IFCMATERIALLAYER structure:
        // Material: IfcMaterial (argument 0) - reference to the material definition
        // LayerThickness: IfcPositiveLengthMeasure (argument 1) - thickness of the layer
        // IsVentilated: IfcLogical (argument 2) - whether the layer is ventilated
        // Name: IfcLabel (argument 3) - optional name for the layer
        // Description: IfcText (argument 4) - optional description
        // Category: IfcLabel (argument 5) - optional category
        // Priority: IfcInteger (argument 6) - optional priority
        
        // Extract material reference if available
        ifcLoader->MoveToArgumentOffset(layerID, 0);
        if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::REF) {
            uint32_t materialRef = ifcLoader->GetRefArgument();
            // Recursively extract properties from the referenced material
            try {
                uint32_t materialType = ifcLoader->GetLineType(materialRef);
                if (materialType == webifc::schema::IFCMATERIAL) {
                    // Extract name from the referenced material
                    ifcLoader->MoveToArgumentOffset(materialRef, 0);
                    if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::STRING) {
                        std::string materialName = ifcLoader->GetDecodedStringArgument();
                        if (!materialName.empty()) {
                            aiString assimpMaterialName(materialName + "_Layer");
                            material->AddProperty(&assimpMaterialName, AI_MATKEY_NAME);
                        }
                    }
                }
            } catch (...) {
                // Material reference extraction failed, continue with other properties
            }
        }
        
        // Extract layer thickness and store as custom property
        try {
            ifcLoader->MoveToArgumentOffset(layerID, 1);
            if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::REAL) {
                float thickness = static_cast<float>(ifcLoader->GetDoubleArgument());
                material->AddProperty(&thickness, 1, "IFC.LayerThickness");
            }
        } catch (...) {
            // Thickness extraction failed
        }
        
        // Extract layer name if available
        try {
            ifcLoader->MoveToArgumentOffset(layerID, 3);
            if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::STRING) {
                std::string layerName = ifcLoader->GetDecodedStringArgument();
                if (!layerName.empty()) {
                    aiString assimpLayerName(layerName);
                    material->AddProperty(&assimpLayerName, "IFC.LayerName");
                }
            }
        } catch (...) {
            // Layer name extraction failed
        }
        
        // Extract layer description if available
        try {
            ifcLoader->MoveToArgumentOffset(layerID, 4);
            if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::STRING) {
                std::string description = ifcLoader->GetDecodedStringArgument();
                if (!description.empty()) {
                    aiString assimpDescription(description);
                    material->AddProperty(&assimpDescription, "IFC.LayerDescription");
                }
            }
        } catch (...) {
            // Description extraction failed
        }
        
    } catch (const std::exception& e) {
        LogDebug("Failed to extract material layer properties: ", e.what());
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
                LogDebug("Failed to process styled item ", itemID, ": ", e.what());
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
        
        // Extract style name - use semantic naming
        std::string styleName = "IFC_SurfaceStyle";
        try {
            ifcLoader->MoveToArgumentOffset(styleID, 0);
            if (ifcLoader->GetTokenType() == webifc::parsing::IfcTokenType::STRING) {
                ifcLoader->MoveToArgumentOffset(styleID, 0);
                std::string extractedName = ifcLoader->GetDecodedStringArgument();
                if (!extractedName.empty()) {
                    styleName = extractedName; // GetDecodedStringArgument already handles IFC decoding
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
        
        LogVerboseDebug("Processed surface style: ", styleID, " -> index ", materialIndex);
        
    } catch (const std::exception& e) {
        LogWarn("IFC: Failed to process surface style ", styleID, ": ", e.what());
    }
}

// SetMeshMaterialFromIFC removed - was dead code with O(n) performance issue

void IFCImporter::BuildSceneGraph(uint32_t modelID, aiScene *pScene) {
    std::lock_guard<std::recursive_mutex> lock(modelManagerMutex);
    
    // Thread safety verification: Ensure Web-IFC operations are protected
    assert(modelManager && "Thread Safety Violation: modelManager accessed without proper initialization");
    
    if (!modelManager) {
        return;
    }
    
    try {
        auto ifcLoader = modelManager->GetIfcLoader(modelID);
        
        // Build proper IFC spatial hierarchy (Project -> Site -> Building -> Storey -> Space -> Elements)
        BuildIFCSpatialHierarchy(ifcLoader, pScene);
        
    } catch (const std::exception &e) {
        LogWarn("IFC: Failed to build spatial hierarchy: ", e.what(), ", falling back to flat hierarchy");
        
        // Fallback: create a simple flat hierarchy
        if (pScene->mNumMeshes > 0) {
            // Link all meshes to root node using exception-safe pattern
            std::unique_ptr<unsigned int[]> meshIndices(new unsigned int[pScene->mNumMeshes]);
            for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
                meshIndices[i] = i;
            }
            
            pScene->mRootNode->mNumMeshes = pScene->mNumMeshes;
            pScene->mRootNode->mMeshes = meshIndices.release();
        }
    }
    
    LogInfo("Built scene graph with ", CountNodesInHierarchy(pScene->mRootNode), " nodes");
}

void IFCImporter::CleanupWebIFC(uint32_t modelID) {
    std::lock_guard<std::recursive_mutex> lock(modelManagerMutex);
    if (modelManager && modelManager->IsModelOpen(modelID)) {
        modelManager->CloseModel(modelID);
        LogDebug("Closed Web-IFC model ", modelID);
    }
}



std::string IFCImporter::GetIFCElementName(webifc::parsing::IfcLoader* ifcLoader, uint32_t expressID) {
    try {
        // Extract the Name attribute (argument 2) from IFC elements
        // IFC structure: GlobalId, OwnerHistory, Name, Description, ...
        ifcLoader->MoveToArgumentOffset(expressID, 2);
        
        std::string decodedName = ifcLoader->GetDecodedStringArgument();
        
        // Only return non-empty, meaningful names
        if (!decodedName.empty() && decodedName != "$" && decodedName != "''") {
            return decodedName;
        }
        
        // If Name is empty/null, try alternative approaches for specific element types
        uint32_t elementType = ifcLoader->GetLineType(expressID);
        
        // For some elements, the Tag field might contain meaningful names
        // Use dynamic schema detection to find the Tag property index
        try {
            int tagArgument = schemaCache.GetPropertyIndex(elementType, "Tag", ifcLoader);
            if (tagArgument >= 0) {
                ifcLoader->MoveToArgumentOffset(expressID, tagArgument);
                
                std::string decodedTag = ifcLoader->GetDecodedStringArgument();
                
                // Return tag if it looks like a meaningful name (not a GUID)
                if (!decodedTag.empty() && decodedTag != "$" && decodedTag != "''" &&
                    decodedTag.find('-') != std::string::npos && decodedTag.length() < 20) {
                    return decodedTag;
                }
            }
        } catch (const std::exception& e) {
            // Tag property not found in schema for this element type - this is expected for many elements
            LogDebug("IFC: Tag property not available for element type ", elementType, ": ", e.what());
        }
        
    } catch (const std::exception& e) {
        LogDebug("IFC: Failed to extract name for element ", expressID, ": ", e.what());
    }
    
    // Return empty string to indicate fallback to expressID should be used
    return "";
}

std::string IFCImporter::GetSemanticMaterialName(aiScene* pScene, unsigned int materialIndex) {
    if (!pScene || !pScene->mMaterials || materialIndex >= pScene->mNumMaterials) {
        // Fallback to generic naming if material not available
        std::string fallbackName;
        fallbackName.reserve(16); // "Material_XX"
        fallbackName = "Material_";
        fallbackName += std::to_string(materialIndex);
        return fallbackName;
    }
    
    aiMaterial* material = pScene->mMaterials[materialIndex];
    if (material) {
        aiString matName;
        if (material->Get(AI_MATKEY_NAME, matName) == aiReturn_SUCCESS) {
            std::string semanticName = matName.C_Str();
            // Use semantic name with Material_ prefix for backward compatibility
            if (!semanticName.empty() && semanticName != "DefaultMaterial") {
                std::string result;
                result.reserve(9 + semanticName.size()); // "Material_" + semantic name
                result = "Material_";
                result += semanticName;
                return result;
            }
        }
    }
    
    // Fallback to index-based naming if no material name available
    std::string fallbackName;
    fallbackName.reserve(16); // "Material_XX"
    fallbackName = "Material_";
    fallbackName += std::to_string(materialIndex);
    return fallbackName;
}

std::unordered_map<uint32_t, uint32_t> IFCImporter::PopulateSpatialContainmentMap(webifc::parsing::IfcLoader* ifcLoader) {
    std::unordered_map<uint32_t, uint32_t> elementToStorey;
    
    try {
        // Use Web-IFC's efficient API to get all spatial containment relationships
        auto spatialContainments = ifcLoader->GetExpressIDsWithType(webifc::schema::IFCRELCONTAINEDINSPATIALSTRUCTURE);
        
        LogDebug("IFC: Found ", spatialContainments.size(), " spatial containment relationships");
        
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
                
                LogDebug("IFC: Spatial containment - storey ", relatingStructure, " contains ", relatedElements.size(), " elements");
                
            } catch (const std::exception& e) {
                LogWarn("IFC: Failed to process spatial containment relationship ", relationshipID, ": ", e.what());
            }
        }
        
        LogInfo("IFC: Built spatial containment map with ", elementToStorey.size(), " element-to-storey mappings");
        
    } catch (const std::exception& e) {
        LogError("IFC: Failed to populate spatial containment map: ", e.what());
    }
    
    return elementToStorey;
}

std::vector<IFCImporter::StoreyInfo> IFCImporter::GetSortedStoreysByElevation(webifc::parsing::IfcLoader* ifcLoader) {
    std::vector<StoreyInfo> storeys;
    
    try {
        // Get all building storey entities using Web-IFC's efficient API
        auto buildingStoreys = ifcLoader->GetExpressIDsWithType(webifc::schema::IFCBUILDINGSTOREY);
        
        // Cache property indices for IFCBUILDINGSTOREY to avoid repeated lookups (O(n) -> O(1))
        int nameIndex = -1;
        int elevationIndex = -1;
        
        try {
            nameIndex = schemaCache.GetPropertyIndex(webifc::schema::IFCBUILDINGSTOREY, "Name", ifcLoader);
        } catch (const std::exception& e) {
            LogWarn("IFC: Name property not available for IFCBUILDINGSTOREY: ", e.what());
        }
        
        try {
            elevationIndex = schemaCache.GetPropertyIndex(webifc::schema::IFCBUILDINGSTOREY, "Elevation", ifcLoader);
        } catch (const std::exception& e) {
            LogWarn("IFC: Elevation property not available for IFCBUILDINGSTOREY: ", e.what());
        }
        
        for (uint32_t storeyID : buildingStoreys) {
            try {
                StoreyInfo storeyInfo;
                storeyInfo.expressID = storeyID;
                
                // Extract storey name using cached index
                if (nameIndex >= 0) {
                    try {
                        ifcLoader->MoveToArgumentOffset(storeyID, nameIndex);
                        storeyInfo.name = ifcLoader->GetDecodedStringArgument();
                    } catch (...) {
                        storeyInfo.name = "Unknown Storey";
                    }
                } else {
                    storeyInfo.name = "Unknown Storey";
                }
                
                // Extract storey elevation using cached index
                if (elevationIndex >= 0) {
                    try {
                        ifcLoader->MoveToArgumentOffset(storeyID, elevationIndex);
                        storeyInfo.elevation = ifcLoader->GetDoubleArgument();
                    } catch (...) {
                        storeyInfo.elevation = 0.0;
                    }
                } else {
                    storeyInfo.elevation = 0.0;
                }
                
                storeys.push_back(storeyInfo);
                
                LogDebug("IFC: Found storey '", storeyInfo.name, "' at elevation ", storeyInfo.elevation);
                
            } catch (const std::exception& e) {
                LogWarn("IFC: Failed to extract elevation for building storey ", storeyID, ": ", e.what());
            }
        }
        
        // Sort storeys by elevation (lowest first - ground floor before upper floors)
        std::sort(storeys.begin(), storeys.end(), 
            [](const StoreyInfo& a, const StoreyInfo& b) {
                return a.elevation < b.elevation;
            });
        
        LogInfo("IFC: Sorted ", storeys.size(), " building storeys by elevation");
        for (const auto& storey : storeys) {
            LogDebug("IFC: Storey '", storey.name, "' at elevation ", storey.elevation);
        }
        
    } catch (const std::exception& e) {
        LogError("IFC: Failed to get sorted storeys by elevation: ", e.what());
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
    
    LogVerboseDebug("Generated texture coordinates for mesh with ", mesh->mNumVertices, " vertices");
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

// ------------------------------------------------------------------------------------------------
// CreateMeshFromFlatMesh helper methods for better maintainability and code organization

IFCImporter::GeometryData IFCImporter::CollectGeometryData(
    uint32_t expressID,
    const webifc::geometry::IfcFlatMesh& flatMesh,
    const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& relMaterials,
    std::unordered_map<std::string, unsigned int>& colorMaterialCache,
    aiScene* pScene) {
    
    GeometryData geomData;
    auto geomProcessor = modelManager->GetGeometryProcessor(currentModelID);
    
    // Process each placed geometry in the flat mesh
    for (const auto& placedGeom : flatMesh.geometries) {
        auto& ifcGeom = geomProcessor->GetGeometry(placedGeom.geometryExpressID);
        const auto& vertexDataVector = ifcGeom.fvertexData;
        const auto& indexDataVector = ifcGeom.indexData;
        
        if (vertexDataVector.empty() || indexDataVector.empty()) {
            continue;
        }
        
        // Use Web-IFC's official vertex format constant
        constexpr size_t VERTEX_FORMAT_SIZE = webifc::geometry::VERTEX_FORMAT_SIZE_FLOATS;
        size_t numVertices = vertexDataVector.size() / VERTEX_FORMAT_SIZE;
        size_t vertexOffset = geomData.vertices.size();
        
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
            geomData.vertices.emplace_back(
                static_cast<float>(transformedPos.x),
                static_cast<float>(transformedPos.y),
                static_cast<float>(transformedPos.z)
            );
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
            std::unique_ptr<unsigned int[]> faceIndices(new unsigned int[3]);
            faceIndices[0] = static_cast<unsigned int>(vertexOffset + indexDataVector[i + 0]);
            faceIndices[1] = static_cast<unsigned int>(vertexOffset + indexDataVector[i + 1]);
            faceIndices[2] = static_cast<unsigned int>(vertexOffset + indexDataVector[i + 2]);
            face.mIndices = faceIndices.release();
            geomData.faces.push_back(face);
            geomData.materialIndices.push_back(materialIndex);
        }
    }
    
    return geomData;
}

void IFCImporter::SetupMeshDataStructures(aiMesh* mesh, const GeometryData& geomData) {
    // Set up mesh data
    mesh->mNumVertices = static_cast<unsigned int>(geomData.vertices.size());
    std::unique_ptr<aiVector3D[]> meshVertices(new aiVector3D[geomData.vertices.size()]);
    mesh->mVertices = meshVertices.release();
    
    for (size_t i = 0; i < geomData.vertices.size(); ++i) {
        mesh->mVertices[i] = geomData.vertices[i];
    }
    
    mesh->mNumFaces = static_cast<unsigned int>(geomData.faces.size());
    std::unique_ptr<aiFace[]> meshFaces(new aiFace[geomData.faces.size()]);
    mesh->mFaces = meshFaces.release();
    for (size_t i = 0; i < geomData.faces.size(); ++i) {
        mesh->mFaces[i] = geomData.faces[i];
    }
}

void IFCImporter::HandleMeshMaterials(aiMesh* mesh, const std::vector<unsigned int>& materialIndices) {
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
        mesh->mName = aiString("IFC_MultiMaterial_Element"); // Semantic name - ExpressID in metadata
        
        // We'll handle the splitting in the calling function
    }
}

void IFCImporter::GenerateTextureCoordinatesForMesh(
    aiMesh* mesh, 
    const aiVector3D* preCalcMinBounds,
    const aiVector3D* preCalcMaxBounds) {
    
    if (!mesh || mesh->mNumVertices == 0) {
        return;
    }
    
    aiVector3D minBounds, maxBounds;
    
    // Use pre-calculated bounds if available (performance optimization)
    if (preCalcMinBounds && preCalcMaxBounds) {
        minBounds = *preCalcMinBounds;
        maxBounds = *preCalcMaxBounds;
    } else {
        // Calculate bounds if not provided
        minBounds = aiVector3D(FLT_MAX, FLT_MAX, FLT_MAX);
        maxBounds = aiVector3D(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            minBounds.x = std::min(minBounds.x, mesh->mVertices[i].x);
            minBounds.y = std::min(minBounds.y, mesh->mVertices[i].y);
            minBounds.z = std::min(minBounds.z, mesh->mVertices[i].z);
            maxBounds.x = std::max(maxBounds.x, mesh->mVertices[i].x);
            maxBounds.y = std::max(maxBounds.y, mesh->mVertices[i].y);
            maxBounds.z = std::max(maxBounds.z, mesh->mVertices[i].z);
        }
    }
    
    // Allocate and generate texture coordinates
    std::unique_ptr<aiVector3D[]> meshTexCoords(new aiVector3D[mesh->mNumVertices]);
    mesh->mTextureCoords[0] = meshTexCoords.release();
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

aiMesh* IFCImporter::CreateMeshFromFlatMesh(
    uint32_t expressID,
    const webifc::geometry::IfcFlatMesh& flatMesh,
    const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& relMaterials,
    std::unordered_map<std::string, unsigned int>& colorMaterialCache,
    aiScene* pScene,
    const aiVector3D* preCalcMinBounds,
    const aiVector3D* preCalcMaxBounds) {
    
    if (flatMesh.geometries.empty()) {
        return nullptr;
    }
    
    std::unique_ptr<aiMesh> mesh(new aiMesh());
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    
    try {
        // 1. Collect geometry data (vertices, faces, materials) from flat mesh
        GeometryData geomData = CollectGeometryData(expressID, flatMesh, relMaterials, colorMaterialCache, pScene);
        
        if (geomData.vertices.empty() || geomData.faces.empty()) {
            return nullptr;
        }
        
        // 2. Set up mesh data structures
        SetupMeshDataStructures(mesh.get(), geomData);
        
        // 3. Handle material assignments (single vs multi-material)
        HandleMeshMaterials(mesh.get(), geomData.materialIndices);
        
        // 4. Generate texture coordinates
        GenerateTextureCoordinatesForMesh(mesh.get(), preCalcMinBounds, preCalcMaxBounds);
        
        return mesh.release();
        
    } catch (const std::exception& e) {
        LogWarn("IFC: Failed to create mesh from flat mesh: ", e.what());
        
        // mesh will be automatically cleaned up by unique_ptr
        return nullptr;
    }
}

unsigned int IFCImporter::GetOrCreateColorMaterial(
    const aiColor4D& color,
    std::unordered_map<std::string, unsigned int>& colorMaterialCache,
    aiScene* pScene) {
    
    // Create hex color string (e.g., "8C8D7EFF") using optimized single-pass approach
    // Pre-computed hex lookup table for better performance
    static const char hexChars[] = "0123456789ABCDEF";
    
    // Build hex string in single pass without temporary string allocations
    std::string colorKey;
    colorKey.reserve(8);
    
    // Convert each color component to 2-digit hex efficiently
    auto appendHex = [&colorKey](float value) {
        int intValue = static_cast<int>(std::round(std::min(std::max(value * 255.0f, 0.0f), 255.0f)));
        colorKey += hexChars[intValue >> 4];
        colorKey += hexChars[intValue & 15];
    };
    
    appendHex(color.r);
    appendHex(color.g);
    appendHex(color.b);
    appendHex(color.a);
    
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
    
    // Add to scene materials using exception-safe RAII pattern
    std::vector<aiMaterial*> newMaterials(pScene->mNumMaterials + 1);
    for (unsigned int i = 0; i < pScene->mNumMaterials; ++i) {
        newMaterials[i] = pScene->mMaterials[i];
    }
    newMaterials[pScene->mNumMaterials] = material;
    
    // Exception-safe allocation: allocate new array before deleting old one
    std::unique_ptr<aiMaterial*[]> newMaterialArray(new aiMaterial*[newMaterials.size()]);
    for (size_t i = 0; i < newMaterials.size(); ++i) {
        newMaterialArray[i] = newMaterials[i];
    }
    
    // Now safely replace the old array
    delete[] pScene->mMaterials;
    pScene->mMaterials = newMaterialArray.release();
    
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
    const std::vector<unsigned int>& materialIndices,
    const aiVector3D& parentMinBounds,
    const aiVector3D& parentMaxBounds,
    aiScene* pScene) {
    
    std::vector<aiMesh*> splitMeshes;
    
    // Group faces by material
    std::unordered_map<unsigned int, std::vector<size_t>> materialToFaceIndices;
    for (size_t i = 0; i < materialIndices.size(); ++i) {
        materialToFaceIndices[materialIndices[i]].push_back(i);
    }
    

    
    // Create a sub-mesh for each material
    for (const auto& [materialIndex, faceIndices] : materialToFaceIndices) {
        std::unique_ptr<aiMesh> subMesh(new aiMesh());
        subMesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
        subMesh->mMaterialIndex = materialIndex;
        
        // Set sub-mesh name with semantic material naming
        std::string elementName = GetIFCElementName(ifcLoader, expressID);
        
        // Get actual material name from scene materials for semantic naming
        std::string materialName = GetSemanticMaterialName(pScene, materialIndex);
        
        if (!elementName.empty()) {
            // Use single allocation for combined name to avoid multiple temporary strings
            std::string combinedName;
            combinedName.reserve(elementName.size() + materialName.size() + 1);
            combinedName = elementName;
            combinedName += "_";
            combinedName += materialName;
            subMesh->mName = aiString(combinedName);
        } else {
            // Use semantic fallback name with material suffix - optimized single allocation
            std::string fallbackName;
            fallbackName.reserve(12 + materialName.size());  // "IFC_Element_" + material name
            fallbackName = "IFC_Element_";
            fallbackName += materialName;
            subMesh->mName = aiString(fallbackName);
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
            std::unique_ptr<unsigned int[]> faceIndices(new unsigned int[3]);
            
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

                    faceIndices[i] = newVertexIndex;
                } else {
                    // Reuse existing vertex
                    faceIndices[i] = it->second;
                }
            }
            
            newFace.mIndices = faceIndices.release();
            subFaces.push_back(newFace);
        }
        
        // Set up sub-mesh data
        subMesh->mNumVertices = static_cast<unsigned int>(subVertices.size());
        std::unique_ptr<aiVector3D[]> subMeshVertices(new aiVector3D[subVertices.size()]);
        subMesh->mVertices = subMeshVertices.release();
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
        std::unique_ptr<aiFace[]> subMeshFaces(new aiFace[subFaces.size()]);
        subMesh->mFaces = subMeshFaces.release();
        for (size_t i = 0; i < subFaces.size(); ++i) {
            subMesh->mFaces[i] = subFaces[i];
        }
        
        // Generate texture coordinates for sub-mesh using optimized bounds calculation
        if (subMesh->mNumVertices > 0) {
            // Allocate texture coordinate array for this sub-mesh
            std::unique_ptr<aiVector3D[]> subMeshTexCoords(new aiVector3D[subMesh->mNumVertices]);
            subMesh->mTextureCoords[0] = subMeshTexCoords.release();
            subMesh->mNumUVComponents[0] = 2; // 2D texture coordinates
            
            // Reuse parent mesh bounds for texture coordinate generation (performance optimization)
            // This avoids recalculating bounding box for each sub-mesh when splitting by materials
            GenerateTextureCoordinates(subMesh.get(), parentMinBounds, parentMaxBounds);
        }
        
        splitMeshes.push_back(subMesh.release());
        

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
    
    // Note: Bounds will be calculated per sub-mesh when generating texture coordinates
    
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
            for (size_t i = 0; i < vertexDataVector.size(); i += webifc::geometry::VERTEX_FORMAT_SIZE_FLOATS) {
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
                std::unique_ptr<unsigned int[]> faceIndices(new unsigned int[3]);
                faceIndices[0] = static_cast<unsigned int>(vertexOffset + indexDataVector[i + 0]);
                faceIndices[1] = static_cast<unsigned int>(vertexOffset + indexDataVector[i + 1]);
                faceIndices[2] = static_cast<unsigned int>(vertexOffset + indexDataVector[i + 2]);
                face.mIndices = faceIndices.release();
                faces.push_back(face);
                materialIndices.push_back(materialIndex);
            }
        }
        
        if (vertices.empty() || faces.empty()) {
            return {};
        }
        
        // Calculate bounding box once for all sub-meshes (optimization)
        aiVector3D minBounds(FLT_MAX, FLT_MAX, FLT_MAX);
        aiVector3D maxBounds(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        
        for (const auto& vertex : vertices) {
            minBounds.x = std::min(minBounds.x, vertex.x);
            minBounds.y = std::min(minBounds.y, vertex.y);
            minBounds.z = std::min(minBounds.z, vertex.z);
            maxBounds.x = std::max(maxBounds.x, vertex.x);
            maxBounds.y = std::max(maxBounds.y, vertex.y);
            maxBounds.z = std::max(maxBounds.z, vertex.z);
        }
        
        // Now split by materials using our splitting function with cached bounds
        return SplitMeshByMaterials(ifcLoader, expressID, vertices, faces, materialIndices, minBounds, maxBounds, pScene);
        
    } catch (const std::exception &e) {
        LogError("IFC: Failed to create split meshes for element ", expressID, ": ", e.what());
        
        // Clean up any partially created faces
        for (auto& face : faces) {
            if (face.mIndices) {
                delete[] face.mIndices;
            }
        }
        
        return {};
    }
}

// ------------------------------------------------------------------------------------------------
// BuildIFCSpatialHierarchy helper methods for better maintainability and code organization

void IFCImporter::HandleMissingProject(aiScene* pScene) {
    LogWarn("IFC: No IfcProject found, using flat hierarchy");
    
    if (pScene->mNumMeshes > 0) {
        // Link all meshes to root node using exception-safe pattern
        std::unique_ptr<unsigned int[]> meshIndices(new unsigned int[pScene->mNumMeshes]);
        for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
            meshIndices[i] = i;
        }
        
        pScene->mRootNode->mNumMeshes = pScene->mNumMeshes;
        pScene->mRootNode->mMeshes = meshIndices.release();
    }
}

aiNode* IFCImporter::CreateProjectNode(webifc::parsing::IfcLoader* ifcLoader) {
    // Use efficient O(1) lookup API instead of O(n) iteration
    std::vector<uint32_t> projectIDs = ifcLoader->GetExpressIDsWithType(webifc::schema::IFCPROJECT);
    
    if (projectIDs.empty()) {
        return nullptr;
    }
    
    // Use the first project as root (there should typically be only one)
    uint32_t projectID = projectIDs[0];
    return CreateNodeFromIFCElement(ifcLoader, projectID, "IFC_Project");
}

std::vector<aiNode*> IFCImporter::BuildSiteHierarchy(webifc::parsing::IfcLoader* ifcLoader, aiNode* projectNode) {
    // Build Sites under Project using efficient O(1) lookup
    std::vector<uint32_t> siteIDs = ifcLoader->GetExpressIDsWithType(webifc::schema::IFCSITE);
    std::vector<aiNode*> siteNodes;
    
    for (uint32_t siteID : siteIDs) {
        aiNode* siteNode = CreateNodeFromIFCElement(ifcLoader, siteID, "IFC_Site");
        siteNode->mParent = projectNode;
        siteNodes.push_back(siteNode);
        
        // Build Buildings under this Site
        std::vector<aiNode*> buildingNodes = BuildBuildingHierarchy(ifcLoader, siteNode);
        AssignChildrenToParent(siteNode, buildingNodes);
    }
    
    return siteNodes;
}

std::vector<aiNode*> IFCImporter::BuildBuildingHierarchy(webifc::parsing::IfcLoader* ifcLoader, aiNode* siteNode) {
    // Build Buildings under Site using efficient O(1) lookup
    std::vector<uint32_t> buildingIDs = ifcLoader->GetExpressIDsWithType(webifc::schema::IFCBUILDING);
    std::vector<aiNode*> buildingNodes;
    
    for (uint32_t buildingID : buildingIDs) {
        aiNode* buildingNode = CreateNodeFromIFCElement(ifcLoader, buildingID, "IFC_Building");
        buildingNode->mParent = siteNode;
        buildingNodes.push_back(buildingNode);
        
        // Build Stories under this Building
        std::vector<aiNode*> storeyNodes = BuildStoreyHierarchy(ifcLoader, buildingNode);
        AssignChildrenToParent(buildingNode, storeyNodes);
    }
    
    return buildingNodes;
}

std::vector<aiNode*> IFCImporter::BuildStoreyHierarchy(webifc::parsing::IfcLoader* ifcLoader, aiNode* buildingNode) {
    // Build Stories under Building - use elevation sorting for proper hierarchy
    std::vector<StoreyInfo> sortedStoreys = GetSortedStoreysByElevation(ifcLoader);
    std::vector<aiNode*> storeyNodes;
    
    for (const StoreyInfo& storeyInfo : sortedStoreys) {
        aiNode* storeyNode = CreateNodeFromIFCElement(ifcLoader, storeyInfo.expressID, "IFC_BuildingStorey");
        storeyNode->mParent = buildingNode;
        storeyNodes.push_back(storeyNode);
        
        LogDebug("IFC: Added storey '", storeyInfo.name, "' at elevation ", storeyInfo.elevation, " to building hierarchy");
        
        // Build Spaces under this Storey
        std::vector<aiNode*> spaceNodes = BuildSpaceHierarchy(ifcLoader, storeyNode);
        AssignChildrenToParent(storeyNode, spaceNodes);
    }
    
    return storeyNodes;
}

std::vector<aiNode*> IFCImporter::BuildSpaceHierarchy(webifc::parsing::IfcLoader* ifcLoader, aiNode* storeyNode) {
    // Build Spaces under Storey (optional) using efficient O(1) lookup
    std::vector<uint32_t> spaceIDs = ifcLoader->GetExpressIDsWithType(webifc::schema::IFCSPACE);
    std::vector<aiNode*> spaceNodes;
    
    for (uint32_t spaceID : spaceIDs) {
        aiNode* spaceNode = CreateNodeFromIFCElement(ifcLoader, spaceID, "IFC_Space");
        spaceNode->mParent = storeyNode;
        spaceNodes.push_back(spaceNode);
    }
    
    return spaceNodes;
}

void IFCImporter::AssignChildrenToParent(aiNode* parent, const std::vector<aiNode*>& children) {
    if (children.empty() || !parent) {
        return;
    }
    
    // Assign children to parent using exception-safe pattern
    std::unique_ptr<aiNode*[]> childArray(new aiNode*[children.size()]);
    for (size_t i = 0; i < children.size(); ++i) {
        childArray[i] = children[i];
    }
    
    parent->mNumChildren = static_cast<unsigned int>(children.size());
    parent->mChildren = childArray.release();
}

void IFCImporter::BuildIFCSpatialHierarchy(webifc::parsing::IfcLoader* ifcLoader, aiScene* pScene) {
    
    // 1. Create project node as the spatial hierarchy root
    aiNode* projectNode = CreateProjectNode(ifcLoader);
    
    if (!projectNode) {
        // No project found, use flat hierarchy
        HandleMissingProject(pScene);
        return;
    }
    
    // 2. Replace the root node with the project node using exception-safe pattern
    std::unique_ptr<aiNode> oldRootNode(pScene->mRootNode); // Take ownership of old node
    pScene->mRootNode = projectNode; // Assign new node first
    // oldRootNode will be automatically deleted when going out of scope
    
    // 3. Build the complete spatial hierarchy: Project → Sites → Buildings → Stories → Spaces
    std::vector<aiNode*> siteNodes = BuildSiteHierarchy(ifcLoader, projectNode);
    AssignChildrenToParent(projectNode, siteNodes);
    
    // 4. Assign meshes to appropriate nodes in the hierarchy
    if (pScene->mNumMeshes > 0) {
        AssignMeshesToHierarchy(projectNode, pScene);
    }
    
    LogInfo("Built IFC spatial hierarchy: Project (", siteNodes.size(), " sites, total nodes: ", CountNodesInHierarchy(projectNode), ")");
}

aiNode* IFCImporter::CreateNodeFromIFCElement(webifc::parsing::IfcLoader* ifcLoader, uint32_t expressID, const std::string& fallbackName) {
    std::unique_ptr<aiNode> node(new aiNode());
    
    try {
        // Special handling for specific IFC types - use hash map for O(1) lookup
        uint32_t elementType = ifcLoader->GetLineType(expressID);
        
        // Use dynamic schema detection to find the best name property
        int nameArgumentIndex = -1;
        bool useSpecialExtraction = false;
        
        // For IFCSPACE, try LongName first (more descriptive for room names)
        if (elementType == webifc::schema::IFCSPACE) {
            try {
                nameArgumentIndex = schemaCache.GetPropertyIndex(elementType, "LongName", ifcLoader);
                useSpecialExtraction = true;
            } catch (...) {
                // LongName not available, will fall back to Name below
            }
        }
        
        // Fall back to standard Name property if no special handling or special handling failed
        if (nameArgumentIndex < 0) {
            try {
                nameArgumentIndex = schemaCache.GetPropertyIndex(elementType, "Name", ifcLoader);
            } catch (const std::exception& e) {
                LogWarn("IFC: Cannot find Name property for element type ", elementType, ": ", e.what());
                // Use semantic naming as fallback
                node->mName = aiString(fallbackName.empty() ? "IFC_Element" : fallbackName);
                node->mTransformation = aiMatrix4x4();
                node->mMetaData = aiMetadata::Alloc(2);
                node->mMetaData->Set(0, "ExpressID", expressID);
                node->mMetaData->Set(1, "IFC.Type", elementType);
                return node.release();
            }
        }
        
        // Try to extract the name from the IFC element
        try {
            ifcLoader->MoveToArgumentOffset(expressID, nameArgumentIndex);
            
            // Use Web-IFC's built-in decoded string handling
            std::string decodedName = ifcLoader->GetDecodedStringArgument();
            
            if (!decodedName.empty()) {
                node->mName = aiString(decodedName);
                


            } else {
                // Use fallback name with optimized string concatenation
                // Use semantic naming - IFC metadata provides ExpressID and Type
                node->mName = aiString(fallbackName.empty() ? "IFC_Element" : fallbackName);
            }
        } catch (...) {
            // If first attempt fails, try the decoded approach or fallback
            try {
                ifcLoader->MoveToLineArgument(expressID, nameArgumentIndex);
                std::string elementName = ifcLoader->GetDecodedStringArgument();
                
                if (!elementName.empty()) {
                    // GetDecodedStringArgument already handles all IFC escape sequences
                    node->mName = aiString(elementName);
                    

                } else {
                                    // Use semantic naming - IFC metadata provides ExpressID and Type
                node->mName = aiString(fallbackName.empty() ? "IFC_Element" : fallbackName);
                }
            } catch (...) {
                // Use semantic naming - IFC metadata provides ExpressID and Type
                node->mName = aiString(fallbackName.empty() ? "IFC_Element" : fallbackName);
            }
        }
        
    } catch (const std::exception &e) {
        // Use semantic naming - IFC metadata provides ExpressID and Type
        node->mName = aiString(fallbackName.empty() ? "IFC_Element" : fallbackName);
        
        LogDebug("Failed to extract name for IFC element ", expressID, ": ", e.what());
    }
    
    // Set identity transformation matrix (can be enhanced with actual IFC placement later)
    node->mTransformation = aiMatrix4x4();
    
    // Store ExpressID and IFC type in metadata for proper hierarchy matching
    uint32_t elementType = ifcLoader->GetLineType(expressID);
    node->mMetaData = aiMetadata::Alloc(2);
    node->mMetaData->Set(0, "ExpressID", expressID);
    node->mMetaData->Set(1, "IFC.Type", elementType);
    
    return node.release();
}

unsigned int IFCImporter::CountNodesInHierarchy(const aiNode* node) const {
    if (!node) return 0;
    
    unsigned int count = 1; // Count this node
    
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        count += CountNodesInHierarchy(node->mChildren[i]);
    }
    
    return count;
}

// ------------------------------------------------------------------------------------------------
// AssignMeshesToHierarchy helper methods for better maintainability and code organization

IFCImporter::NodeLookupMaps IFCImporter::BuildNodeLookupMaps(aiNode* rootNode) {
    NodeLookupMaps maps;
    
    // Build maps with single tree traversal
    std::function<void(aiNode*)> buildLookupMaps = [&](aiNode* currentNode) {
        if (!currentNode) return;
        
        if (currentNode->mMetaData) {
            uint32_t nodeType = 0;
            uint32_t nodeExpressID = 0;
            bool hasType = currentNode->mMetaData->Get("IFC.Type", nodeType);
            bool hasExpressID = currentNode->mMetaData->Get("ExpressID", nodeExpressID);
            
            if (hasExpressID) {
                maps.expressIDToNode[nodeExpressID] = currentNode;
            }
            
            if (hasType) {
                maps.entityTypeToNodes[nodeType].push_back(currentNode);
            }
        }
        
        // Recursively process children
        for (unsigned int i = 0; i < currentNode->mNumChildren; ++i) {
            buildLookupMaps(currentNode->mChildren[i]);
        }
    };
    
    buildLookupMaps(rootNode);
    return maps;
}

IFCImporter::MeshGrouping IFCImporter::GroupMeshesByStorey(aiScene* pScene) {
    MeshGrouping grouping;
    
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
                grouping.storeyToMeshes[storeyID].push_back(i);
                
                LogVerboseDebug("IFC: Mesh ", i, " (element ", expressID, ") assigned to storey ", storeyID);
            } else {
                // Element not found in spatial containment map - add to unassigned
                grouping.unassignedMeshes.push_back(i);
                
                LogVerboseDebug("IFC: Mesh ", i, " (element ", expressID, ") not found in spatial containment - unassigned");
            }
        } else {
            // No IFC metadata - add to unassigned
            grouping.unassignedMeshes.push_back(i);
            
            std::string meshName = pScene->mMeshes[i]->mName.C_Str();
            LogVerboseDebug("IFC: Mesh ", i, " ('", meshName, "') has no IFC metadata - unassigned");
        }
    }
    
    return grouping;
}

void IFCImporter::CreateNodesForStoreyMeshes(
    const std::unordered_map<uint32_t, std::vector<unsigned int>>& storeyToMeshes,
    const NodeLookupMaps& lookupMaps,
    aiNode* rootNode,
    aiScene* pScene,
    std::vector<unsigned int>& orphanedMeshes) {
    
    // O(1) lookup function for storey nodes by ExpressID
    auto findStoreyByExpressID = [&](uint32_t targetStoreyID) -> aiNode* {
        auto it = lookupMaps.expressIDToNode.find(targetStoreyID);
        if (it != lookupMaps.expressIDToNode.end()) {
            aiNode* foundNode = it->second;
            
            // Verify it's actually a building storey
            if (foundNode->mMetaData) {
                uint32_t nodeType = 0;
                if (foundNode->mMetaData->Get("IFC.Type", nodeType) && 
                    nodeType == webifc::schema::IFCBUILDINGSTOREY) {
                    LogVerboseDebug("IFC: Found storey node for ExpressID ", targetStoreyID, " via O(1) lookup");
                    return foundNode;
                }
            }
        }
        return nullptr;
    };
    
    // Process each storey and its assigned meshes
    for (const auto& [storeyID, meshIndices] : storeyToMeshes) {
        // Find the storey node for this storeyID
        aiNode* storeyNode = findStoreyByExpressID(storeyID);
        if (!storeyNode) {
            LogDebug("IFC: Could not find storey node for storey ", storeyID, " - ", meshIndices.size(), " meshes will be handled by coordinate-based assignment");
            
            // Add comprehensive debug logging for orphaned meshes
            for (unsigned int meshIndex : meshIndices) {
                aiMesh* mesh = pScene->mMeshes[meshIndex];
                if (mesh && mesh->mNumVertices > 0) {
                    double meshCenterY = CalculateMeshCenterY(mesh);
                    auto bbox = CalculateMeshBoundingBox(mesh);
                    LogDebug("IFC: Orphaned mesh ", meshIndex, " ('", mesh->mName.C_Str(), "') - CenterY: ", meshCenterY, 
                            ", BBox: [", bbox.first.x, ",", bbox.first.y, ",", bbox.first.z, "] to [", 
                            bbox.second.x, ",", bbox.second.y, ",", bbox.second.z, "], Target storeyID: ", storeyID);
                }
            }
            
            // Add these meshes to orphaned list for coordinate-based assignment
            orphanedMeshes.insert(orphanedMeshes.end(), meshIndices.begin(), meshIndices.end());
            continue;
        }
        
        // Group meshes by ExpressID to create proper hierarchy for multi-material elements
        std::unordered_map<uint32_t, std::vector<unsigned int>> expressIDToMeshes;
        std::unordered_map<uint32_t, IFCMeshMetadata> expressIDToMetadata;
        
        // Group meshes by their ExpressID (IFC element)
        for (unsigned int meshIndex : meshIndices) {
            auto metadataIt = meshToIFCMetadata.find(meshIndex);
            if (metadataIt != meshToIFCMetadata.end()) {
                uint32_t expressID = metadataIt->second.expressID;
                expressIDToMeshes[expressID].push_back(meshIndex);
                expressIDToMetadata[expressID] = metadataIt->second;
            } else {
                // Fallback for meshes without metadata - treat as individual elements
                expressIDToMeshes[0].push_back(meshIndex);
            }
        }
        
        // Create nodes for each ExpressID group
        for (const auto& [expressID, meshIndicesForElement] : expressIDToMeshes) {
            if (meshIndicesForElement.size() == 1) {
                // Single mesh - create node directly
                aiNode* meshNode = CreateSingleMeshNode(meshIndicesForElement[0], storeyNode, pScene);
                
                // Add mesh node as child to the storey
                unsigned int newChildCount = storeyNode->mNumChildren + 1;
                std::unique_ptr<aiNode*[]> newChildren(new aiNode*[newChildCount]);
                
                // Copy existing children
                for (unsigned int i = 0; i < storeyNode->mNumChildren; ++i) {
                    newChildren[i] = storeyNode->mChildren[i];
                }
                
                // Add new mesh node
                newChildren[storeyNode->mNumChildren] = meshNode;
                
                // Update storey node
                delete[] storeyNode->mChildren;
                storeyNode->mChildren = newChildren.release();
                storeyNode->mNumChildren = newChildCount;
                
            } else {
                // Multi-material element - create parent node with child nodes for each material
                auto metadataIt = expressIDToMetadata.find(expressID);
                const IFCMeshMetadata& metadata = (metadataIt != expressIDToMetadata.end()) ? 
                                                  metadataIt->second : IFCMeshMetadata{0, "", ""};
                
                aiNode* parentNode = CreateMultiMaterialElementNode(meshIndicesForElement, expressID, metadata, storeyNode, pScene);
                
                // Add parent node to the storey
                unsigned int newChildCount = storeyNode->mNumChildren + 1;
                std::unique_ptr<aiNode*[]> newChildren(new aiNode*[newChildCount]);
                
                // Copy existing children
                for (unsigned int i = 0; i < storeyNode->mNumChildren; ++i) {
                    newChildren[i] = storeyNode->mChildren[i];
                }
                
                // Add new parent node
                newChildren[storeyNode->mNumChildren] = parentNode;
                
                // Update storey node
                delete[] storeyNode->mChildren;
                storeyNode->mChildren = newChildren.release();
                storeyNode->mNumChildren = newChildCount;
            }
        }
        
        LogInfo("IFC: Assigned ", meshIndices.size(), " meshes to storey ", storeyID);
    }
}

bool IFCImporter::TryCoordinateBasedStoreyAssignment(
    const std::vector<unsigned int>& unassignedMeshes,
    const NodeLookupMaps& lookupMaps,
    aiScene* pScene,
    std::vector<unsigned int>& remainingUnassignedMeshes) {
    
    remainingUnassignedMeshes.clear();
    
    // Find all building storey nodes with their elevations
    auto storeyIt = lookupMaps.entityTypeToNodes.find(webifc::schema::IFCBUILDINGSTOREY);
    if (storeyIt == lookupMaps.entityTypeToNodes.end() || storeyIt->second.empty()) {
        LogDebug("IFC: No building storeys found for coordinate-based assignment");
        remainingUnassignedMeshes = unassignedMeshes;
        return false;
    }
    
    // Collect storey information with elevations
    struct StoreyWithElevation {
        aiNode* node;
        double elevation;
        uint32_t expressID;
    };
    
    std::vector<StoreyWithElevation> storeysWithElevations;
    
    for (aiNode* storeyNode : storeyIt->second) {
        if (!storeyNode || !storeyNode->mMetaData) continue;
        
        uint32_t storeyExpressID = 0;
        if (!storeyNode->mMetaData->Get("ExpressID", storeyExpressID)) continue;
        
        // Get elevation from the sorted storeys list (more reliable than re-parsing)
        auto sortedStoreys = GetSortedStoreysByElevation(modelManager->GetIfcLoader(currentModelID));
        
        for (const auto& storeyInfo : sortedStoreys) {
            if (storeyInfo.expressID == storeyExpressID) {
                storeysWithElevations.push_back({storeyNode, storeyInfo.elevation, storeyExpressID});
                break;
            }
        }
    }
    
    if (storeysWithElevations.empty()) {
        LogDebug("IFC: No storeys with valid elevations found for coordinate-based assignment");
        remainingUnassignedMeshes = unassignedMeshes;
        return false;
    }
    
    // Sort storeys by elevation for efficient assignment
    std::sort(storeysWithElevations.begin(), storeysWithElevations.end(),
        [](const StoreyWithElevation& a, const StoreyWithElevation& b) {
            return a.elevation < b.elevation;
        });
    
    LogInfo("IFC: Found ", storeysWithElevations.size(), " storeys for coordinate-based assignment");
    for (const auto& storey : storeysWithElevations) {
        LogDebug("IFC: Storey '", storey.node->mName.C_Str(), "' at elevation ", storey.elevation);
    }
    
    // Calculate estimated storey Y-ranges based on elevations and typical heights (IFC Y-up)
    std::vector<std::pair<StoreyWithElevation, std::pair<double, double>>> storeysWithYRanges;
    
    for (size_t i = 0; i < storeysWithElevations.size(); ++i) {
        const auto& storey = storeysWithElevations[i];
        
        // Estimate storey height based on distance to next storey or use default
        double storeyHeight = 3.0; // Default storey height in meters
        
        if (i < storeysWithElevations.size() - 1) {
            // Height is distance to next storey
            double nextElevation = storeysWithElevations[i + 1].elevation;
            storeyHeight = nextElevation - storey.elevation;
            
            // Sanity check - ensure reasonable storey height
            if (storeyHeight < 0.5 || storeyHeight > 10.0) {
                storeyHeight = 3.0; // Use default if unreasonable
            }
        }
        
        // Calculate Y-range for this storey (IFC Y-up coordinate system)
        double minY = storey.elevation;
        double maxY = storey.elevation + storeyHeight;
        
        storeysWithYRanges.push_back({storey, {minY, maxY}});
        
        LogDebug("IFC: Storey '", storey.node->mName.C_Str(), "' (ID: ", storey.expressID, ") elevation: ", storey.elevation, ", Y-range: [", minY, ", ", maxY, "] (height: ", storeyHeight, ")");
    }
    
    // Log all storey Y-ranges for debugging stair mesh assignment
    LogInfo("🔍 STOREY DEBUG: All storey Y-ranges for coordinate-based assignment:");
    for (const auto& [storeyWithElevation, yRange] : storeysWithYRanges) {
        double quarterY = yRange.first + (yRange.second - yRange.first) * 0.25;
        LogInfo("🔍 STOREY DEBUG: '", storeyWithElevation.node->mName.C_Str(), 
               "' elevation: ", storeyWithElevation.elevation, 
               ", Y-range: [", yRange.first, ", ", yRange.second, "], quarterY: ", quarterY);
    }
    
    // Assign each unassigned mesh based on center Y coordinate closest to storey quarter Y-point
    unsigned int assignedCount = 0;
    
    for (unsigned int meshIndex : unassignedMeshes) {
        aiMesh* mesh = pScene->mMeshes[meshIndex];
        if (!mesh || mesh->mNumVertices == 0) {
            remainingUnassignedMeshes.push_back(meshIndex);
            continue;
        }
        
                // Calculate mesh center Y coordinate (representative elevation of the item in IFC Y-up system)
        double meshCenterY = CalculateMeshCenterY(mesh);
        auto meshBBox = CalculateMeshBoundingBox(mesh);
        
        LogDebug("IFC: Analyzing mesh ", meshIndex, " ('", mesh->mName.C_Str(), "') - CenterY: ", meshCenterY, 
                ", BBox: [", meshBBox.first.x, ",", meshBBox.first.y, ",", meshBBox.first.z, "] to [", 
                meshBBox.second.x, ",", meshBBox.second.y, ",", meshBBox.second.z, "]");
        
        // Find the storey whose quarter Y elevation is closest to the mesh's center Y
        aiNode* targetStorey = nullptr;
        std::string targetStoreyName;
        double closestDistance = std::numeric_limits<double>::max();
        uint32_t closestStoreyID = 0;
        
        for (const auto& [storeyWithElevation, yRange] : storeysWithYRanges) {
            double storeyMinY = yRange.first;
            double storeyMaxY = yRange.second;
            double storeyQuarterY = storeyMinY + (storeyMaxY - storeyMinY) * 0.25;
            double distance = std::abs(meshCenterY - storeyQuarterY);
            
            LogDebug("IFC: Checking storey '", storeyWithElevation.node->mName.C_Str(), "' (ID: ", storeyWithElevation.expressID, 
                    ") quarterY: ", storeyQuarterY, " (range [", storeyMinY, ", ", storeyMaxY, "]) - distance to mesh centerY ", meshCenterY, " = ", distance);
            
            // Check if this storey is closer than the previous closest
            if (distance < closestDistance) {
                closestDistance = distance;
                targetStorey = storeyWithElevation.node;
                targetStoreyName = storeyWithElevation.node->mName.C_Str();
                closestStoreyID = storeyWithElevation.expressID;
                
                LogDebug("IFC: NEW CLOSEST! Storey '", targetStoreyName, "' with distance ", distance);
            }
        }
        
        if (targetStorey) {
            LogDebug("IFC: FINAL ASSIGNMENT: Mesh centerY ", meshCenterY, " assigned to closest storey '", 
                        targetStoreyName, "' (ID: ", closestStoreyID, ") with distance ", closestDistance);
        } else {
            LogDebug("IFC: NO STOREY found for mesh ", meshIndex, " with centerY ", meshCenterY, " - will remain unassigned");
        }
        
        if (targetStorey) {
            // Create mesh node and assign to target storey
            aiNode* meshNode = CreateSingleMeshNode(meshIndex, targetStorey, pScene);
            
            // Add mesh node as child to the target storey
            unsigned int newChildCount = targetStorey->mNumChildren + 1;
                std::unique_ptr<aiNode*[]> newChildren(new aiNode*[newChildCount]);
                
                // Copy existing children
            for (unsigned int i = 0; i < targetStorey->mNumChildren; ++i) {
                newChildren[i] = targetStorey->mChildren[i];
                }
                
                // Add new mesh node
            newChildren[targetStorey->mNumChildren] = meshNode;
            
            // Update target storey node
            delete[] targetStorey->mChildren;
            targetStorey->mChildren = newChildren.release();
            targetStorey->mNumChildren = newChildCount;
            
            assignedCount++;
            
            LogInfo("IFC: Assigned mesh '", mesh->mName.C_Str(), "' (center Y ", meshCenterY, 
                   ") to storey '", targetStoreyName, "'");
        } else {
            remainingUnassignedMeshes.push_back(meshIndex);
        }
    }
    
    LogInfo("IFC: Coordinate-based assignment: ", assignedCount, " meshes assigned, ", 
           remainingUnassignedMeshes.size(), " still unassigned");
    
    return assignedCount > 0;
}

std::pair<aiVector3D, aiVector3D> IFCImporter::CalculateMeshBoundingBox(aiMesh* mesh) {
    if (!mesh || mesh->mNumVertices == 0) {
        return {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    }
    
    aiVector3D minBounds = mesh->mVertices[0];
    aiVector3D maxBounds = mesh->mVertices[0];
    
    // Calculate bounding box
    for (unsigned int i = 1; i < mesh->mNumVertices; ++i) {
        const aiVector3D& vertex = mesh->mVertices[i];
        
        minBounds.x = std::min(minBounds.x, vertex.x);
        minBounds.y = std::min(minBounds.y, vertex.y);
        minBounds.z = std::min(minBounds.z, vertex.z);
        
        maxBounds.x = std::max(maxBounds.x, vertex.x);
        maxBounds.y = std::max(maxBounds.y, vertex.y);
        maxBounds.z = std::max(maxBounds.z, vertex.z);
    }
    
    return {minBounds, maxBounds};
}

aiVector3D IFCImporter::CalculateMeshBoundingBoxCenter(aiMesh* mesh) {
    auto [minBounds, maxBounds] = CalculateMeshBoundingBox(mesh);
    
    // Return center point
    return aiVector3D(
        (minBounds.x + maxBounds.x) * 0.5f,
        (minBounds.y + maxBounds.y) * 0.5f,
        (minBounds.z + maxBounds.z) * 0.5f
    );
}

double IFCImporter::CalculateMeshCenterY(aiMesh* mesh) {
    auto [minBounds, maxBounds] = CalculateMeshBoundingBox(mesh);
    return static_cast<double>((minBounds.y + maxBounds.y) * 0.5);  // IFC uses Y-up coordinate system
}

void IFCImporter::HandleUnassignedMeshes(
    const std::vector<unsigned int>& unassignedMeshes,
    const NodeLookupMaps& lookupMaps,
    aiNode* rootNode,
    aiScene* pScene) {
    
    if (unassignedMeshes.empty()) {
        return;
    }
    
    // Try coordinate-based assignment to storeys first
    std::vector<unsigned int> stillUnassignedMeshes;
    unsigned int coordinateAssignedCount = 0;
    
    if (TryCoordinateBasedStoreyAssignment(unassignedMeshes, lookupMaps, pScene, stillUnassignedMeshes)) {
        coordinateAssignedCount = unassignedMeshes.size() - stillUnassignedMeshes.size();
        LogInfo("IFC: Coordinate-based assignment: ", coordinateAssignedCount, " meshes assigned to storeys by elevation");
    } else {
        // If coordinate-based assignment fails, all meshes remain unassigned
        stillUnassignedMeshes = unassignedMeshes;
        LogDebug("IFC: Coordinate-based assignment failed, using semantic fallback");
    }
    
    // Handle remaining unassigned meshes - all assignment methods have failed
    if (!stillUnassignedMeshes.empty()) {
        LogError("IFC: Failed to assign ", stillUnassignedMeshes.size(), " meshes to any storey after both spatial containment and coordinate-based assignment attempts");
        
        // Log detailed information about failed meshes for debugging
        for (unsigned int meshIndex : stillUnassignedMeshes) {
            aiMesh* mesh = pScene->mMeshes[meshIndex];
            if (mesh && mesh->mNumVertices > 0) {
                double meshCenterY = CalculateMeshCenterY(mesh);
                auto meshBBox = CalculateMeshBoundingBox(mesh);
                LogError("IFC: Unassigned mesh ", meshIndex, " ('", mesh->mName.C_Str(), "') - CenterY: ", meshCenterY, 
                        ", BBox: [", meshBBox.first.x, ",", meshBBox.first.y, ",", meshBBox.first.z, "] to [", 
                        meshBBox.second.x, ",", meshBBox.second.y, ",", meshBBox.second.z, "]");
            }
        }
        
        throw DeadlyImportError("IFC: Unable to assign ", stillUnassignedMeshes.size(), 
                              " meshes to building storeys. This indicates a coordinate system mismatch or missing storey information in the IFC file.");
    }
    
    LogInfo("IFC: Total assignment summary - Coordinate-based: ", coordinateAssignedCount, 
           ", Failed assignments: ", stillUnassignedMeshes.size());
}

aiNode* IFCImporter::CreateSingleMeshNode(unsigned int meshIndex, aiNode* parent, aiScene* pScene) {
    std::string meshName = pScene->mMeshes[meshIndex]->mName.C_Str();
    
    std::unique_ptr<aiNode> meshNode(new aiNode());
    meshNode->mName = aiString(meshName);
    meshNode->mParent = parent;
    
    // Add IFC metadata to the mesh node
    auto metadataIt = meshToIFCMetadata.find(meshIndex);
    if (metadataIt != meshToIFCMetadata.end()) {
        const auto& ifcMeta = metadataIt->second;
        meshNode->mMetaData = aiMetadata::Alloc(2);
        meshNode->mMetaData->Set(0, "IFC.ExpressID", ifcMeta.expressID);
        meshNode->mMetaData->Set(1, "IFC.Type", aiString(ifcMeta.ifcType.c_str()));
    }
    
    meshNode->mNumMeshes = 1;
    std::unique_ptr<unsigned int[]> meshIndices(new unsigned int[1]);
    meshNode->mMeshes = meshIndices.release();
    meshNode->mMeshes[0] = meshIndex;
    
    return meshNode.release();
}

aiNode* IFCImporter::CreateMultiMaterialElementNode(
    const std::vector<unsigned int>& meshIndices,
    uint32_t expressID,
    const IFCMeshMetadata& metadata,
    aiNode* parent,
    aiScene* pScene) {
    
    std::string elementName = "IFC_Element";
    if (!metadata.elementName.empty()) {
        elementName = metadata.elementName;
    }
    
    // Create parent node for the element
    std::unique_ptr<aiNode> parentNode(new aiNode());
    parentNode->mName = aiString(elementName);
    parentNode->mParent = parent;
    
    // Add IFC metadata to parent node
    if (expressID != 0) {
        parentNode->mMetaData = aiMetadata::Alloc(2);
        parentNode->mMetaData->Set(0, "IFC.ExpressID", expressID);
        parentNode->mMetaData->Set(1, "IFC.Type", aiString(metadata.ifcType.c_str()));
    }
    
    // Create child nodes for each material mesh
    parentNode->mNumChildren = static_cast<unsigned int>(meshIndices.size());
    std::unique_ptr<aiNode*[]> childrenArray(new aiNode*[parentNode->mNumChildren]);
    parentNode->mChildren = childrenArray.release();
    
    for (unsigned int i = 0; i < meshIndices.size(); ++i) {
        unsigned int meshIndex = meshIndices[i];
        std::string meshName = pScene->mMeshes[meshIndex]->mName.C_Str();
        
        std::unique_ptr<aiNode> childNode(new aiNode());
        childNode->mName = aiString(meshName);
        childNode->mParent = parentNode.get();
        
        // Add IFC metadata to child node (same as parent, but represents the material-specific mesh)
        auto childMetadataIt = meshToIFCMetadata.find(meshIndex);
        if (childMetadataIt != meshToIFCMetadata.end()) {
            const auto& ifcMeta = childMetadataIt->second;
            childNode->mMetaData = aiMetadata::Alloc(2);
            childNode->mMetaData->Set(0, "IFC.ExpressID", ifcMeta.expressID);
            childNode->mMetaData->Set(1, "IFC.Type", aiString(ifcMeta.ifcType.c_str()));
        }
        
        childNode->mNumMeshes = 1;
        std::unique_ptr<unsigned int[]> childMeshIndices(new unsigned int[1]);
        childNode->mMeshes = childMeshIndices.release();
        childNode->mMeshes[0] = meshIndex;
        
        parentNode->mChildren[i] = childNode.release();
    }
    
    return parentNode.release();
}

void IFCImporter::AssignMeshesToHierarchy(aiNode* node, aiScene* pScene) {
    // Assign meshes to their correct storeys based on spatial containment relationships
    
    if (!node || pScene->mNumMeshes == 0) {
        return;
    }
    
    // 1. Build node lookup maps for O(1) access
    NodeLookupMaps lookupMaps = BuildNodeLookupMaps(node);
    
    // 2. Group meshes by storey based on spatial containment
    MeshGrouping meshGrouping = GroupMeshesByStorey(pScene);
    
    // 3. Create nodes for assigned meshes in their respective storeys
    std::vector<unsigned int> orphanedMeshes;
    CreateNodesForStoreyMeshes(meshGrouping.storeyToMeshes, lookupMaps, node, pScene, orphanedMeshes);
    
    // 4. Combine unassigned meshes with orphaned meshes for coordinate-based assignment
    std::vector<unsigned int> allUnassignedMeshes = meshGrouping.unassignedMeshes;
    allUnassignedMeshes.insert(allUnassignedMeshes.end(), orphanedMeshes.begin(), orphanedMeshes.end());
    
    LogInfo("IFC: Assignment summary - Spatially assigned: ", 
           (pScene->mNumMeshes - allUnassignedMeshes.size()), 
           ", Unassigned (will use coordinate-based): ", allUnassignedMeshes.size(),
           " (original unassigned: ", meshGrouping.unassignedMeshes.size(), 
           ", orphaned: ", orphanedMeshes.size(), ")");
    
    // 5. Handle all unassigned meshes with coordinate-based assignment and semantic fallback
    HandleUnassignedMeshes(allUnassignedMeshes, lookupMaps, node, pScene);
}

aiNode* IFCImporter::FindNodeByIFCEntityType(aiNode* rootNode, const std::string& entityPrefix) {
    // Helper function to find nodes by IFC entity type prefix (language-independent)
    // Note: IFC spatial hierarchies are typically small, so O(n) traversal is acceptable here
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

aiNode* IFCImporter::FindSemanticParentForUnassignedItems(
    aiNode* rootNode, 
    const std::unordered_map<uint32_t, std::vector<aiNode*>>& entityTypeToNodes) {
    // Find appropriate parent for unassigned items using semantic spatial hierarchy
    // Priority: Building → Site → Project (avoid putting meshes at project level)
    // Use O(1) lookup maps instead of expensive O(n³) tree traversals
    
    if (!rootNode) return nullptr;
    
    // Priority 1: Look for building nodes using O(1) lookup (preferred for unassigned items)
    auto buildingIt = entityTypeToNodes.find(webifc::schema::IFCBUILDING);
    if (buildingIt != entityTypeToNodes.end() && !buildingIt->second.empty()) {
        // Use the first available building node
        aiNode* buildingNode = buildingIt->second[0];
        LogDebug("IFC: Using building node for unassigned items: ", buildingNode->mName.C_Str());
        return buildingNode;
    }
    
    // Priority 2: Look for site nodes using O(1) lookup
    auto siteIt = entityTypeToNodes.find(webifc::schema::IFCSITE);
    if (siteIt != entityTypeToNodes.end() && !siteIt->second.empty()) {
        // Use the first available site node
        aiNode* siteNode = siteIt->second[0];
        LogDebug("IFC: Using site node for unassigned items: ", siteNode->mName.C_Str());
        return siteNode;
    }
    
    // Priority 3: Look for project nodes using O(1) lookup (avoid this level for meshes)
    auto projectIt = entityTypeToNodes.find(webifc::schema::IFCPROJECT);
    if (projectIt != entityTypeToNodes.end() && !projectIt->second.empty()) {
        // Use the first available project node
        aiNode* projectNode = projectIt->second[0];
        LogDebug("IFC: Using project node for unassigned items: ", projectNode->mName.C_Str());
        return projectNode;
    }
    
    // Priority 4: Final fallback to root node
    LogDebug("IFC: Using root node for unassigned items: ", rootNode->mName.C_Str());
    return rootNode;
}



#endif // !! ASSIMP_BUILD_NO_IFC_IMPORTER

