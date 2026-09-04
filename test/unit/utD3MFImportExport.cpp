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
#include "AbstractImportExportBase.h"
#include "Common/ScenePrivate.h"
#include "Common/UnitAxisContract.h"
#include "UnitTestPCH.h"

#include <assimp/Exceptional.h>
#include <assimp/DefaultIOSystem.h>
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/SceneCombiner.h>
#include <assimp/commonMetaData.h>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#ifdef ASSIMP_USE_LIB3MF
#include "AssetLib/3MF/Lib3MFBridge.h"
#include <lib3mf_abi.hpp>
#include <lib3mf_types.hpp>
#endif

#include <memory>
#include <string>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

namespace {

// Author a scene that owns its mesh, material, root node, and (optionally) metadata.
// The returned scene must be passed by raw pointer to Assimp::Exporter::Export and is
// owned by the caller — delete with `delete` once the export completes.
aiScene *makeSingleMeshScene(const std::vector<aiVector3D> &vertices,
                             const std::vector<std::array<unsigned int, 3>> &triangles) {
    auto *scene = new aiScene();

    scene->mNumMaterials = 1;
    scene->mMaterials = new aiMaterial *[1];
    scene->mMaterials[0] = new aiMaterial();

    scene->mNumMeshes = 1;
    scene->mMeshes = new aiMesh *[1];

    auto *mesh = new aiMesh();
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh->mMaterialIndex = 0;
    mesh->mName = aiString("AuthoredMesh");

    mesh->mNumVertices = static_cast<unsigned int>(vertices.size());
    mesh->mVertices = new aiVector3D[vertices.size()];
    for (size_t i = 0; i < vertices.size(); ++i) {
        mesh->mVertices[i] = vertices[i];
    }

    mesh->mNumFaces = static_cast<unsigned int>(triangles.size());
    mesh->mFaces = new aiFace[triangles.size()];
    for (size_t i = 0; i < triangles.size(); ++i) {
        mesh->mFaces[i].mNumIndices = 3;
        mesh->mFaces[i].mIndices = new unsigned int[3];
        mesh->mFaces[i].mIndices[0] = triangles[i][0];
        mesh->mFaces[i].mIndices[1] = triangles[i][1];
        mesh->mFaces[i].mIndices[2] = triangles[i][2];
    }

    scene->mMeshes[0] = mesh;

    scene->mRootNode = new aiNode();
    scene->mRootNode->mName = aiString("Root");
    scene->mRootNode->mNumMeshes = 1;
    scene->mRootNode->mMeshes = new unsigned int[1];
    scene->mRootNode->mMeshes[0] = 0;

    return scene;
}

// Axis-aligned box centred at origin with the given half-extents (8 unique verts, 12 tris).
aiScene *makeBoxScene(const aiVector3D &halfExtents) {
    const float x = halfExtents.x, y = halfExtents.y, z = halfExtents.z;
    std::vector<aiVector3D> v = {
        { -x, -y, -z }, { +x, -y, -z }, { +x, +y, -z }, { -x, +y, -z },
        { -x, -y, +z }, { +x, -y, +z }, { +x, +y, +z }, { -x, +y, +z }
    };
    std::vector<std::array<unsigned int, 3>> t = {
        { 0, 2, 1 }, { 0, 3, 2 }, // -Z
        { 4, 5, 6 }, { 4, 6, 7 }, // +Z
        { 0, 1, 5 }, { 0, 5, 4 }, // -Y
        { 3, 7, 6 }, { 3, 6, 2 }, // +Y
        { 0, 4, 7 }, { 0, 7, 3 }, // -X
        { 1, 2, 6 }, { 1, 6, 5 }  // +X
    };
    return makeSingleMeshScene(v, t);
}

// Per-face-expanded box: each of the 6 quads contributes 4 fresh duplicate vertices
// (24 total) so a vertex-position welder must collapse them down to 8 unique positions.
aiScene *makePerFaceBoxScene(const aiVector3D &halfExtents) {
    const float x = halfExtents.x, y = halfExtents.y, z = halfExtents.z;
    std::vector<aiVector3D> v;
    std::vector<std::array<unsigned int, 3>> t;

    auto pushQuad = [&](const aiVector3D &a, const aiVector3D &b,
                        const aiVector3D &c, const aiVector3D &d) {
        unsigned int base = static_cast<unsigned int>(v.size());
        v.push_back(a); v.push_back(b); v.push_back(c); v.push_back(d);
        t.push_back({ base + 0, base + 1, base + 2 });
        t.push_back({ base + 0, base + 2, base + 3 });
    };

    pushQuad({ -x, -y, -z }, { -x, +y, -z }, { +x, +y, -z }, { +x, -y, -z }); // -Z
    pushQuad({ -x, -y, +z }, { +x, -y, +z }, { +x, +y, +z }, { -x, +y, +z }); // +Z
    pushQuad({ -x, -y, -z }, { +x, -y, -z }, { +x, -y, +z }, { -x, -y, +z }); // -Y
    pushQuad({ -x, +y, -z }, { -x, +y, +z }, { +x, +y, +z }, { +x, +y, -z }); // +Y
    pushQuad({ -x, -y, -z }, { -x, -y, +z }, { -x, +y, +z }, { -x, +y, -z }); // -X
    pushQuad({ +x, -y, -z }, { +x, +y, -z }, { +x, +y, +z }, { +x, -y, +z }); // +X

    return makeSingleMeshScene(v, t);
}

aiVector3D meshMin(const aiMesh *mesh) {
    aiVector3D m(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max());
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        m.x = std::min(m.x, mesh->mVertices[i].x);
        m.y = std::min(m.y, mesh->mVertices[i].y);
        m.z = std::min(m.z, mesh->mVertices[i].z);
    }
    return m;
}

aiVector3D meshMax(const aiMesh *mesh) {
    aiVector3D m(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                 std::numeric_limits<float>::lowest());
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        m.x = std::max(m.x, mesh->mVertices[i].x);
        m.y = std::max(m.y, mesh->mVertices[i].y);
        m.z = std::max(m.z, mesh->mVertices[i].z);
    }
    return m;
}

aiVector3D meshExtent(const aiMesh *mesh) {
    aiVector3D mx = meshMax(mesh);
    aiVector3D mn = meshMin(mesh);
    return aiVector3D(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
}

#ifdef ASSIMP_USE_LIB3MF
bool findGlobalTransformForMesh(const aiNode *node, unsigned int meshIndex,
        const aiMatrix4x4 &parent, aiMatrix4x4 &result) {
    const aiMatrix4x4 global = parent * node->mTransformation;
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        if (node->mMeshes[i] == meshIndex) {
            result = global;
            return true;
        }
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        if (findGlobalTransformForMesh(node->mChildren[i], meshIndex, global, result)) {
            return true;
        }
    }
    return false;
}
#endif

#ifdef ASSIMP_USE_LIB3MF
void addManifoldTopologyForMesh(aiScene *scene, unsigned int meshIndex) {
    const aiMesh *mesh = scene->mMeshes[meshIndex];
    Assimp::ManifoldMeshTopology topology;
    topology.mSourceMeshIndex = meshIndex;
    topology.mPositions.assign(mesh->mVertices, mesh->mVertices + mesh->mNumVertices);
    for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
        const aiFace &face = mesh->mFaces[faceIndex];
        if (face.mNumIndices != 3) {
            continue;
        }
        topology.mIndices.insert(
                topology.mIndices.end(), face.mIndices, face.mIndices + 3);
    }
    topology.mRuns.push_back({
            meshIndex, mesh->mMaterialIndex, 0, topology.mIndices.size()
    });
    Assimp::ScenePriv(scene)->mManifoldMeshes.push_back(std::move(topology));
}

std::vector<uint8_t> readBytes(const char *path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<uint8_t>(
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>());
}
#endif

} // namespace

class utD3MFImporterExporter : public AbstractImportExportBase {
public:
    bool importerTest() override {
        Assimp::Importer importer;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", aiProcess_ValidateDataStructure);
        return (nullptr != scene);
    }

#ifndef ASSIMP_BUILD_NO_EXPORT
    bool exporterTest() override {
        Assimp::Importer importer;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
        if (!scene) {
            return false;
        }
        Assimp::Exporter exporter;
        aiReturn result = exporter.Export(scene, "3mf", "ut_3mf_test_export.3mf");
        std::remove("ut_3mf_test_export.3mf");
        return AI_SUCCESS == result;
    }
#endif
};

// ===== IMPORT TESTS =====

TEST_F(utD3MFImporterExporter, import3MFFromFileTest) {
    EXPECT_TRUE(importerTest());
}

TEST_F(utD3MFImporterExporter, import3MFBoxGeometry) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    ASSERT_GE(scene->mNumMeshes, 1u);

    const aiMesh *mesh = scene->mMeshes[0];
    ASSERT_NE(nullptr, mesh);
    EXPECT_EQ(8u, mesh->mNumVertices);
    EXPECT_EQ(12u, mesh->mNumFaces);

    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        EXPECT_EQ(3u, mesh->mFaces[f].mNumIndices);
    }
}

