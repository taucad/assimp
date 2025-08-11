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
#include <memory>
#include <vector>

// Forward declarations for Web-IFC (always available to avoid compilation issues)
namespace webifc::manager {
    class ModelManager;
}
namespace webifc::geometry {
    struct IfcFlatMesh;
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
                circleSegments(12) {}

        bool skipSpaceRepresentations;
        bool useCustomTriangulation;
        bool skipAnnotations;
        float conicSamplingAngle;
        int cylindricalTessellation;
        bool coordinateToOrigin;
        int circleSegments;
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
    Settings settings;

    // Web-IFC related members
    webifc::manager::ModelManager* modelManager;
    uint32_t currentModelID;
    
    // Web-IFC integration methods
    void InitializeWebIFC();
    void LoadModelWithWebIFC(const std::string &pFile, aiScene *pScene, IOSystem *pIOHandler);
    void ExtractGeometry(uint32_t modelID, aiScene *pScene);
    void ExtractMaterials(uint32_t modelID, aiScene *pScene);
    void BuildSceneGraph(uint32_t modelID, aiScene *pScene);
    aiMesh* ConvertWebIFCMesh(const webifc::geometry::IfcFlatMesh &flatMesh, uint32_t geometryIndex);
    aiMaterial* CreateMaterialFromColor(const aiColor4D &color, const std::string &name);
    void CleanupWebIFC(uint32_t modelID);

}; // !class IFCImporter

} // end of namespace Assimp
#endif // !INCLUDED_AI_IFC_LOADER_H
