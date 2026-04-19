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

// Tier 3B — generated cross-format round-trip tests for the
// `AI_METADATA_UNIT_SCALE_TO_METERS` / `AI_METADATA_UP_AXIS` contract.
//
// For every (source-frame, exporter) cell in the matrix we author a
// synthetic axis-aligned tall box with explicit contract metadata in the
// source frame, push it through the live exporter, then re-import the
// produced file and compare the post-bake axis-aligned bounding-box
// extents against the analytical expectation derived purely from the
// source vs. target frame.
//
// "Source frame" stands in for an importer landing point: every
// importer in Tiers 1A/1B writes the contract before yielding a scene,
// so testing against a synthetic scene with the same contract metadata
// is equivalent to testing against the importer's output (the importer's
// own utFmt tests already validate parsing). This deliberately avoids
// requiring fixture files for every (importer, exporter) cell — the
// contract itself is the unit of behaviour we want to lock down.
//
// Identity short-circuit: when the source frame matches the exporter
// target the bake helper must perform no transform; the on-disk extents
// must equal the authored extents to within float tolerance.

#include "UnitTestPCH.h"

#include <assimp/commonMetaData.h>
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#define rt_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define rt_mkdir(path) mkdir((path), 0755)
#endif

using namespace Assimp;

namespace {

struct Frame {
    const char *label;
    double unitToMeters;
    int32_t upAxis; // 0=X, 1=Y, 2=Z
};

struct ExporterTarget {
    const char *exporterId; // string passed to Exporter::Export
    const char *outputExt; // file extension (no leading dot)
    Frame target; // canonical frame the exporter emits in
    // Re-import strategy: if true, use Assimp::Importer to round-trip
    // and read the resulting mesh extents directly. If false, scan the
    // on-disk text payload via the supplied scanner. Some text exporters
    // (X3D, USD) round-trip too lossily through their importers to be a
    // reliable axis check, so we anchor on the on-disk vertex tables.
    bool roundTrip;
    // Token-scan helper (only used when roundTrip == false). Returns the
    // axis-aligned extent computed from the on-disk vertex table.
    aiVector3D (*scanExtent)(const std::string &path);
};

// ---------------------------------------------------------------------------
// Scene authoring
// ---------------------------------------------------------------------------

aiScene *makeContractBoxScene(const aiVector3D &halfExtents, const Frame &source) {
    const float x = halfExtents.x, y = halfExtents.y, z = halfExtents.z;
    const std::array<aiVector3D, 8> v = {
        aiVector3D(-x, -y, -z), aiVector3D(+x, -y, -z), aiVector3D(+x, +y, -z), aiVector3D(-x, +y, -z),
        aiVector3D(-x, -y, +z), aiVector3D(+x, -y, +z), aiVector3D(+x, +y, +z), aiVector3D(-x, +y, +z)
    };
    const std::array<std::array<unsigned int, 3>, 12> t = { {
        { 0, 2, 1 }, { 0, 3, 2 },
        { 4, 5, 6 }, { 4, 6, 7 },
        { 0, 1, 5 }, { 0, 5, 4 },
        { 3, 7, 6 }, { 3, 6, 2 },
        { 0, 4, 7 }, { 0, 7, 3 },
        { 1, 2, 6 }, { 1, 6, 5 }
    } };

    auto *scene = new aiScene();
    scene->mNumMaterials = 1;
    scene->mMaterials = new aiMaterial *[1];
    scene->mMaterials[0] = new aiMaterial();

    auto *mesh = new aiMesh();
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh->mMaterialIndex = 0;
    mesh->mName = aiString("RoundTripBox");
    mesh->mNumVertices = static_cast<unsigned int>(v.size());
    mesh->mVertices = new aiVector3D[v.size()];
    mesh->mNormals = new aiVector3D[v.size()];
    for (size_t i = 0; i < v.size(); ++i) {
        mesh->mVertices[i] = v[i];
        // Radial outward-pointing normals — sufficient to exercise the
        // bake helper's normal rotation path.
        aiVector3D n = v[i];
        n.Normalize();
        mesh->mNormals[i] = n;
    }
    mesh->mNumFaces = static_cast<unsigned int>(t.size());
    mesh->mFaces = new aiFace[t.size()];
    for (size_t i = 0; i < t.size(); ++i) {
        mesh->mFaces[i].mNumIndices = 3;
        mesh->mFaces[i].mIndices = new unsigned int[3];
        mesh->mFaces[i].mIndices[0] = t[i][0];
        mesh->mFaces[i].mIndices[1] = t[i][1];
        mesh->mFaces[i].mIndices[2] = t[i][2];
    }
    scene->mNumMeshes = 1;
    scene->mMeshes = new aiMesh *[1];
    scene->mMeshes[0] = mesh;

    scene->mRootNode = new aiNode();
    scene->mRootNode->mName = aiString("Root");
    scene->mRootNode->mNumMeshes = 1;
    scene->mRootNode->mMeshes = new unsigned int[1];
    scene->mRootNode->mMeshes[0] = 0;

    scene->mMetaData = new aiMetadata();
    scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, source.unitToMeters);
    scene->mMetaData->Add(AI_METADATA_UP_AXIS, source.upAxis);
    return scene;
}