TEST_F(utD3MFImporterExporter, import3MFHasRootNode) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mRootNode);
}

TEST_F(utD3MFImporterExporter, import3MFHasMaterial) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    EXPECT_GE(scene->mNumMaterials, 1u);
}

// ===== EXPORT TESTS =====

#ifndef ASSIMP_BUILD_NO_EXPORT

TEST_F(utD3MFImporterExporter, export3MFBasicMesh) {
    EXPECT_TRUE(exporterTest());
}

TEST_F(utD3MFImporterExporter, export3MFProducesFile) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
    ASSERT_NE(nullptr, scene);

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_export_test.3mf"));

    FILE *f = fopen("ut_3mf_export_test.3mf", "rb");
    ASSERT_NE(nullptr, f);

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    EXPECT_GT(size, 0);

    std::remove("ut_3mf_export_test.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFWithMaterials) {
    // Import a GLB (which has materials), then export as 3MF
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-Binary/BoxTextured.glb", 0);
    if (!scene) {
        // If glTF test model not available, skip
        return;
    }

    Assimp::Exporter exporter;
    EXPECT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_mat_test.3mf"));
    std::remove("ut_3mf_mat_test.3mf");
}

#ifdef ASSIMP_USE_LIB3MF
TEST_F(utD3MFImporterExporter, export3MFPreservesExtMeshManifoldAsOneClosedObject) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(
            ASSIMP_TEST_MODELS_DIR "/glTF2/EXT_mesh_manifold/TwoMaterialBox.glb", 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(2u, scene->mNumMeshes);
    const Assimp::ScenePrivateData *privateData = Assimp::ScenePriv(scene);
    ASSERT_NE(nullptr, privateData);
    ASSERT_EQ(1u, privateData->mManifoldMeshes.size());
    const Assimp::ManifoldMeshTopology &topology = privateData->mManifoldMeshes[0];
    ASSERT_EQ(40u, topology.mPositions.size());
    ASSERT_EQ(60u, topology.mIndices.size());
    ASSERT_EQ(2u, topology.mRuns.size());
    EXPECT_EQ(30u, topology.mRuns[0].mIndexCount);
    EXPECT_EQ(30u, topology.mRuns[1].mIndexCount);

    const char *outPath = "ut_3mf_ext_mesh_manifold.3mf";
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", outPath))
            << exporter.GetErrorString();

    Lib3MF_Model model = nullptr;
    Lib3MF_Reader reader = nullptr;
    Lib3MF_MeshObjectIterator iterator = nullptr;
    Lib3MF_MeshObject object = nullptr;
    ASSERT_EQ(LIB3MF_SUCCESS, lib3mf_createmodel(&model));
    ASSERT_EQ(LIB3MF_SUCCESS, lib3mf_model_queryreader(model, "3mf", &reader));
    ASSERT_EQ(LIB3MF_SUCCESS, lib3mf_reader_readfromfile(reader, outPath));
    ASSERT_EQ(LIB3MF_SUCCESS, lib3mf_model_getmeshobjects(model, &iterator));
    bool hasNext = false;
    ASSERT_EQ(LIB3MF_SUCCESS,
            lib3mf_resourceiterator_movenext(
                    static_cast<Lib3MF_ResourceIterator>(iterator), &hasNext));
    ASSERT_TRUE(hasNext);
    ASSERT_EQ(LIB3MF_SUCCESS,
            lib3mf_meshobjectiterator_getcurrentmeshobject(iterator, &object));

    Lib3MF_uint32 vertexCount = 0;
    Lib3MF_uint32 triangleCount = 0;
    ASSERT_EQ(LIB3MF_SUCCESS, lib3mf_meshobject_getvertexcount(object, &vertexCount));
    ASSERT_EQ(LIB3MF_SUCCESS, lib3mf_meshobject_gettrianglecount(object, &triangleCount));
    ASSERT_EQ(topology.mPositions.size(), vertexCount);
    ASSERT_EQ(topology.mIndices.size() / 3, triangleCount);

    std::vector<Lib3MF::sPosition> vertices(vertexCount);
    std::vector<Lib3MF::sTriangle> triangles(triangleCount);
    std::vector<Lib3MF::sTriangleProperties> properties(triangleCount);
    Lib3MF_uint64 needed = 0;
    ASSERT_EQ(LIB3MF_SUCCESS,
            lib3mf_meshobject_getvertices(object, vertices.size(), &needed, vertices.data()));
    ASSERT_EQ(vertices.size(), needed);
    ASSERT_EQ(LIB3MF_SUCCESS,
            lib3mf_meshobject_gettriangleindices(
                    object, triangles.size(), &needed, triangles.data()));
    ASSERT_EQ(triangles.size(), needed);
    ASSERT_EQ(LIB3MF_SUCCESS,
            lib3mf_meshobject_getalltriangleproperties(
                    object, properties.size(), &needed, properties.data()));
    ASSERT_EQ(properties.size(), needed);

    aiMatrix4x4 nodeTransform;
    ASSERT_TRUE(findGlobalTransformForMesh(
            scene->mRootNode, topology.mRuns[0].mMeshIndex,
            aiMatrix4x4(), nodeTransform));
    aiMatrix4x4 unitScale;
    const float vertexScale = static_cast<float>(
            Assimp::readSceneUnitScaleToMeters(scene) / 1e-3);
    unitScale.a1 = vertexScale;
    unitScale.b2 = vertexScale;
    unitScale.c3 = vertexScale;
    aiMatrix4x4 axis;
    const int32_t sourceUpAxis = Assimp::readSceneUpAxis(scene);
    if (sourceUpAxis >= 0) {
        axis = Assimp::buildAxisRotationMatrix(sourceUpAxis, 2);
    }
    const aiMatrix4x4 expectedTransform = axis * unitScale * nodeTransform;
    for (size_t i = 0; i < vertices.size(); ++i) {
        const aiVector3D expected = expectedTransform * topology.mPositions[i];
        EXPECT_NEAR(expected.x, vertices[i].m_Coordinates[0], 1e-6f);
        EXPECT_NEAR(expected.y, vertices[i].m_Coordinates[1], 1e-6f);
        EXPECT_NEAR(expected.z, vertices[i].m_Coordinates[2], 1e-6f);
    }
    for (size_t i = 0; i < triangles.size(); ++i) {
        EXPECT_EQ(topology.mIndices[i * 3], triangles[i].m_Indices[0]);
        EXPECT_EQ(topology.mIndices[i * 3 + 1], triangles[i].m_Indices[1]);
        EXPECT_EQ(topology.mIndices[i * 3 + 2], triangles[i].m_Indices[2]);
        const Lib3MF_uint32 expectedProperty = properties[i < 10 ? 0 : 10].m_PropertyIDs[0];
        EXPECT_EQ(expectedProperty, properties[i].m_PropertyIDs[0]);
    }
    EXPECT_NE(properties[0].m_PropertyIDs[0], properties[10].m_PropertyIDs[0]);
    bool manifoldAndOriented = false;
    ASSERT_EQ(LIB3MF_SUCCESS,
            lib3mf_meshobject_ismanifoldandoriented(object, &manifoldAndOriented));
    EXPECT_TRUE(manifoldAndOriented);
    ASSERT_EQ(LIB3MF_SUCCESS,
            lib3mf_resourceiterator_movenext(
                    static_cast<Lib3MF_ResourceIterator>(iterator), &hasNext));
    EXPECT_FALSE(hasNext);
    lib3mf_release(object);
    lib3mf_release(iterator);
    lib3mf_release(reader);
    lib3mf_release(model);
    Assimp::Importer reimporter;
    const aiScene *roundtrip = reimporter.ReadFile(outPath, 0);
    ASSERT_NE(nullptr, roundtrip) << reimporter.GetErrorString();
    EXPECT_EQ(1u, roundtrip->mNumMeshes);
    if (roundtrip->mNumMeshes == 1u) {
        EXPECT_EQ(40u, roundtrip->mMeshes[0]->mNumVertices);
        EXPECT_EQ(20u, roundtrip->mMeshes[0]->mNumFaces);
    }

    std::remove(outPath);
}
#endif

