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

/** @file  IFCLoader.h
 *  @brief Declaration of the Industry Foundation Classes (IFC) loader main class
 *        Web-IFC integration for improved performance and compatibility with
 *        IFC4, IFC2x3, IFC2x2, IFC1.5, IFC1.4, IFC1.3, IFC1.2, IFC1.1, IFC1.0,
 */
#ifndef INCLUDED_AI_IFC_LOADER_H
#define INCLUDED_AI_IFC_LOADER_H

#include <assimp/BaseImporter.h>
#include <assimp/LogAux.h>
#include <assimp/mesh.h>
#include <memory>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>

#include <glm/glm.hpp>

// Forward declarations for Web-IFC
namespace webifc::parsing { class IfcLoader; }

// Forward declaration for schema cache class
class SchemaArgumentCache;

// Forward declarations for Web-IFC (always available to avoid compilation issues)
namespace webifc::manager {
    class ModelManager;
}
namespace webifc::geometry {
    struct IfcFlatMesh;
    class IfcGeometryLoader;
}
namespace webifc::parsing {
    class IfcLoader;
}

namespace Assimp {

// -------------------------------------------------------------------------------------------
/** Load the IFC format, which is an open specification to describe building and construction
    industry data. This implementation uses the Web-IFC library for enhanced performance
    and broader schema support.

    See http://en.wikipedia.org/wiki/Industry_Foundation_Classes
*/
// -------------------------------------------------------------------------------------------
class IFCImporter : public BaseImporter, public LogFunctions<IFCImporter> {
public:
    // loader settings, publicly accessible via their corresponding AI_CONFIG constants
    struct Settings {
        Settings() :
                skipSpaceRepresentations(true), 
                useCustomTriangulation(true), 
                skipAnnotations(true), 
                conicSamplingAngle(10.f), 
                cylindricalTessellation(32),
                coordinateToOrigin(false),
                circleSegments(12),
                skipOpeningElements(true),
                skipSpaceGeometry(true),
                skipOpeningStandardCase(true) {}

        bool skipSpaceRepresentations;
        bool useCustomTriangulation;
        bool skipAnnotations;
        float conicSamplingAngle;
        int cylindricalTessellation;
        bool coordinateToOrigin;
        int circleSegments;
        
        // Configurable geometry exclusion settings for better flexibility
        bool skipOpeningElements;      // Skip IFCOPENINGELEMENT (typically voids)
        bool skipSpaceGeometry;        // Skip IFCSPACE geometry (rooms/spaces)
        bool skipOpeningStandardCase;  // Skip IFCOPENINGSTANDARDCASE (standard voids)
    };

    IFCImporter();
    ~IFCImporter() override;

    // --------------------
    bool CanRead(const std::string &pFile,
            IOSystem *pIOHandler,
            bool checkSig) const override;

protected:
    // --------------------
    const aiImporterDesc *GetInfo() const override;

    // --------------------
    void SetupProperties(const Importer *pImp) override;

    // --------------------
    void InternReadFile(const std::string &pFile,
            aiScene *pScene,
            IOSystem *pIOHandler) override;

private:
    
    // Schema argument cache for efficient property lookups
    class SchemaArgumentCache {
    private:
        std::unordered_map<uint32_t, std::unordered_map<std::string, int>> typeToPropertyIndices;
        
    public:
        // Get argument index for a property name dynamically from schema
        int GetPropertyIndex(uint32_t elementType, const std::string& propertyName, 
                           webifc::parsing::IfcLoader* ifcLoader);
        
        // Clear all cached data (useful for cleanup)
        void Clear() { typeToPropertyIndices.clear(); }
        
        // Get number of cached element types (for debugging/monitoring)
        size_t GetCachedTypeCount() const { return typeToPropertyIndices.size(); }
    };
    
    Settings settings;

    // Web-IFC related members with thread safety
    std::unique_ptr<webifc::manager::ModelManager> modelManager;
    uint32_t currentModelID;
    
