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

// Tests of the USD exporters (usda, usdc, usdz). Outputs are produced in
// memory with ExportToBlob and inspected as text (USDA) or as bytes (USDC
// magic, USDZ zip layout); OpenUSD-level validation of the same exporters is
// done by scripts/usd/validate-exports.sh.

#include "UnitTestPCH.h"

#include <assimp/DefaultLogger.hpp>
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/LogStream.hpp>
#include <assimp/commonMetaData.h>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace Assimp;

namespace {

struct Blob {
    std::string name;
    std::vector<uint8_t> data;
    std::string Text() const { return std::string(data.begin(), data.end()); }
};

// Exports to memory; empty result on failure.
std::vector<Blob> ExportBlobs(const aiScene *scene, const char *format, const ExportProperties *props = nullptr) {
    std::vector<Blob> out;
    Exporter exporter;
    const aiExportDataBlob *blob = exporter.ExportToBlob(scene, format, 0u, props);
    for (; blob != nullptr; blob = blob->next) {
        const uint8_t *p = static_cast<const uint8_t *>(blob->data);
        out.push_back({ blob->name.C_Str(), std::vector<uint8_t>(p, p + blob->size) });
    }
    return out;
}

std::string ExportUsda(const aiScene *scene, const ExportProperties *props = nullptr) {
    const std::vector<Blob> blobs = ExportBlobs(scene, "usda", props);
    return blobs.empty() ? std::string() : blobs[0].Text();
}

bool StartsWith(const std::vector<uint8_t> &data, const char *magic, size_t offset = 0) {
    const size_t n = std::strlen(magic);
    return data.size() >= offset + n && std::memcmp(data.data() + offset, magic, n) == 0;
}

bool IsPng(const std::vector<uint8_t> &d, size_t offset = 0) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    return d.size() >= offset + 8 && std::memcmp(d.data() + offset, sig, 8) == 0;
}

uint32_t Be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint32_t Le32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint16_t Le16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// One zip member, read from its local file header.
struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    size_t dataOffset = 0;
    size_t size = 0;
};

// Walks the local headers of a zip archive; false on a malformed archive.
bool ParseZip(const std::vector<uint8_t> &z, std::vector<ZipEntry> &entries) {
    size_t off = 0;
    while (off + 4 <= z.size() && Le32(&z[off]) == 0x04034b50u) {
        if (off + 30 > z.size()) {
            return false;
        }
        ZipEntry e;
        const uint16_t flags = Le16(&z[off + 6]);
        e.method = Le16(&z[off + 8]);
        const uint32_t csize = Le32(&z[off + 18]);
        e.size = Le32(&z[off + 22]);
        const uint16_t nlen = Le16(&z[off + 26]);
        const uint16_t xlen = Le16(&z[off + 28]);
        if ((flags & 0x08) != 0 || off + 30 + nlen + xlen + csize > z.size()) {
            return false; // data descriptors are not allowed in USDZ
        }
        e.name.assign(reinterpret_cast<const char *>(&z[off + 30]), nlen);
        e.dataOffset = off + 30 + nlen + xlen;
        entries.push_back(e);
        off = e.dataOffset + csize;
    }
    return !entries.empty() && off + 4 <= z.size() && Le32(&z[off]) == 0x02014b50u;
}

std::vector<uint8_t> Member(const std::vector<uint8_t> &z, const ZipEntry &e) {
    return std::vector<uint8_t>(z.begin() + e.dataOffset, z.begin() + e.dataOffset + e.size);
}

size_t Count(const std::string &text, const std::string &needle) {
    size_t n = 0;
    for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + needle.size())) {
        ++n;
    }
    return n;
}

// Top-level prims of a USDA layer: lines starting with "def " in column 0.
std::vector<std::string> RootPrims(const std::string &usda) {
    std::vector<std::string> names;
    static const std::regex re(R"(^def \w+ \"([^\"]+)\")");
    std::istringstream in(usda);
    std::string line;
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            names.push_back(m[1]);
        }
    }
    return names;
}

std::string DefaultPrim(const std::string &usda) {
    static const std::regex re(R"(defaultPrim = \"([^\"]+)\")");
    std::smatch m;
    return std::regex_search(usda, m, re) ? std::string(m[1]) : std::string();
}