TEST_F(utD3MFImporterExporter, importExtMeshManifoldRejectsInvalidContracts) {
    struct InvalidCase {
        const char *file;
        const char *error;
    };
    const InvalidCase cases[] = {
        { "MissingMergeValues.glb",
          "GLTF: EXT_mesh_manifold in mesh meshes[0] (\"Shape 1\") must define mergeIndices and mergeValues together" },
        { "MergeCountMismatch.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"Shape 1\"): merge accessors must be compatible unsigned SCALAR accessors" },
        { "InvalidCanonicalIndexType.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"Shape 1\"): manifoldPrimitive indices must use an unsigned SCALAR accessor" },
        { "CanonicalIndexOutOfRange.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"Shape 1\"): canonical index 3 is out of range" },
        { "CanonicalCountMismatch.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"CanonicalCountMismatch\"): canonical index count must equal the render index count and be divisible by three" },
        { "PositionNonFinite.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"Shape 1\"): POSITION contains a non-finite value at vertex 0" },
        { "UnequalMergePositions.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"Shape 1\"): merge accessor entry 0 joins vertices with different POSITION values" },
        { "MergeEntryOutOfRange.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"Shape 1\"): merge accessor entry 0 is out of range or duplicated" },
        { "MergeEntryDuplicated.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"Shape 1\"): merge accessor entry 1 is out of range or duplicated" },
        { "MergeReconstructionMismatch.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"Shape 1\"): merge accessors do not exactly reproduce the canonical index stream" },
        { "CanonicalMaterial.glb",
          "GLTF: EXT_mesh_manifold manifoldPrimitive in mesh meshes[0] (\"Shape 1\") must not contain material or targets" },
        { "CanonicalExtraAttribute.glb",
          "GLTF: EXT_mesh_manifold manifoldPrimitive in mesh meshes[0] (\"Shape 1\") must contain only a POSITION accessor" },
        { "CanonicalMorphTarget.glb",
          "GLTF: EXT_mesh_manifold manifoldPrimitive in mesh meshes[0] (\"Shape 1\") must not contain material or targets" },
        { "RenderAttributeMismatch.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"Shape 1\"): same attributes in different render primitives must reference the same accessors" },
        { "RenderIndexBufferViewMismatch.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"Shape 1\"): all render primitive indices must reference the same bufferView" },
        { "RenderIndexOutOfRange.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"Shape 1\"): render primitive 0 contains an out-of-range index" },
        { "RenderPrimitiveUnindexed.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"Shape 1\"): render primitive 0 must use indexed TRIANGLES" },
        { "OpenEdge.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"OpenEdge\"): canonical half-edge (1, 2) does not have exactly one reverse half-edge" },
        { "SameDirectionEdge.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"SameDirectionEdge\"): canonical half-edge (0, 1) does not have exactly one reverse half-edge" },
        { "OversubscribedEdge.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"OversubscribedEdge\"): canonical half-edge (0, 1) does not have exactly one reverse half-edge" },
        { "DisconnectedVertexLink.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"DisconnectedVertexLink\"): canonical vertex 0 has disconnected incident triangle fans" },
        { "RepeatedTriangleVertex.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"RepeatedTriangleVertex\"): canonical triangle 0 repeats a vertex" },
        { "ZeroAreaTriangle.glb",
          "GLTF: EXT_mesh_manifold mesh meshes[0] (\"ZeroAreaTriangle\"): canonical triangle 0 has zero area" },
    };

    for (const InvalidCase &testCase : cases) {
        SCOPED_TRACE(testCase.file);
        Assimp::Importer importer;
        const std::string path = std::string(ASSIMP_TEST_MODELS_DIR) +
                "/glTF2/EXT_mesh_manifold/invalid/" + testCase.file;
        EXPECT_EQ(nullptr, importer.ReadFile(path, 0));
        EXPECT_STREQ(testCase.error, importer.GetErrorString());
    }
}

TEST_F(utD3MFImporterExporter, copyScenePreservesExtMeshManifoldTopology) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(
            ASSIMP_TEST_MODELS_DIR "/glTF2/EXT_mesh_manifold/TwoMaterialBox.glb", 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();

    aiScene *rawCopy = nullptr;
    Assimp::SceneCombiner::CopyScene(&rawCopy, scene);
    std::unique_ptr<aiScene> copy(rawCopy);
    ASSERT_NE(nullptr, copy);
    const Assimp::ScenePrivateData *sourcePrivate = Assimp::ScenePriv(scene);
    const Assimp::ScenePrivateData *copyPrivate = Assimp::ScenePriv(copy.get());
    ASSERT_NE(nullptr, sourcePrivate);
    ASSERT_NE(nullptr, copyPrivate);
    ASSERT_EQ(1u, sourcePrivate->mManifoldMeshes.size());
    ASSERT_EQ(1u, copyPrivate->mManifoldMeshes.size());

    const auto &source = sourcePrivate->mManifoldMeshes[0];
    const auto &copied = copyPrivate->mManifoldMeshes[0];
    ASSERT_EQ(source.mPositions.size(), copied.mPositions.size());
    ASSERT_EQ(source.mIndices, copied.mIndices);
    ASSERT_EQ(source.mRuns.size(), copied.mRuns.size());
    for (size_t i = 0; i < source.mPositions.size(); ++i) {
        EXPECT_EQ(source.mPositions[i], copied.mPositions[i]);
    }
    for (size_t i = 0; i < source.mRuns.size(); ++i) {
        EXPECT_EQ(source.mRuns[i].mMeshIndex, copied.mRuns[i].mMeshIndex);
        EXPECT_EQ(source.mRuns[i].mMaterialIndex, copied.mRuns[i].mMaterialIndex);
        EXPECT_EQ(source.mRuns[i].mFirstIndex, copied.mRuns[i].mFirstIndex);
        EXPECT_EQ(source.mRuns[i].mIndexCount, copied.mRuns[i].mIndexCount);
    }
}

TEST_F(utD3MFImporterExporter, export3MFRejectsTopologyChangingPostProcessForManifoldSource) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(
            ASSIMP_TEST_MODELS_DIR "/glTF2/EXT_mesh_manifold/TwoMaterialBox.glb", 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();

    Assimp::Exporter exporter;
    EXPECT_EQ(AI_FAILURE, exporter.Export(
            scene, "3mf", "ut_3mf_unsafe_postprocess.3mf",
            aiProcess_PreTransformVertices));
    EXPECT_EQ(
            "3MF export cannot preserve EXT_mesh_manifold after topology-changing "
            "post-processing flags: " +
                    std::to_string(aiProcess_PreTransformVertices) +
                    " (source mesh 0)",
            exporter.GetErrorString());
    std::remove("ut_3mf_unsafe_postprocess.3mf");
}

#ifdef ASSIMP_USE_LIB3MF
TEST_F(utD3MFImporterExporter, export3MFExactPathSupportsCanonicalRenderIndices) {
    std::unique_ptr<aiScene> scene(makeBoxScene(aiVector3D(1.0f)));
    addManifoldTopologyForMesh(scene.get(), 0);

    Assimp::Exporter exporter;
    const char *outPath = "ut_3mf_ext_mesh_manifold_one_material.3mf";
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene.get(), "3mf", outPath))
            << exporter.GetErrorString();

    Assimp::Importer reimporter;
    const aiScene *roundtrip = reimporter.ReadFile(outPath, 0);
    ASSERT_NE(nullptr, roundtrip) << reimporter.GetErrorString();
    ASSERT_EQ(1u, roundtrip->mNumMeshes);
    EXPECT_EQ(8u, roundtrip->mMeshes[0]->mNumVertices);
    EXPECT_EQ(12u, roundtrip->mMeshes[0]->mNumFaces);
    std::remove(outPath);
}

TEST_F(utD3MFImporterExporter, export3MFExactPathAppliesNodeUnitAndAxisTransforms) {
    std::unique_ptr<aiScene> scene(makeBoxScene(aiVector3D(1.0f, 2.0f, 3.0f)));
    addManifoldTopologyForMesh(scene.get(), 0);
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    delete[] scene->mRootNode->mMeshes;
    scene->mRootNode->mMeshes = nullptr;
    scene->mRootNode->mNumMeshes = 0;
    aiMatrix4x4 translation;
    aiMatrix4x4::Translation(aiVector3D(5.0f, 0.0f, 0.0f), translation);
    scene->mRootNode->mTransformation = translation;

    auto *child = new aiNode();
    child->mParent = scene->mRootNode;
    child->mNumMeshes = 1;
    child->mMeshes = new unsigned int[1]{0};
    aiMatrix4x4 rotation;
    aiMatrix4x4::RotationZ(static_cast<ai_real>(AI_MATH_PI_F * 0.5f), rotation);
    aiMatrix4x4 scaling;
    aiMatrix4x4::Scaling(aiVector3D(2.0f, 3.0f, 4.0f), scaling);
    child->mTransformation = rotation * scaling;
    scene->mRootNode->mNumChildren = 1;
    scene->mRootNode->mChildren = new aiNode *[1]{child};

    Assimp::Exporter exporter;
    const char *outPath = "ut_3mf_ext_mesh_manifold_transform.3mf";
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene.get(), "3mf", outPath))
            << exporter.GetErrorString();
    Assimp::Importer reimporter;
    const aiScene *roundtrip = reimporter.ReadFile(outPath, 0);
    ASSERT_NE(nullptr, roundtrip) << reimporter.GetErrorString();
    ASSERT_EQ(1u, roundtrip->mNumMeshes);
    const aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    EXPECT_NEAR(12000.0f, extent.x, 1e-2f);
    EXPECT_NEAR(24000.0f, extent.y, 1e-2f);
    EXPECT_NEAR(4000.0f, extent.z, 1e-2f);
    std::remove(outPath);
}

