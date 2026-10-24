/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2025, assimp team

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

/** @file Exporter.cpp

Assimp export interface. While it's public interface bears many similarities
to the import interface (in fact, it is largely symmetric), the internal
implementations differs a lot. Exporters are considered stateless and are
simple callbacks which we maintain in a global list along with their
description strings.

Here we implement only the C++ interface (Assimp::Exporter).
*/

#ifndef ASSIMP_BUILD_NO_EXPORT

#include <assimp/BlobIOSystem.h>
#include <assimp/SceneCombiner.h>
#include <assimp/DefaultIOSystem.h>
#include <assimp/Exporter.hpp>
#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/Exceptional.h>

#include "Common/DefaultProgressHandler.h"
#include "Common/BaseProcess.h"
#include "Common/ScenePrivate.h"
#include "Common/UnitAxisContract.h"
#include "PostProcessing/CalcTangentsProcess.h"
#include "PostProcessing/MakeVerboseFormat.h"
#include "PostProcessing/JoinVerticesProcess.h"
#include "PostProcessing/ConvertToLHProcess.h"
#include "PostProcessing/PretransformVertices.h"

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace Assimp {

#ifdef _MSC_VER
#    pragma warning( disable : 4800 )
#endif // _MSC_VER


// PostStepRegistry.cpp
void GetPostProcessingStepInstanceList(std::vector< BaseProcess* >& out);

// ------------------------------------------------------------------------------------------------
// Exporter worker function prototypes. Do not use const, because some exporter need to convert
// the scene temporary
#ifndef ASSIMP_BUILD_NO_COLLADA_EXPORTER
void ExportSceneCollada(const char*,IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_X_EXPORTER
void ExportSceneXFile(const char*,IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_STEP_EXPORTER
void ExportSceneStep(const char*,IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_OBJ_EXPORTER
void ExportSceneObj(const char*,IOSystem*, const aiScene*, const ExportProperties*);
void ExportSceneObjNoMtl(const char*,IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_STL_EXPORTER
void ExportSceneSTL(const char*,IOSystem*, const aiScene*, const ExportProperties*);
void ExportSceneSTLBinary(const char*,IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_PLY_EXPORTER
void ExportScenePly(const char*,IOSystem*, const aiScene*, const ExportProperties*);
void ExportScenePlyBinary(const char*, IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_3DS_EXPORTER
void ExportScene3DS(const char*, IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_GLTF_EXPORTER
void ExportSceneGLTF(const char*, IOSystem*, const aiScene*, const ExportProperties*);
void ExportSceneGLB(const char*, IOSystem*, const aiScene*, const ExportProperties*);
void ExportSceneGLTF2(const char*, IOSystem*, const aiScene*, const ExportProperties*);
void ExportSceneGLB2(const char*, IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_ASSBIN_EXPORTER
void ExportSceneAssbin(const char*, IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_ASSXML_EXPORTER
void ExportSceneAssxml(const char*, IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_X3D_EXPORTER
void ExportSceneX3D(const char*, IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_FBX_EXPORTER
void ExportSceneFBX(const char*, IOSystem*, const aiScene*, const ExportProperties*);
void ExportSceneFBXA(const char*, IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_3MF_EXPORTER
void ExportScene3MF( const char*, IOSystem*, const aiScene*, const ExportProperties* );
#endif
#ifndef ASSIMP_BUILD_NO_M3D_EXPORTER
void ExportSceneM3D(const char*, IOSystem*, const aiScene*, const ExportProperties*);
void ExportSceneM3DA(const char*, IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_ASSJSON_EXPORTER
void ExportAssimp2Json(const char* , IOSystem*, const aiScene* , const Assimp::ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_PBRT_EXPORTER
void ExportScenePbrt(const char*, IOSystem*, const aiScene*, const ExportProperties*);
#endif
#ifndef ASSIMP_BUILD_NO_USD_EXPORTER
void ExportSceneUSDA(const char*, IOSystem*, const aiScene*, const ExportProperties*);
void ExportSceneUSDC(const char*, IOSystem*, const aiScene*, const ExportProperties*);
void ExportSceneUSDZ(const char*, IOSystem*, const aiScene*, const ExportProperties*);
#endif

static void setupExporterArray(std::vector<Exporter::ExportFormatEntry> &exporters) {
	(void)exporters;

#ifndef ASSIMP_BUILD_NO_COLLADA_EXPORTER
	exporters.emplace_back("collada", "COLLADA - Digital Asset Exchange Schema", "dae", &ExportSceneCollada);
#endif

#ifndef ASSIMP_BUILD_NO_X_EXPORTER
	exporters.emplace_back("x", "X Files", "x", &ExportSceneXFile,
			aiProcess_MakeLeftHanded | aiProcess_FlipWindingOrder | aiProcess_FlipUVs);
#endif

#ifndef ASSIMP_BUILD_NO_STEP_EXPORTER
	exporters.emplace_back("stp", "Step Files", "stp", &ExportSceneStep, 0);
#endif

#ifndef ASSIMP_BUILD_NO_OBJ_EXPORTER
	exporters.emplace_back("obj", "Wavefront OBJ format", "obj", &ExportSceneObj);
	exporters.emplace_back("objnomtl", "Wavefront OBJ format without material file", "obj", &ExportSceneObjNoMtl);
#endif

#ifndef ASSIMP_BUILD_NO_STL_EXPORTER
	// The STL writer applies node transforms and derives missing face normals itself (the same
	// arithmetic as PreTransformVertices + GenNormals), so it reads the caller's scene instead of a
	// pre-transformed, un-indexed, normal-generated copy.
	exporters.emplace_back("stl", "Stereolithography", "stl", &ExportSceneSTL,
			aiProcess_Triangulate);
	exporters.emplace_back("stlb", "Stereolithography (binary)", "stl", &ExportSceneSTLBinary,
			aiProcess_Triangulate);
#endif

#ifndef ASSIMP_BUILD_NO_PLY_EXPORTER
	exporters.emplace_back("ply", "Stanford Polygon Library", "ply", &ExportScenePly,
			aiProcess_PreTransformVertices);
	exporters.emplace_back("plyb", "Stanford Polygon Library (binary)", "ply", &ExportScenePlyBinary,
			aiProcess_PreTransformVertices);
#endif

#ifndef ASSIMP_BUILD_NO_3DS_EXPORTER
	exporters.emplace_back("3ds", "Autodesk 3DS (legacy)", "3ds", &ExportScene3DS,
			aiProcess_Triangulate | aiProcess_SortByPType | aiProcess_JoinIdenticalVertices);
#endif

#if !defined(ASSIMP_BUILD_NO_GLTF_EXPORTER) && !defined(ASSIMP_BUILD_NO_GLTF2_EXPORTER)
	exporters.emplace_back("gltf2", "GL Transmission Format v. 2", "gltf", &ExportSceneGLTF2,
			aiProcess_JoinIdenticalVertices | aiProcess_Triangulate | aiProcess_SortByPType);
	exporters.emplace_back("glb2", "GL Transmission Format v. 2 (binary)", "glb", &ExportSceneGLB2,
			aiProcess_JoinIdenticalVertices | aiProcess_Triangulate | aiProcess_SortByPType);
#endif

#if !defined(ASSIMP_BUILD_NO_GLTF_EXPORTER) && !defined(ASSIMP_BUILD_NO_GLTF1_EXPORTER)
	exporters.emplace_back("gltf", "GL Transmission Format", "gltf", &ExportSceneGLTF,
			aiProcess_JoinIdenticalVertices | aiProcess_Triangulate | aiProcess_SortByPType);
	exporters.emplace_back("glb", "GL Transmission Format (binary)", "glb", &ExportSceneGLB,
			aiProcess_JoinIdenticalVertices | aiProcess_Triangulate | aiProcess_SortByPType);
#endif

#ifndef ASSIMP_BUILD_NO_ASSBIN_EXPORTER
	exporters.emplace_back("assbin", "Assimp Binary File", "assbin", &ExportSceneAssbin, 0);
#endif

#ifndef ASSIMP_BUILD_NO_ASSXML_EXPORTER
	exporters.emplace_back("assxml", "Assimp XML Document", "assxml", &ExportSceneAssxml, 0);
#endif

#ifndef ASSIMP_BUILD_NO_X3D_EXPORTER
	exporters.emplace_back("x3d", "Extensible 3D", "x3d", &ExportSceneX3D, 0);
#endif

#ifndef ASSIMP_BUILD_NO_FBX_EXPORTER
	exporters.emplace_back("fbx", "Autodesk FBX (binary)", "fbx", &ExportSceneFBX, 0);
	exporters.emplace_back("fbxa", "Autodesk FBX (ascii)", "fbx", &ExportSceneFBXA, 0);
#endif

#ifndef ASSIMP_BUILD_NO_M3D_EXPORTER
	exporters.push_back(Exporter::ExportFormatEntry("m3d", "Model 3D (binary)", "m3d", &ExportSceneM3D, 0));
	exporters.push_back(Exporter::ExportFormatEntry("m3da", "Model 3D (ascii)", "a3d", &ExportSceneM3DA, 0));
#endif

#ifndef ASSIMP_BUILD_NO_3MF_EXPORTER
	// 3MF requires triangulated meshes (Core Spec §4.1). The lib3mf bridge
	// traverses nodes and bakes their world transforms while it writes vertices,
	// so pre-transforming here would erase source-mesh provenance. Callers
	// using the top-level `Exporter::Export` get spec-compliant output without
	// having to know the bridge's prerequisites.
	//
	// R2 + R4 (docs/research/3mf-export-rendering-artifacts.md):
	// JoinIdenticalVertices  — welds bit-identical duplicates within each
	//                          aiMesh (per JoinVerticesProcess.cpp), which
	//                          tightens vertex indices without merging meshes
	//                          (per-mesh material/color structure preserved).
	// FindDegenerates        — drops zero-area triangles before write so the
	//                          R3 bridge skip path doesn't need to defend
	//                          against runtime-introduced degeneracy.
	// FindInvalidData        — drops NaN/Inf coordinates and meshes with
	//                          degenerate AABBs (legitimate sub-mm CAD meshes
	//                          still have non-zero AABBs, so this is safe).
	exporters.emplace_back("3mf", "The 3MF-File-Format", "3mf", &ExportScene3MF,
		aiProcess_Triangulate
		| aiProcess_JoinIdenticalVertices
		| aiProcess_FindDegenerates
		| aiProcess_FindInvalidData);
#endif

#ifndef ASSIMP_BUILD_NO_PBRT_EXPORTER
	exporters.emplace_back("pbrt", "pbrt-v4 scene description file", "pbrt", &ExportScenePbrt, aiProcess_ConvertToLeftHanded | aiProcess_Triangulate | aiProcess_SortByPType);
#endif

#ifndef ASSIMP_BUILD_NO_ASSJSON_EXPORTER
	exporters.emplace_back("assjson", "Assimp JSON Document", "json", &ExportAssimp2Json, 0);
#endif

#ifndef ASSIMP_BUILD_NO_USD_EXPORTER
	exporters.emplace_back("usda", "Universal Scene Description (ASCII)", "usda", &ExportSceneUSDA, 0);
	exporters.emplace_back("usdc", "Universal Scene Description (Crate)", "usdc", &ExportSceneUSDC, 0);
	exporters.emplace_back("usdz", "Universal Scene Description (USDZ)", "usdz", &ExportSceneUSDZ, 0);
#endif
}

class ExporterPimpl {
public:
    ExporterPimpl()
    : blob()
    , mIOSystem(new Assimp::DefaultIOSystem())
    , mIsDefaultIOHandler(true)
    , mProgressHandler( nullptr )
    , mIsDefaultProgressHandler( true )
    , mPostProcessingSteps()
    , mError()
    , mExporters() {
        GetPostProcessingStepInstanceList(mPostProcessingSteps);

        // grab all built-in exporters
		setupExporterArray(mExporters);
    }

    ~ExporterPimpl() {
        delete blob;

        // Delete all post-processing plug-ins
        for( unsigned int a = 0; a < mPostProcessingSteps.size(); a++) {
            delete mPostProcessingSteps[a];
        }
        delete mProgressHandler;
    }

public:
    aiExportDataBlob* blob;
    std::shared_ptr< Assimp::IOSystem > mIOSystem;
    bool mIsDefaultIOHandler;

    /** The progress handler */
    ProgressHandler *mProgressHandler;
    bool mIsDefaultProgressHandler;

    /** Post processing steps we can apply at the imported data. */
    std::vector< BaseProcess* > mPostProcessingSteps;

    /** Last fatal export error */
    std::string mError;

    /** Exporters, this includes those registered using #Assimp::Exporter::RegisterExporter */
    std::vector<Exporter::ExportFormatEntry> mExporters;
};

} // end of namespace Assimp

using namespace Assimp;

// ------------------------------------------------------------------------------------------------
Exporter :: Exporter()
: pimpl(new ExporterPimpl()) {
    pimpl->mProgressHandler = new DefaultProgressHandler();
}

// ------------------------------------------------------------------------------------------------
Exporter::~Exporter() {
	ai_assert(nullptr != pimpl);
	FreeBlob();
    delete pimpl;
}

// ------------------------------------------------------------------------------------------------
void Exporter::SetIOHandler( IOSystem* pIOHandler) {
	ai_assert(nullptr != pimpl);
	pimpl->mIsDefaultIOHandler = !pIOHandler;
    pimpl->mIOSystem.reset(pIOHandler);
}

// ------------------------------------------------------------------------------------------------
IOSystem* Exporter::GetIOHandler() const {
	ai_assert(nullptr != pimpl);
	return pimpl->mIOSystem.get();
}

// ------------------------------------------------------------------------------------------------
bool Exporter::IsDefaultIOHandler() const {
	ai_assert(nullptr != pimpl);
	return pimpl->mIsDefaultIOHandler;
}

// ------------------------------------------------------------------------------------------------
void Exporter::SetProgressHandler(ProgressHandler* pHandler) {
    ai_assert(nullptr != pimpl);

    if ( nullptr == pHandler) {
        // Release pointer in the possession of the caller
        pimpl->mProgressHandler = new DefaultProgressHandler();
        pimpl->mIsDefaultProgressHandler = true;
        return;
    }

    if (pimpl->mProgressHandler == pHandler) {
        return;
    }

    delete pimpl->mProgressHandler;
    pimpl->mProgressHandler = pHandler;
    pimpl->mIsDefaultProgressHandler = false;
}

// ------------------------------------------------------------------------------------------------
const aiExportDataBlob* Exporter::ExportToBlob( const aiScene* pScene, const char* pFormatId,
                                                unsigned int pPreprocessing, const ExportProperties* pProperties) {
	ai_assert(nullptr != pimpl);
    if (pimpl->blob) {
        delete pimpl->blob;
        pimpl->blob = nullptr;
    }

    auto baseName = pProperties ? pProperties->GetPropertyString(AI_CONFIG_EXPORT_BLOB_NAME, AI_BLOBIO_MAGIC) : AI_BLOBIO_MAGIC;

    std::shared_ptr<IOSystem> old = pimpl->mIOSystem;
    BlobIOSystem *blobio = new BlobIOSystem(baseName);
    pimpl->mIOSystem = std::shared_ptr<IOSystem>( blobio );

    if (AI_SUCCESS != Export(pScene,pFormatId,blobio->GetMagicFileName(), pPreprocessing, pProperties)) {
        pimpl->mIOSystem = old;
        return nullptr;
    }

    pimpl->blob = blobio->GetBlobChain();
    pimpl->mIOSystem = old;

    return pimpl->blob;
}

// ------------------------------------------------------------------------------------------------
namespace {

// Users who build scenes from scratch will likely not set AI_SCENE_FLAGS_NON_VERBOSE_FORMAT, so a
// scene that claims to be indexed is checked for shared vertices. Only needed when a step runs.
bool IsVerboseScene(const aiScene *pScene) {
    return !(pScene->mFlags & AI_SCENE_FLAGS_NON_VERBOSE_FORMAT) || MakeVerboseFormatProcess::IsVerboseFormat(pScene);
}

// Exporters that never modify the scene they are handed, so they read the caller's scene when no
// post-processing has to run. The ones that bake the unit/axis contract into the meshes in place
// (`bakeContractTransformIntoMeshes`) stay read-only only while that bake is the identity for their
// target frame; the others apply any transform into temporaries or while writing.
struct ReadOnlyExporter {
    const char *id;
    bool bakesContract;
    double targetUnitToMeters;
    int32_t targetUpAxis;
};

const ReadOnlyExporter kReadOnlyExporters[] = {
    { "usda", false, 0.0, 0 },
    { "usdc", false, 0.0, 0 },
    { "usdz", false, 0.0, 0 },
    { "gltf2", false, 0.0, 0 },  // contract, normal normalisation and UV flip go into temporaries
    { "glb2", false, 0.0, 0 },
    { "stl", false, 0.0, 0 },    // node transforms, contract and face normals are applied while writing
    { "stlb", false, 0.0, 0 },
    { "assjson", false, 0.0, 0 }, // copies the scene itself only when meshes must be split
    { "stp", false, 0.0, 0 },
    { "3mf", false, 0.0, 0 },    // the lib3mf bridge bakes node transforms while it writes
    { "obj", true, 1.0, 1 },
    { "objnomtl", true, 1.0, 1 },
    { "ply", true, 1.0, 1 },
    { "plyb", true, 1.0, 1 },
    { "collada", true, 1.0, 1 },
    { "fbx", true, 1e-2, 1 },
    { "fbxa", true, 1e-2, 1 },
};

bool IsReadOnlyExport(const aiScene *pScene, const char *pFormatId) {
    for (const ReadOnlyExporter &entry : kReadOnlyExporters) {
        if (!strcmp(entry.id, pFormatId)) {
            return !entry.bakesContract ||
                   !resolveContractTransform(pScene, entry.targetUnitToMeters, entry.targetUpAxis).active;
        }
    }
    return false;
}

bool AllMeshes(const aiScene *pScene, bool (*predicate)(const aiMesh *)) {
    for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
        if (pScene->mMeshes[i] == nullptr || !predicate(pScene->mMeshes[i])) {
            return false;
        }
    }
    return true;
}

// True when PretransformVertices would hand the PLY writer the same vertex and face data: one mesh,
// referenced by exactly one node, whose world transform is the identity by the step's own
// `IsIdentity` test. (The step then copies positions, normals and tangents verbatim, and the PLY
// writer reads nothing it drops.)
bool IsPretransformIdentity(const aiScene *pScene) {
    if (pScene->mNumMeshes != 1 || pScene->mRootNode == nullptr) {
        return false;
    }
    unsigned int references = 0;
    std::vector<std::pair<const aiNode *, aiMatrix4x4>> stack;
    stack.emplace_back(pScene->mRootNode, pScene->mRootNode->mTransformation);
    while (!stack.empty()) {
        const aiNode *node = stack.back().first;
        const aiMatrix4x4 world = stack.back().second;
        stack.pop_back();
        if (node->mNumMeshes != 0) {
            references += node->mNumMeshes;
            if (references > 1 || !world.IsIdentity()) {
                return false;
            }
        }
        for (unsigned int c = 0; c < node->mNumChildren; ++c) {
            stack.emplace_back(node->mChildren[c], world * node->mChildren[c]->mTransformation);
        }
    }
    return references == 1;
}

// Drop enforced steps that provably leave this scene unchanged, so they neither force a scene copy
// nor un-index it for nothing.
unsigned int DropNoOpSteps(const aiScene *pScene, const char *pFormatId, unsigned int pp, const ExportProperties *pProperties) {
    // Normal generation never replaces existing normals unless forced.
    if ((pp & (aiProcess_GenNormals | aiProcess_GenSmoothNormals)) && !(pp & aiProcess_ForceGenNormals) &&
            AllMeshes(pScene, [](const aiMesh *m) { return m->HasNormals(); })) {
        pp &= ~(aiProcess_GenNormals | aiProcess_GenSmoothNormals);
    }
    // Triangulation only touches meshes that declare polygons.
    if ((pp & aiProcess_Triangulate) && AllMeshes(pScene, [](const aiMesh *m) {
            return m->mPrimitiveTypes != 0 && !(m->mPrimitiveTypes & aiPrimitiveType_POLYGON);
        })) {
        pp &= ~aiProcess_Triangulate;
    }
    // Sorting by primitive type only splits meshes that mix types.
    if ((pp & aiProcess_SortByPType) && AllMeshes(pScene, [](const aiMesh *m) {
            const unsigned int types = m->mPrimitiveTypes & (aiPrimitiveType_POINT | aiPrimitiveType_LINE |
                                                              aiPrimitiveType_TRIANGLE | aiPrimitiveType_POLYGON);
            return types != 0 && (types & (types - 1)) == 0;
        })) {
        pp &= ~aiProcess_SortByPType;
    }
    // The PLY writer reads only mesh arrays, so pre-transforming a lone, untransformed mesh is a copy.
    const bool pointCloud = pProperties != nullptr && pProperties->GetPropertyBool(AI_CONFIG_EXPORT_POINT_CLOUDS);
    if ((pp & aiProcess_PreTransformVertices) && !pointCloud &&
            (!strcmp(pFormatId, "ply") || !strcmp(pFormatId, "plyb")) && IsPretransformIdentity(pScene)) {
        pp &= ~aiProcess_PreTransformVertices;
    }
    return pp;
}

} // namespace

// ------------------------------------------------------------------------------------------------
aiReturn Exporter::Export( const aiScene* pScene, const char* pFormatId, const char* pPath,
        unsigned int pPreprocessing, const ExportProperties* pProperties) {
    ASSIMP_BEGIN_EXCEPTION_REGION();
	ai_assert(nullptr != pimpl);
    
    if (!pScene) {
        pimpl->mError = "Export failed: Scene is null";
        return AI_FAILURE;
    }
    
    pimpl->mError = "";
    const auto updateProgress = [this](int step) {
        if (!pimpl->mProgressHandler->UpdateFileWrite(step, 4)) {
            pimpl->mError = "Export cancelled by progress handler";
            return false;
        }
        return true;
    };
    if (!updateProgress(0)) {
        return AI_FAILURE;
    }

    for (size_t i = 0; i < pimpl->mExporters.size(); ++i) {
        const Exporter::ExportFormatEntry& exp = pimpl->mExporters[i];
        if (!strcmp(exp.mDescription.id,pFormatId)) {
            try {
                const ScenePrivateData* const priv = ScenePriv(pScene);

                if (!strcmp(pFormatId, "3mf") && priv && !priv->mManifoldMeshes.empty()) {
                    const unsigned int safeManifoldSteps =
                            aiProcess_CalcTangentSpace |
                            aiProcess_JoinIdenticalVertices |
                            aiProcess_GenNormals |
                            aiProcess_GenSmoothNormals |
                            aiProcess_Triangulate |
                            aiProcess_GenUVCoords |
                            aiProcess_FindDegenerates |
                            aiProcess_FindInvalidData |
                            aiProcess_ImproveCacheLocality |
                            aiProcess_SortByPType |
                            aiProcess_TransformUVCoords |
                            aiProcess_FlipUVs |
                            aiProcess_ValidateDataStructure;
                    const unsigned int unsafeSteps =
                            (priv->mPPStepsApplied | pPreprocessing) & ~safeManifoldSteps;
                    if (unsafeSteps != 0) {
                        throw DeadlyExportError(
                                "3MF export cannot preserve EXT_mesh_manifold after topology-changing "
                                "post-processing flags: ", unsafeSteps,
                                " (source mesh ",
                                priv->mManifoldMeshes.front().mSourceMeshIndex, ")");
                    }
                }

                // steps that are not idempotent, i.e. we might need to run them again, usually to get back to the
                // original state before the step was applied first. When checking which steps we don't need
                // to run, those are excluded.
                const unsigned int nonIdempotentSteps = aiProcess_FlipWindingOrder | aiProcess_FlipUVs | aiProcess_MakeLeftHanded;

                // Erase all pp steps that were already applied to this scene
                unsigned int pp = (exp.mEnforcePP | pPreprocessing) & ~(priv && !priv->mIsCopy
                    ? (priv->mPPStepsApplied & ~nonIdempotentSteps)
                    : 0u);
                pp = DropNoOpSteps(pScene, pFormatId, pp, pProperties);

                // Post-processing works on a copy. Exporters that only read the scene
                // export the caller's scene directly when nothing has to change, which
                // saves a full copy of every mesh (the dominant cost for large scenes).
                const bool needsCopy = pp != 0 || !IsReadOnlyExport(pScene, pFormatId);
                std::unique_ptr<aiScene> scenecopy;
                if (needsCopy) {
                    aiScene* scenecopy_tmp = nullptr;
                    SceneCombiner::CopyScene(&scenecopy_tmp,pScene);
                    scenecopy.reset(scenecopy_tmp);
                }
                if (!updateProgress(1)) {
                    return AI_FAILURE;
                }

                // If no extra post-processing was specified, and we obtained this scene from an
                // Assimp importer, apply the reverse steps automatically.
                // TODO: either drop this, or document it. Otherwise it is just a bad surprise.
                //if (!pPreprocessing && priv) {
                //  pp |= (nonIdempotentSteps & priv->mPPStepsApplied);
                //}

                // If the input scene is not in verbose format, but there is at least post-processing step that relies on it,
                // we need to run the MakeVerboseFormat step first. Only an active step justifies un-indexing the
                // copy: JoinIdenticalVertices enforced by the exporter but already applied by the importer is not
                // active, and the scene is already in the joined form the exporter asks for. When the copy is
                // un-indexed for another step, it is joined again afterwards unless JoinIdenticalVertices is itself
                // one of the active steps (it runs last in the registry).
                bool must_join_again = false;
                if (pp != 0 && !IsVerboseScene(pScene)) {
                    bool verbosify = false;
                    for( unsigned int a = 0; a < pimpl->mPostProcessingSteps.size(); a++) {
                        BaseProcess* const p = pimpl->mPostProcessingSteps[a];

                        if (p->IsActive(pp) && p->RequireVerboseFormat()) {
                            verbosify = true;
                            break;
                        }
                    }

                    if (verbosify) {
                        ASSIMP_LOG_DEBUG("export: Scene data not in verbose format, applying MakeVerboseFormat step first");

                        MakeVerboseFormatProcess proc;
                        proc.Execute(scenecopy.get());

                        if (!(pp & aiProcess_JoinIdenticalVertices)) {
                            must_join_again = true;
                        }
                    }
                }

                if (!updateProgress(2)) {
                    return AI_FAILURE;
                }

                if (pp) {
                    // the three 'conversion' steps need to be executed first because all other steps rely on the standard data layout
                    {
                        FlipWindingOrderProcess step;
                        if (step.IsActive(pp)) {
                            step.Execute(scenecopy.get());
                        }
                    }

                    {
                        FlipUVsProcess step;
                        if (step.IsActive(pp)) {
                            step.Execute(scenecopy.get());
                        }
                    }

                    {
                        MakeLeftHandedProcess step;
                        if (step.IsActive(pp)) {
                            step.Execute(scenecopy.get());
                        }
                    }

                    bool exportPointCloud(false);
                    if (nullptr != pProperties) {
                        exportPointCloud = pProperties->GetPropertyBool(AI_CONFIG_EXPORT_POINT_CLOUDS);
                    }

                    // dispatch other processes
                    for( unsigned int a = 0; a < pimpl->mPostProcessingSteps.size(); a++) {
                        BaseProcess* const p = pimpl->mPostProcessingSteps[a];

                        if (p->IsActive(pp)
                            && !dynamic_cast<FlipUVsProcess*>(p)
                            && !dynamic_cast<FlipWindingOrderProcess*>(p)
                            && !dynamic_cast<MakeLeftHandedProcess*>(p)) {
                            if (dynamic_cast<PretransformVertices*>(p) && exportPointCloud) {
                                continue;
                            }
                            p->Execute(scenecopy.get());
                        }
                    }
                    ScenePrivateData* const privOut = ScenePriv(scenecopy.get());
                    ai_assert(nullptr != privOut);

                    privOut->mPPStepsApplied |= pp;
                }

                if (!updateProgress(3)) {
                    return AI_FAILURE;
                }

                if(must_join_again) {
                    JoinVerticesProcess proc;
                    proc.Execute(scenecopy.get());
                }

                ExportProperties emptyProperties;  // Never pass nullptr ExportProperties so Exporters don't have to worry.
                ExportProperties* pProp = pProperties ? (ExportProperties*)pProperties : &emptyProperties;
        		pProp->SetPropertyBool("bJoinIdenticalVertices", pp & aiProcess_JoinIdenticalVertices);
                exp.mExportFunction(pPath,pimpl->mIOSystem.get(),scenecopy ? scenecopy.get() : pScene, pProp);

                // The writer may already have committed output; this is notification only.
                pimpl->mProgressHandler->UpdateFileWrite(4, 4);
            } catch (DeadlyExportError& err) {
                pimpl->mError = err.what();
                return AI_FAILURE;
            }
            return AI_SUCCESS;
        }
    }

    pimpl->mError = std::string("Found no exporter to handle this file format: ") + pFormatId;
    ASSIMP_END_EXCEPTION_REGION(aiReturn);

    return AI_FAILURE;
}

// ------------------------------------------------------------------------------------------------
const char* Exporter::GetErrorString() const {
	ai_assert(nullptr != pimpl);
    return pimpl->mError.c_str();
}

// ------------------------------------------------------------------------------------------------
void Exporter::FreeBlob() {
	ai_assert(nullptr != pimpl);
    delete pimpl->blob;
    pimpl->blob = nullptr;

    pimpl->mError = "";
}

// ------------------------------------------------------------------------------------------------
const aiExportDataBlob* Exporter::GetBlob() const {
	ai_assert(nullptr != pimpl);
	return pimpl->blob;
}

// ------------------------------------------------------------------------------------------------
const aiExportDataBlob* Exporter::GetOrphanedBlob() const {
	ai_assert(nullptr != pimpl);
	const aiExportDataBlob *tmp = pimpl->blob;
    pimpl->blob = nullptr;
    return tmp;
}

// ------------------------------------------------------------------------------------------------
size_t Exporter::GetExportFormatCount() const {
	ai_assert(nullptr != pimpl);
    return pimpl->mExporters.size();
}

// ------------------------------------------------------------------------------------------------
const aiExportFormatDesc* Exporter::GetExportFormatDescription( size_t index ) const {
	ai_assert(nullptr != pimpl);
	if (index >= GetExportFormatCount()) {
        return nullptr;
    }

    // Return from static storage if the requested index is built-in.
	if (index < pimpl->mExporters.size()) {
		return &pimpl->mExporters[index].mDescription;
    }

    return &pimpl->mExporters[index].mDescription;
}

// ------------------------------------------------------------------------------------------------
aiReturn Exporter::RegisterExporter(const ExportFormatEntry& desc) {
	ai_assert(nullptr != pimpl);
	for (const ExportFormatEntry &e : pimpl->mExporters) {
        if (!strcmp(e.mDescription.id,desc.mDescription.id)) {
            return aiReturn_FAILURE;
        }
    }

    pimpl->mExporters.push_back(desc);
    return aiReturn_SUCCESS;
}

// ------------------------------------------------------------------------------------------------
void Exporter::UnregisterExporter(const char* id) {
	ai_assert(nullptr != pimpl);
	for (std::vector<ExportFormatEntry>::iterator it = pimpl->mExporters.begin();
            it != pimpl->mExporters.end(); ++it) {
        if (!strcmp((*it).mDescription.id,id)) {
            pimpl->mExporters.erase(it);
            break;
        }
    }
}

// ------------------------------------------------------------------------------------------------
ExportProperties::ExportProperties() = default;

// ------------------------------------------------------------------------------------------------
ExportProperties::ExportProperties(const ExportProperties &other) = default;

bool ExportProperties::SetPropertyCallback(const char *szName, const std::function<void *(void *)> &f) {
    return SetGenericProperty<std::function<void *(void *)>>(mCallbackProperties, szName, f);
}

std::function<void *(void *)> ExportProperties::GetPropertyCallback(const char *szName) const {
    return GetGenericProperty<std::function<void *(void *)>>(mCallbackProperties, szName, nullptr);
}

bool ExportProperties::HasPropertyCallback(const char *szName) const {
    return HasGenericProperty<std::function<void *(void *)>>(mCallbackProperties, szName);
}

// ------------------------------------------------------------------------------------------------
// Set a configuration property
bool ExportProperties::SetPropertyInteger(const char* szName, int iValue) {
    return SetGenericProperty<int>(mIntProperties, szName,iValue);
}

// ------------------------------------------------------------------------------------------------
// Set a configuration property
bool ExportProperties::SetPropertyFloat(const char* szName, ai_real iValue) {
    return SetGenericProperty<ai_real>(mFloatProperties, szName,iValue);
}

// ------------------------------------------------------------------------------------------------
// Set a configuration property
bool ExportProperties::SetPropertyString(const char* szName, const std::string& value) {
    return SetGenericProperty<std::string>(mStringProperties, szName,value);
}

// ------------------------------------------------------------------------------------------------
// Set a configuration property
bool ExportProperties::SetPropertyMatrix(const char* szName, const aiMatrix4x4& value) {
    return SetGenericProperty<aiMatrix4x4>(mMatrixProperties, szName,value);
}

// ------------------------------------------------------------------------------------------------
// Get a configuration property
int ExportProperties::GetPropertyInteger(const char* szName, int iErrorReturn /*= 0xffffffff*/) const {
    return GetGenericProperty<int>(mIntProperties,szName,iErrorReturn);
}

// ------------------------------------------------------------------------------------------------
// Get a configuration property
ai_real ExportProperties::GetPropertyFloat(const char* szName, ai_real iErrorReturn /*= 10e10*/) const {
    return GetGenericProperty<ai_real>(mFloatProperties,szName,iErrorReturn);
}

// ------------------------------------------------------------------------------------------------
// Get a configuration property
const std::string ExportProperties::GetPropertyString(const char* szName,
        const std::string& iErrorReturn /*= ""*/) const {
    return GetGenericProperty<std::string>(mStringProperties,szName,iErrorReturn);
}

// ------------------------------------------------------------------------------------------------
// Has a configuration property
const aiMatrix4x4 ExportProperties::GetPropertyMatrix(const char* szName,
        const aiMatrix4x4& iErrorReturn /*= aiMatrix4x4()*/) const {
    return GetGenericProperty<aiMatrix4x4>(mMatrixProperties,szName,iErrorReturn);
}

// ------------------------------------------------------------------------------------------------
// Has a configuration property
bool ExportProperties::HasPropertyInteger(const char* szName) const {
    return HasGenericProperty<int>(mIntProperties, szName);
}

// ------------------------------------------------------------------------------------------------
// Has a configuration property
bool ExportProperties::HasPropertyBool(const char* szName) const {
    return HasGenericProperty<int>(mIntProperties, szName);
}

// ------------------------------------------------------------------------------------------------
// Has a configuration property
bool ExportProperties::HasPropertyFloat(const char* szName) const {
    return HasGenericProperty<ai_real>(mFloatProperties, szName);
}

// ------------------------------------------------------------------------------------------------
// Has a configuration property
bool ExportProperties::HasPropertyString(const char* szName) const {
    return HasGenericProperty<std::string>(mStringProperties, szName);
}

// ------------------------------------------------------------------------------------------------
// Has a configuration property
bool ExportProperties::HasPropertyMatrix(const char* szName) const {
    return HasGenericProperty<aiMatrix4x4>(mMatrixProperties, szName);
}


#endif // !ASSIMP_BUILD_NO_EXPORT
