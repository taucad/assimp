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
#if !defined(ASSIMP_BUILD_NO_EXPORT) && !defined(ASSIMP_BUILD_NO_STL_EXPORTER)

#include "STLExporter.h"
#include "Common/IOStreamBuf.h"
#include "Common/UnitAxisContract.h"
#include <assimp/version.h>
#include <assimp/IOSystem.hpp>
#include <assimp/scene.h>
#include <assimp/Exporter.hpp>
#include <memory>
#include <utility>
#include <vector>
#include <assimp/Exceptional.h>
#include <assimp/ByteSwapper.h>

using namespace Assimp;

namespace Assimp {

namespace {
// STL is technically unitless, but the de-facto 3D-printing toolchain
// (Bambu, Cura, PrusaSlicer, MeshLab, etc.) treats STL as millimetres + Z-up.
// Bake the contract transform (vertices + normals) so files authored in any
// other frame land on the slicer bed at correct scale/orientation. Importers
// that haven't adopted the contract still write byte-identical output because
// `bakeContractTransformIntoMeshes` short-circuits when
// `AI_METADATA_UNIT_SCALE_TO_METERS` is absent (the opt-in gate).
constexpr double kStlTargetUnitToMeters = 1e-3; // millimetres
constexpr int32_t kStlTargetUpAxis = 2; // Z-up

// The contract transform is applied while writing; the scene is never modified.
ContractTransform stlContract(const aiScene *pScene) {
    return resolveContractTransform(pScene, kStlTargetUnitToMeters, kStlTargetUpAxis);
}
} // namespace

// ------------------------------------------------------------------------------------------------
// Worker function for exporting a scene to Stereolithograpy. Prototyped and registered in Exporter.cpp
void ExportSceneSTL(const char* pFile,IOSystem* pIOSystem, const aiScene* pScene, const ExportProperties* pProperties )
{
    bool exportPointClouds = pProperties->GetPropertyBool(AI_CONFIG_EXPORT_POINT_CLOUDS);

    std::unique_ptr<IOStream> outfile (pIOSystem->Open(pFile,"wt"));
    if (outfile == nullptr) {
        throw DeadlyExportError("could not open output .stl file: " + std::string(pFile));
    }
    IOStreamBuf outbuf(outfile.get());

    // invoke the exporter, streaming into the output file
    STLExporter exporter(&outbuf, pFile, pScene, exportPointClouds, false, stlContract(pScene));
    exporter.mOutput.flush();
    if (exporter.mOutput.fail() || outbuf.failed()) {
        throw DeadlyExportError("output data creation failed. Most likely the file became too large: " + std::string(pFile));
    }
}

void ExportSceneSTLBinary(const char* pFile,IOSystem* pIOSystem, const aiScene* pScene, const ExportProperties* pProperties )
{
    bool exportPointClouds = pProperties->GetPropertyBool(AI_CONFIG_EXPORT_POINT_CLOUDS);

    std::unique_ptr<IOStream> outfile (pIOSystem->Open(pFile,"wb"));
    if (outfile == nullptr) {
        throw DeadlyExportError("could not open output .stl file: " + std::string(pFile));
    }
    IOStreamBuf outbuf(outfile.get());

    // invoke the exporter, streaming into the output file
    STLExporter exporter(&outbuf, pFile, pScene, exportPointClouds, true, stlContract(pScene));
    exporter.mOutput.flush();
    if (exporter.mOutput.fail() || outbuf.failed()) {
        throw DeadlyExportError("output data creation failed. Most likely the file became too large: " + std::string(pFile));
    }
}

} // end of namespace Assimp

static constexpr char SolidToken[]    = "solid";
static constexpr char EndSolidToken[] = "endsolid";