// Compute the analytical post-bake axis-aligned extent given the source
// authored half-extents and the source/target frames. The bake helper
// applies (a) a uniform scale of `source.unitToMeters / target.unitToMeters`
// and (b) an axis swap that maps `source.upAxis -> target.upAxis`. The
// remaining two axes follow from the right-handed swap convention used by
// `buildAxisRotationMatrix`.
aiVector3D analyticalExtent(const aiVector3D &halfExtents, const Frame &source, const Frame &target) {
    const float scale = static_cast<float>(source.unitToMeters / target.unitToMeters);
    const float ax = halfExtents.x * 2.0f * scale;
    const float ay = halfExtents.y * 2.0f * scale;
    const float az = halfExtents.z * 2.0f * scale;
    if (source.upAxis == target.upAxis) {
        return aiVector3D(ax, ay, az);
    }
    // Y <-> Z swap: matches buildAxisRotationMatrix which preserves +X
    // and rotates the other two axes around it.
    if ((source.upAxis == 1 && target.upAxis == 2) ||
        (source.upAxis == 2 && target.upAxis == 1)) {
        return aiVector3D(ax, az, ay);
    }
    // X <-> Y swap.
    if ((source.upAxis == 0 && target.upAxis == 1) ||
        (source.upAxis == 1 && target.upAxis == 0)) {
        return aiVector3D(ay, ax, az);
    }
    // X <-> Z swap.
    if ((source.upAxis == 0 && target.upAxis == 2) ||
        (source.upAxis == 2 && target.upAxis == 0)) {
        return aiVector3D(az, ay, ax);
    }
    // Unreachable for the {0, 1, 2} domain, but return the unrotated
    // extent rather than zero so a regression in this helper can be
    // distinguished from a bake regression.
    return aiVector3D(ax, ay, az);
}

aiVector3D extentFromScene(const aiScene *scene) {
    aiVector3D mn(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max());
    aiVector3D mx(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                  std::numeric_limits<float>::lowest());
    bool any = false;
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh *mesh = scene->mMeshes[m];
        if (mesh == nullptr) continue;
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            const aiVector3D &v = mesh->mVertices[i];
            mn.x = std::min(mn.x, v.x);
            mn.y = std::min(mn.y, v.y);
            mn.z = std::min(mn.z, v.z);
            mx.x = std::max(mx.x, v.x);
            mx.y = std::max(mx.y, v.y);
            mx.z = std::max(mx.z, v.z);
            any = true;
        }
    }
    if (!any) return aiVector3D(0, 0, 0);
    return aiVector3D(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
}

// ---------------------------------------------------------------------------
// Token scanners — used for exporters whose importers are too lossy to
// reliably round-trip an axis assertion. The scanners aggregate every
// vertex-table chunk in the on-disk file into a single bounding extent.
// ---------------------------------------------------------------------------