TEST_F(utD3MFImporterExporter, export3MFExactPathCorrectsReflectedWinding) {
    std::unique_ptr<aiScene> scene(makeBoxScene(aiVector3D(1.0f)));
    addManifoldTopologyForMesh(scene.get(), 0);
    scene->mRootNode->mTransformation.a1 = -1.0f;

    Assimp::Exporter exporter;
    const char *outPath = "ut_3mf_ext_mesh_manifold_reflected.3mf";
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene.get(), "3mf", outPath))
            << exporter.GetErrorString();
    Assimp::Importer reimporter;
    const aiScene *roundtrip = reimporter.ReadFile(outPath, 0);
    ASSERT_NE(nullptr, roundtrip) << reimporter.GetErrorString();
    ASSERT_EQ(1u, roundtrip->mNumMeshes);
    EXPECT_EQ(12u, roundtrip->mMeshes[0]->mNumFaces);
    std::remove(outPath);
}

TEST_F(utD3MFImporterExporter, export3MFExactPathOrientsDisconnectedComponentsIndependently) {
    std::unique_ptr<aiScene> scene(makeBoxScene(aiVector3D(1.0f)));
    addManifoldTopologyForMesh(scene.get(), 0);
    auto &topology = Assimp::ScenePriv(scene.get())->mManifoldMeshes[0];
    const std::vector<aiVector3D> positions = topology.mPositions;
    const std::vector<unsigned int> indices = topology.mIndices;
    topology.mPositions.clear();
    for (const aiVector3D &position : positions) {
        topology.mPositions.emplace_back(position.x - 3.0f, position.y, position.z);
    }
    for (const aiVector3D &position : positions) {
        topology.mPositions.emplace_back(position.x + 3.0f, position.y, position.z);
    }
    topology.mIndices = indices;
    for (size_t i = 0; i < indices.size(); i += 3) {
        topology.mIndices.push_back(indices[i] + 8);
        topology.mIndices.push_back(indices[i + 2] + 8);
        topology.mIndices.push_back(indices[i + 1] + 8);
    }
    topology.mRuns[0].mIndexCount = topology.mIndices.size();

    Assimp::Exporter exporter;
    const char *outPath = "ut_3mf_ext_mesh_manifold_components.3mf";
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene.get(), "3mf", outPath))
            << exporter.GetErrorString();
    Assimp::Importer reimporter;
    const aiScene *roundtrip = reimporter.ReadFile(outPath, 0);
    ASSERT_NE(nullptr, roundtrip) << reimporter.GetErrorString();
    ASSERT_EQ(1u, roundtrip->mNumMeshes);
    EXPECT_EQ(16u, roundtrip->mMeshes[0]->mNumVertices);
    EXPECT_EQ(24u, roundtrip->mMeshes[0]->mNumFaces);
    std::remove(outPath);
}

TEST_F(utD3MFImporterExporter, export3MFExactPathEmitsEachNodeInstance) {
    std::unique_ptr<aiScene> scene(makeBoxScene(aiVector3D(1.0f)));
    addManifoldTopologyForMesh(scene.get(), 0);
    delete[] scene->mRootNode->mMeshes;
    scene->mRootNode->mMeshes = nullptr;
    scene->mRootNode->mNumMeshes = 0;
    scene->mRootNode->mNumChildren = 2;
    scene->mRootNode->mChildren = new aiNode *[2];
    for (unsigned int i = 0; i < 2; ++i) {
        auto *child = new aiNode();
        child->mParent = scene->mRootNode;
        child->mNumMeshes = 1;
        child->mMeshes = new unsigned int[1]{0};
        aiMatrix4x4::Translation(
                aiVector3D(i == 0 ? -3.0f : 3.0f, 0.0f, 0.0f),
                child->mTransformation);
        scene->mRootNode->mChildren[i] = child;
    }

    Assimp::Exporter exporter;
    const char *outPath = "ut_3mf_ext_mesh_manifold_instances.3mf";
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene.get(), "3mf", outPath))
            << exporter.GetErrorString();
    Assimp::Importer reimporter;
    const aiScene *roundtrip = reimporter.ReadFile(outPath, 0);
    ASSERT_NE(nullptr, roundtrip) << reimporter.GetErrorString();
    EXPECT_EQ(2u, roundtrip->mNumMeshes);
    std::remove(outPath);
}

TEST_F(utD3MFImporterExporter, export3MFSupportsMixedExactAndFallbackMeshes) {
    std::unique_ptr<aiScene> scene(makeBoxScene(aiVector3D(1.0f)));
    std::unique_ptr<aiScene> secondScene(makeBoxScene(aiVector3D(1.0f)));
    for (unsigned int i = 0; i < secondScene->mMeshes[0]->mNumVertices; ++i) {
        secondScene->mMeshes[0]->mVertices[i].x += 4.0f;
    }
    aiMesh *secondMesh = secondScene->mMeshes[0];
    secondScene->mMeshes[0] = nullptr;
    aiMesh *firstMesh = scene->mMeshes[0];
    delete[] scene->mMeshes;
    scene->mNumMeshes = 2;
    scene->mMeshes = new aiMesh *[2]{firstMesh, secondMesh};
    delete[] scene->mRootNode->mMeshes;
    scene->mRootNode->mNumMeshes = 2;
    scene->mRootNode->mMeshes = new unsigned int[2]{0, 1};
    addManifoldTopologyForMesh(scene.get(), 0);

    Assimp::Exporter exporter;
    const char *outPath = "ut_3mf_ext_mesh_manifold_mixed.3mf";
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene.get(), "3mf", outPath))
            << exporter.GetErrorString();
    Assimp::Importer reimporter;
    const aiScene *roundtrip = reimporter.ReadFile(outPath, 0);
    ASSERT_NE(nullptr, roundtrip) << reimporter.GetErrorString();
    EXPECT_EQ(2u, roundtrip->mNumMeshes);
    std::remove(outPath);
}

TEST_F(utD3MFImporterExporter, export3MFExactPathRejectsZeroVolumeComponent) {
    std::unique_ptr<aiScene> scene(makeBoxScene(aiVector3D(1.0f)));
    addManifoldTopologyForMesh(scene.get(), 0);
    auto &topology = Assimp::ScenePriv(scene.get())->mManifoldMeshes[0];
    topology.mPositions = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}
    };
    topology.mIndices = {
        0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3
    };
    topology.mRuns[0].mIndexCount = topology.mIndices.size();

    Assimp::Exporter exporter;
    EXPECT_EQ(AI_FAILURE, exporter.Export(
            scene.get(), "3mf", "ut_3mf_ext_mesh_manifold_zero_volume.3mf"));
    EXPECT_STREQ(
            "3MF EXT_mesh_manifold source mesh 0 component containing triangle 0 "
            "has zero signed volume",
            exporter.GetErrorString());
    std::remove("ut_3mf_ext_mesh_manifold_zero_volume.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFExactPathRejectsStalePrimitiveRuns) {
    std::unique_ptr<aiScene> scene(makeBoxScene(aiVector3D(1.0f)));
    addManifoldTopologyForMesh(scene.get(), 0);
    Assimp::ScenePriv(scene.get())->mManifoldMeshes[0].mRuns[0].mFirstIndex = 3;

    Assimp::Exporter exporter;
    EXPECT_EQ(AI_FAILURE, exporter.Export(
            scene.get(), "3mf", "ut_3mf_ext_mesh_manifold_stale.3mf"));
    EXPECT_STREQ(
            "3MF EXT_mesh_manifold source mesh 0 has stale or invalid primitive-run provenance",
            exporter.GetErrorString());
    std::remove("ut_3mf_ext_mesh_manifold_stale.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFExactPathIsByteDeterministic) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(
            ASSIMP_TEST_MODELS_DIR "/glTF2/EXT_mesh_manifold/TwoMaterialBox.glb", 0);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();

    Assimp::Exporter exporter;
    const char *firstPath = "ut_3mf_ext_mesh_manifold_deterministic_1.3mf";
    const char *secondPath = "ut_3mf_ext_mesh_manifold_deterministic_2.3mf";
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", firstPath))
            << exporter.GetErrorString();
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", secondPath))
            << exporter.GetErrorString();
    EXPECT_EQ(readBytes(firstPath), readBytes(secondPath));
    std::remove(firstPath);
    std::remove(secondPath);
}
#endif

// ===== ROUNDTRIP TESTS =====