// ------------------------------------------------------------------------------------------------
namespace {
// Every placement of a mesh in the node graph, in depth-first pre-order, with its world transform:
// exactly the meshes (and the order) PretransformVertices produced for this exporter before.
void collectInstances(const aiNode *root, std::vector<STLExporter::Instance> &out) {
    std::vector<std::pair<const aiNode *, aiMatrix4x4>> stack;
    stack.emplace_back(root, root->mTransformation);
    while (!stack.empty()) {
        const aiNode *node = stack.back().first;
        const aiMatrix4x4 world = stack.back().second;
        stack.pop_back();
        for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
            STLExporter::Instance instance;
            instance.mesh = node->mMeshes[i];
            instance.world = world;
            instance.identity = world.IsIdentity();
            aiMatrix4x4 normalMatrix = world;
            normalMatrix.Inverse().Transpose();
            instance.normal = aiMatrix3x3(normalMatrix);
            out.push_back(instance);
        }
        for (unsigned int c = node->mNumChildren; c-- > 0;) {
            const aiNode *child = node->mChildren[c];
            stack.emplace_back(child, world * child->mTransformation);
        }
    }
}
} // namespace

STLExporter::STLExporter(std::streambuf* out, const char* _filename, const aiScene* pScene, bool exportPointClouds, bool binary,
        const ContractTransform &contract) : mOutput(out), filename(_filename) , endl("\n"), mContract(contract)
{
    if (pScene->mRootNode != nullptr) {
        collectInstances(pScene->mRootNode, mInstances);
    } else {
        for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
            Instance instance;
            instance.mesh = i;
            mInstances.push_back(instance);
        }
    }
    for (const Instance &instance : mInstances) {
        if (instance.mesh >= pScene->mNumMeshes || pScene->mMeshes[instance.mesh] == nullptr) {
            throw DeadlyExportError("STL: a node references a mesh that does not exist");
        }
    }
    // make sure that all formatting happens using the standard, C locale and not the user's current locale
    const std::locale& l = std::locale("C");
    mOutput.imbue(l);
    mOutput.precision(ASSIMP_AI_REAL_TEXT_PRECISION);
    if (binary) {
        char buf[80] = {0} ;
        buf[0] = 'A'; buf[1] = 's'; buf[2] = 's'; buf[3] = 'i'; buf[4] = 'm'; buf[5] = 'p';
        buf[6] = 'S'; buf[7] = 'c'; buf[8] = 'e'; buf[9] = 'n'; buf[10] = 'e';
        mOutput.write(buf, 80);
        unsigned int meshnum = 0;
        for (const Instance &instance : mInstances) {
            const aiMesh *m = pScene->mMeshes[instance.mesh];
            for (unsigned int j = 0; j < m->mNumFaces; ++j) {
                if (m->mFaces[j].mNumIndices >= 3) {
                    meshnum++;
                }
            }
        }
        AI_SWAP4(meshnum);
        mOutput.write((char *)&meshnum, 4);

        if (exportPointClouds) {
            throw DeadlyExportError("This functionality is not yet implemented for binary output.");
        }

        for (const Instance &instance : mInstances) {
            WriteMeshBinary(pScene->mMeshes[instance.mesh], instance);
        }
    } else {

        // Exporting only point clouds
        if (exportPointClouds) {
            WritePointCloud("Assimp_Pointcloud", pScene );
            return;
        }

        // Export the assimp mesh
        const std::string name = "AssimpScene";
        mOutput << SolidToken << " " << name << endl;
        for (const Instance &instance : mInstances) {
            WriteMesh(pScene->mMeshes[instance.mesh], instance);
        }
        mOutput << EndSolidToken << " " << name << endl;
    }
}

// ------------------------------------------------------------------------------------------------
void STLExporter::WritePointCloud(const std::string &name, const aiScene* pScene) {
    mOutput << " " << SolidToken << " " << name << endl;
    aiVector3D nor;
    mOutput << " facet normal " << nor.x << " " << nor.y << " " << nor.z << endl;
    for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
        aiMesh *mesh = pScene->mMeshes[i];
        if (nullptr == mesh) {
            continue;
        }

        for (unsigned int a = 0; a < mesh->mNumVertices; ++a) {
            aiVector3D v = mesh->mVertices[a];
            mContract.applyToPoint(v);
            mOutput << "  vertex " << v.x << " " << v.y << " " << v.z << endl;
            mOutput << "  vertex " << v.x << " " << v.y << " " << v.z << endl;
            mOutput << "  vertex " << v.x << " " << v.y << " " << v.z << endl;
        }
    }
    mOutput << EndSolidToken << " " << name << endl;
}