    /** 
     * Thread Safety Documentation for Web-IFC Operations:
     * 
     * CRITICAL: Web-IFC's ModelManager is NOT thread-safe by design. All operations
     * that access modelManager MUST be protected by modelManagerMutex.
     * 
     * Protected Operations:
     * - modelManager initialization (InitializeWebIFC)
     * - Model creation/loading (LoadModelWithWebIFC) 
     * - Geometry extraction (ExtractGeometry)
     * - Material extraction (ExtractMaterials)
     * - Scene graph building (BuildSceneGraph)
     * - Model cleanup (CleanupWebIFC, destructor)
     * 
     * Mutex Type: std::recursive_mutex is used because some protected methods
     * may call other protected methods (e.g., BuildSceneGraph -> AssignMeshesToHierarchy).
     * 
     * Verified Thread Safety Invariants:
     * 1. modelManager is never accessed without holding the mutex
     * 2. currentModelID modifications are atomic under mutex protection
     * 3. Model lifecycle (create -> use -> destroy) is serialized
     * 4. Multiple imports in different threads are safely serialized
     * 
     * Performance Note: While this serializes IFC imports, Web-IFC's underlying
     * C++ implementation with WASM bindings requires this protection level.
     */
    mutable std::recursive_mutex modelManagerMutex; 
    
    std::unordered_map<uint32_t, unsigned int> materialIDToIndex; // Express ID -> material index mapping
    
    // Schema-dependent argument indices for dynamic schema compatibility
    mutable SchemaArgumentCache schemaCache;
    
    // Web-IFC integration methods
    void InitializeWebIFC();
    void LoadModelWithWebIFC(const std::string &pFile, aiScene *pScene, IOSystem *pIOHandler);
    void ExtractGeometry(uint32_t modelID, aiScene *pScene);
    void ExtractMaterials(uint32_t modelID, aiScene *pScene);
    
    // ExtractGeometry helper methods for better maintainability
    std::vector<uint32_t> FilterGeometricElementTypes(const std::unordered_set<uint32_t>& allElementTypes);


    
    // Streaming approach for large file support
    void ProcessMeshesStreaming(
        const std::vector<uint32_t>& geometricElementTypes,
        const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& relMaterials,
        webifc::parsing::IfcLoader* loader,
        std::vector<aiMesh*>& meshes,
        std::unordered_map<std::string, unsigned int>& colorMaterialCache,
        bool& needsDefaultMaterial,
        aiScene* pScene);
    void ProcessSingleMesh(
        uint32_t expressID,
        const webifc::geometry::IfcFlatMesh& flatMesh,
        const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& relMaterials,
        webifc::parsing::IfcLoader* loader,
        std::vector<aiMesh*>& meshes,
        std::unordered_map<std::string, unsigned int>& colorMaterialCache,
        bool& needsDefaultMaterial,
        aiScene* pScene);
    
    void SetupSceneMeshes(std::vector<aiMesh*>& meshes, bool needsDefaultMaterial, aiScene* pScene);
    
    // CreateMeshFromFlatMesh helper methods for better maintainability
    struct GeometryData {
        std::vector<aiVector3D> vertices;
        std::vector<aiFace> faces;
        std::vector<unsigned int> materialIndices;
    };
    
    GeometryData CollectGeometryData(
        uint32_t expressID,
        const webifc::geometry::IfcFlatMesh& flatMesh,
        const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& relMaterials,
        std::unordered_map<std::string, unsigned int>& colorMaterialCache,
        aiScene* pScene);
    void SetupMeshDataStructures(aiMesh* mesh, const GeometryData& geomData);
    void HandleMeshMaterials(aiMesh* mesh, const std::vector<unsigned int>& materialIndices);
    void GenerateTextureCoordinatesForMesh(
        aiMesh* mesh, 
        const aiVector3D* preCalcMinBounds = nullptr,
        const aiVector3D* preCalcMaxBounds = nullptr);
    
    // Forward declarations and data structures
    struct IFCMeshMetadata {
        uint32_t expressID;
        std::string ifcType;
        std::string elementName;
    };
    
    // AssignMeshesToHierarchy helper methods for better maintainability
    struct NodeLookupMaps {
        std::unordered_map<uint32_t, aiNode*> expressIDToNode;
        std::unordered_map<uint32_t, std::vector<aiNode*>> entityTypeToNodes;
    };
    
    struct MeshGrouping {
        std::unordered_map<uint32_t, std::vector<unsigned int>> storeyToMeshes;
        std::vector<unsigned int> unassignedMeshes;
    };
    
