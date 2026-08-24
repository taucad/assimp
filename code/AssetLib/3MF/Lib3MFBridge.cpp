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

#ifdef ASSIMP_USE_LIB3MF

#include "Lib3MFBridge.h"

#include "Common/UnitAxisContract.h"

#include <assimp/Exceptional.h>
#include <assimp/commonMetaData.h>
#include <assimp/metadata.h>
#include <assimp/scene.h>
#include <assimp/mesh.h>
#include <assimp/material.h>
#include <assimp/DefaultLogger.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Exporter.hpp>

#include <lib3mf_types.hpp>
#include <lib3mf_abi.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace Assimp {
namespace D3MF {

namespace {

constexpr uint64_t FnvOffset = 14695981039346656037ULL;
constexpr uint64_t FnvPrime = 1099511628211ULL;

uint64_t hashBytes(uint64_t hash, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t index = 0; index < size; ++index) {
        hash = (hash ^ bytes[index]) * FnvPrime;
    }
    return hash;
}

uint64_t hashSceneNode(uint64_t hash, const aiNode *node) {
    if (node == nullptr) {
        return hash;
    }
    hash = hashBytes(hash, node->mName.C_Str(), node->mName.length);
    hash = hashBytes(hash, &node->mTransformation, sizeof(node->mTransformation));
    hash = hashBytes(hash, node->mMeshes, node->mNumMeshes * sizeof(*node->mMeshes));
    for (unsigned int index = 0; index < node->mNumChildren; ++index) {
        hash = hashSceneNode(hash, node->mChildren[index]);
    }
    return hash;
}

uint64_t hashScene(const aiScene *scene) {
    uint64_t hash = hashBytes(FnvOffset, &scene->mNumMeshes, sizeof(scene->mNumMeshes));
    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        const aiMesh *mesh = scene->mMeshes[meshIndex];
        hash = hashBytes(hash, mesh->mName.C_Str(), mesh->mName.length);
        hash = hashBytes(hash, mesh->mVertices, mesh->mNumVertices * sizeof(*mesh->mVertices));
        for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
            const aiFace &face = mesh->mFaces[faceIndex];
            hash = hashBytes(hash, face.mIndices, face.mNumIndices * sizeof(*face.mIndices));
        }
    }
    return hashSceneNode(hash, scene->mRootNode);
}