TEST_F(utD3MFImporterExporter, roundtrip3MFBox) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene);
    ASSERT_GE(scene->mNumMeshes, 1u);

    unsigned int origVertices = scene->mMeshes[0]->mNumVertices;
    unsigned int origFaces = scene->mMeshes[0]->mNumFaces;

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_roundtrip_box.3mf"));

    Assimp::Importer importer2;
    const aiScene *scene2 = importer2.ReadFile("ut_3mf_roundtrip_box.3mf", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene2);
    ASSERT_GE(scene2->mNumMeshes, 1u);

    EXPECT_EQ(origVertices, scene2->mMeshes[0]->mNumVertices);
    EXPECT_EQ(origFaces, scene2->mMeshes[0]->mNumFaces);

    std::remove("ut_3mf_roundtrip_box.3mf");
}

TEST_F(utD3MFImporterExporter, roundtrip3MFPreservesTriangles) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
    ASSERT_NE(nullptr, scene);

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_roundtrip_tri.3mf"));

    Assimp::Importer importer2;
    const aiScene *scene2 = importer2.ReadFile("ut_3mf_roundtrip_tri.3mf", 0);
    ASSERT_NE(nullptr, scene2);

    EXPECT_EQ(scene->mNumMeshes, scene2->mNumMeshes);
    for (unsigned int i = 0; i < scene2->mNumMeshes && i < scene->mNumMeshes; ++i) {
        for (unsigned int f = 0; f < scene2->mMeshes[i]->mNumFaces; ++f) {
            EXPECT_EQ(3u, scene2->mMeshes[i]->mFaces[f].mNumIndices);
        }
    }

    std::remove("ut_3mf_roundtrip_tri.3mf");
}

TEST_F(utD3MFImporterExporter, roundtrip3MFWithMaterial) {
    // Use a GLB with material, export to 3MF, reimport and verify color preserved
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-Binary/BoxTextured.glb", 0);
    if (!scene) {
        return;
    }
    ASSERT_GE(scene->mNumMaterials, 1u);

    // Get original material color
    aiColor4D origColor(0.5f, 0.5f, 0.5f, 1.0f);
    scene->mMaterials[0]->Get(AI_MATKEY_COLOR_DIFFUSE, origColor);

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_roundtrip_mat.3mf"));

    Assimp::Importer importer2;
    const aiScene *scene2 = importer2.ReadFile("ut_3mf_roundtrip_mat.3mf", 0);
    ASSERT_NE(nullptr, scene2);
    ASSERT_GE(scene2->mNumMaterials, 1u);

    aiColor4D reimportedColor;
    ASSERT_EQ(AI_SUCCESS, scene2->mMaterials[0]->Get(AI_MATKEY_COLOR_DIFFUSE, reimportedColor));

    EXPECT_NEAR(origColor.r, reimportedColor.r, 0.02f);
    EXPECT_NEAR(origColor.g, reimportedColor.g, 0.02f);
    EXPECT_NEAR(origColor.b, reimportedColor.b, 0.02f);

    std::remove("ut_3mf_roundtrip_mat.3mf");
}

// ===== R8' — UNIT, AXIS, WELD, PROPERTY-RESOLVER TESTS =====

#ifdef ASSIMP_USE_LIB3MF
TEST_F(utD3MFImporterExporter, export3MFRescalesToMillimeterWhenSceneUnitScaleToMetersIsOne) {
    // 0.5m half-extent → 1m cube, declared as meters via the contract metadata.
    aiScene *scene = makeBoxScene(aiVector3D(0.5f, 0.5f, 0.5f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2)); // already Z, no rotation
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_unit_meters.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_unit_meters.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    EXPECT_NEAR(1000.0f, extent.x, 0.5f);
    EXPECT_NEAR(1000.0f, extent.y, 0.5f);
    EXPECT_NEAR(1000.0f, extent.z, 0.5f);
    std::remove("ut_3mf_unit_meters.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFRotatesYUpSourceToZUpByDefault) {
    // Tall box on +Y axis: 100×500×100 in scene units.
    aiScene *scene = makeBoxScene(aiVector3D(50.0f, 250.0f, 50.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1)); // Y-up source
    // Identity scale fallback (no UnitScaleToMeters metadata)
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_axis_zup.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_axis_zup.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    // After Y→Z rotation, the long axis (originally Y) lands on Z.
    EXPECT_NEAR(100.0f, extent.x, 1.0f);
    EXPECT_NEAR(100.0f, extent.y, 1.0f);
    EXPECT_NEAR(500.0f, extent.z, 1.0f);
    std::remove("ut_3mf_axis_zup.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFRetainsYUpWhenExportUpAxisPropertyIsOne) {
    aiScene *scene = makeBoxScene(aiVector3D(50.0f, 250.0f, 50.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(1));

    Assimp::Exporter exporter;
    Assimp::ExportProperties props;
    props.SetPropertyInteger("3MF_EXPORT_UPAXIS", 1);
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_axis_yup_optin.3mf", 0, &props));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_axis_yup_optin.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    EXPECT_NEAR(100.0f, extent.x, 1.0f);
    EXPECT_NEAR(500.0f, extent.y, 1.0f);
    EXPECT_NEAR(100.0f, extent.z, 1.0f);
    std::remove("ut_3mf_axis_yup_optin.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFThrowsDeadlyExportErrorWhenUpAxisPropertyOutOfRange) {
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    Assimp::Exporter exporter;
    Assimp::ExportProperties props;
    props.SetPropertyInteger("3MF_EXPORT_UPAXIS", 7);
    aiReturn rc = exporter.Export(scene, "3mf", "ut_3mf_bad_axis.3mf", 0, &props);
    EXPECT_EQ(AI_FAILURE, rc);
    std::string err = exporter.GetErrorString();
    EXPECT_NE(std::string::npos, err.find("3MF_EXPORT_UPAXIS"));
    EXPECT_NE(std::string::npos, err.find("7"));
    EXPECT_NE(std::string::npos, err.find("[0, 2]"));
    delete scene;
    std::remove("ut_3mf_bad_axis.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFAppliesIdentityTransformWhenSceneHasNoContractMetadataAndNoProperties) {
    // Unit cube with NO mMetaData and NO ExportProperties → identity transform path.
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    ASSERT_EQ(nullptr, scene->mMetaData);
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_identity.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_identity.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    EXPECT_NEAR(2.0f, extent.x, 1e-3f);
    EXPECT_NEAR(2.0f, extent.y, 1e-3f);
    EXPECT_NEAR(2.0f, extent.z, 1e-3f);
    std::remove("ut_3mf_identity.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFIgnoresLegacyUnitScaleFactorKey) {
    // FBX-shaped scene: only the legacy `UnitScaleFactor` (cm-relative) is set.
    // The 3MF bridge MUST NOT read this key — it is disjoint from the new contract.
    // If misread as meters-per-unit, a 1.0 value would silently scale by 1000×.
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add("UnitScaleFactor", 1.0); // legacy FBX key — semantics: cm
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_legacy_key.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_legacy_key.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    // Identity transform means the 2-unit cube re-imports as a 2-unit cube,
    // not a 2000× scaled-up monstrosity.
    EXPECT_NEAR(2.0f, extent.x, 1e-3f);
    EXPECT_NEAR(2.0f, extent.y, 1e-3f);
    EXPECT_NEAR(2.0f, extent.z, 1e-3f);
    std::remove("ut_3mf_legacy_key.3mf");
}

TEST_F(utD3MFImporterExporter, export3MFCollapsesPositionDuplicatesFromPerFaceNormalSeams) {
    // Per-face-expanded cube has 24 source vertices that share 8 unique positions.
    aiScene *scene = makePerFaceBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    ASSERT_EQ(24u, scene->mMeshes[0]->mNumVertices);
    ASSERT_EQ(12u, scene->mMeshes[0]->mNumFaces);
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_weld.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_weld.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    EXPECT_EQ(8u, roundtrip->mMeshes[0]->mNumVertices);
    EXPECT_EQ(12u, roundtrip->mMeshes[0]->mNumFaces);
    std::remove("ut_3mf_weld.3mf");
}
#endif

// ===== R10' — IMPORTER METADATA TESTS =====

TEST_F(utD3MFImporterExporter, import3MFWritesUnitScaleToMetersAndUpAxisToSceneMetadata) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMetaData);

    double meters = 0.0;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, meters));
    EXPECT_NEAR(1e-3, meters, 1e-9);

    int32_t upAxis = -1;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, upAxis));
    EXPECT_EQ(2, upAxis);

    aiString sourceFormat;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_SOURCE_FORMAT, sourceFormat));
    EXPECT_STREQ("3mf Importer", sourceFormat.C_Str());
}

TEST_F(utD3MFImporterExporter, importContractUnitScaleOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_IMPORT_3MF_UNIT_SCALE_TO_METERS, 1.0f);
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMetaData);

    double meters = 0.0;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, meters));
    EXPECT_NEAR(1.0, meters, 1e-9) << "AI_CONFIG_IMPORT_3MF_UNIT_SCALE_TO_METERS override must win over the file-declared unit";

    int32_t upAxis = -1;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, upAxis));
    EXPECT_EQ(2, upAxis) << "Up-axis must remain 3MF-spec Z when only the unit override is set";
}