    NodeLookupMaps BuildNodeLookupMaps(aiNode* rootNode);
    MeshGrouping GroupMeshesByStorey(aiScene* pScene);
    void CreateNodesForStoreyMeshes(
        const std::unordered_map<uint32_t, std::vector<unsigned int>>& storeyToMeshes,
        const NodeLookupMaps& lookupMaps,
        aiNode* rootNode,
        aiScene* pScene,
        std::vector<unsigned int>& orphanedMeshes);
    void HandleUnassignedMeshes(
        const std::vector<unsigned int>& unassignedMeshes,
        const NodeLookupMaps& lookupMaps,
        aiNode* rootNode,
        aiScene* pScene);
    bool TryCoordinateBasedStoreyAssignment(
        const std::vector<unsigned int>& unassignedMeshes,
        const NodeLookupMaps& lookupMaps,
        aiScene* pScene,
        std::vector<unsigned int>& remainingUnassignedMeshes);
    std::pair<aiVector3D, aiVector3D> CalculateMeshBoundingBox(aiMesh* mesh);
    aiVector3D CalculateMeshBoundingBoxCenter(aiMesh* mesh);
    double CalculateMeshCenterY(aiMesh* mesh);
    aiNode* CreateSingleMeshNode(unsigned int meshIndex, aiNode* parent, aiScene* pScene);
    aiNode* CreateMultiMaterialElementNode(
        const std::vector<unsigned int>& meshIndices,
        uint32_t expressID,
        const IFCMeshMetadata& metadata,
        aiNode* parent,
        aiScene* pScene);
    
    // BuildIFCSpatialHierarchy helper methods for better maintainability
    void HandleMissingProject(aiScene* pScene);
    aiNode* CreateProjectNode(webifc::parsing::IfcLoader* ifcLoader);
    std::vector<aiNode*> BuildSiteHierarchy(webifc::parsing::IfcLoader* ifcLoader, aiNode* projectNode);
    std::vector<aiNode*> BuildBuildingHierarchy(webifc::parsing::IfcLoader* ifcLoader, aiNode* siteNode);
    std::vector<aiNode*> BuildStoreyHierarchy(webifc::parsing::IfcLoader* ifcLoader, aiNode* buildingNode);
    std::vector<aiNode*> BuildSpaceHierarchy(webifc::parsing::IfcLoader* ifcLoader, aiNode* storeyNode);
    void AssignChildrenToParent(aiNode* parent, const std::vector<aiNode*>& children);
        
    void BuildSceneGraph(uint32_t modelID, aiScene *pScene);
    aiMesh* ConvertWebIFCMesh(const webifc::geometry::IfcFlatMesh &flatMesh, uint32_t geometryIndex);
    aiMesh* CreateMeshFromFlatMesh(
        uint32_t expressID,
        const webifc::geometry::IfcFlatMesh& flatMesh,
        const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& relMaterials,
        std::unordered_map<std::string, unsigned int>& colorMaterialCache,
        aiScene* pScene,
        const aiVector3D* preCalcMinBounds = nullptr,
        const aiVector3D* preCalcMaxBounds = nullptr);
        
    // Split a multi-material mesh into separate meshes by material
    std::vector<aiMesh*> SplitMeshByMaterials(
        webifc::parsing::IfcLoader* ifcLoader,
        uint32_t expressID,
        const std::vector<aiVector3D>& vertices,
        const std::vector<aiFace>& faces,
        const std::vector<unsigned int>& materialIndices,
        const aiVector3D& parentMinBounds,
        const aiVector3D& parentMaxBounds,
        aiScene* pScene);
        
    // Create split meshes directly from flat mesh (for multi-material meshes)
    std::vector<aiMesh*> CreateSplitMeshesFromFlatMesh(
        webifc::parsing::IfcLoader* ifcLoader,
        uint32_t expressID,
        const webifc::geometry::IfcFlatMesh& flatMesh,
        const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& relMaterials,
        std::unordered_map<std::string, unsigned int>& colorMaterialCache,
        aiScene* pScene);
    aiMaterial* CreateMaterialFromColor(const aiColor4D &color, const std::string &name);
    unsigned int GetOrCreateColorMaterial(
        const aiColor4D& color,
        std::unordered_map<std::string, unsigned int>& colorMaterialCache,
        aiScene* pScene);
    // IFC material extraction methods
    void ExtractIFCMaterials(
        webifc::parsing::IfcLoader* ifcLoader,
        const webifc::geometry::IfcGeometryLoader& geomLoader,
        std::vector<aiMaterial*>& materials,
        std::unordered_map<uint32_t, unsigned int>& materialIDToIndex);
    
    aiMaterial* ExtractSingleIFCMaterial(
        webifc::parsing::IfcLoader* ifcLoader,
        uint32_t materialID,
        const std::vector<std::pair<uint32_t, uint32_t>>& definitions);
    
    
    void ExtractMaterialProperties(
        webifc::parsing::IfcLoader* ifcLoader,
        const std::vector<std::pair<uint32_t, uint32_t>>& definitions,
        aiMaterial* material);
    