aiVector3D scanX3DPointAttr(const std::string &path) {
    std::ifstream in(path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    aiVector3D mn(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max());
    aiVector3D mx(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                  std::numeric_limits<float>::lowest());
    bool found = false;
    const std::string anchor = "point='";
    size_t cursor = 0;
    while (true) {
        size_t pos = content.find(anchor, cursor);
        if (pos == std::string::npos) break;
        pos += anchor.size();
        size_t end = content.find('\'', pos);
        if (end == std::string::npos) break;
        std::istringstream iss(content.substr(pos, end - pos));
        float x, y, z;
        while (iss >> x >> y >> z) {
            mn.x = std::min(mn.x, x);
            mn.y = std::min(mn.y, y);
            mn.z = std::min(mn.z, z);
            mx.x = std::max(mx.x, x);
            mx.y = std::max(mx.y, y);
            mx.z = std::max(mx.z, z);
            found = true;
        }
        cursor = end + 1;
    }
    if (!found) return aiVector3D(0, 0, 0);
    return aiVector3D(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
}

aiVector3D scanUsdaPointArray(const std::string &path) {
    std::ifstream in(path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    aiVector3D mn(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max());
    aiVector3D mx(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                  std::numeric_limits<float>::lowest());
    bool found = false;
    const std::string blockAnchor = "points = [";
    size_t blockCursor = 0;
    while (true) {
        size_t blockPos = content.find(blockAnchor, blockCursor);
        if (blockPos == std::string::npos) break;
        size_t blockEnd = content.find(']', blockPos);
        if (blockEnd == std::string::npos) break;
        std::string block = content.substr(blockPos, blockEnd - blockPos);
        size_t triCursor = 0;
        while (true) {
            size_t open = block.find('(', triCursor);
            if (open == std::string::npos) break;
            size_t close = block.find(')', open);
            if (close == std::string::npos) break;
            std::string triplet = block.substr(open + 1, close - open - 1);
            std::replace(triplet.begin(), triplet.end(), ',', ' ');
            std::istringstream iss(triplet);
            float x, y, z;
            if (iss >> x >> y >> z) {
                mn.x = std::min(mn.x, x);
                mn.y = std::min(mn.y, y);
                mn.z = std::min(mn.z, z);
                mx.x = std::max(mx.x, x);
                mx.y = std::max(mx.y, y);
                mx.z = std::max(mx.z, z);
                found = true;
            }
            triCursor = close + 1;
        }
        blockCursor = blockEnd + 1;
    }
    if (!found) return aiVector3D(0, 0, 0);
    return aiVector3D(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
}

bool ensureContractDir() {
    rt_mkdir("rt");
    rt_mkdir("rt/contract");
    return true;
}

// ---------------------------------------------------------------------------
// Frame and exporter tables
// ---------------------------------------------------------------------------

const std::vector<Frame> &sourceFrames() {
    // The source frames cover every importer landing point in Tiers 1A/1B
    // collapsed into the unique (unitToMeters, upAxis) tuples they emit.
    // Every importer writes one of these tuples (millimetres + Z-up for
    // STL/3MF; metres + Y-up for OBJ/glTF/X3D/X/COLLADA/PLY/USD; metres +
    // Z-up for 3DS/IFC/OFF; centimetres + Y-up for FBX). Adding overrides
    // here would just re-test the bake helper, which utUnitAxisContract
    // already covers exhaustively.
    static const std::vector<Frame> frames = {
        { "mm_zup", 1e-3, 2 }, // STL / 3MF native frame
        { "m_yup", 1.0, 1 }, // OBJ / glTF / X3D / X / COLLADA / PLY / USD native
        { "m_zup", 1.0, 2 }, // 3DS / IFC / OFF native
        { "cm_yup", 1e-2, 1 }, // FBX native
    };
    return frames;
}

const std::vector<ExporterTarget> &exporterTargets() {
    // Re-import strategy is chosen per exporter:
    //   - Round-tripped through Importer when the format's importer is a
    //     faithful inverse for vertex extents (mesh-only scenes survive
    //     OBJ/PLY/STL/3MF/glTF round-trip without geometric distortion).
    //   - Token-scanned when the importer side is lossy on extent
    //     (X3D, USD), or when round-tripping through the importer would
    //     require fixture-side dependencies the test cannot provide.
    static const std::vector<ExporterTarget> targets = {
        // glTF2 — m + Y-up canonical frame (cf. glTF 2.0 spec §3.4).
        { "gltf2", "gltf", { "m_yup", 1.0, 1 }, true, nullptr },
        { "glb2", "glb", { "m_yup", 1.0, 1 }, true, nullptr },
        // OBJ — m + Y-up canonical (matches OBJExporter target).
        { "obj", "obj", { "m_yup", 1.0, 1 }, true, nullptr },
        { "objnomtl", "obj", { "m_yup", 1.0, 1 }, true, nullptr },
        // PLY — m + Y-up canonical (matches PLYExporter target).
        { "ply", "ply", { "m_yup", 1.0, 1 }, true, nullptr },
        { "plyb", "ply", { "m_yup", 1.0, 1 }, true, nullptr },
        // STL — mm + Z-up canonical (matches STLExporter target).
        { "stl", "stl", { "mm_zup", 1e-3, 2 }, true, nullptr },
        { "stlb", "stl", { "mm_zup", 1e-3, 2 }, true, nullptr },
        // 3MF is intentionally excluded from this cross-format matrix:
        //  (a) the format is a ZIP archive of compressed XML, so a token
        //      scanner cannot read vertex extents off disk without pulling
        //      in a zip dependency that the test layer does not link, and
        //  (b) the 3MF importer has a pre-existing round-trip regression
        //      (re-imported 3MF files can yield empty meshes) that is
        //      tracked outside this contract work.
        // Direct 3MF contract behaviour is already covered by
        // utD3MFImportExport (`export3MFReadsUnitScaleToMetersMetadata*`,
        // `roundtrip3MFBox*`). Re-adding 3MF here once the importer side
        // round-trips cleanly is a one-line table edit.
        // X3D — m + Y-up canonical; importer is too lossy for axis check.
        { "x3d", "x3d", { "m_yup", 1.0, 1 }, false, &scanX3DPointAttr },
        // USDA — m + Y-up canonical; importer round-trip is lossy and
        // some configurations crash inside aiProcess_ValidateDataStructure
        // on USD's own re-imports. Token scan is the reliable assertion.
        { "usda", "usda", { "m_yup", 1.0, 1 }, false, &scanUsdaPointArray },
    };
    return targets;
}

struct MatrixCase {
    Frame source;
    ExporterTarget target;
};

class ContractRoundTrip : public ::testing::TestWithParam<MatrixCase> {
protected:
    void SetUp() override {
        ensureContractDir();
    }
};

aiVector3D readBackExtent(const ExporterTarget &target, const std::string &path) {
    if (!target.roundTrip) {
        return target.scanExtent(path);
    }
    Assimp::Importer reimporter;
    const aiScene *scene = reimporter.ReadFile(path, aiProcess_Triangulate);
    if (scene == nullptr) {
        return aiVector3D(0, 0, 0);
    }
    return extentFromScene(scene);
}

} // namespace

TEST_P(ContractRoundTrip, BakeMatchesAnalyticalExtent) {
    const MatrixCase &gp = GetParam();
    const aiVector3D halfExtents(1.0f, 1.0f, 5.0f); // tall on +Z in source frame

    aiScene *scene = makeContractBoxScene(halfExtents, gp.source);
    const aiVector3D expected = analyticalExtent(halfExtents, gp.source, gp.target.target);

    const std::string filename = std::string("rt/contract/") + gp.source.label + "_to_" +
                                 gp.target.exporterId + "." + gp.target.outputExt;

    Assimp::Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene, gp.target.exporterId, filename))
            << "Export failed for " << gp.source.label << " -> " << gp.target.exporterId;
    delete scene;

    aiVector3D actual = readBackExtent(gp.target, filename);
    // Tolerance: 1e-3 covers FBX's centimetre canonicalisation rounding
    // (largest dimension can land near 1000 cm) and X3D's textual round
    // trip through fast_atof — same band the per-format exporter tests
    // use for axis-bake assertions.
    EXPECT_NEAR(expected.x, actual.x, 1e-3f) << "x axis bake mismatch";
    EXPECT_NEAR(expected.y, actual.y, 1e-3f) << "y axis bake mismatch";
    EXPECT_NEAR(expected.z, actual.z, 1e-3f) << "z axis bake mismatch";

    std::remove(filename.c_str());
}

namespace {

std::vector<MatrixCase> buildMatrix() {
    std::vector<MatrixCase> out;
    for (const auto &source : sourceFrames()) {
        for (const auto &target : exporterTargets()) {
            out.push_back({ source, target });
        }
    }
    return out;
}

std::string caseName(const ::testing::TestParamInfo<MatrixCase> &info) {
    return std::string(info.param.source.label) + "_to_" + info.param.target.exporterId;
}

} // namespace

INSTANTIATE_TEST_SUITE_P(AllFrames, ContractRoundTrip,
                          ::testing::ValuesIn(buildMatrix()),
                          caseName);