uint64_t splitMix64(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::string deterministicUuid(uint64_t sceneHash, uint64_t ordinal) {
    const uint64_t high = splitMix64(sceneHash ^ ordinal);
    const uint64_t low = splitMix64(high);
    uint8_t bytes[16];
    for (unsigned int index = 0; index < 8; ++index) {
        bytes[index] = static_cast<uint8_t>(high >> (index * 8));
        bytes[index + 8] = static_cast<uint8_t>(low >> (index * 8));
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x80); // UUIDv8: content-derived.
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
    char uuid[37];
    std::snprintf(uuid, sizeof(uuid),
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return uuid;
}

class Lib3MFHandle {
public:
    explicit Lib3MFHandle(Lib3MF_Base handle = nullptr) : mHandle(handle) {}
    ~Lib3MFHandle() {
        if (mHandle) {
            lib3mf_release(mHandle);
        }
    }
    Lib3MFHandle(const Lib3MFHandle &) = delete;
    Lib3MFHandle &operator=(const Lib3MFHandle &) = delete;
    Lib3MFHandle(Lib3MFHandle &&other) noexcept : mHandle(other.mHandle) { other.mHandle = nullptr; }
    Lib3MFHandle &operator=(Lib3MFHandle &&other) noexcept {
        if (this != &other) {
            if (mHandle) { lib3mf_release(mHandle); }
            mHandle = other.mHandle;
            other.mHandle = nullptr;
        }
        return *this;
    }

    Lib3MF_Base get() const { return mHandle; }
    Lib3MF_Base *ptr() { return &mHandle; }

    template <typename T>
    T as() const { return static_cast<T>(mHandle); }

private:
    Lib3MF_Base mHandle;
};

void checkResult(Lib3MFResult result, Lib3MF_Base instance, const char *context) {
    if (result != LIB3MF_SUCCESS) {
        std::string errorMsg = context;
        if (instance) {
            Lib3MF_uint32 neededChars = 0;
            bool hasError = false;
            lib3mf_getlasterror(instance, 0, &neededChars, nullptr, &hasError);
            if (hasError && neededChars > 0) {
                std::vector<char> buffer(neededChars + 1, '\0');
                lib3mf_getlasterror(instance, neededChars + 1, &neededChars, buffer.data(), &hasError);
                errorMsg += ": ";
                errorMsg += buffer.data();
            }
        }
        throw DeadlyExportError(errorMsg);
    }
}

void checkImportResult(Lib3MFResult result, Lib3MF_Base instance, const char *context) {
    if (result != LIB3MF_SUCCESS) {
        std::string errorMsg = context;
        if (instance) {
            Lib3MF_uint32 neededChars = 0;
            bool hasError = false;
            lib3mf_getlasterror(instance, 0, &neededChars, nullptr, &hasError);
            if (hasError && neededChars > 0) {
                std::vector<char> buffer(neededChars + 1, '\0');
                lib3mf_getlasterror(instance, neededChars + 1, &neededChars, buffer.data(), &hasError);
                errorMsg += ": ";
                errorMsg += buffer.data();
            }
        }
        throw DeadlyImportError(errorMsg);
    }
}

Lib3MF::sTransform aiMatrixToLib3MFTransform(const aiMatrix4x4 &m) {
    Lib3MF::sTransform t;
    t.m_Fields[0][0] = m.a1; t.m_Fields[0][1] = m.b1; t.m_Fields[0][2] = m.c1;
    t.m_Fields[1][0] = m.a2; t.m_Fields[1][1] = m.b2; t.m_Fields[1][2] = m.c2;
    t.m_Fields[2][0] = m.a3; t.m_Fields[2][1] = m.b3; t.m_Fields[2][2] = m.c3;
    t.m_Fields[3][0] = m.a4; t.m_Fields[3][1] = m.b4; t.m_Fields[3][2] = m.c4;
    return t;
}

aiMatrix4x4 lib3MFTransformToAiMatrix(const Lib3MF::sTransform &t) {
    aiMatrix4x4 m;
    m.a1 = t.m_Fields[0][0]; m.b1 = t.m_Fields[0][1]; m.c1 = t.m_Fields[0][2]; m.d1 = 0.0f;
    m.a2 = t.m_Fields[1][0]; m.b2 = t.m_Fields[1][1]; m.c2 = t.m_Fields[1][2]; m.d2 = 0.0f;
    m.a3 = t.m_Fields[2][0]; m.b3 = t.m_Fields[2][1]; m.c3 = t.m_Fields[2][2]; m.d3 = 0.0f;
    m.a4 = t.m_Fields[3][0]; m.b4 = t.m_Fields[3][1]; m.c4 = t.m_Fields[3][2]; m.d4 = 1.0f;
    return m;
}

Lib3MF::sColor aiColorToLib3MF(const aiColor4D &c) {
    Lib3MF::sColor color;
    color.m_Red = static_cast<Lib3MF_uint8>(c.r * 255.0f);
    color.m_Green = static_cast<Lib3MF_uint8>(c.g * 255.0f);
    color.m_Blue = static_cast<Lib3MF_uint8>(c.b * 255.0f);
    color.m_Alpha = static_cast<Lib3MF_uint8>(c.a * 255.0f);
    return color;
}

aiColor4D lib3MFColorToAi(const Lib3MF::sColor &c) {
    return aiColor4D(
        c.m_Red / 255.0f,
        c.m_Green / 255.0f,
        c.m_Blue / 255.0f,
        c.m_Alpha / 255.0f
    );
}

// ===== EXPORT IMPLEMENTATION =====

Lib3MF::eModelUnit stringToModelUnit(const std::string &unitStr) {
    if (unitStr == "micron")      return Lib3MF::eModelUnit::MicroMeter;
    if (unitStr == "millimeter")  return Lib3MF::eModelUnit::MilliMeter;
    if (unitStr == "centimeter")  return Lib3MF::eModelUnit::CentiMeter;
    if (unitStr == "inch")        return Lib3MF::eModelUnit::Inch;
    if (unitStr == "foot")        return Lib3MF::eModelUnit::Foot;
    if (unitStr == "meter")       return Lib3MF::eModelUnit::Meter;
    return Lib3MF::eModelUnit::MilliMeter;
}

// Conversion from each lib3mf model unit to meters. Used to drive vertex rescaling
// in both directions (source-meters → target-units on export, model-unit → meters on
// import metadata write).
double modelUnitToMeters(Lib3MF::eModelUnit unit) {
    switch (unit) {
        case Lib3MF::eModelUnit::MicroMeter:  return 1e-6;
        case Lib3MF::eModelUnit::MilliMeter:  return 1e-3;
        case Lib3MF::eModelUnit::CentiMeter:  return 1e-2;
        case Lib3MF::eModelUnit::Inch:        return 0.0254;
        case Lib3MF::eModelUnit::Foot:        return 0.3048;
        case Lib3MF::eModelUnit::Meter:       return 1.0;
    }
    return 1e-3;
}

// Axis/unit resolver helpers (validateUpAxisInt, buildAxisRotationMatrix,
// readSceneUnitScaleToMeters, readSceneUpAxis) live in
// code/Common/UnitAxisContract.h so every importer/exporter that adopts the
// contract uses the same implementation. We `using`-import the symbols here
// to keep the existing call sites unchanged after the refactor.
using ::Assimp::validateUpAxisInt;
using ::Assimp::buildAxisRotationMatrix;
using ::Assimp::readSceneUnitScaleToMeters;
using ::Assimp::readSceneUpAxis;
using ::Assimp::writeContractMetadata;

void collectMeshNodes(const aiScene *scene, const aiNode *node,
                      const aiMatrix4x4 &parentTransform,
                      std::vector<std::pair<unsigned int, aiMatrix4x4>> &meshEntries) {
    aiMatrix4x4 globalTransform = parentTransform * node->mTransformation;
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        meshEntries.emplace_back(node->mMeshes[i], globalTransform);
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        collectMeshNodes(scene, node->mChildren[i], globalTransform, meshEntries);
    }
}

// Quantises a vertex position into a 64-bit hash key with `epsilon`-sized buckets.
// Two positions land in the same bucket iff their componentwise distance is below
// the epsilon — the welder collapses them to a single output vertex.
struct QuantisedKey {
    int64_t x, y, z;
    bool operator==(const QuantisedKey &o) const noexcept {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct QuantisedKeyHash {
    size_t operator()(const QuantisedKey &k) const noexcept {
        // 64-bit splitmix-style avalanche then xor — sufficient for vertex dedup.
        auto mix = [](uint64_t v) {
            v ^= v >> 30; v *= 0xbf58476d1ce4e5b9ULL;
            v ^= v >> 27; v *= 0x94d049bb133111ebULL;
            v ^= v >> 31;
            return v;
        };
        uint64_t h = mix(static_cast<uint64_t>(k.x));
        h ^= mix(static_cast<uint64_t>(k.y)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= mix(static_cast<uint64_t>(k.z)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return static_cast<size_t>(h);
    }
};

// Position-only welder: collapses duplicate vertex positions and rewrites
// triangle indices so non-manifold "seams" introduced by per-face normals or
// per-face UVs disappear before the geometry crosses into lib3mf. Only positions
// are compared — normals/UVs are NOT propagated to lib3mf, so seam attributes are
// irrelevant. Triangles that become degenerate (two indices collapse to one)
// after welding are dropped — they were never going to be printable.
void weldByPosition(std::vector<Lib3MF::sPosition> &vertices,
                    std::vector<Lib3MF::sTriangle> &triangles,
                    double epsilon) {
    if (vertices.empty() || triangles.empty()) {
        return;
    }

    const double inv = 1.0 / epsilon;
    std::unordered_map<QuantisedKey, uint32_t, QuantisedKeyHash> dedup;
    dedup.reserve(vertices.size());

    std::vector<Lib3MF::sPosition> outVertices;
    outVertices.reserve(vertices.size());
    std::vector<uint32_t> remap(vertices.size());

    for (size_t i = 0; i < vertices.size(); ++i) {
        QuantisedKey key{
            static_cast<int64_t>(std::llround(vertices[i].m_Coordinates[0] * inv)),
            static_cast<int64_t>(std::llround(vertices[i].m_Coordinates[1] * inv)),
            static_cast<int64_t>(std::llround(vertices[i].m_Coordinates[2] * inv)),
        };
        auto [it, inserted] = dedup.try_emplace(key, static_cast<uint32_t>(outVertices.size()));
        if (inserted) {
            outVertices.push_back(vertices[i]);
        }
        remap[i] = it->second;
    }

    std::vector<Lib3MF::sTriangle> outTriangles;
    outTriangles.reserve(triangles.size());
    for (const auto &tri : triangles) {
        Lib3MF::sTriangle remapped;
        remapped.m_Indices[0] = remap[tri.m_Indices[0]];
        remapped.m_Indices[1] = remap[tri.m_Indices[1]];
        remapped.m_Indices[2] = remap[tri.m_Indices[2]];
        if (remapped.m_Indices[0] == remapped.m_Indices[1] ||
            remapped.m_Indices[1] == remapped.m_Indices[2] ||
            remapped.m_Indices[0] == remapped.m_Indices[2]) {
            continue;
        }
        outTriangles.push_back(remapped);
    }

    vertices.swap(outVertices);
    triangles.swap(outTriangles);
}

void exportToLib3MF(const aiScene *pScene, std::vector<Lib3MF_uint8> &outputBuffer,
                    const ExportProperties *pProperties) {
    Lib3MFHandle model;
    checkResult(lib3mf_createmodel(model.ptr()), nullptr, "Failed to create lib3mf model");
    const uint64_t sceneHash = hashScene(pScene);
    const std::string buildUuid = deterministicUuid(sceneHash, 0);
    checkResult(lib3mf_model_setbuilduuid(model.as<Lib3MF_Model>(), buildUuid.c_str()),
            model.get(), "Failed to set deterministic build UUID");

    // ---- TARGET UNIT (3MF file) ----
    // Resolver: ExportProperty `3MF_EXPORT_UNIT` (string) → spec default `millimeter`.
    std::string targetUnitStr = "millimeter";
    if (pProperties) {
        targetUnitStr = pProperties->GetPropertyString("3MF_EXPORT_UNIT", "millimeter");
    }
    Lib3MF::eModelUnit targetUnit = stringToModelUnit(targetUnitStr);
    const double targetUnitToMeters = modelUnitToMeters(targetUnit);
    checkResult(
        lib3mf_model_setunit(model.as<Lib3MF_Model>(), targetUnit),
        model.get(), "Failed to set 3MF model unit"
    );

    // ---- TARGET UP-AXIS (3MF coordinate system) ----
    // Resolver: ExportProperty `3MF_EXPORT_UPAXIS` (int32, 0=X, 1=Y, 2=Z) → 3MF
    // Core Spec §3.3 default of +Z. Out-of-range values throw DeadlyExportError.
    int32_t targetUpAxis = 2;
    if (pProperties) {
        targetUpAxis = pProperties->GetPropertyInteger("3MF_EXPORT_UPAXIS", 2);
        targetUpAxis = validateUpAxisInt(targetUpAxis, "3MF_EXPORT_UPAXIS");
    }

    // ---- SOURCE UNIT-SCALE ----
    // Resolver: scene `AI_METADATA_UNIT_SCALE_TO_METERS` → identity (no rescale).
    // Identity here means the resulting 3MF declares the target unit but does not
    // rescale vertices — caller-source-units are written literally. This is the
    // conservative "visible-tiny" default (per research-doc R8h) for unmigrated
    // importers — never silently 100×-corrupt user geometry.
    const double sourceUnitToMeters = readSceneUnitScaleToMeters(pScene);
    const double vertexScale = (sourceUnitToMeters > 0.0)
        ? (sourceUnitToMeters / targetUnitToMeters)
        : 1.0;

    // ---- SOURCE UP-AXIS ----
    // Resolver: scene `AI_METADATA_UP_AXIS` → -1 (skip rotation entirely).
    const int32_t sourceUpAxis = readSceneUpAxis(pScene);
    aiMatrix4x4 axisRotation; // identity by default
    if (sourceUpAxis >= 0) {
        validateUpAxisInt(sourceUpAxis, AI_METADATA_UP_AXIS " (scene metadata)");
        axisRotation = buildAxisRotationMatrix(sourceUpAxis, targetUpAxis);
    }

    if (pProperties) {
        std::string app = pProperties->GetPropertyString("3MF_EXPORT_APPLICATION", "");
        if (!app.empty()) {
            Lib3MFHandle metadataGroup;
            checkResult(
                lib3mf_model_getmetadatagroup(model.as<Lib3MF_Model>(), metadataGroup.ptr()),
                model.get(), "Failed to get metadata group"
            );
            Lib3MFHandle metadata;
            checkResult(
                lib3mf_metadatagroup_addmetadata(
                    metadataGroup.as<Lib3MF_MetaDataGroup>(),
                    "", "Application", app.c_str(), "xs:string", true,
                    metadata.ptr()
                ),
                metadataGroup.get(), "Failed to add Application metadata"
            );
        }
    }

    Lib3MFHandle baseMaterialGroup;
    std::vector<Lib3MF_uint32> materialPropertyIDs;

    if (pScene->mNumMaterials > 0) {
        checkResult(
            lib3mf_model_addbasematerialgroup(model.as<Lib3MF_Model>(), baseMaterialGroup.ptr()),
            model.get(), "Failed to create base material group"
        );

        for (unsigned int i = 0; i < pScene->mNumMaterials; ++i) {
            const aiMaterial *mat = pScene->mMaterials[i];
            aiColor4D diffuse(0.8f, 0.8f, 0.8f, 1.0f);
            mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse);

            aiString matName;
            if (mat->Get(AI_MATKEY_NAME, matName) != AI_SUCCESS) {
                matName = aiString("Material_" + std::to_string(i));
            }

            Lib3MF::sColor color = aiColorToLib3MF(diffuse);
            Lib3MF_uint32 propID = 0;
            checkResult(
                lib3mf_basematerialgroup_addmaterial(
                    baseMaterialGroup.as<Lib3MF_BaseMaterialGroup>(),
                    matName.C_Str(), &color, &propID
                ),
                baseMaterialGroup.get(), "Failed to add material"
            );
            materialPropertyIDs.push_back(propID);
        }
    }

    std::vector<std::pair<unsigned int, aiMatrix4x4>> meshEntries;
    if (pScene->mRootNode) {
        collectMeshNodes(pScene, pScene->mRootNode, aiMatrix4x4(), meshEntries);
    }

    if (meshEntries.empty()) {
        for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
            meshEntries.emplace_back(i, aiMatrix4x4());
        }
    }

    uint64_t meshOrdinal = 0;
    for (const auto &entry : meshEntries) {
        unsigned int meshIdx = entry.first;
        const aiMatrix4x4 &nodeTransform = entry.second;
        const aiMesh *mesh = pScene->mMeshes[meshIdx];

        Lib3MFHandle meshObject;
        checkResult(
            lib3mf_model_addmeshobject(model.as<Lib3MF_Model>(), meshObject.ptr()),
            model.get(), "Failed to add mesh object"
        );
        const std::string objectUuid = deterministicUuid(sceneHash, 1 + meshOrdinal * 2);
        checkResult(lib3mf_object_setuuid(meshObject.as<Lib3MF_Object>(), objectUuid.c_str()),
                meshObject.get(), "Failed to set deterministic object UUID");

        if (mesh->mName.length > 0) {
            lib3mf_object_setname(meshObject.as<Lib3MF_Object>(), mesh->mName.C_Str());
        }

        // Bake (axis rotation) × (unit scale) × (per-node hierarchy transform) into
        // every vertex so the build item can carry an identity transform. The
        // `aiProcess_PreTransformVertices` pp flag (R9') normally collapses the
        // hierarchy ahead of the bridge, but we still apply `nodeTransform` here so
        // direct `Lib3MFBridge::ExportScene` callers (bypassing the top-level
        // Exporter) get correct output too.
        aiMatrix4x4 unitScaleMatrix;
        unitScaleMatrix.a1 = static_cast<float>(vertexScale);
        unitScaleMatrix.b2 = static_cast<float>(vertexScale);
        unitScaleMatrix.c3 = static_cast<float>(vertexScale);

        const aiMatrix4x4 vertexTransform = axisRotation * unitScaleMatrix * nodeTransform;

        std::vector<Lib3MF::sPosition> vertices(mesh->mNumVertices);
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            aiVector3D p = vertexTransform * mesh->mVertices[v];
            vertices[v].m_Coordinates[0] = p.x;
            vertices[v].m_Coordinates[1] = p.y;
            vertices[v].m_Coordinates[2] = p.z;
        }

        // R3: build the triangle vector via push_back so non-triangle faces
        // are silently skipped instead of leaving zero-initialized degenerate
        // slots. lib3mf rejects degenerate triangles and would throw on any
        // mesh containing a non-triangle face if we left the old indexed
        // assignment (see docs/research/3mf-export-rendering-artifacts.md R3).
        std::vector<Lib3MF::sTriangle> triangles;
        triangles.reserve(mesh->mNumFaces);
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace &face = mesh->mFaces[f];
            if (face.mNumIndices != 3) {
                continue;
            }
            Lib3MF::sTriangle tri{};
            tri.m_Indices[0] = face.mIndices[0];
            tri.m_Indices[1] = face.mIndices[1];
            tri.m_Indices[2] = face.mIndices[2];
            triangles.push_back(tri);
        }

        // Position-only weld closes the per-face-normal seams that produced the
        // "non-manifold edge" diagnostics in slicers. Epsilon is sized at one
        // micrometre in scene units (post-rescale) so that tessellation noise
        // disappears but legitimately distinct CAD vertices survive.
        weldByPosition(vertices, triangles, 1e-6);

        checkResult(
            lib3mf_meshobject_setgeometry(
                meshObject.as<Lib3MF_MeshObject>(),
                vertices.size(), vertices.data(),
                triangles.size(), triangles.data()
            ),
            meshObject.get(), "Failed to set mesh geometry"
        );

        if (!materialPropertyIDs.empty() && mesh->mMaterialIndex < materialPropertyIDs.size()) {
            Lib3MF_uint32 matPropID = materialPropertyIDs[mesh->mMaterialIndex];

            Lib3MF_uint32 uniqueResID = 0;
            lib3mf_resource_getuniqueresourceid(baseMaterialGroup.as<Lib3MF_Resource>(), &uniqueResID);

            lib3mf_meshobject_setobjectlevelproperty(
                meshObject.as<Lib3MF_MeshObject>(), uniqueResID, matPropID
            );

            std::vector<Lib3MF::sTriangleProperties> triProps(triangles.size());
            for (size_t f = 0; f < triangles.size(); ++f) {
                triProps[f].m_ResourceID = uniqueResID;
                triProps[f].m_PropertyIDs[0] = matPropID;
                triProps[f].m_PropertyIDs[1] = matPropID;
                triProps[f].m_PropertyIDs[2] = matPropID;
            }
            lib3mf_meshobject_setalltriangleproperties(
                meshObject.as<Lib3MF_MeshObject>(),
                triProps.size(), triProps.data()
            );
        }

        // Build-item transform is identity — every transform contribution has
        // already been baked into the vertex stream above.
        Lib3MFHandle buildItem;
        Lib3MF::sTransform identityTransform = aiMatrixToLib3MFTransform(aiMatrix4x4());
        checkResult(
            lib3mf_model_addbuilditem(
                model.as<Lib3MF_Model>(),
                meshObject.as<Lib3MF_Object>(),
                &identityTransform,
                buildItem.ptr()
            ),
            model.get(), "Failed to add build item"
        );
        const std::string itemUuid = deterministicUuid(sceneHash, 2 + meshOrdinal * 2);
        checkResult(lib3mf_builditem_setuuid(buildItem.as<Lib3MF_BuildItem>(), itemUuid.c_str()),
                buildItem.get(), "Failed to set deterministic build item UUID");
        ++meshOrdinal;
    }

    Lib3MFHandle writer;
    checkResult(
        lib3mf_model_querywriter(model.as<Lib3MF_Model>(), "3mf", writer.ptr()),
        model.get(), "Failed to create 3MF writer"
    );

    // R1: configure lib3mf decimal precision via the writer. lib3mf's default
    // is 6 decimal digits with truncation toward zero, which loses ~1µm of
    // asymmetric precision per coordinate and creates nm-scale gaps between
    // independent <object> mesh boundaries (visible in slicers as missing
    // fragments). Default to 9 (preserves full float precision); callers may
    // override via the "3MF_EXPORT_DECIMAL_PRECISION" ExportProperties key.
    // Valid range is [1, 16]; out-of-range values cause lib3mf to throw,
    // which we propagate as DeadlyExportError via checkResult.
    // See docs/research/3mf-export-rendering-artifacts.md (R1).
    const Lib3MF_uint32 precision = pProperties != nullptr
        ? static_cast<Lib3MF_uint32>(
            pProperties->GetPropertyInteger("3MF_EXPORT_DECIMAL_PRECISION", 9))
        : 9u;
    checkResult(
        lib3mf_writer_setdecimalprecision(writer.as<Lib3MF_Writer>(), precision),
        writer.get(), "Failed to set 3MF decimal precision"
    );

    Lib3MF_uint64 neededSize = 0;
    checkResult(
        lib3mf_writer_writetobuffer(writer.as<Lib3MF_Writer>(), 0, &neededSize, nullptr),
        writer.get(), "Failed to get 3MF buffer size"
    );

    outputBuffer.resize(static_cast<size_t>(neededSize));
    Lib3MF_uint64 writtenSize = 0;
    checkResult(
        lib3mf_writer_writetobuffer(
            writer.as<Lib3MF_Writer>(),
            neededSize, &writtenSize,
            outputBuffer.data()
        ),
        writer.get(), "Failed to write 3MF to buffer"
    );
    outputBuffer.resize(static_cast<size_t>(writtenSize));
}

// ===== IMPORT IMPLEMENTATION =====

void importFromLib3MF(aiScene *pScene, const std::vector<Lib3MF_uint8> &inputBuffer) {
    Lib3MFHandle model;
    checkImportResult(lib3mf_createmodel(model.ptr()), nullptr, "Failed to create lib3mf model");

    Lib3MFHandle reader;
    checkImportResult(
        lib3mf_model_queryreader(model.as<Lib3MF_Model>(), "3mf", reader.ptr()),
        model.get(), "Failed to create 3MF reader"
    );

    lib3mf_reader_setstrictmodeactive(reader.as<Lib3MF_Reader>(), false);

    checkImportResult(
        lib3mf_reader_readfrombuffer(
            reader.as<Lib3MF_Reader>(),
            inputBuffer.size(),
            inputBuffer.data()
        ),
        reader.get(), "Failed to read 3MF from buffer"
    );

    // First pass: count meshes
    Lib3MFHandle meshIterator;
    checkImportResult(
        lib3mf_model_getmeshobjects(model.as<Lib3MF_Model>(), meshIterator.ptr()),
        model.get(), "Failed to get mesh objects"
    );

    struct MeshData {
        std::string name;
        Lib3MF_uint32 vertexCount;
        Lib3MF_uint32 triangleCount;
    };
    std::vector<MeshData> meshDataList;

    bool hasNext = false;
    lib3mf_resourceiterator_movenext(meshIterator.as<Lib3MF_ResourceIterator>(), &hasNext);
    while (hasNext) {
        Lib3MFHandle currentMesh;
        checkImportResult(
            lib3mf_meshobjectiterator_getcurrentmeshobject(
                meshIterator.as<Lib3MF_MeshObjectIterator>(), currentMesh.ptr()
            ),
            meshIterator.get(), "Failed to get current mesh object"
        );

        MeshData md;
        Lib3MF_MeshObject meshObj = currentMesh.as<Lib3MF_MeshObject>();

        Lib3MF_uint32 neededChars = 0;
        lib3mf_object_getname(meshObj, 0, &neededChars, nullptr);
        if (neededChars > 0) {
            std::vector<char> nameBuf(neededChars + 1, '\0');
            lib3mf_object_getname(meshObj, neededChars + 1, &neededChars, nameBuf.data());
            md.name = nameBuf.data();
        }

        lib3mf_meshobject_getvertexcount(meshObj, &md.vertexCount);
        lib3mf_meshobject_gettrianglecount(meshObj, &md.triangleCount);

        meshDataList.push_back(md);

        lib3mf_resourceiterator_movenext(meshIterator.as<Lib3MF_ResourceIterator>(), &hasNext);
    }

    if (meshDataList.empty()) {
        return;
    }

    // Build materials from base material groups
    Lib3MFHandle baseMaterialIterator;
    lib3mf_model_getbasematerialgroups(model.as<Lib3MF_Model>(), baseMaterialIterator.ptr());

    struct MaterialInfo {
        Lib3MF_uint32 resourceID;
        Lib3MF_uint32 propertyID;
        std::string name;
        Lib3MF::sColor color;
    };
    std::vector<MaterialInfo> materialInfos;
    std::map<std::pair<Lib3MF_uint32, Lib3MF_uint32>, unsigned int> materialMap;

    if (baseMaterialIterator.get()) {
        bool hasBMNext = false;
        lib3mf_resourceiterator_movenext(baseMaterialIterator.as<Lib3MF_ResourceIterator>(), &hasBMNext);
        while (hasBMNext) {
            Lib3MFHandle bmGroup;
            lib3mf_basematerialgroupiterator_getcurrentbasematerialgroup(
                baseMaterialIterator.as<Lib3MF_BaseMaterialGroupIterator>(), bmGroup.ptr()
            );

            Lib3MF_uint32 resourceID = 0;
            lib3mf_resource_getuniqueresourceid(bmGroup.as<Lib3MF_Resource>(), &resourceID);

            Lib3MF_uint32 matCount = 0;
            lib3mf_basematerialgroup_getcount(bmGroup.as<Lib3MF_BaseMaterialGroup>(), &matCount);

            std::vector<Lib3MF_uint32> propIDs(matCount);
            Lib3MF_uint64 propCount = 0;
            lib3mf_basematerialgroup_getallpropertyids(
                bmGroup.as<Lib3MF_BaseMaterialGroup>(),
                matCount, &propCount, propIDs.data()
            );

            for (Lib3MF_uint32 m = 0; m < matCount; ++m) {
                MaterialInfo info;
                info.resourceID = resourceID;
                info.propertyID = propIDs[m];

                Lib3MF_uint32 nameChars = 0;
                lib3mf_basematerialgroup_getname(
                    bmGroup.as<Lib3MF_BaseMaterialGroup>(), propIDs[m], 0, &nameChars, nullptr
                );
                if (nameChars > 0) {
                    std::vector<char> nameBuf(nameChars + 1, '\0');
                    lib3mf_basematerialgroup_getname(
                        bmGroup.as<Lib3MF_BaseMaterialGroup>(), propIDs[m],
                        nameChars + 1, &nameChars, nameBuf.data()
                    );
                    info.name = nameBuf.data();
                }

                lib3mf_basematerialgroup_getdisplaycolor(
                    bmGroup.as<Lib3MF_BaseMaterialGroup>(), propIDs[m], &info.color
                );

                unsigned int matIdx = static_cast<unsigned int>(materialInfos.size());
                materialMap[{resourceID, propIDs[m]}] = matIdx;
                materialInfos.push_back(info);
            }

            lib3mf_resourceiterator_movenext(baseMaterialIterator.as<Lib3MF_ResourceIterator>(), &hasBMNext);
        }
    }

    if (materialInfos.empty()) {
        MaterialInfo defaultMat;
        defaultMat.resourceID = 0;
        defaultMat.propertyID = 0;
        defaultMat.name = "DefaultMaterial";
        defaultMat.color = {200, 200, 200, 255};
        materialInfos.push_back(defaultMat);
    }

    pScene->mNumMaterials = static_cast<unsigned int>(materialInfos.size());
    pScene->mMaterials = new aiMaterial *[pScene->mNumMaterials];
    for (unsigned int i = 0; i < pScene->mNumMaterials; ++i) {
        aiMaterial *mat = new aiMaterial();
        aiString name(materialInfos[i].name);
        mat->AddProperty(&name, AI_MATKEY_NAME);

        aiColor4D diffuse = lib3MFColorToAi(materialInfos[i].color);
        mat->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);

        pScene->mMaterials[i] = mat;
    }

    // Second pass: build aiMeshes
    Lib3MFHandle meshIterator2;
    lib3mf_model_getmeshobjects(model.as<Lib3MF_Model>(), meshIterator2.ptr());

    pScene->mNumMeshes = static_cast<unsigned int>(meshDataList.size());
    pScene->mMeshes = new aiMesh *[pScene->mNumMeshes];

    unsigned int meshIndex = 0;
    bool hasNext2 = false;
    lib3mf_resourceiterator_movenext(meshIterator2.as<Lib3MF_ResourceIterator>(), &hasNext2);
    while (hasNext2 && meshIndex < pScene->mNumMeshes) {
        Lib3MFHandle currentMesh;
        lib3mf_meshobjectiterator_getcurrentmeshobject(
            meshIterator2.as<Lib3MF_MeshObjectIterator>(), currentMesh.ptr()
        );

        Lib3MF_MeshObject meshObj = currentMesh.as<Lib3MF_MeshObject>();
        Lib3MF_uint32 vertexCount = 0, triangleCount = 0;
        lib3mf_meshobject_getvertexcount(meshObj, &vertexCount);
        lib3mf_meshobject_gettrianglecount(meshObj, &triangleCount);

        aiMesh *aiMeshPtr = new aiMesh();
        aiMeshPtr->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;

        Lib3MF_uint32 nameChars = 0;
        lib3mf_object_getname(meshObj, 0, &nameChars, nullptr);
        if (nameChars > 0) {
            std::vector<char> nameBuf(nameChars + 1, '\0');
            lib3mf_object_getname(meshObj, nameChars + 1, &nameChars, nameBuf.data());
            aiMeshPtr->mName = aiString(nameBuf.data());
        }

        aiMeshPtr->mNumVertices = vertexCount;
        aiMeshPtr->mVertices = new aiVector3D[vertexCount];
        for (Lib3MF_uint32 v = 0; v < vertexCount; ++v) {
            Lib3MF::sPosition pos;
            lib3mf_meshobject_getvertex(meshObj, v, &pos);
            aiMeshPtr->mVertices[v].x = pos.m_Coordinates[0];
            aiMeshPtr->mVertices[v].y = pos.m_Coordinates[1];
            aiMeshPtr->mVertices[v].z = pos.m_Coordinates[2];
        }

        aiMeshPtr->mNumFaces = triangleCount;
        aiMeshPtr->mFaces = new aiFace[triangleCount];
        for (Lib3MF_uint32 f = 0; f < triangleCount; ++f) {
            Lib3MF::sTriangle tri;
            lib3mf_meshobject_gettriangle(meshObj, f, &tri);
            aiMeshPtr->mFaces[f].mNumIndices = 3;
            aiMeshPtr->mFaces[f].mIndices = new unsigned int[3];
            aiMeshPtr->mFaces[f].mIndices[0] = tri.m_Indices[0];
            aiMeshPtr->mFaces[f].mIndices[1] = tri.m_Indices[1];
            aiMeshPtr->mFaces[f].mIndices[2] = tri.m_Indices[2];
        }

        unsigned int assignedMaterial = 0;
        if (triangleCount > 0) {
            Lib3MF::sTriangleProperties triProp;
            Lib3MFResult propResult = lib3mf_meshobject_gettriangleproperties(meshObj, 0, &triProp);
            if (propResult == LIB3MF_SUCCESS && triProp.m_ResourceID != 0) {
                auto it = materialMap.find({triProp.m_ResourceID, triProp.m_PropertyIDs[0]});
                if (it != materialMap.end()) {
                    assignedMaterial = it->second;
                }
            }
        }
        aiMeshPtr->mMaterialIndex = assignedMaterial;

        pScene->mMeshes[meshIndex] = aiMeshPtr;
        ++meshIndex;

        lib3mf_resourceiterator_movenext(meshIterator2.as<Lib3MF_ResourceIterator>(), &hasNext2);
    }

    // Collect build item transforms
    Lib3MFHandle buildItemIterator;
    lib3mf_model_getbuilditems(model.as<Lib3MF_Model>(), buildItemIterator.ptr());

    struct BuildItemInfo {
        bool hasTransform;
        Lib3MF::sTransform transform;
    };
    std::vector<BuildItemInfo> buildItems;

    if (buildItemIterator.get()) {
        bool hasBINext = false;
        lib3mf_builditemiterator_movenext(
            buildItemIterator.as<Lib3MF_BuildItemIterator>(), &hasBINext
        );
        while (hasBINext) {
            Lib3MFHandle buildItem;
            lib3mf_builditemiterator_getcurrent(
                buildItemIterator.as<Lib3MF_BuildItemIterator>(), buildItem.ptr()
            );

            BuildItemInfo info;
            info.hasTransform = false;
            lib3mf_builditem_hasobjecttransform(
                buildItem.as<Lib3MF_BuildItem>(), &info.hasTransform
            );
            if (info.hasTransform) {
                lib3mf_builditem_getobjecttransform(
                    buildItem.as<Lib3MF_BuildItem>(), &info.transform
                );
            }
            buildItems.push_back(info);

            lib3mf_builditemiterator_movenext(
                buildItemIterator.as<Lib3MF_BuildItemIterator>(), &hasBINext
            );
        }
    }

    pScene->mRootNode = new aiNode();
    pScene->mRootNode->mName = aiString("3MF_Root");

    if (pScene->mNumMeshes > 0) {
        pScene->mRootNode->mNumChildren = pScene->mNumMeshes;
        pScene->mRootNode->mChildren = new aiNode *[pScene->mNumMeshes];
        std::memset(pScene->mRootNode->mChildren, 0, sizeof(aiNode*) * pScene->mNumMeshes);

        for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
            aiNode *child = new aiNode();
            child->mName = aiString("Mesh_" + std::to_string(i));
            child->mParent = pScene->mRootNode;
            child->mNumMeshes = 1;
            child->mMeshes = new unsigned int[1];
            child->mMeshes[0] = i;

            if (i < buildItems.size() && buildItems[i].hasTransform) {
                child->mTransformation = lib3MFTransformToAiMatrix(buildItems[i].transform);
            }

            pScene->mRootNode->mChildren[i] = child;
        }
    }

    // ---- CONTRACT METADATA (R10') ----
    // Read the actual file unit from lib3mf and project it into the cross-importer
    // contract so downstream exporters can rescale spec-correctly. 3MF's coordinate
    // system is normatively +Z up (3MF Core Spec §3.3) — set unconditionally.
    // The shared `writeContractMetadata` helper handles allocation, key collision,
    // and SOURCE_FORMAT registration uniformly across every adopting importer.
    Lib3MF::eModelUnit modelUnit = Lib3MF::eModelUnit::MilliMeter;
    lib3mf_model_getunit(model.as<Lib3MF_Model>(), &modelUnit);
    const double unitScaleToMeters = modelUnitToMeters(modelUnit);
    writeContractMetadata(pScene, unitScaleToMeters, static_cast<int32_t>(2), "3MF");
}

} // anonymous namespace