// Captures warnings of the default logger for the lifetime of the object.
class LogCapture : public LogStream {
public:
    std::string text;
    void write(const char *message) override { text += message; }
};

class ScopedLog {
public:
    ScopedLog() {
        DefaultLogger::create("", Logger::NORMAL, 0);
        mStream = new LogCapture;
        DefaultLogger::get()->attachStream(mStream, Logger::Warn | Logger::Err | Logger::Info);
    }
    ~ScopedLog() { DefaultLogger::kill(); }
    const std::string &Text() const { return mStream->text; }

private:
    LogCapture *mStream = nullptr;
};

aiMesh *MakeTriangle(const char *name) {
    aiMesh *mesh = new aiMesh;
    mesh->mName.Set(name);
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh->mNumVertices = 3;
    mesh->mVertices = new aiVector3D[3]{ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
    mesh->mNormals = new aiVector3D[3]{ { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } };
    mesh->mTextureCoords[0] = new aiVector3D[3]{ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0.25f, 0 } };
    mesh->mNumUVComponents[0] = 2;
    mesh->mNumFaces = 1;
    mesh->mFaces = new aiFace[1];
    mesh->mFaces[0].mNumIndices = 3;
    mesh->mFaces[0].mIndices = new unsigned int[3]{ 0, 1, 2 };
    mesh->mMaterialIndex = 0;
    return mesh;
}

aiNode *AddChild(aiNode *parent, const char *name) {
    aiNode *child = new aiNode(name);
    parent->addChildren(1, &child);
    return child;
}

void AttachMesh(aiNode *node, unsigned mesh) {
    delete[] node->mMeshes;
    node->mNumMeshes = 1;
    node->mMeshes = new unsigned int[1]{ mesh };
}

// Root "Scene" with one child "Body" holding a textured-ready triangle.
std::unique_ptr<aiScene> MakeScene() {
    std::unique_ptr<aiScene> scene(new aiScene);
    scene->mRootNode = new aiNode("Scene");
    scene->mNumMaterials = 1;
    scene->mMaterials = new aiMaterial *[1] { new aiMaterial };
    aiString matName("Mat");
    scene->mMaterials[0]->AddProperty(&matName, AI_MATKEY_NAME);
    scene->mNumMeshes = 1;
    scene->mMeshes = new aiMesh *[1] { MakeTriangle("Tri") };
    AttachMesh(AddChild(scene->mRootNode, "Body"), 0);
    return scene;
}

std::unique_ptr<aiScene> ImportFile(Importer &importer, const char *path) {
    const aiScene *s = importer.ReadFile(path, aiProcess_ValidateDataStructure);
    if (s == nullptr) {
        return nullptr;
    }
    return std::unique_ptr<aiScene>(importer.GetOrphanedScene());
}

} // namespace

class utUSDZExport : public ::testing::Test {};

// ------------------------------------------------------------------------------------------------
// Registration and failure handling

TEST_F(utUSDZExport, formatsAreRegistered) {
    Exporter exporter;
    bool usda = false, usdc = false, usdz = false;
    for (size_t i = 0; i < exporter.GetExportFormatCount(); ++i) {
        const std::string id = exporter.GetExportFormatDescription(i)->id;
        usda = usda || id == "usda";
        usdc = usdc || id == "usdc";
        usdz = usdz || id == "usdz";
    }
    EXPECT_TRUE(usda);
    EXPECT_TRUE(usdc);
    EXPECT_TRUE(usdz);
}

TEST_F(utUSDZExport, nullSceneFails) {
    Exporter exporter;
    EXPECT_EQ(nullptr, exporter.ExportToBlob(nullptr, "usdz"));
}

TEST_F(utUSDZExport, unknownFormatFails) {
    auto scene = MakeScene();
    Exporter exporter;
    EXPECT_EQ(nullptr, exporter.ExportToBlob(scene.get(), "usdx"));
}

// ------------------------------------------------------------------------------------------------
// Stage structure