TEST_F(utD3MFImporterExporter, importContractUpAxisOverrideRespected) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_3MF_UP_AXIS, 1); // force Y-up
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
    ASSERT_NE(nullptr, scene);
    ASSERT_NE(nullptr, scene->mMetaData);

    int32_t upAxis = -1;
    ASSERT_TRUE(scene->mMetaData->Get(AI_METADATA_UP_AXIS, upAxis));
    EXPECT_EQ(1, upAxis) << "AI_CONFIG_IMPORT_3MF_UP_AXIS override must win over the spec-default Z";
}

TEST_F(utD3MFImporterExporter, importContractInvalidUpAxisOverrideFailsImport) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_3MF_UP_AXIS, 7); // out of [0, 2]
    const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/3MF/box.3mf", 0);
    EXPECT_EQ(nullptr, scene) << "Invalid AI_CONFIG_IMPORT_3MF_UP_AXIS override must fail import via DeadlyImportError";
    EXPECT_NE(std::string::npos, std::string(importer.GetErrorString()).find("IMPORT_3MF_UP_AXIS"));
}

#ifdef ASSIMP_USE_LIB3MF
TEST_F(utD3MFImporterExporter, export3MFReadsUnitScaleToMetersMetadataWhenPropertyAbsent) {
    // 1-unit cube authored as centimeters via the contract; export with no properties.
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 0.01); // cm
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_unit_meta.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_unit_meta.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);
    // Source = cm (0.01 m/unit), default target = mm (0.001 m/unit).
    // scale = 0.01 / 0.001 = 10 → 2-unit cube → 20 mm.
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    EXPECT_NEAR(20.0f, extent.x, 1e-2f);
    EXPECT_NEAR(20.0f, extent.y, 1e-2f);
    EXPECT_NEAR(20.0f, extent.z, 1e-2f);
    std::remove("ut_3mf_unit_meta.3mf");
}
#endif

TEST_F(utD3MFImporterExporter, roundtrip3MFPreservesUnitsAndAxisAcrossExportImport) {
    aiScene *scene = makeBoxScene(aiVector3D(5.0f, 5.0f, 5.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1e-3); // mm
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2)); // Z
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_roundtrip_units.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_roundtrip_units.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_NE(nullptr, roundtrip->mMetaData);

    double meters = 0.0;
    ASSERT_TRUE(roundtrip->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, meters));
    EXPECT_NEAR(1e-3, meters, 1e-9);
    int32_t up = -1;
    ASSERT_TRUE(roundtrip->mMetaData->Get(AI_METADATA_UP_AXIS, up));
    EXPECT_EQ(2, up);

    // Guard the mesh-extent assertions behind an ASSERT_GE so the test
    // reports a clean failure (rather than segfaulting on a null mesh) on
    // the pre-existing 3MF round-trip regression where the importer can
    // re-read the package metadata but loses the model body. Tracked under
    // the same baseline as `roundtrip3MFBox` / `roundtrip3MFPreservesTriangles`.
    ASSERT_GE(roundtrip->mNumMeshes, 1u) << "3MF re-importer dropped meshes";
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    EXPECT_NEAR(10.0f, extent.x, 1e-3f);
    EXPECT_NEAR(10.0f, extent.y, 1e-3f);
    EXPECT_NEAR(10.0f, extent.z, 1e-3f);
    std::remove("ut_3mf_roundtrip_units.3mf");
}

#ifdef ASSIMP_USE_LIB3MF
TEST_F(utD3MFImporterExporter, export3MFRespectsExportUnitProperty) {
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, 1.0); // meters
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, static_cast<int32_t>(2));
    Assimp::Exporter exporter;
    Assimp::ExportProperties props;
    props.SetPropertyString("3MF_EXPORT_UNIT", "centimeter");
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_unit_prop.3mf", 0, &props));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_unit_prop.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_NE(nullptr, roundtrip->mMetaData);

    double meters = 0.0;
    ASSERT_TRUE(roundtrip->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, meters));
    EXPECT_NEAR(0.01, meters, 1e-9);

    // Guard against the pre-existing 3MF re-importer regression that drops
    // mesh bodies — fail cleanly rather than segfault.
    ASSERT_GE(roundtrip->mNumMeshes, 1u) << "3MF re-importer dropped meshes";
    // 1m source → 100 cm output.
    aiVector3D extent = meshExtent(roundtrip->mMeshes[0]);
    EXPECT_NEAR(200.0f, extent.x, 1e-2f);
    EXPECT_NEAR(200.0f, extent.y, 1e-2f);
    EXPECT_NEAR(200.0f, extent.z, 1e-2f);
    std::remove("ut_3mf_unit_prop.3mf");
}
#endif

TEST_F(utD3MFImporterExporter, export3MFDeclaresMillimeterUnitWhenNoPropertiesPassed) {
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_default_unit.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_default_unit.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_NE(nullptr, roundtrip->mMetaData);

    double meters = 0.0;
    ASSERT_TRUE(roundtrip->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, meters));
    EXPECT_NEAR(1e-3, meters, 1e-9);
    std::remove("ut_3mf_default_unit.3mf");
}

// ===== R9' — DEFAULT POST-PROCESSING FLAGS TEST =====

#ifdef ASSIMP_USE_LIB3MF
TEST_F(utD3MFImporterExporter, export3MFViaTopLevelExporterFlattensSceneGraph) {
    // Author a scene where a child node has a translation transform.
    // Without aiProcess_PreTransformVertices in the exporter's enforced PP flags,
    // the child's translation is exported as a build-item transform, not baked into
    // vertex positions. With the R9' default flags, the translation MUST be baked.
    aiScene *scene = makeBoxScene(aiVector3D(1.0f, 1.0f, 1.0f));
    delete scene->mRootNode;

    auto *root = new aiNode();
    root->mName = aiString("Root");
    auto *child = new aiNode();
    child->mName = aiString("ChildOffset");
    child->mTransformation.a4 = 100.0f; // translate +X by 100 units
    child->mNumMeshes = 1;
    child->mMeshes = new unsigned int[1];
    child->mMeshes[0] = 0;
    child->mParent = root;

    root->mNumChildren = 1;
    root->mChildren = new aiNode *[1];
    root->mChildren[0] = child;
    scene->mRootNode = root;

    Assimp::Exporter exporter;
    // Caller passes ZERO pp flags — only enforced flags run.
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, "3mf", "ut_3mf_flatten.3mf"));
    delete scene;

    Assimp::Importer importer2;
    const aiScene *roundtrip = importer2.ReadFile("ut_3mf_flatten.3mf", 0);
    ASSERT_NE(nullptr, roundtrip);
    ASSERT_GE(roundtrip->mNumMeshes, 1u);

    // Build-item transforms should be identity — translation is baked in.
    ASSERT_NE(nullptr, roundtrip->mRootNode);
    for (unsigned int i = 0; i < roundtrip->mRootNode->mNumChildren; ++i) {
        const aiNode *c = roundtrip->mRootNode->mChildren[i];
        EXPECT_TRUE(c->mTransformation.IsIdentity());
    }

    // Vertex centroid should be ≈ (100, 0, 0) baked in (default unit scale + identity axis).
    aiVector3D mn = meshMin(roundtrip->mMeshes[0]);
    aiVector3D mx = meshMax(roundtrip->mMeshes[0]);
    aiVector3D centroid((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);
    EXPECT_NEAR(100.0f, centroid.x, 1e-2f);
    EXPECT_NEAR(0.0f, centroid.y, 1e-2f);
    EXPECT_NEAR(0.0f, centroid.z, 1e-2f);
    std::remove("ut_3mf_flatten.3mf");
}

// ===== R11 — GLB→3MF CROSS-FORMAT ROUNDTRIP =====

TEST_F(utD3MFImporterExporter, glbToThreeMfRoundtripPreservesPhysicalDimensions) {
    Assimp::Importer importer;
    const aiScene *gltfScene = importer.ReadFile(
        ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-Binary/BoxTextured.glb", 0);
    ASSERT_NE(nullptr, gltfScene);
    ASSERT_GE(gltfScene->mNumMeshes, 1u);

    aiVector3D gltfExtent = meshExtent(gltfScene->mMeshes[0]);
    // BoxTextured.glb is a 1m unit cube in glTF (Y-up, meters).

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(gltfScene, "3mf", "ut_3mf_glb_roundtrip.3mf"));

    Assimp::Importer importer2;
    const aiScene *threeMfScene = importer2.ReadFile("ut_3mf_glb_roundtrip.3mf", 0);
    ASSERT_NE(nullptr, threeMfScene);
    ASSERT_GE(threeMfScene->mNumMeshes, 1u);
    aiVector3D threeMfExtent = meshExtent(threeMfScene->mMeshes[0]);

    // Meters → mm: 1000× larger in every axis, with the original Y-extent landing on Z.
    EXPECT_NEAR(gltfExtent.x * 1000.0f, threeMfExtent.x, 1.0f);
    EXPECT_NEAR(gltfExtent.z * 1000.0f, threeMfExtent.y, 1.0f);
    EXPECT_NEAR(gltfExtent.y * 1000.0f, threeMfExtent.z, 1.0f);
    std::remove("ut_3mf_glb_roundtrip.3mf");
}
#endif