// ===== PUBLIC API =====

void Lib3MFBridge::ExportScene(const aiScene *pScene, const std::string &pFile, IOSystem *pIOSystem,
                               const ExportProperties *pProperties) {
    if (!pScene) {
        throw DeadlyExportError("lib3mf export: null scene");
    }

    std::vector<Lib3MF_uint8> buffer;
    exportToLib3MF(pScene, buffer, pProperties);

    std::unique_ptr<IOStream> outfile(pIOSystem->Open(pFile, "wb"));
    if (!outfile) {
        throw DeadlyExportError("lib3mf export: could not open output file: " + pFile);
    }
    outfile->Write(buffer.data(), 1, buffer.size());
}

void Lib3MFBridge::ImportScene(aiScene *pScene, const std::string &pFile, IOSystem *pIOSystem) {
    if (!pScene) {
        throw DeadlyImportError("lib3mf import: null scene");
    }

    std::unique_ptr<IOStream> infile(pIOSystem->Open(pFile, "rb"));
    if (!infile) {
        throw DeadlyImportError("lib3mf import: could not open file: " + pFile);
    }

    size_t fileSize = infile->FileSize();
    std::vector<Lib3MF_uint8> buffer(fileSize);
    infile->Read(buffer.data(), 1, fileSize);

    importFromLib3MF(pScene, buffer);
}

} // namespace D3MF
} // namespace Assimp

#endif // ASSIMP_USE_LIB3MF