// ------------------------------------------------------------------------------------------------
aiVector3D STLExporter::Position(const aiMesh *m, unsigned int index, const Instance &instance) const {
    aiVector3D v = m->mVertices[index];
    if (!instance.identity) {
        v = instance.world * v;
    }
    mContract.applyToPoint(v);
    return v;
}

// The arithmetic of the pipeline this replaces, step by step, so outputs keep their bytes:
// PretransformVertices (normals through the inverse-transpose, normalized), GenNormals for meshes
// without normals (the flat normal of the pre-transformed face, NormalizeSafe), the contract bake
// (rotation), then the per-face average this writer always took.
aiVector3D STLExporter::FaceNormal(const aiMesh *m, const aiFace &f, const Instance &instance, bool safe) const {
    aiVector3D nor;
    if (m->mNormals) {
        for (unsigned int a = 0; a < f.mNumIndices; ++a) {
            aiVector3D n = m->mNormals[f.mIndices[a]];
            if (!instance.identity) {
                n = (instance.normal * n).Normalize();
            }
            mContract.applyToDirection(n);
            nor += n;
        }
    } else {
        const auto world = [&](unsigned int index) {
            const aiVector3D &v = m->mVertices[index];
            return instance.identity ? v : instance.world * v;
        };
        const aiVector3D p0 = world(f.mIndices[0]);
        const aiVector3D p1 = world(f.mIndices[1]);
        const aiVector3D p2 = world(f.mIndices[f.mNumIndices - 1]);
        aiVector3D faceNormal = ((p1 - p0) ^ (p2 - p0)).NormalizeSafe();
        mContract.applyToDirection(faceNormal);
        for (unsigned int a = 0; a < f.mNumIndices; ++a) {
            nor += faceNormal;
        }
    }
    if (safe) {
        nor.NormalizeSafe();
    } else {
        nor.Normalize();
    }
    return nor;
}

void STLExporter::WriteMesh(const aiMesh* m, const Instance &instance) {
    for (unsigned int i = 0; i < m->mNumFaces; ++i) {
        const aiFace& f = m->mFaces[i];
        if (f.mNumIndices < 3) {
            continue;
        }

        const aiVector3D nor = FaceNormal(m, f, instance, true);
        mOutput << " facet normal " << nor.x << " " << nor.y << " " << nor.z << endl;
        mOutput << "  outer loop" << endl;
        for (unsigned int a = 0; a < f.mNumIndices; ++a) {
            const aiVector3D v = Position(m, f.mIndices[a], instance);
            mOutput << "  vertex " << v.x << " " << v.y << " " << v.z << endl;
        }

        mOutput << "  endloop" << endl;
        mOutput << " endfacet" << endl << endl;
    }
}

void STLExporter::WriteMeshBinary(const aiMesh* m, const Instance &instance) {
    for (unsigned int i = 0; i < m->mNumFaces; ++i) {
        const aiFace& f = m->mFaces[i];
        if (f.mNumIndices < 3) {
            continue;
        }

        const aiVector3D nor = FaceNormal(m, f, instance, false);
        // STL binary files use 4-byte floats. This may possibly cause loss of precision
        // for clients using 8-byte doubles
        float nx = (float) nor.x;
        float ny = (float) nor.y;
        float nz = (float) nor.z;
        AI_SWAP4(nx); AI_SWAP4(ny); AI_SWAP4(nz);
        mOutput.write((char *)&nx, 4); mOutput.write((char *)&ny, 4); mOutput.write((char *)&nz, 4);
        for(unsigned int a = 0; a < f.mNumIndices; ++a) {
            const aiVector3D v = Position(m, f.mIndices[a], instance);
            float vx = (float) v.x, vy = (float) v.y, vz = (float) v.z;
            AI_SWAP4(vx); AI_SWAP4(vy); AI_SWAP4(vz);
            mOutput.write((char *)&vx, 4); mOutput.write((char *)&vy, 4); mOutput.write((char *)&vz, 4);
        }
        char dummy[2] = {0};
        mOutput.write(dummy, 2);
    }
}

#endif