// ===== R1: lib3mf decimal precision (3MF_EXPORT_DECIMAL_PRECISION) =====
//
// Background: lib3mf's writer truncates vertex coordinates to N decimal digits
// using fixed-point conversion (NOT rounding). The lib3mf default is 6, which
// loses ~1µm of asymmetric precision and causes nm-scale gaps between separate
// mesh objects in 3MF (visible as missing fragments in slicers like Bambu Studio).
// The Lib3MFBridge bumps the default to 9 and exposes the lib3mf writer setting
// via the ExportProperties key "3MF_EXPORT_DECIMAL_PRECISION".
//
// See docs/research/3mf-export-rendering-artifacts.md (R1).

#ifdef ASSIMP_USE_LIB3MF
namespace {

// Build a closed tetrahedron where the first vertex carries enough
// non-trivial decimal digits to expose lib3mf's truncation behavior. Caller
// owns the returned scene.
aiScene *buildSinglePrecisionVertexScene() {
    aiScene *scene = new aiScene();
    scene->mNumMeshes = 1;
    scene->mMeshes = new aiMesh *[1];
    aiMesh *mesh = new aiMesh();
    scene->mMeshes[0] = mesh;

    mesh->mNumVertices = 4;
    mesh->mVertices = new aiVector3D[4];
    // Diagnostic vertex: float repr of 0.123456789 ≈ 0.12345679f.
    // - precision=6 truncation -> "0.123456" (re-imports as 0.123456f, error ~7e-7)
    // - precision=9 truncation -> "0.123456790" (re-imports as ~0.12345679f, error <1e-8)
    mesh->mVertices[0] = aiVector3D(0.123456789f, 0.0f, 0.0f);
    mesh->mVertices[1] = aiVector3D(1.0f, 0.0f, 0.0f);
    mesh->mVertices[2] = aiVector3D(0.0f, 1.0f, 0.0f);
    mesh->mVertices[3] = aiVector3D(0.0f, 0.0f, 1.0f);

    const unsigned int faces[4][3] = {
        {0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}
    };
    mesh->mNumFaces = 4;
    mesh->mFaces = new aiFace[4];
    for (unsigned int i = 0; i < 4; ++i) {
        mesh->mFaces[i].mNumIndices = 3;
        mesh->mFaces[i].mIndices = new unsigned int[3]{
            faces[i][0], faces[i][1], faces[i][2]
        };
    }

    scene->mNumMaterials = 1;
    scene->mMaterials = new aiMaterial *[1];
    scene->mMaterials[0] = new aiMaterial();

    scene->mRootNode = new aiNode();
    scene->mRootNode->mNumMeshes = 1;
    scene->mRootNode->mMeshes = new unsigned int[1]{0};

    return scene;
}

} // namespace

TEST_F(utD3MFImporterExporter, export3MFDefaultPrecisionIs9Digits) {
    std::unique_ptr<aiScene> scene(buildSinglePrecisionVertexScene());

    Assimp::Exporter exporter;
    const char *outPath = "ut_3mf_precision_default.3mf";
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene.get(), "3mf", outPath));

    Assimp::Importer importer;
    const aiScene *reimported = importer.ReadFile(outPath, 0);
    ASSERT_NE(nullptr, reimported);
    ASSERT_GE(reimported->mNumMeshes, 1u);
    ASSERT_GE(reimported->mMeshes[0]->mNumVertices, 1u);

    const float reimportedX = reimported->mMeshes[0]->mVertices[0].x;
    // 1e-7 tolerance: precision-6 truncation drops the seventh decimal digit
    // (~7e-7 of error), failing this bound. Precision-9 truncation loses
    // <1e-8, well within the bound.
    EXPECT_NEAR(0.123456789f, reimportedX, 1e-7f);

    std::remove(outPath);
}

TEST_F(utD3MFImporterExporter, export3MFRespectsExplicitDecimalPrecision) {
    std::unique_ptr<aiScene> scene(buildSinglePrecisionVertexScene());

    Assimp::ExportProperties props;
    props.SetPropertyInteger("3MF_EXPORT_DECIMAL_PRECISION", 12);

    Assimp::Exporter exporter;
    const char *outPath = "ut_3mf_precision_12.3mf";
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene.get(), "3mf", outPath, 0u, &props));

    Assimp::Importer importer;
    const aiScene *reimported = importer.ReadFile(outPath, 0);
    ASSERT_NE(nullptr, reimported);
    ASSERT_GE(reimported->mNumMeshes, 1u);
    ASSERT_GE(reimported->mMeshes[0]->mNumVertices, 1u);

    // 12-digit precision exceeds float's ~7 useful decimal digits, so the
    // re-imported value is bounded by float precision (~6e-8) rather than
    // string truncation. Asserting <1e-7 confirms the writer wrote enough
    // digits to preserve full float precision.
    const float reimportedX = reimported->mMeshes[0]->mVertices[0].x;
    EXPECT_NEAR(0.123456789f, reimportedX, 1e-7f);

    std::remove(outPath);
}

TEST_F(utD3MFImporterExporter, export3MFRejectsOutOfRangePrecision) {
    // lib3mf's CModelWriter::SetDecimalPrecision asserts the precision is
    // in [1, 16]; values outside that range cause it to throw, which the
    // bridge propagates as a non-AI_SUCCESS export return.
    {
        std::unique_ptr<aiScene> scene(buildSinglePrecisionVertexScene());
        Assimp::ExportProperties props;
        props.SetPropertyInteger("3MF_EXPORT_DECIMAL_PRECISION", 0);
        Assimp::Exporter exporter;
        const char *outPath = "ut_3mf_precision_0.3mf";
        EXPECT_NE(AI_SUCCESS, exporter.Export(scene.get(), "3mf", outPath, 0u, &props));
        std::remove(outPath);
    }
    {
        std::unique_ptr<aiScene> scene(buildSinglePrecisionVertexScene());
        Assimp::ExportProperties props;
        props.SetPropertyInteger("3MF_EXPORT_DECIMAL_PRECISION", 17);
        Assimp::Exporter exporter;
        const char *outPath = "ut_3mf_precision_17.3mf";
        EXPECT_NE(AI_SUCCESS, exporter.Export(scene.get(), "3mf", outPath, 0u, &props));
        std::remove(outPath);
    }
}
#endif

// ===== R2: 3MF exporter mEnforcePP — multi-mesh structure preserved =====
//
// The 3MF exporter now enforces aiProcess_Triangulate | aiProcess_JoinIdenticalVertices
// (plus FindDegenerates / FindInvalidData defensively, see R4). JoinIdenticalVertices
// operates per-aiMesh (see PostProcessing/JoinVerticesProcess.cpp) so it MUST NOT
// merge distinct meshes — preserving per-mesh material colors that 3MF stores as
// per-object base material refs.
//
// See docs/research/3mf-export-rendering-artifacts.md (R2).

namespace {

aiMesh *buildColoredBoxMesh(unsigned int materialIndex, float offsetX) {
    aiMesh *mesh = new aiMesh();
    const aiVector3D vertices[8] = {
        {offsetX - 1, -1, -1}, {offsetX + 1, -1, -1},
        {offsetX + 1, +1, -1}, {offsetX - 1, +1, -1},
        {offsetX - 1, -1, +1}, {offsetX + 1, -1, +1},
        {offsetX + 1, +1, +1}, {offsetX - 1, +1, +1},
    };
    const unsigned int triangles[12][3] = {
        {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4}, {3, 7, 6}, {3, 6, 2},
        {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5},
    };
    mesh->mNumVertices = 8;
    mesh->mVertices = new aiVector3D[8];
    std::copy(std::begin(vertices), std::end(vertices), mesh->mVertices);
    mesh->mNumFaces = 12;
    mesh->mFaces = new aiFace[12];
    for (unsigned int i = 0; i < 12; ++i) {
        mesh->mFaces[i].mNumIndices = 3;
        mesh->mFaces[i].mIndices = new unsigned int[3]{
            triangles[i][0], triangles[i][1], triangles[i][2]
        };
    }
    mesh->mMaterialIndex = materialIndex;
    return mesh;
}

aiMaterial *buildColoredMaterial(float r, float g, float b) {
    aiMaterial *mat = new aiMaterial();
    aiColor4D color(r, g, b, 1.0f);
    mat->AddProperty(&color, 1, AI_MATKEY_COLOR_DIFFUSE);
    return mat;
}

} // namespace