TEST_F(utUSDZExport, singleRootIsDefaultPrimComponent) {
    auto scene = MakeScene();
    // a second top-level child: the stage must still have one root prim
    AttachMesh(AddChild(scene->mRootNode, "Other"), 0);
    const std::string usda = ExportUsda(scene.get());
    ASSERT_FALSE(usda.empty());
    const std::vector<std::string> roots = RootPrims(usda);
    ASSERT_EQ(1u, roots.size());
    EXPECT_EQ(roots[0], DefaultPrim(usda));
    EXPECT_EQ("Scene", roots[0]);
    EXPECT_NE(std::string::npos, usda.find("kind = \"component\""));
    EXPECT_NE(std::string::npos, usda.find("upAxis = \"Y\""));
    EXPECT_NE(std::string::npos, usda.find("metersPerUnit = 1"));
    // a static scene authors no time codes
    EXPECT_EQ(std::string::npos, usda.find("timeCodesPerSecond"));
}

TEST_F(utUSDZExport, sharedMeshIsWrittenPerUser) {
    auto scene = MakeScene();
    AttachMesh(AddChild(scene->mRootNode, "Copy"), 0);
    const std::string usda = ExportUsda(scene.get());
    EXPECT_EQ(2u, Count(usda, "def Mesh \"Tri\""));
    EXPECT_EQ(2u, Count(usda, "point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]"));
}

TEST_F(utUSDZExport, namesAreSanitisedAndUnique) {
    auto scene = MakeScene();
    AddChild(scene->mRootNode, "a b");
    AddChild(scene->mRootNode, "a-b");
    AddChild(scene->mRootNode, "1st");
    const std::string usda = ExportUsda(scene.get());
    EXPECT_NE(std::string::npos, usda.find("\"a_b\""));
    EXPECT_NE(std::string::npos, usda.find("\"a_b_1\""));
    EXPECT_NE(std::string::npos, usda.find("\"_1st\""));
}

TEST_F(utUSDZExport, unitsComeFromMetadata) {
    auto scene = MakeScene();
    scene->mMetaData = aiMetadata::Alloc(1);
    scene->mMetaData->Set(0, AI_METADATA_UNIT_SCALE_TO_METERS, 0.01);
    const std::string usda = ExportUsda(scene.get());
    EXPECT_NE(std::string::npos, usda.find("metersPerUnit = 0.01"));
    // geometry is not scaled
    EXPECT_NE(std::string::npos, usda.find("points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]"));
}

TEST_F(utUSDZExport, zUpBecomesRootRotation) {
    auto scene = MakeScene();
    scene->mMetaData = aiMetadata::Alloc(1);
    scene->mMetaData->Set(0, AI_METADATA_UP_AXIS, int32_t(2));
    const std::string usda = ExportUsda(scene.get());
    EXPECT_NE(std::string::npos, usda.find("upAxis = \"Y\""));
    // Z-up -> Y-up on the root prim: (x, y, z) -> (x, z, -y)
    EXPECT_NE(std::string::npos, usda.find("( (1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1) )"));
    EXPECT_NE(std::string::npos, usda.find("points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]"));
}

TEST_F(utUSDZExport, uvsAreNotFlipped) {
    auto scene = MakeScene();
    const std::string usda = ExportUsda(scene.get());
    EXPECT_NE(std::string::npos, usda.find("texCoord2f[] primvars:st = [(0, 0), (1, 0), (0, 0.25)]"));
}

TEST_F(utUSDZExport, pointCloudBecomesPoints) {
    auto scene = MakeScene();
    aiMesh *mesh = scene->mMeshes[0];
    mesh->mPrimitiveTypes = aiPrimitiveType_POINT;
    delete[] mesh->mFaces;
    mesh->mNumFaces = 3;
    mesh->mFaces = new aiFace[3];
    for (unsigned i = 0; i < 3; ++i) {
        mesh->mFaces[i].mNumIndices = 1;
        mesh->mFaces[i].mIndices = new unsigned int[1]{ i };
    }
    const std::string usda = ExportUsda(scene.get());
    EXPECT_NE(std::string::npos, usda.find("def Points \"Tri\""));
    EXPECT_EQ(std::string::npos, usda.find("def Mesh"));
    EXPECT_NE(std::string::npos, usda.find("float[] widths = ["));
}

// ------------------------------------------------------------------------------------------------
// Materials and textures