    void ExtractColorFromRGB(
        webifc::parsing::IfcLoader* ifcLoader,
        uint32_t colorID,
        aiColor4D& outColor);
    
    void ExtractRenderingProperties(
        webifc::parsing::IfcLoader* ifcLoader,
        uint32_t renderingID,
        aiColor4D& diffuseColor,
        aiColor4D& specularColor,
        float& shininess);
    
    void ExtractMaterialLayerProperties(
        webifc::parsing::IfcLoader* ifcLoader,
        uint32_t layerID,
        aiMaterial* material);
    
    void ProcessStyledItems(
        webifc::parsing::IfcLoader* ifcLoader,
        const std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>& styledItems,
        std::vector<aiMaterial*>& materials,
        std::unordered_map<uint32_t, unsigned int>& materialIDToIndex);
    
    void ProcessSurfaceStyle(
        webifc::parsing::IfcLoader* ifcLoader,
        uint32_t styleID,
        uint32_t itemID,
        std::vector<aiMaterial*>& materials,
        std::unordered_map<uint32_t, unsigned int>& materialIDToIndex);
    
    // SetMeshMaterialFromIFC removed - was dead code with O(n) performance issue
    

    void CleanupWebIFC(uint32_t modelID);
    
    aiColor4D ConvertWebIFCColor(const glm::dvec4& webifcColor);
    
    // sRGB to linear RGB conversion for proper color handling
    aiColor4D ConvertSRGBToLinear(const aiColor4D& srgbColor);
    
    // Texture coordinate generation
    void GenerateTextureCoordinates(aiMesh* mesh, const aiVector3D& minBounds, const aiVector3D& maxBounds);
    
    // Spatial hierarchy building
    void BuildIFCSpatialHierarchy(webifc::parsing::IfcLoader* ifcLoader, aiScene* pScene);
    aiNode* CreateNodeFromIFCElement(webifc::parsing::IfcLoader* ifcLoader, uint32_t expressID, const std::string& fallbackName = "");
    unsigned int CountNodesInHierarchy(const aiNode* node) const;
    void AssignMeshesToHierarchy(aiNode* node, aiScene* pScene);
    
    // IFC element name extraction for meaningful mesh names
    std::string GetIFCElementName(webifc::parsing::IfcLoader* ifcLoader, uint32_t expressID);
    std::string GetSemanticMaterialName(aiScene* pScene, unsigned int materialIndex);
    
    // IFC mesh metadata storage
    std::unordered_map<unsigned int, IFCMeshMetadata> meshToIFCMetadata; // meshIndex -> IFC metadata
    
    // Spatial containment mapping for correct storey assignment
    std::unordered_map<uint32_t, uint32_t> PopulateSpatialContainmentMap(webifc::parsing::IfcLoader* ifcLoader);
    std::unordered_map<uint32_t, uint32_t> elementToStoreyMap; // expressID -> storeyID mapping
    
    // STREAMING SPATIAL HIERARCHY - Memory-efficient lazy lookup
    std::optional<uint32_t> GetElementSpatialContainer(uint32_t elementID, webifc::parsing::IfcLoader* ifcLoader);
    mutable std::unordered_map<uint32_t, uint32_t> spatialContainerCache; // Small cache for recent lookups
    
    // Storey elevation mapping and sorting for semantic hierarchy
    struct StoreyInfo {
        uint32_t expressID;
        double elevation;
        std::string name;
    };
    std::vector<StoreyInfo> GetSortedStoreysByElevation(webifc::parsing::IfcLoader* ifcLoader);
    
    // LAZY SCENE GRAPH CONSTRUCTION
    void BuildLazySceneGraphFromMeshes(webifc::parsing::IfcLoader* ifcLoader, aiNode* projectNode, aiScene* pScene);
    void CreateElementNodesUnderStorey(aiNode* storeyNode, const std::vector<unsigned int>& meshIndices, aiScene* pScene);
    void AssignMeshesToNode(aiNode* node, const std::vector<unsigned int>& meshIndices, aiScene* pScene);
    
    aiNode* FindSemanticParentForUnassignedItems(
        aiNode* rootNode, 
        const std::unordered_map<uint32_t, std::vector<aiNode*>>& entityTypeToNodes);
    aiNode* FindNodeByIFCEntityType(aiNode* rootNode, const std::string& entityPrefix);
    


}; // !class IFCImporter

} // end of namespace Assimp
#endif // !INCLUDED_AI_IFC_LOADER_H