TEST_F(utD3MFImporterExporter, export3MFPreservesMeshCountWithJoinIdenticalVertices) {
    std::unique_ptr<aiScene> scene(new aiScene());
    scene->mNumMeshes = 3;
    scene->mMeshes = new aiMesh *[3];
    scene->mMeshes[0] = buildColoredBoxMesh(0, -4.0f);
    scene->mMeshes[1] = buildColoredBoxMesh(1, 0.0f);
    scene->mMeshes[2] = buildColoredBoxMesh(2, 4.0f);

    scene->mNumMaterials = 3;
    scene->mMaterials = new aiMaterial *[3];
    scene->mMaterials[0] = buildColoredMaterial(1.0f, 0.0f, 0.0f);
    scene->mMaterials[1] = buildColoredMaterial(0.0f, 1.0f, 0.0f);
    scene->mMaterials[2] = buildColoredMaterial(0.0f, 0.0f, 1.0f);

    scene->mRootNode = new aiNode();
    scene->mRootNode->mNumMeshes = 3;
    scene->mRootNode->mMeshes = new unsigned int[3]{0, 1, 2};

    Assimp::Exporter exporter;
    const char *outPath = "ut_3mf_multi_mesh.3mf";
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene.get(), "3mf", outPath));

    Assimp::Importer importer;
    const aiScene *reimported = importer.ReadFile(outPath, 0);
    ASSERT_NE(nullptr, reimported);

    // Per-mesh material structure must be preserved — the enforced
    // JoinIdenticalVertices step must NOT collapse meshes together.
    EXPECT_EQ(3u, reimported->mNumMeshes);
    EXPECT_EQ(3u, reimported->mNumMaterials);

    // Per-mesh colors must round-trip via the 3MF base material group.
    aiColor4D colors[3];
    for (unsigned int i = 0; i < 3 && i < reimported->mNumMaterials; ++i) {
        ASSERT_EQ(AI_SUCCESS,
            reimported->mMaterials[i]->Get(AI_MATKEY_COLOR_DIFFUSE, colors[i]));
    }
    // Channel sums (R+G+B) must each cover one primary channel — order is
    // implementation-defined so we assert presence rather than ordering.
    float rTotal = 0.0f, gTotal = 0.0f, bTotal = 0.0f;
    for (unsigned int i = 0; i < 3; ++i) {
        rTotal += colors[i].r;
        gTotal += colors[i].g;
        bTotal += colors[i].b;
    }
    EXPECT_NEAR(1.0f, rTotal, 0.05f);
    EXPECT_NEAR(1.0f, gTotal, 0.05f);
    EXPECT_NEAR(1.0f, bTotal, 0.05f);

    std::remove(outPath);
}

TEST_F(utD3MFImporterExporter, export3MFTriangulatesQuadFaces) {
    std::unique_ptr<aiScene> scene(new aiScene());
    scene->mNumMeshes = 1;
    scene->mMeshes = new aiMesh *[1];
    aiMesh *mesh = new aiMesh();
    scene->mMeshes[0] = mesh;

    const aiVector3D vertices[8] = {
        {-1, -1, -1}, {+1, -1, -1}, {+1, +1, -1}, {-1, +1, -1},
        {-1, -1, +1}, {+1, -1, +1}, {+1, +1, +1}, {-1, +1, +1},
    };
    const unsigned int quads[6][4] = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
        {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5},
    };
    mesh->mNumVertices = 8;
    mesh->mVertices = new aiVector3D[8];
    std::copy(std::begin(vertices), std::end(vertices), mesh->mVertices);
    mesh->mNumFaces = 6;
    mesh->mFaces = new aiFace[6];
    for (unsigned int i = 0; i < 6; ++i) {
        mesh->mFaces[i].mNumIndices = 4;
        mesh->mFaces[i].mIndices = new unsigned int[4]{
            quads[i][0], quads[i][1], quads[i][2], quads[i][3]
        };
    }

    scene->mNumMaterials = 1;
    scene->mMaterials = new aiMaterial *[1];
    scene->mMaterials[0] = new aiMaterial();

    scene->mRootNode = new aiNode();
    scene->mRootNode->mNumMeshes = 1;
    scene->mRootNode->mMeshes = new unsigned int[1]{0};

    Assimp::Exporter exporter;
    const char *outPath = "ut_3mf_quad_triangulate.3mf";
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene.get(), "3mf", outPath));

    Assimp::Importer importer;
    const aiScene *reimported = importer.ReadFile(outPath, 0);
    ASSERT_NE(nullptr, reimported);
    ASSERT_GE(reimported->mNumMeshes, 1u);

    EXPECT_EQ(12u, reimported->mMeshes[0]->mNumFaces);
    for (unsigned int f = 0; f < reimported->mMeshes[0]->mNumFaces; ++f) {
        EXPECT_EQ(3u, reimported->mMeshes[0]->mFaces[f].mNumIndices);
    }

    std::remove(outPath);
}

TEST_F(utD3MFImporterExporter, export3MFWeldsIdenticalVerticesWithinMesh) {
    std::unique_ptr<aiScene> scene(makePerFaceBoxScene(aiVector3D(1.0f)));

    Assimp::Exporter exporter;
    const char *outPath = "ut_3mf_weld_vertices.3mf";
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene.get(), "3mf", outPath));

    Assimp::Importer importer;
    const aiScene *reimported = importer.ReadFile(outPath, 0);
    ASSERT_NE(nullptr, reimported);
    ASSERT_GE(reimported->mNumMeshes, 1u);

    EXPECT_EQ(8u, reimported->mMeshes[0]->mNumVertices);
    EXPECT_EQ(12u, reimported->mMeshes[0]->mNumFaces);

    std::remove(outPath);
}

// ===== R3: Lib3MFBridge handles non-triangle faces without polluting output =====
//
// Prior to R3, the bridge allocated a triangle vector sized to mNumFaces and
// only assigned slots for triangle faces, leaving zero-initialized degenerate
// triangles for non-triangles. lib3mf rejects degenerate triangles, so any
// scene reaching the bridge with a non-triangle face caused the export to
// throw. R3 switches to push_back so non-triangles are silently skipped and
// the triangle vector size matches the actual triangle count.
//
// This test bypasses Assimp::Exporter (which now enforces Triangulate via R2)
// and calls the bridge directly to isolate the bug.
//
// See docs/research/3mf-export-rendering-artifacts.md (R3).

#ifdef ASSIMP_USE_LIB3MF

TEST_F(utD3MFImporterExporter, export3MFBridgeRejectsOpenGeometryAfterSkippingNonTriangleFaces) {
    std::unique_ptr<aiScene> scene(new aiScene());
    scene->mNumMeshes = 1;
    scene->mMeshes = new aiMesh *[1];
    aiMesh *mesh = new aiMesh();
    scene->mMeshes[0] = mesh;

    mesh->mNumVertices = 5;
    mesh->mVertices = new aiVector3D[5];
    mesh->mVertices[0] = aiVector3D(0.0f, 0.0f, 0.0f);
    mesh->mVertices[1] = aiVector3D(1.0f, 0.0f, 0.0f);
    mesh->mVertices[2] = aiVector3D(2.0f, 1.0f, 0.0f);
    mesh->mVertices[3] = aiVector3D(1.0f, 2.0f, 0.0f);
    mesh->mVertices[4] = aiVector3D(0.0f, 1.0f, 0.0f);

    // One pentagon face plus one well-formed triangle. The bridge must skip
    // the pentagon (no degenerate slot) and keep the triangle.
    mesh->mNumFaces = 2;
    mesh->mFaces = new aiFace[2];
    mesh->mFaces[0].mNumIndices = 5;
    mesh->mFaces[0].mIndices = new unsigned int[5]{0, 1, 2, 3, 4};
    mesh->mFaces[1].mNumIndices = 3;
    mesh->mFaces[1].mIndices = new unsigned int[3]{0, 1, 2};

    scene->mNumMaterials = 1;
    scene->mMaterials = new aiMaterial *[1];
    scene->mMaterials[0] = new aiMaterial();

    scene->mRootNode = new aiNode();
    scene->mRootNode->mNumMeshes = 1;
    scene->mRootNode->mMeshes = new unsigned int[1]{0};

    Assimp::DefaultIOSystem ioSystem;
    const std::string outPath = "ut_3mf_bridge_polygon.3mf";

    // Bypass Assimp::Exporter to prove the bridge rejects the open triangle
    // left after unsupported faces are skipped.
    EXPECT_THROW(
            Assimp::D3MF::Lib3MFBridge::ExportScene(scene.get(), outPath, &ioSystem),
            DeadlyExportError);
    std::remove(outPath.c_str());
}

#endif // ASSIMP_USE_LIB3MF

#endif // ASSIMP_BUILD_NO_EXPORT