TEST_F(utUSDZExport, boxTexturedUsdaMaterialNetwork) {
    Importer importer;
    auto scene = ImportFile(importer, ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-Binary/BoxTextured.glb");
    ASSERT_NE(nullptr, scene);
    const std::vector<Blob> blobs = ExportBlobs(scene.get(), "usda");
    ASSERT_EQ(2u, blobs.size()); // layer + sidecar texture
    const std::string usda = blobs[0].Text();
    EXPECT_EQ(0u, usda.rfind("#usda 1.0", 0));
    ASSERT_EQ(1u, RootPrims(usda).size());
    EXPECT_EQ(RootPrims(usda)[0], DefaultPrim(usda));
    EXPECT_NE(std::string::npos, usda.find("def Scope \"Materials\""));
    EXPECT_NE(std::string::npos, usda.find("prepend apiSchemas = [\"MaterialBindingAPI\"]"));
    EXPECT_NE(std::string::npos, usda.find("rel material:binding = <"));
    EXPECT_NE(std::string::npos, usda.find("uniform token info:id = \"UsdPreviewSurface\""));
    EXPECT_NE(std::string::npos, usda.find("uniform token info:id = \"UsdUVTexture\""));
    EXPECT_NE(std::string::npos, usda.find("uniform token info:id = \"UsdPrimvarReader_float2\""));
    EXPECT_NE(std::string::npos, usda.find("float2 inputs:st.connect = <"));
    EXPECT_NE(std::string::npos, usda.find("token inputs:sourceColorSpace = \"sRGB\""));
    EXPECT_NE(std::string::npos, usda.find("asset inputs:file = @./"));
    // no absolute or source-tree paths leak into the layer (T11)
    EXPECT_EQ(std::string::npos, usda.find("/Users/"));
    EXPECT_EQ(std::string::npos, usda.find(ASSIMP_TEST_MODELS_DIR));
    EXPECT_TRUE(IsPng(blobs[1].data));
}

TEST_F(utUSDZExport, rawTextureIsEncodedAsPng) {
    auto scene = MakeScene();
    scene->mNumTextures = 1;
    scene->mTextures = new aiTexture *[1] { new aiTexture };
    aiTexture *tex = scene->mTextures[0];
    tex->mWidth = 2;
    tex->mHeight = 2;
    tex->mFilename.Set("checker");
    tex->pcData = new aiTexel[4];
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned char v = (i % 3 == 0) ? 255 : 0;
        tex->pcData[i].r = tex->pcData[i].g = tex->pcData[i].b = v;
        tex->pcData[i].a = 255;
    }
    aiString path("*0");
    scene->mMaterials[0]->AddProperty(&path, AI_MATKEY_TEXTURE_DIFFUSE(0));

    const std::vector<Blob> blobs = ExportBlobs(scene.get(), "usdz");
    ASSERT_EQ(1u, blobs.size());
    std::vector<ZipEntry> entries;
    ASSERT_TRUE(ParseZip(blobs[0].data, entries));
    const ZipEntry *png = nullptr;
    for (const ZipEntry &e : entries) {
        if (e.name.rfind("textures/", 0) == 0) {
            png = &e;
        }
    }
    ASSERT_NE(nullptr, png);
    EXPECT_EQ(".png", png->name.substr(png->name.size() - 4));
    const std::vector<uint8_t> data = Member(blobs[0].data, *png);
    ASSERT_TRUE(IsPng(data));
    ASSERT_GE(data.size(), 24u);
    EXPECT_TRUE(StartsWith(data, "IHDR", 12));
    EXPECT_EQ(2u, Be32(&data[16]));
    EXPECT_EQ(2u, Be32(&data[20]));
}

TEST_F(utUSDZExport, externalTextureIsCopiedNextToLayer) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "assimp_usd_export_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    Importer importer;
    const std::string input = ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/BoxTextured.gltf";
    auto scene = ImportFile(importer, input.c_str());
    ASSERT_NE(nullptr, scene);
    ExportProperties props;
    props.SetPropertyString(AI_CONFIG_EXPORT_USD_SOURCE_DIRECTORY, ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF");
    Exporter exporter;
    const fs::path out = dir / "box.usda";
    ASSERT_EQ(AI_SUCCESS, exporter.Export(scene.get(), "usda", out.string(), 0u, &props)) << exporter.GetErrorString();

    std::ifstream in(out);
    const std::string usda((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(std::string::npos, usda.find("@./box_CesiumLogoFlat.png@"));
    const fs::path sidecar = dir / "box_CesiumLogoFlat.png";
    ASSERT_TRUE(fs::exists(sidecar));
    EXPECT_EQ(fs::file_size(sidecar), fs::file_size(ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF/CesiumLogoFlat.png"));
    fs::remove_all(dir);
}

// ------------------------------------------------------------------------------------------------
// USDC and USDZ containers

TEST_F(utUSDZExport, usdcHasCrateMagic) {
    auto scene = MakeScene();
    const std::vector<Blob> blobs = ExportBlobs(scene.get(), "usdc");
    ASSERT_EQ(1u, blobs.size());
    EXPECT_TRUE(StartsWith(blobs[0].data, "PXR-USDC"));
}

TEST_F(utUSDZExport, usdzPackageLayout) {
    Importer importer;
    auto scene = ImportFile(importer, ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-Binary/BoxTextured.glb");
    ASSERT_NE(nullptr, scene);
    const std::vector<Blob> blobs = ExportBlobs(scene.get(), "usdz");
    ASSERT_EQ(1u, blobs.size()); // textures live inside the package
    const std::vector<uint8_t> &z = blobs[0].data;
    std::vector<ZipEntry> entries;
    ASSERT_TRUE(ParseZip(z, entries));
    ASSERT_GE(entries.size(), 2u);
    // the default layer comes first and is a crate file
    EXPECT_EQ(".usdc", entries[0].name.substr(entries[0].name.size() - 5));
    EXPECT_TRUE(StartsWith(Member(z, entries[0]), "PXR-USDC"));
    bool hasTexture = false;
    for (const ZipEntry &e : entries) {
        EXPECT_EQ(0u, e.method) << e.name << " must be stored";
        EXPECT_EQ(0u, e.dataOffset % 64) << e.name << " must be 64-byte aligned";
        EXPECT_EQ(std::string::npos, e.name.find("..")) << e.name;
        EXPECT_NE('/', e.name[0]) << e.name;
        if (e.name.rfind("textures/", 0) == 0) {
            hasTexture = true;
            EXPECT_TRUE(IsPng(Member(z, e))) << e.name;
        }
    }
    EXPECT_TRUE(hasTexture);
}

// ------------------------------------------------------------------------------------------------
// Animation, skinning, cameras and lights

TEST_F(utUSDZExport, nodeAnimationKeepsKeyTimesAndRestValues) {
    auto scene = MakeScene();
    aiNode *body = scene->mRootNode->FindNode("Body");
    body->mTransformation = aiMatrix4x4(aiVector3D(2, 2, 2), aiQuaternion(), aiVector3D(0, 0, 0));
    scene->mNumAnimations = 1;
    scene->mAnimations = new aiAnimation *[1] { new aiAnimation };
    aiAnimation *anim = scene->mAnimations[0];
    anim->mTicksPerSecond = 30;
    anim->mDuration = 30;
    anim->mNumChannels = 1;
    anim->mChannels = new aiNodeAnim *[1] { new aiNodeAnim };
    aiNodeAnim *ch = anim->mChannels[0];
    ch->mNodeName.Set("Body");
    ch->mNumPositionKeys = 3;
    ch->mPositionKeys = new aiVectorKey[3]{ { 0, { 0, 0, 0 } }, { 15, { 1, 0, 0 } }, { 30, { 2, 0, 0 } } };

    const std::string usda = ExportUsda(scene.get());
    EXPECT_NE(std::string::npos, usda.find("timeCodesPerSecond = 30"));
    EXPECT_NE(std::string::npos, usda.find("startTimeCode = 0"));
    EXPECT_NE(std::string::npos, usda.find("endTimeCode = 30"));
    EXPECT_NE(std::string::npos, usda.find("xformOp:translate.timeSamples"));
    EXPECT_NE(std::string::npos, usda.find("15: (1, 0, 0)"));
    // channels without keys keep the rest pose, not identity
    EXPECT_NE(std::string::npos, usda.find("float3 xformOp:scale = (2, 2, 2)"));
    EXPECT_EQ(std::string::npos, usda.find("xformOp:orient.timeSamples"));
}

TEST_F(utUSDZExport, animationsCanBeDisabled) {
    auto scene = MakeScene();
    scene->mNumAnimations = 1;
    scene->mAnimations = new aiAnimation *[1] { new aiAnimation };
    aiAnimation *anim = scene->mAnimations[0];
    anim->mTicksPerSecond = 24;
    anim->mDuration = 1;
    anim->mNumChannels = 1;
    anim->mChannels = new aiNodeAnim *[1] { new aiNodeAnim };
    anim->mChannels[0]->mNodeName.Set("Body");
    anim->mChannels[0]->mNumPositionKeys = 2;
    anim->mChannels[0]->mPositionKeys = new aiVectorKey[2]{ { 0, { 0, 0, 0 } }, { 1, { 1, 0, 0 } } };
    ExportProperties props;
    props.SetPropertyBool(AI_CONFIG_EXPORT_USD_ANIMATIONS, false);
    const std::string usda = ExportUsda(scene.get(), &props);
    EXPECT_EQ(std::string::npos, usda.find("timeSamples"));
    EXPECT_EQ(std::string::npos, usda.find("timeCodesPerSecond"));
}

TEST_F(utUSDZExport, cesiumManSkeleton) {
    Importer importer;
    auto scene = ImportFile(importer, ASSIMP_TEST_MODELS_DIR "/glTF2/CesiumMan/CesiumMan.glb");
    if (scene == nullptr) {
        scene = ImportFile(importer, ASSIMP_TEST_MODELS_DIR "/glTF2/CesiumMan/CesiumMan.glb");
    }
    ASSERT_NE(nullptr, scene);
    const std::string usda = ExportUsda(scene.get());
    EXPECT_EQ(1u, Count(usda, "def SkelRoot "));
    EXPECT_EQ(1u, Count(usda, "def Skeleton "));
    EXPECT_EQ(1u, Count(usda, "def SkelAnimation "));
    EXPECT_NE(std::string::npos, usda.find("prepend apiSchemas = [\"SkelBindingAPI\"]"));
    EXPECT_NE(std::string::npos, usda.find("primvars:skel:jointIndices"));
    EXPECT_NE(std::string::npos, usda.find("primvars:skel:jointWeights"));
    EXPECT_NE(std::string::npos, usda.find("timeCodesPerSecond = 1000"));
    EXPECT_EQ(1u, RootPrims(usda).size());
}

TEST_F(utUSDZExport, cameraFocalLengthFromFov) {
    auto scene = MakeScene();
    AddChild(scene->mRootNode, "Cam");
    scene->mNumCameras = 1;
    scene->mCameras = new aiCamera *[1] { new aiCamera };
    aiCamera *cam = scene->mCameras[0];
    cam->mName.Set("Cam");
    cam->mHorizontalFOV = static_cast<float>(AI_MATH_PI / 2.0);
    cam->mAspect = 1.0f;
    cam->mClipPlaneNear = 0.1f;
    cam->mClipPlaneFar = 100.0f;
    const std::string usda = ExportUsda(scene.get());
    EXPECT_NE(std::string::npos, usda.find("def Camera \"Cam\""));
    // focal = 0.5 * 20.955 / tan(45 deg), the field of view is the full angle
    EXPECT_NE(std::string::npos, usda.find("float focalLength = 10.4775"));
    EXPECT_NE(std::string::npos, usda.find("float horizontalAperture = 20.955"));
}

TEST_F(utUSDZExport, lightsAreDroppedFromUsdzUnlessRequested) {
    auto scene = MakeScene();
    AddChild(scene->mRootNode, "Lamp");
    scene->mNumLights = 1;
    scene->mLights = new aiLight *[1] { new aiLight };
    scene->mLights[0]->mName.Set("Lamp");
    scene->mLights[0]->mType = aiLightSource_POINT;
    scene->mLights[0]->mColorDiffuse = aiColor3D(1, 1, 1);

    EXPECT_NE(std::string::npos, ExportUsda(scene.get()).find("def SphereLight \"Lamp\""));
    {
        ScopedLog log;
        ASSERT_EQ(1u, ExportBlobs(scene.get(), "usdz").size());
        EXPECT_NE(std::string::npos, log.Text().find("1 light(s) dropped"));
    }
    {
        ScopedLog log;
        ExportProperties props;
        props.SetPropertyBool(AI_CONFIG_EXPORT_USDZ_LIGHTS, true);
        ASSERT_EQ(1u, ExportBlobs(scene.get(), "usdz", &props).size());
        EXPECT_EQ(std::string::npos, log.Text().find("light(s) dropped"));
    }
}

// ------------------------------------------------------------------------------------------------
// UsdPhysics

TEST_F(utUSDZExport, physicsFromMetadata) {
    auto scene = MakeScene();
    aiNode *body = scene->mRootNode->FindNode("Body");
    body->mMetaData = aiMetadata::Alloc(3);
    body->mMetaData->Set(0, "usdPhysics:rigidBodyEnabled", true);
    body->mMetaData->Set(1, "usdPhysics:mass", 2.5f);
    body->mMetaData->Set(2, "usdPhysics:collisionEnabled", true);
    aiNode *hinge = AddChild(scene->mRootNode, "Hinge");
    hinge->mMetaData = aiMetadata::Alloc(3);
    hinge->mMetaData->Set(0, "usdPhysics:jointType", aiString(std::string("revolute")));
    hinge->mMetaData->Set(1, "usdPhysics:body0", aiString(std::string("Body")));
    hinge->mMetaData->Set(2, "usdPhysics:axis", aiString(std::string("X")));
    scene->mMetaData = aiMetadata::Alloc(1);
    scene->mMetaData->Set(0, "usdPhysics:gravityMagnitude", 9.81f);

    const std::string usda = ExportUsda(scene.get());
    EXPECT_NE(std::string::npos, usda.find("PhysicsRigidBodyAPI"));
    EXPECT_NE(std::string::npos, usda.find("PhysicsMassAPI"));
    EXPECT_NE(std::string::npos, usda.find("PhysicsCollisionAPI"));
    EXPECT_NE(std::string::npos, usda.find("float physics:mass = 2.5"));
    EXPECT_NE(std::string::npos, usda.find("def PhysicsScene \"PhysicsScene\""));
    EXPECT_NE(std::string::npos, usda.find("float physics:gravityMagnitude = 9.81"));
    EXPECT_NE(std::string::npos, usda.find("def PhysicsRevoluteJoint \"Hinge\""));
    EXPECT_NE(std::string::npos, usda.find("rel physics:body0 = </Scene/Body>"));
    EXPECT_NE(std::string::npos, usda.find("uniform token physics:axis = \"X\""));
    EXPECT_EQ(1u, RootPrims(usda).size());

    {
        ScopedLog log;
        ASSERT_EQ(1u, ExportBlobs(scene.get(), "usdz").size());
        EXPECT_NE(std::string::npos, log.Text().find("1 physics joint(s) dropped"));
    }
    {
        ScopedLog log;
        ExportProperties props;
        props.SetPropertyBool(AI_CONFIG_EXPORT_USDZ_PHYSICS_JOINTS, true);
        ASSERT_EQ(1u, ExportBlobs(scene.get(), "usdz", &props).size());
        EXPECT_EQ(std::string::npos, log.Text().find("physics joint(s) dropped"));
    }
}

// ------------------------------------------------------------------------------------------------
// Round trip through the USD importer

#ifndef ASSIMP_BUILD_NO_USD_IMPORTER
TEST_F(utUSDZExport, roundTripThroughImporter) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "assimp_usd_roundtrip_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Importer importer;
    auto scene = ImportFile(importer, ASSIMP_TEST_MODELS_DIR "/glTF2/BoxTextured-glTF-Binary/BoxTextured.glb");
    ASSERT_NE(nullptr, scene);
    for (const char *fmt : { "usda", "usdc", "usdz" }) {
        Exporter exporter;
        const fs::path out = dir / (std::string("box.") + fmt);
        ASSERT_EQ(AI_SUCCESS, exporter.Export(scene.get(), fmt, out.string())) << fmt << ": " << exporter.GetErrorString();
        Importer reader;
        const aiScene *back = reader.ReadFile(out.string(), 0u);
        ASSERT_NE(nullptr, back) << fmt << ": " << reader.GetErrorString();
        EXPECT_EQ(1u, back->mNumMeshes) << fmt;
        EXPECT_EQ(24u, back->mMeshes[0]->mNumVertices) << fmt;
    }
    fs::remove_all(dir);
}
#endif
