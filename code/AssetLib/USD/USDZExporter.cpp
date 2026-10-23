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


/** @file USDZExporter.cpp
 *  @brief USDA / USDC / USDZ export through LightUSD.
 *
 *  The exporter builds a LightUSD Stage bottom-up from the aiScene and
 *  serialises it in memory; every byte reaches the caller through the
 *  IOSystem handed to the export function.
 *
 *  Layout of the generated layer:
 *    /<Root>                      the aiScene root node, defaultPrim, kind=component
 *      <node prims>               Xform (or SkelRoot / Camera / light / physics joint)
 *        <mesh prims>             one Mesh or Points prim per aiMesh reference
 *      Materials (Scope)          UsdPreviewSurface networks
 *      PhysicsScene               only when the scene carries gravity metadata
 *
 *  Conventions follow the shared importer/exporter contract: units and up
 *  axis come from scene metadata (no geometry bake), UVs are written as-is
 *  (assimp and USD both use a bottom-left origin) and node names become prim
 *  names, sanitised and made unique among siblings.
 */

#ifndef ASSIMP_BUILD_NO_EXPORT
#ifndef ASSIMP_BUILD_NO_USD_EXPORTER

#include "USDZExporter.h"
#include "usdz-writer.hh"

#include <assimp/DefaultLogger.hpp>
#include <assimp/Exceptional.h>
#include <assimp/Exporter.hpp>
#include <assimp/GltfMaterial.h>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/commonMetaData.h>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "image-loader.hh"
#include "image-writer.hh"
#include "prim-types.hh"
#include "stage.hh"
#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdPhysics.hh"
#include "usdShade.hh"
#include "usdSkel.hh"
#include "usda-writer.hh"
#include "crate-writer.hh"
#include "usdc-writer.hh"
#include "value-types.hh"

namespace Assimp {
namespace {

namespace lu = lightusd;
namespace luv = lightusd::value;

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToDeg = 180.0 / kPi;

// ------------------------------------------------------------------------------------------------
// Options

enum class OutputFormat { USDA, USDC, USDZ };

struct Options {
    OutputFormat format = OutputFormat::USDA;
    bool animations = true;
    int animationIndex = 0;
    bool clearcoat = true;
    bool lights = true;
    bool physicsJoints = true;
    bool compressFloatArrays = true;
    std::string sourceDirectory;
    int64_t maxFileBytes = 0;
    int64_t maxMemoryBytes = 0;
};

Options ReadOptions(const ExportProperties *props, OutputFormat format) {
#ifdef __EMSCRIPTEN__
    // WebAssembly has a 4 GiB address space; keep well below it.
    constexpr int64_t kDefaultLimit = int64_t(2048) << 20;
#else
    // Native builds: effectively unlimited (LightUSD's own defaults are far
    // smaller than the meshes assimp routinely exports).
    constexpr int64_t kDefaultLimit = std::numeric_limits<int64_t>::max() / 4;
#endif
    Options o;
    o.format = format;
    bool usdzLights = false;
    bool usdzJoints = false;
    int maxFileMb = 0;
    int maxMemoryMb = 0;
    if (props != nullptr) {
        o.animations = props->GetPropertyBool(AI_CONFIG_EXPORT_USD_ANIMATIONS, true);
        o.animationIndex = props->GetPropertyInteger(AI_CONFIG_EXPORT_USD_ANIMATION_INDEX, 0);
        o.clearcoat = props->GetPropertyBool(AI_CONFIG_EXPORT_USD_CLEARCOAT, true);
        usdzLights = props->GetPropertyBool(AI_CONFIG_EXPORT_USDZ_LIGHTS, false);
        usdzJoints = props->GetPropertyBool(AI_CONFIG_EXPORT_USDZ_PHYSICS_JOINTS, false);
        o.sourceDirectory = props->GetPropertyString(AI_CONFIG_EXPORT_USD_SOURCE_DIRECTORY, "");
        maxFileMb = props->GetPropertyInteger(AI_CONFIG_EXPORT_USD_MAX_FILE_SIZE_MB, 0);
        maxMemoryMb = props->GetPropertyInteger(AI_CONFIG_EXPORT_USD_MAX_MEMORY_MB, 0);
        o.compressFloatArrays = props->GetPropertyBool(AI_CONFIG_EXPORT_USD_COMPRESS_FLOAT_ARRAYS, true);
    }
    // ARKit's USDZ profile allows neither UsdLux lights nor UsdPhysics joints.
    o.lights = format != OutputFormat::USDZ || usdzLights;
    o.physicsJoints = format != OutputFormat::USDZ || usdzJoints;
    o.maxFileBytes = maxFileMb > 0 ? (int64_t(maxFileMb) << 20) : kDefaultLimit;
    o.maxMemoryBytes = maxMemoryMb > 0 ? (int64_t(maxMemoryMb) << 20) : kDefaultLimit;
    return o;
}

// ------------------------------------------------------------------------------------------------
// Names

bool IsAsciiAlpha(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool IsAsciiDigit(unsigned char c) {
    return c >= '0' && c <= '9';
}

/// Valid USD identifier: [A-Za-z_][A-Za-z0-9_]*. Non-ASCII bytes become '_'
/// so the layer stays readable by OpenUSD releases without UTF-8 identifiers.
std::string SanitizeIdentifier(const std::string &in, const char *fallback) {
    std::string out;
    out.reserve(in.size() + 1);
    for (const char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        out.push_back((IsAsciiAlpha(c) || IsAsciiDigit(c) || c == '_') ? ch : '_');
    }
    if (out.empty()) {
        out = fallback;
    }
    if (IsAsciiDigit(static_cast<unsigned char>(out[0]))) {
        out.insert(out.begin(), '_');
    }
    return out;
}

/// Safe file-name component: [A-Za-z0-9_.-], no leading dot.
std::string SanitizeFileName(const std::string &in, const char *fallback) {
    std::string out;
    out.reserve(in.size());
    for (const char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (IsAsciiAlpha(c) || IsAsciiDigit(c) || c == '_' || c == '-' || c == '.') {
            out.push_back(ch);
        } else if (!out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }
    while (!out.empty() && (out.front() == '.' || out.front() == '_')) {
        out.erase(out.begin());
    }
    while (!out.empty() && (out.back() == '.' || out.back() == '_')) {
        out.pop_back();
    }
    return out.empty() ? std::string(fallback) : out;
}

/// Unique, sanitised names within one namespace (the children of one prim).
class NameRegistry {
public:
    std::string Claim(const std::string &raw, const char *fallback) {
        const std::string base = SanitizeIdentifier(raw, fallback);
        std::string name = base;
        for (unsigned n = 1; !mUsed.insert(name).second; ++n) {
            name = base + "_" + std::to_string(n);
        }
        return name;
    }

private:
    std::unordered_set<std::string> mUsed;
};

std::string ToLower(std::string s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

std::string FileStem(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        base.resize(dot);
    }
    return base;
}


std::string UrlDecode(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && hex(in[i + 1]) >= 0 && hex(in[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hex(in[i + 1]) * 16 + hex(in[i + 2])));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// Math

/// assimp matrices are row-major with column vectors; USD matrix4d uses row
/// vectors, so the element layout is the transpose.
luv::matrix4d ToUsdMatrix(const aiMatrix4x4 &m) {
    luv::matrix4d r;
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned col = 0; col < 4; ++col) {
            r.m[row][col] = static_cast<double>(m[col][row]);
        }
    }
    return r;
}

bool IsIdentity(const aiMatrix4x4 &m, ai_real eps = static_cast<ai_real>(1e-6)) {
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned col = 0; col < 4; ++col) {
            const ai_real expected = row == col ? ai_real(1) : ai_real(0);
            if (std::abs(m[row][col] - expected) > eps) {
                return false;
            }
        }
    }
    return true;
}

bool NearlyEqual(const aiMatrix4x4 &a, const aiMatrix4x4 &b, ai_real eps) {
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned col = 0; col < 4; ++col) {
            if (std::abs(a[row][col] - b[row][col]) > eps * std::max<ai_real>(1, std::abs(a[row][col]))) {
                return false;
            }
        }
    }
    return true;
}

aiMatrix4x4 Inverse(aiMatrix4x4 m) {
    return m.Inverse();
}

/// Frame of a camera or light: looks down `dir` (USD: -Z forward, +Y up).
aiMatrix4x4 FrameFromDirection(const aiVector3D &pos, aiVector3D dir, aiVector3D up) {
    if (dir.SquareLength() < 1e-12f) {
        dir = aiVector3D(0, 0, -1);
    }
    aiVector3D z = -dir.Normalize();
    if (up.SquareLength() < 1e-12f) {
        up = aiVector3D(0, 1, 0);
    }
    aiVector3D x = up ^ z;
    if (x.SquareLength() < 1e-12f) {
        x = (std::abs(z.x) < 0.9f ? aiVector3D(1, 0, 0) : aiVector3D(0, 1, 0)) ^ z;
    }
    x.Normalize();
    const aiVector3D y = z ^ x;
    return aiMatrix4x4(x.x, y.x, z.x, pos.x,
            x.y, y.y, z.y, pos.y,
            x.z, y.z, z.z, pos.z,
            0, 0, 0, 1);
}

luv::quatf ToQuatf(const aiQuaternion &q) {
    luv::quatf r;
    r.imag = { static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z) };
    r.real = static_cast<float>(q.w);
    return r;
}

luv::half3 ToHalf3(const aiVector3D &v) {
    return { luv::float_to_half_full(static_cast<float>(v.x)),
        luv::float_to_half_full(static_cast<float>(v.y)),
        luv::float_to_half_full(static_cast<float>(v.z)) };
}

template <class Key>
size_t KeyIndex(const Key *keys, unsigned count, double t) {
    // last key with mTime <= t (binary search; P6)
    const Key *end = keys + count;
    const Key *it = std::upper_bound(keys, end, t, [](double v, const Key &k) { return v < k.mTime; });
    return it == keys ? 0 : static_cast<size_t>(it - keys - 1);
}

aiVector3D SampleVector(const aiVectorKey *keys, unsigned count, double t) {
    const size_t i = KeyIndex(keys, count, t);
    if (i + 1 >= count || t <= keys[i].mTime) {
        return keys[i].mValue;
    }
    const double span = keys[i + 1].mTime - keys[i].mTime;
    const ai_real f = span > 0 ? static_cast<ai_real>((t - keys[i].mTime) / span) : ai_real(0);
    return keys[i].mValue + (keys[i + 1].mValue - keys[i].mValue) * f;
}

aiQuaternion SampleQuat(const aiQuatKey *keys, unsigned count, double t) {
    const size_t i = KeyIndex(keys, count, t);
    if (i + 1 >= count || t <= keys[i].mTime) {
        return keys[i].mValue;
    }
    const double span = keys[i + 1].mTime - keys[i].mTime;
    const ai_real f = span > 0 ? static_cast<ai_real>((t - keys[i].mTime) / span) : ai_real(0);
    aiQuaternion out;
    aiQuaternion::Interpolate(out, keys[i].mValue, keys[i + 1].mValue, f);
    return out.Normalize();
}

// ------------------------------------------------------------------------------------------------
// Metadata

bool MetaNumber(const aiMetadata *md, const char *key, double &out) {
    if (md == nullptr) {
        return false;
    }
    for (unsigned i = 0; i < md->mNumProperties; ++i) {
        if (std::strcmp(md->mKeys[i].C_Str(), key) != 0) {
            continue;
        }
        const aiMetadataEntry &e = md->mValues[i];
        if (e.mData == nullptr) {
            return false;
        }
        switch (e.mType) {
        case AI_DOUBLE: out = *static_cast<const double *>(e.mData); return true;
        case AI_FLOAT: out = *static_cast<const float *>(e.mData); return true;
        case AI_INT32: out = *static_cast<const int32_t *>(e.mData); return true;
        case AI_UINT32: out = *static_cast<const uint32_t *>(e.mData); return true;
        case AI_INT64: out = static_cast<double>(*static_cast<const int64_t *>(e.mData)); return true;
        case AI_UINT64: out = static_cast<double>(*static_cast<const uint64_t *>(e.mData)); return true;
        case AI_BOOL: out = *static_cast<const bool *>(e.mData) ? 1.0 : 0.0; return true;
        default: return false;
        }
    }
    return false;
}

bool MetaBool(const aiMetadata *md, const char *key, bool &out) {
    double v = 0;
    if (!MetaNumber(md, key, v)) {
        return false;
    }
    out = v != 0.0;
    return true;
}

bool MetaFloat(const aiMetadata *md, const char *key, float &out) {
    double v = 0;
    if (!MetaNumber(md, key, v)) {
        return false;
    }
    out = static_cast<float>(v);
    return true;
}

bool MetaVector(const aiMetadata *md, const char *key, aiVector3D &out) {
    return md != nullptr && md->Get(key, out);
}

bool MetaString(const aiMetadata *md, const char *key, std::string &out) {
    aiString s;
    if (md == nullptr || !md->Get(key, s)) {
        return false;
    }
    out = s.C_Str();
    return true;
}

// ------------------------------------------------------------------------------------------------
// LightUSD helpers

void AddApiSchema(lu::PrimMeta &meta, lu::APISchemas::APIName api, const std::string &instance = std::string()) {
    lu::APISchemas &schemas = meta.get_apiSchemas_mutable();
    schemas.listOpQual = lu::ListEditQual::Prepend;
    for (const auto &entry : schemas.names) {
        if (entry.first == api && entry.second == instance) {
            return;
        }
    }
    schemas.names.emplace_back(api, instance);
}

template <class T>
void AddAttribute(std::map<std::string, lu::Property> &props, const std::string &name, const T &value,
        bool uniform = false) {
    lu::Attribute attr;
    attr.set_value(value);
    if (uniform) {
        attr.variability() = lu::Variability::Uniform;
    }
    props[name] = lu::Property(std::move(attr), /*custom*/ false);
}

void AddRelationship(std::map<std::string, lu::Property> &props, const std::string &name,
        const std::vector<std::string> &targets) {
    lu::Relationship rel;
    if (targets.size() == 1) {
        rel.set(lu::Path(targets[0], ""));
    } else {
        std::vector<lu::Path> paths;
        paths.reserve(targets.size());
        for (const std::string &t : targets) {
            paths.emplace_back(t, "");
        }
        rel.set(paths);
    }
    props[name] = lu::Property(rel, /*custom*/ false);
}

lu::Relationship MakeRelationship(const std::string &target) {
    lu::Relationship rel;
    rel.set(lu::Path(target, ""));
    return rel;
}

template <class T>
void AddPrimvar(std::map<std::string, lu::Property> &props, const std::string &name, T &&values,
        lu::Interpolation interp, uint32_t elementSize = 0) {
    lu::Attribute attr;
    attr.set_value(std::forward<T>(values));
    attr.metas().set_interpolation_enum(interp);
    if (elementSize > 1) {
        attr.metas().set_elementSize(elementSize);
    }
    props["primvars:" + name] = lu::Property(std::move(attr), /*custom*/ false);
}

/// Matrix xformOp, skipped when the transform is the identity.
void AddMatrixOp(lu::Xformable &x, const aiMatrix4x4 &m, const std::string &suffix = std::string()) {
    if (IsIdentity(m)) {
        return;
    }
    lu::XformOp op;
    op.op_type = lu::XformOp::OpType::Transform;
    op.suffix = suffix;
    op.set_value(ToUsdMatrix(m));
    x.xformOps.push_back(std::move(op));
}

// ------------------------------------------------------------------------------------------------
// Textures

bool IsPng(const std::vector<uint8_t> &b) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    return b.size() >= 8 && std::memcmp(b.data(), sig, 8) == 0;
}

bool IsJpeg(const std::vector<uint8_t> &b) {
    return b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
}

bool IsExr(const std::vector<uint8_t> &b) {
    return b.size() >= 4 && b[0] == 0x76 && b[1] == 0x2F && b[2] == 0x31 && b[3] == 0x01;
}

/// Encodes 8-bit RGB/RGBA pixels as PNG. The one PNG encoder used by the
/// exporter (raw aiTexture texels and transcoded formats alike, D21).
bool EncodePng(int width, int height, int channels, std::vector<uint8_t> &&pixels, std::vector<uint8_t> &png,
        std::string &err) {
    lu::Image img;
    img.width = width;
    img.height = height;
    img.channels = channels;
    img.bpp = 8;
    img.format = lu::Image::PixelFormat::UInt;
    img.data = std::move(pixels);
    lu::image::WriteOption opt;
    opt.format = lu::image::WriteImageFormat::PNG;
    auto res = lu::image::WriteImageToMemory(img, opt);
    if (!res) {
        err = res.error();
        return false;
    }
    png = std::move(res.value());
    return true;
}

/// Raw aiTexture texels (BGRA8888) to PNG.
bool RawTextureToPng(const aiTexture &tex, std::vector<uint8_t> &png, std::string &err) {
    if (tex.mWidth == 0 || tex.mHeight == 0 || tex.pcData == nullptr) {
        err = "empty texture";
        return false;
    }
    const size_t count = size_t(tex.mWidth) * size_t(tex.mHeight);
    bool opaque = true;
    for (size_t i = 0; i < count && opaque; ++i) {
        opaque = tex.pcData[i].a == 255;
    }
    const int channels = opaque ? 3 : 4;
    std::vector<uint8_t> pixels(count * size_t(channels));
    for (size_t i = 0; i < count; ++i) {
        const aiTexel &t = tex.pcData[i];
        uint8_t *dst = &pixels[i * size_t(channels)];
        dst[0] = t.r;
        dst[1] = t.g;
        dst[2] = t.b;
        if (channels == 4) {
            dst[3] = t.a;
        }
    }
    return EncodePng(static_cast<int>(tex.mWidth), static_cast<int>(tex.mHeight), channels, std::move(pixels), png, err);
}

/// Any image LightUSD can decode (PNG, JPEG, BMP, TGA, GIF, PSD, HDR, ...) to PNG.
bool TranscodeToPng(const std::vector<uint8_t> &bytes, const std::string &uri, std::vector<uint8_t> &png,
        std::string &err) {
    auto loaded = lu::image::LoadImageFromMemory(bytes.data(), bytes.size(), uri);
    if (!loaded) {
        err = loaded.error();
        return false;
    }
    const lu::Image &img = loaded.value().image;
    if (img.width <= 0 || img.height <= 0 || img.channels <= 0 || img.channels > 4) {
        err = "unsupported image layout";
        return false;
    }
    const size_t count = size_t(img.width) * size_t(img.height);
    const int inC = img.channels;
    const int outC = (inC == 2 || inC == 4) ? 4 : 3;
    std::vector<uint8_t> pixels(count * size_t(outC));
    auto sample = [&](size_t i, int c) -> uint8_t {
        const size_t idx = i * size_t(inC) + size_t(c);
        if (img.format == lu::Image::PixelFormat::Float && img.bpp == 32) {
            float v = 0;
            std::memcpy(&v, img.data.data() + idx * 4, 4);
            v = std::isfinite(v) ? std::min(1.0f, std::max(0.0f, v)) : 0.0f;
            return static_cast<uint8_t>(v * 255.0f + 0.5f);
        }
        if (img.bpp == 16) {
            uint16_t v = 0;
            std::memcpy(&v, img.data.data() + idx * 2, 2);
            return static_cast<uint8_t>(v >> 8);
        }
        return img.data[idx];
    };
    const size_t bytesPerSample = img.bpp == 32 ? 4 : (img.bpp == 16 ? 2 : 1);
    if (img.data.size() < count * size_t(inC) * bytesPerSample) {
        err = "truncated image data";
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        uint8_t *dst = &pixels[i * size_t(outC)];
        if (inC <= 2) {
            dst[0] = dst[1] = dst[2] = sample(i, 0);
            if (outC == 4) {
                dst[3] = sample(i, 1);
            }
        } else {
            dst[0] = sample(i, 0);
            dst[1] = sample(i, 1);
            dst[2] = sample(i, 2);
            if (outC == 4) {
                dst[3] = sample(i, 3);
            }
        }
    }
    return EncodePng(img.width, img.height, outC, std::move(pixels), png, err);
}

std::vector<luv::token> ToTokens(const std::vector<std::string> &names);

struct TextureAsset {
    std::vector<uint8_t> bytes;
    std::string fileName;  // member name (USDZ) or sidecar file name (USDA/USDC)
    std::string assetPath; // value of inputs:file
};

// ------------------------------------------------------------------------------------------------
// The exporter

class UsdExporter {
public:
    UsdExporter(const char *path, IOSystem *io, const aiScene *scene, const Options &opts);
    void Run();

private:
    enum class NodeKind { Xform, SkelRoot, Camera, Light, PhysicsJoint };

    struct NodeInfo {
        const aiNode *node = nullptr;
        int parent = -1;
        int depth = 0;
        std::vector<int> children;     // structural children (aiNode order)
        aiMatrix4x4 world;
        bool joint = false;            // skeleton joint: no prim, animated through SkelAnimation
        bool insideJoint = false;      // descendant of a joint (relocated under the skeleton parent)
        int exportParent = -1;
        std::vector<int> exportChildren;
        aiMatrix4x4 local;             // transform authored on the prim
        bool bakedLocal = false;       // local re-derived because the node was relocated
        std::string name;
        std::string path;
        NameRegistry names;            // namespace of the prim's children
        NodeKind kind = NodeKind::Xform;
        bool skelRootInside = false;   // has a SkelRoot ancestor (or is one)
        int typedCamera = -1;          // camera folded into the node prim
        int typedLight = -1;           // light folded into the node prim
        bool jointChild = false;       // physics joint authored as a child prim
        std::vector<unsigned> cameras; // cameras exported as child prims
        std::vector<unsigned> lights;  // lights exported as child prims
        const aiNodeAnim *anim = nullptr;
        double animScale = 1.0;
        std::vector<std::pair<double, bool>> visibility; // (time code, visible)
    };

    struct SkeletonPlan {
        bool valid = false;
        int parentNode = -1;            // P: skeleton space is this node's space
        std::vector<int> joints;        // node indices, parents first
        std::vector<std::string> tokens;
        std::unordered_map<int, int> jointOfNode;
        std::vector<aiMatrix4x4> bind;  // skeleton-space bind transforms
        std::vector<bool> bindSet;
        std::vector<std::pair<const aiNodeAnim *, double>> channels; // per joint, may be null
        int skelRootNode = -1;
        std::string path;
        std::string animationPath;
        // blend shapes of skinned meshes are driven by the skeleton's animation
        std::unordered_map<unsigned, std::vector<std::string>> shapeTokens; // mesh -> tokens
        std::vector<std::string> allTokens;
        std::vector<float> defaultWeights;
        std::vector<std::pair<const aiMeshMorphAnim *, double>> tokenChannel;
        std::vector<unsigned> tokenShape; // anim-mesh index inside its morph channel
        bool HasAnimation() const;
    };

    using MorphChannel = std::pair<const aiMeshMorphAnim *, double>;

    // planning
    void IndexNodes();
    void PlanSkeleton();
    void PlanExportTree();
    void PlanSkelRoots();
    void PlanAttachments();
    void PlanAnimations();
    void PlanSkinnedBlendShapes();
    bool MeshHasPolygons(const aiMesh *mesh) const;
    bool MeshHasPoints(const aiMesh *mesh) const;
    bool IsSkinned(unsigned meshIndex) const { return meshIndex < mSkinned.size() && mSkinned[meshIndex]; }
    int Lca(int a, int b) const;
    int FindNode(const std::string &name) const;
    MorphChannel FindMorphChannel(int nodeIndex, const aiMesh *mesh) const;
    void TrackTime(double t);

    // building
    lu::Prim BuildNode(int index);
    template <class T>
    void SetupXformable(T &x, int index, const aiMatrix4x4 *extra);
    template <class T>
    void ApplyNodeExtras(T &prim, int index);
    void AppendMeshPrims(int index, std::vector<lu::Prim> &out);
    bool ConvertMeshGeometry(const aiMesh *mesh, lu::GeomMesh &usd);
    void ConvertPointsGeometry(const aiMesh *mesh, lu::GeomPoints &usd);
    void AddSkinning(unsigned meshIndex, lu::GeomMesh &usd);
    std::vector<std::string> BlendShapeNames(const aiMesh *mesh) const;
    void AppendBlendShapes(const aiMesh *mesh, const std::string &meshPath, const std::vector<std::string> &tokens,
            lu::GeomMesh &usd, std::vector<lu::Prim> &meshChildren);
    lu::Prim BuildSkeletonPrim();
    lu::Prim BuildSkelAnimation(const std::string &name, const SkeletonPlan *skel,
            const std::vector<std::string> &tokens, const std::vector<float> &defaults,
            const std::vector<MorphChannel> &channels, const std::vector<unsigned> &shapeOfToken);
    lu::Prim BuildCameraPrim(unsigned cameraIndex, const std::string &name, int nodeIndex, bool typedNode);
    lu::Prim BuildLightPrim(unsigned lightIndex, const std::string &name, int nodeIndex, bool typedNode);
    lu::Prim BuildPhysicsJointPrim(int nodeIndex, const std::string &name);
    template <class T>
    void AddPhysicsApis(T &prim, int nodeIndex);
    lu::Prim BuildPhysicsScene(const std::string &name);
    bool HasPhysicsJoint(int nodeIndex) const;

    // materials and textures
    std::string MaterialPath(unsigned materialIndex, bool vertexColor);
    lu::Prim BuildMaterial(unsigned materialIndex, bool vertexColor, const std::string &name, const std::string &path);
    std::string PhysicsMaterialPath(int nodeIndex);
    int RequestTexture(const std::string &path);
    bool ReadExternalTexture(const std::string &path, std::vector<uint8_t> &bytes, std::string &resolved);

    // output
    void WriteOutput(const lu::Stage &stage);
    bool SaveCrate(const lu::Stage &stage, std::vector<uint8_t> &out, std::string &err);
    void WriteFile(const std::string &name, const uint8_t *data, size_t size, const char *mode);

    const aiScene *mScene;
    IOSystem *mIO;
    std::string mPath;
    Options mOpts;

    std::vector<NodeInfo> mNodes;
    std::unordered_map<std::string, int> mNodeByName;
    std::vector<std::vector<int>> mMeshUsers;
    std::vector<bool> mSkinned;
    SkeletonPlan mSkel;
    NameRegistry mRootNames; // the pseudo-root namespace

    // animation
    std::vector<const aiAnimation *> mAnimations;
    double mTimeCodesPerSecond = 24.0;
    double mStartTime = std::numeric_limits<double>::infinity();
    double mEndTime = -std::numeric_limits<double>::infinity();
    std::unordered_map<std::string, MorphChannel> mMorphByName;
    std::unordered_map<unsigned, std::vector<std::pair<const aiPropertyAnim *, double>>> mMaterialAnims;

    // materials
    std::string mMaterialsPath;
    std::string mMaterialsName;
    NameRegistry mMaterialNames;
    std::map<std::pair<unsigned, bool>, std::string> mMaterialPaths;
    std::unordered_map<int, std::string> mPhysicsMaterialPaths;
    std::vector<lu::Prim> mMaterialPrims;
    bool mGltfSource = false;

    // textures
    std::vector<TextureAsset> mTextures;
    std::unordered_map<std::string, int> mTextureByKey;
    NameRegistry mTextureNames;
    std::string mOutputDir;
    std::string mOutputStem;

    // shared meshes are converted once (P1/P5)
    std::unordered_map<unsigned, lu::GeomMesh> mMeshCache;
    std::unordered_map<unsigned, unsigned> mMeshUsesLeft;

    // ARKit requires a Y-up stage: Z-up (or X-up) sources get a rotation on the root prim
    aiMatrix4x4 mAxisCorrection;

    // LightUSD writes token[] arrays of 16+ elements integer-compressed, which
    // OpenUSD reads as raw indices; such stages are written uncompressed.
    bool mLongTokenArrays = false;
    std::vector<luv::token> Tokens(const std::vector<std::string> &names) {
        mLongTokenArrays = mLongTokenArrays || names.size() >= 16;
        return ToTokens(names);
    }

    unsigned mDroppedLights = 0;
    unsigned mDroppedJoints = 0;
};

bool UsdExporter::SkeletonPlan::HasAnimation() const {
    for (const auto &c : channels) {
        if (c.first != nullptr) {
            return true;
        }
    }
    return !allTokens.empty();
}

UsdExporter::UsdExporter(const char *path, IOSystem *io, const aiScene *scene, const Options &opts) :
        mScene(scene), mIO(io), mPath(path), mOpts(opts) {
    const size_t slash = mPath.find_last_of("/\\");
    mOutputDir = slash == std::string::npos ? std::string() : mPath.substr(0, slash + 1);
    mOutputStem = SanitizeFileName(FileStem(mPath), "scene");
    std::string format;
    if (MetaString(mScene->mMetaData, AI_METADATA_SOURCE_FORMAT, format)) {
        format = ToLower(format);
        mGltfSource = format.find("gltf") != std::string::npos || format.find("glb") != std::string::npos;
    }
}

int UsdExporter::FindNode(const std::string &name) const {
    auto it = mNodeByName.find(name);
    return it == mNodeByName.end() ? -1 : it->second;
}

void UsdExporter::TrackTime(double t) {
    mStartTime = std::min(mStartTime, t);
    mEndTime = std::max(mEndTime, t);
}

bool UsdExporter::MeshHasPolygons(const aiMesh *mesh) const {
    for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
        if (mesh->mFaces[f].mNumIndices >= 3) {
            return true;
        }
    }
    return false;
}

bool UsdExporter::MeshHasPoints(const aiMesh *mesh) const {
    return mesh->mNumVertices > 0 && (mesh->mPrimitiveTypes & aiPrimitiveType_POINT) != 0;
}

int UsdExporter::Lca(int a, int b) const {
    if (a < 0 || b < 0) {
        return a < 0 ? b : a;
    }
    while (mNodes[a].depth > mNodes[b].depth) a = mNodes[a].parent;
    while (mNodes[b].depth > mNodes[a].depth) b = mNodes[b].parent;
    while (a != b) {
        a = mNodes[a].parent;
        b = mNodes[b].parent;
    }
    return a;
}

void UsdExporter::IndexNodes() {
    std::vector<std::pair<const aiNode *, int>> stack;
    stack.emplace_back(mScene->mRootNode, -1);
    while (!stack.empty()) {
        const aiNode *node = stack.back().first;
        const int parent = stack.back().second;
        stack.pop_back();
        const int index = static_cast<int>(mNodes.size());
        mNodes.emplace_back();
        NodeInfo &info = mNodes.back();
        info.node = node;
        info.parent = parent;
        if (parent >= 0) {
            info.depth = mNodes[parent].depth + 1;
            info.world = mNodes[parent].world * node->mTransformation;
            mNodes[parent].children.push_back(index);
        } else {
            info.world = node->mTransformation;
        }
        info.local = node->mTransformation;
        mNodeByName.emplace(node->mName.C_Str(), index);
        for (unsigned c = node->mNumChildren; c-- > 0;) {
            if (node->mChildren[c] != nullptr) {
                stack.emplace_back(node->mChildren[c], index);
            }
        }
    }
    mMeshUsers.assign(mScene->mNumMeshes, {});
    for (size_t n = 0; n < mNodes.size(); ++n) {
        const aiNode *node = mNodes[n].node;
        for (unsigned i = 0; i < node->mNumMeshes; ++i) {
            const unsigned m = node->mMeshes[i];
            if (m < mScene->mNumMeshes && mScene->mMeshes[m] != nullptr) {
                mMeshUsers[m].push_back(static_cast<int>(n));
            } else {
                ASSIMP_LOG_WARN("USD export: node '", node->mName.C_Str(), "' references missing mesh ", m);
            }
        }
    }
}

void UsdExporter::PlanSkeleton() {
    mSkinned.assign(mScene->mNumMeshes, false);
    std::set<int> boneNodes;
    for (unsigned m = 0; m < mScene->mNumMeshes; ++m) {
        const aiMesh *mesh = mScene->mMeshes[m];
        if (mesh == nullptr || mesh->mNumBones == 0 || mMeshUsers[m].empty() || !MeshHasPolygons(mesh)) {
            continue;
        }
        bool resolved = true;
        for (unsigned b = 0; b < mesh->mNumBones && resolved; ++b) {
            const aiBone *bone = mesh->mBones[b];
            resolved = bone != nullptr && FindNode(bone->mName.C_Str()) >= 0;
        }
        if (!resolved) {
            ASSIMP_LOG_WARN("USD export: mesh '", mesh->mName.C_Str(),
                    "' has bones without a matching node; exported unskinned");
            continue;
        }
        mSkinned[m] = true;
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            boneNodes.insert(FindNode(mesh->mBones[b]->mName.C_Str()));
        }
    }
    if (boneNodes.empty()) {
        return;
    }
    int lca = -1;
    for (int b : boneNodes) {
        lca = Lca(lca, b);
    }
    const int parent = boneNodes.count(lca) ? mNodes[lca].parent : lca;
    if (parent < 0) {
        ASSIMP_LOG_WARN("USD export: the skeleton is rooted at the scene root node; skinning is not exported");
        mSkinned.assign(mScene->mNumMeshes, false);
        return;
    }
    // joints: every node on the paths from P down to the bones
    std::set<int> joints;
    for (int b : boneNodes) {
        for (int n = b; n != parent; n = mNodes[n].parent) {
            joints.insert(n);
        }
    }
    SkeletonPlan &skel = mSkel;
    skel.valid = true;
    skel.parentNode = parent;
    skel.joints.assign(joints.begin(), joints.end()); // preorder indices: parents first
    std::unordered_map<int, NameRegistry> jointNames;
    for (size_t j = 0; j < skel.joints.size(); ++j) {
        const int n = skel.joints[j];
        mNodes[n].joint = true;
        skel.jointOfNode[n] = static_cast<int>(j);
        const int p = mNodes[n].parent;
        const std::string name = jointNames[p].Claim(mNodes[n].node->mName.C_Str(), "joint");
        skel.tokens.push_back(p == parent ? name : skel.tokens[skel.jointOfNode[p]] + "/" + name);
    }
    skel.channels.assign(skel.joints.size(), { nullptr, 1.0 });
    // bind transforms in skeleton space: inv(W_P) * W_mesh * inv(offset) for bones
    const aiMatrix4x4 invParent = Inverse(mNodes[parent].world);
    skel.bind.assign(skel.joints.size(), aiMatrix4x4());
    skel.bindSet.assign(skel.joints.size(), false);
    bool mismatch = false;
    for (unsigned m = 0; m < mScene->mNumMeshes; ++m) {
        if (!mSkinned[m]) {
            continue;
        }
        const aiMesh *mesh = mScene->mMeshes[m];
        const aiMatrix4x4 &meshWorld = mNodes[mMeshUsers[m].front()].world;
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const aiBone *bone = mesh->mBones[b];
            const int j = skel.jointOfNode[FindNode(bone->mName.C_Str())];
            const aiMatrix4x4 bind = invParent * meshWorld * Inverse(bone->mOffsetMatrix);
            if (!skel.bindSet[j]) {
                skel.bind[j] = bind;
                skel.bindSet[j] = true;
            } else if (!NearlyEqual(skel.bind[j], bind, static_cast<ai_real>(1e-3))) {
                mismatch = true;
            }
        }
    }
    if (mismatch) {
        ASSIMP_LOG_WARN("USD export: skinned meshes disagree on a joint's bind pose; the first mesh's bind pose is used");
    }
    for (size_t j = 0; j < skel.joints.size(); ++j) {
        if (!skel.bindSet[j]) {
            skel.bind[j] = invParent * mNodes[skel.joints[j]].world; // intermediate non-bone joint
        }
    }
}

void UsdExporter::PlanExportTree() {
    const int skelParent = mSkel.valid ? mSkel.parentNode : -1;
    unsigned relocated = 0;
    for (size_t i = 0; i < mNodes.size(); ++i) {
        NodeInfo &info = mNodes[i];
        if (info.parent >= 0) {
            const NodeInfo &parent = mNodes[info.parent];
            info.insideJoint = parent.joint || parent.insideJoint;
        }
        if (info.joint) {
            continue;
        }
        if (info.parent < 0) {
            info.exportParent = -1;
            std::string rootName = info.node->mName.C_Str();
            if (rootName.empty()) {
                rootName = FileStem(mPath);
            }
            info.name = mRootNames.Claim(rootName, "Root");
            info.path = "/" + info.name;
            continue;
        }
        if (mNodes[info.parent].joint) {
            // attached to a joint: re-parented under the skeleton's parent with
            // its bind-pose transform baked (it no longer follows the joint)
            info.exportParent = skelParent;
            info.local = Inverse(mNodes[skelParent].world) * info.world;
            info.bakedLocal = true;
            ++relocated;
        } else {
            info.exportParent = info.parent;
        }
        NodeInfo &parent = mNodes[info.exportParent];
        parent.exportChildren.push_back(static_cast<int>(i));
        info.name = parent.names.Claim(info.node->mName.C_Str(), "Node");
        info.path = parent.path + "/" + info.name;
    }
    if (relocated > 0) {
        ASSIMP_LOG_WARN("USD export: ", relocated,
                " node(s) attached to skeleton joints were moved under the skeleton's parent with their rest transform");
    }
    if (mSkel.valid) {
        NodeInfo &p = mNodes[skelParent];
        mSkel.path = p.path + "/" + p.names.Claim("Skeleton", "Skeleton");
        mSkel.animationPath = mSkel.path + "/Animation";
    }
    mMaterialsName = mNodes[0].names.Claim("Materials", "Materials");
    mMaterialsPath = mNodes[0].path + "/" + mMaterialsName;
}

void UsdExporter::PlanSkelRoots() {
    if (mSkel.valid) {
        // LCA of the skeleton's parent and every node instancing a skinned mesh
        int root = mSkel.parentNode;
        for (unsigned m = 0; m < mScene->mNumMeshes; ++m) {
            if (!mSkinned[m]) {
                continue;
            }
            for (int user : mMeshUsers[m]) {
                const int placed = mNodes[user].insideJoint || mNodes[user].joint ? mSkel.parentNode : user;
                root = Lca(root, placed);
            }
        }
        mSkel.skelRootNode = root;
    }
    for (size_t i = 0; i < mNodes.size(); ++i) {
        NodeInfo &info = mNodes[i];
        if (info.joint) {
            continue;
        }
        const bool inside = info.exportParent >= 0 && mNodes[info.exportParent].skelRootInside;
        bool wantsRoot = mSkel.valid && static_cast<int>(i) == mSkel.skelRootNode;
        if (!inside && !wantsRoot) {
            for (unsigned k = 0; k < info.node->mNumMeshes && !wantsRoot; ++k) {
                const unsigned m = info.node->mMeshes[k];
                if (m < mScene->mNumMeshes && mScene->mMeshes[m] != nullptr) {
                    const aiMesh *mesh = mScene->mMeshes[m];
                    wantsRoot = mesh->mNumAnimMeshes > 0 && !IsSkinned(m) && MeshHasPolygons(mesh);
                }
            }
        }
        if (!inside && wantsRoot) {
            info.kind = NodeKind::SkelRoot;
        }
        info.skelRootInside = inside || info.kind == NodeKind::SkelRoot;
    }
}

bool UsdExporter::HasPhysicsJoint(int nodeIndex) const {
    std::string type;
    return MetaString(mNodes[nodeIndex].node->mMetaData, "usdPhysics:jointType", type);
}

void UsdExporter::PlanAttachments() {
    auto isLeaf = [&](int n) {
        const NodeInfo &info = mNodes[n];
        return n != 0 && info.kind == NodeKind::Xform && info.node->mNumMeshes == 0 && info.exportChildren.empty() &&
               info.cameras.empty() && info.lights.empty() && info.typedCamera < 0 && info.typedLight < 0 &&
               !(mSkel.valid && n == mSkel.parentNode) && !HasPhysicsJoint(n);
    };
    for (unsigned c = 0; c < mScene->mNumCameras; ++c) {
        const aiCamera *cam = mScene->mCameras[c];
        if (cam == nullptr) {
            continue;
        }
        int n = FindNode(cam->mName.C_Str());
        if (n < 0 || mNodes[n].joint) {
            ASSIMP_LOG_WARN("USD export: camera '", cam->mName.C_Str(), "' has no node; placed under the root prim");
            n = 0;
        }
        if (isLeaf(n)) {
            mNodes[n].typedCamera = static_cast<int>(c);
            mNodes[n].kind = NodeKind::Camera;
        } else {
            mNodes[n].cameras.push_back(c);
        }
    }
    for (unsigned l = 0; l < mScene->mNumLights; ++l) {
        const aiLight *light = mScene->mLights[l];
        if (light == nullptr) {
            continue;
        }
        if (light->mType != aiLightSource_DIRECTIONAL && light->mType != aiLightSource_POINT &&
                light->mType != aiLightSource_SPOT && light->mType != aiLightSource_AREA) {
            ASSIMP_LOG_WARN("USD export: light '", light->mName.C_Str(), "' has an unsupported type; skipped");
            continue;
        }
        if (!mOpts.lights) {
            ++mDroppedLights;
            continue;
        }
        int n = FindNode(light->mName.C_Str());
        if (n < 0 || mNodes[n].joint) {
            ASSIMP_LOG_WARN("USD export: light '", light->mName.C_Str(), "' has no node; placed under the root prim");
            n = 0;
        }
        if (isLeaf(n)) {
            mNodes[n].typedLight = static_cast<int>(l);
            mNodes[n].kind = NodeKind::Light;
        } else {
            mNodes[n].lights.push_back(l);
        }
    }
    if (mDroppedLights > 0) {
        ASSIMP_LOG_WARN("USD export: ", mDroppedLights, " light(s) dropped: ARKit USDZ does not allow UsdLux (set ",
                AI_CONFIG_EXPORT_USDZ_LIGHTS, " to keep them)");
    }
    for (size_t n = 0; n < mNodes.size(); ++n) {
        if (mNodes[n].joint || !HasPhysicsJoint(static_cast<int>(n))) {
            continue;
        }
        if (!mOpts.physicsJoints) {
            ++mDroppedJoints;
            continue;
        }
        const NodeInfo &info = mNodes[n];
        const bool leaf = n != 0 && info.kind == NodeKind::Xform && info.node->mNumMeshes == 0 &&
                          info.exportChildren.empty() && info.cameras.empty() && info.lights.empty() &&
                          !(mSkel.valid && static_cast<int>(n) == mSkel.parentNode);
        if (leaf) {
            mNodes[n].kind = NodeKind::PhysicsJoint;
        } else {
            mNodes[n].jointChild = true;
        }
    }
    if (mDroppedJoints > 0) {
        ASSIMP_LOG_WARN("USD export: ", mDroppedJoints, " physics joint(s) dropped: ARKit USDZ does not allow them (set ",
                AI_CONFIG_EXPORT_USDZ_PHYSICS_JOINTS, " to keep them)");
    }
}

void UsdExporter::PlanAnimations() {
    if (!mOpts.animations || mScene->mNumAnimations == 0 || mScene->mAnimations == nullptr) {
        return;
    }
    // All animations are exported together when they drive disjoint targets
    // (one USD layer has one timeline); otherwise only the selected one (D24).
    std::set<std::string> targets;
    bool disjoint = true;
    for (unsigned a = 0; a < mScene->mNumAnimations && disjoint; ++a) {
        const aiAnimation *anim = mScene->mAnimations[a];
        if (anim == nullptr) {
            continue;
        }
        for (unsigned c = 0; c < anim->mNumChannels && disjoint; ++c) {
            disjoint = targets.insert(std::string("n:") + anim->mChannels[c]->mNodeName.C_Str()).second;
        }
        for (unsigned c = 0; c < anim->mNumMorphMeshChannels && disjoint; ++c) {
            disjoint = targets.insert(std::string("m:") + anim->mMorphMeshChannels[c]->mName.C_Str()).second;
        }
        for (unsigned c = 0; c < anim->mNumPropertyChannels && disjoint; ++c) {
            const aiPropertyAnim *p = anim->mPropertyChannels[c];
            disjoint = targets.insert("p:" + std::to_string(p->mTargetType) + ":" + std::to_string(p->mTargetIndex) +
                                      ":" + p->mTargetProperty.C_Str()).second;
        }
    }
    if (disjoint) {
        for (unsigned a = 0; a < mScene->mNumAnimations; ++a) {
            if (mScene->mAnimations[a] != nullptr) {
                mAnimations.push_back(mScene->mAnimations[a]);
            }
        }
    } else {
        int index = mOpts.animationIndex;
        if (index < 0 || static_cast<unsigned>(index) >= mScene->mNumAnimations || mScene->mAnimations[index] == nullptr) {
            ASSIMP_LOG_WARN("USD export: animation index ", index, " is out of range; using 0");
            index = 0;
        }
        ASSIMP_LOG_WARN("USD export: the scene's ", mScene->mNumAnimations,
                " animations drive the same targets; only animation ", index, " is exported (",
                AI_CONFIG_EXPORT_USD_ANIMATION_INDEX, " selects another)");
        if (mScene->mAnimations[index] != nullptr) {
            mAnimations.push_back(mScene->mAnimations[index]);
        }
    }
    if (mAnimations.empty()) {
        return;
    }
    // Time codes are the source ticks of the first animation (D25).
    mTimeCodesPerSecond = mAnimations.front()->mTicksPerSecond > 0 ? mAnimations.front()->mTicksPerSecond : 24.0;
    unsigned unmatched = 0;
    unsigned dropped = 0;
    for (const aiAnimation *anim : mAnimations) {
        const double scale = anim->mTicksPerSecond > 0 ? mTimeCodesPerSecond / anim->mTicksPerSecond : 1.0;
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim *ch = anim->mChannels[c];
            if (ch == nullptr) {
                continue;
            }
            const int n = FindNode(ch->mNodeName.C_Str());
            if (n < 0) {
                ++unmatched;
                continue;
            }
            if (ch->mNumPositionKeys == 0 && ch->mNumRotationKeys == 0 && ch->mNumScalingKeys == 0) {
                continue;
            }
            if (mNodes[n].joint) {
                mSkel.channels[mSkel.jointOfNode[n]] = { ch, scale };
            } else if (mNodes[n].bakedLocal) {
                ++dropped;
                continue;
            } else {
                mNodes[n].anim = ch;
                mNodes[n].animScale = scale;
            }
            for (unsigned k = 0; k < ch->mNumPositionKeys; ++k) TrackTime(ch->mPositionKeys[k].mTime * scale);
            for (unsigned k = 0; k < ch->mNumRotationKeys; ++k) TrackTime(ch->mRotationKeys[k].mTime * scale);
            for (unsigned k = 0; k < ch->mNumScalingKeys; ++k) TrackTime(ch->mScalingKeys[k].mTime * scale);
        }
        for (unsigned c = 0; c < anim->mNumMorphMeshChannels; ++c) {
            const aiMeshMorphAnim *ch = anim->mMorphMeshChannels[c];
            if (ch == nullptr || ch->mNumKeys == 0) {
                continue;
            }
            mMorphByName.emplace(ch->mName.C_Str(), MorphChannel(ch, scale));
            for (unsigned k = 0; k < ch->mNumKeys; ++k) TrackTime(ch->mKeys[k].mTime * scale);
        }
        for (unsigned c = 0; c < anim->mNumPropertyChannels; ++c) {
            const aiPropertyAnim *p = anim->mPropertyChannels[c];
            if (p == nullptr || p->mNumKeys == 0) {
                continue;
            }
            if (p->mTargetType == aiPropertyAnimTarget_MATERIAL_COLOR) {
                if (p->mTargetIndex < mScene->mNumMaterials) {
                    mMaterialAnims[p->mTargetIndex].emplace_back(p, scale);
                }
            } else if (p->mTargetType == aiPropertyAnimTarget_VISIBILITY) {
                // the target index counts nodes in depth-first pre-order
                if (p->mTargetIndex < mNodes.size() && !mNodes[p->mTargetIndex].joint) {
                    auto &vis = mNodes[p->mTargetIndex].visibility;
                    for (unsigned k = 0; k < p->mNumKeys; ++k) {
                        vis.emplace_back(p->mKeys[k].mTime * scale, p->mKeys[k].mValues[0] > 0.5f);
                    }
                }
            } else {
                continue;
            }
            for (unsigned k = 0; k < p->mNumKeys; ++k) TrackTime(p->mKeys[k].mTime * scale);
        }
    }
    if (unmatched > 0) {
        ASSIMP_LOG_WARN("USD export: ", unmatched, " animation channel(s) target unknown nodes and were skipped");
    }
    if (dropped > 0) {
        ASSIMP_LOG_WARN("USD export: ", dropped, " animation channel(s) on nodes moved off skeleton joints were dropped");
    }
}

UsdExporter::MorphChannel UsdExporter::FindMorphChannel(int nodeIndex, const aiMesh *mesh) const {
    auto it = mMorphByName.find(mNodes[nodeIndex].node->mName.C_Str());
    if (it == mMorphByName.end()) {
        it = mMorphByName.find(mesh->mName.C_Str());
    }
    return it == mMorphByName.end() ? MorphChannel(nullptr, 1.0) : it->second;
}

std::vector<std::string> UsdExporter::BlendShapeNames(const aiMesh *mesh) const {
    NameRegistry names;
    std::vector<std::string> out;
    out.reserve(mesh->mNumAnimMeshes);
    for (unsigned k = 0; k < mesh->mNumAnimMeshes; ++k) {
        const aiAnimMesh *am = mesh->mAnimMeshes[k];
        const std::string raw = (am != nullptr && am->mName.length > 0) ? std::string(am->mName.C_Str())
                                                                          : "shape" + std::to_string(k);
        out.push_back(names.Claim(raw, "shape"));
    }
    return out;
}

void UsdExporter::PlanSkinnedBlendShapes() {
    if (!mSkel.valid) {
        return;
    }
    NameRegistry tokens;
    for (unsigned m = 0; m < mScene->mNumMeshes; ++m) {
        const aiMesh *mesh = mScene->mMeshes[m];
        if (!mSkinned[m] || mesh->mNumAnimMeshes == 0) {
            continue;
        }
        MorphChannel channel(nullptr, 1.0);
        for (int user : mMeshUsers[m]) {
            channel = FindMorphChannel(user, mesh);
            if (channel.first != nullptr) {
                break;
            }
        }
        const std::vector<std::string> names = BlendShapeNames(mesh);
        const std::string prefix = SanitizeIdentifier(mesh->mName.C_Str(), "mesh") + "_";
        std::vector<std::string> &meshTokens = mSkel.shapeTokens[m];
        for (unsigned k = 0; k < mesh->mNumAnimMeshes; ++k) {
            meshTokens.push_back(tokens.Claim(prefix + names[k], "shape"));
            mSkel.allTokens.push_back(meshTokens.back());
            const aiAnimMesh *am = mesh->mAnimMeshes[k];
            mSkel.defaultWeights.push_back(am != nullptr ? am->mWeight : 0.0f);
            mSkel.tokenChannel.push_back(channel);
            mSkel.tokenShape.push_back(k);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Prim building

template <class T>
lu::Animatable<T> MakeAnimatable(T &&value) {
    lu::Animatable<T> a;
    a.set(std::move(value));
    return a;
}

std::string LastPathElement(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::vector<luv::token> ToTokens(const std::vector<std::string> &names) {
    std::vector<luv::token> out;
    out.reserve(names.size());
    for (const std::string &n : names) {
        out.emplace_back(n);
    }
    return out;
}

float SampleMorphWeight(const aiMeshMorphAnim *ch, unsigned shape, double ticks) {
    auto weightAt = [&](const aiMeshMorphKey &key) -> float {
        for (unsigned i = 0; i < key.mNumValuesAndWeights; ++i) {
            if (key.mValues[i] == shape) {
                return static_cast<float>(key.mWeights[i]);
            }
        }
        return 0.0f;
    };
    const size_t i = KeyIndex(ch->mKeys, ch->mNumKeys, ticks);
    if (i + 1 >= ch->mNumKeys || ticks <= ch->mKeys[i].mTime) {
        return weightAt(ch->mKeys[i]);
    }
    const double span = ch->mKeys[i + 1].mTime - ch->mKeys[i].mTime;
    const double f = span > 0 ? (ticks - ch->mKeys[i].mTime) / span : 0.0;
    const float a = weightAt(ch->mKeys[i]);
    const float b = weightAt(ch->mKeys[i + 1]);
    return static_cast<float>(a + (b - a) * f);
}

void SortUnique(std::vector<double> &v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

template <class T>
void UsdExporter::SetupXformable(T &x, int index, const aiMatrix4x4 *extra) {
    const NodeInfo &info = mNodes[index];
    const bool axisFix = index == 0 && !IsIdentity(mAxisCorrection);
    if (axisFix && info.anim != nullptr) {
        AddMatrixOp(x, mAxisCorrection, "upAxis");
    }
    if (info.anim != nullptr) {
        // Node animation: translate / orient / scale ops sampled at the source
        // keys; components without keys keep the rest pose (D23).
        const aiNodeAnim *ch = info.anim;
        const double sc = info.animScale;
        aiVector3D restS, restT;
        aiQuaternion restR;
        info.local.Decompose(restS, restR, restT);
        if (!NearlyEqual(aiMatrix4x4(restS, restR, restT), info.local, static_cast<ai_real>(1e-4))) {
            ASSIMP_LOG_WARN("USD export: animated node '", info.node->mName.C_Str(),
                    "' has a sheared rest transform; the shear is lost");
        }
        lu::XformOp t;
        t.op_type = lu::XformOp::OpType::Translate;
        if (ch->mNumPositionKeys > 0) {
            for (unsigned k = 0; k < ch->mNumPositionKeys; ++k) {
                const aiVector3D &v = ch->mPositionKeys[k].mValue;
                t.set_timesample(static_cast<float>(ch->mPositionKeys[k].mTime * sc), luv::double3{ v.x, v.y, v.z });
            }
        } else {
            t.set_value(luv::double3{ restT.x, restT.y, restT.z });
        }
        lu::XformOp r;
        r.op_type = lu::XformOp::OpType::Orient;
        if (ch->mNumRotationKeys > 0) {
            for (unsigned k = 0; k < ch->mNumRotationKeys; ++k) {
                aiQuaternion q = ch->mRotationKeys[k].mValue;
                r.set_timesample(static_cast<float>(ch->mRotationKeys[k].mTime * sc), ToQuatf(q.Normalize()));
            }
        } else {
            r.set_value(ToQuatf(restR));
        }
        lu::XformOp s;
        s.op_type = lu::XformOp::OpType::Scale;
        if (ch->mNumScalingKeys > 0) {
            for (unsigned k = 0; k < ch->mNumScalingKeys; ++k) {
                const aiVector3D &v = ch->mScalingKeys[k].mValue;
                s.set_timesample(static_cast<float>(ch->mScalingKeys[k].mTime * sc),
                        luv::float3{ static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z) });
            }
        } else {
            s.set_value(luv::float3{ static_cast<float>(restS.x), static_cast<float>(restS.y), static_cast<float>(restS.z) });
        }
        x.xformOps.push_back(std::move(t));
        x.xformOps.push_back(std::move(r));
        x.xformOps.push_back(std::move(s));
        if (extra != nullptr) {
            AddMatrixOp(x, *extra, "local");
        }
    } else {
        const aiMatrix4x4 local = axisFix ? mAxisCorrection * info.local : info.local;
        AddMatrixOp(x, extra != nullptr ? local * *extra : local);
    }
    if (!info.visibility.empty()) {
        lu::Animatable<lu::Visibility> vis;
        for (const auto &key : info.visibility) {
            vis.add_sample(key.first, key.second ? lu::Visibility::Inherited : lu::Visibility::Invisible);
        }
        x.visibility.set_value(vis);
    }
}

lu::Prim UsdExporter::BuildNode(int index) {
    NodeInfo &info = mNodes[index];
    std::vector<lu::Prim> kids;
    kids.reserve(info.exportChildren.size() + info.node->mNumMeshes + 2);
    for (int c : info.exportChildren) {
        kids.push_back(BuildNode(c));
    }
    AppendMeshPrims(index, kids);
    for (unsigned c : info.cameras) {
        kids.push_back(BuildCameraPrim(c, info.names.Claim(mScene->mCameras[c]->mName.C_Str(), "Camera"), index, false));
    }
    for (unsigned l : info.lights) {
        kids.push_back(BuildLightPrim(l, info.names.Claim(mScene->mLights[l]->mName.C_Str(), "Light"), index, false));
    }
    if (info.jointChild) {
        kids.push_back(BuildPhysicsJointPrim(index, info.names.Claim(info.name + "_joint", "Joint")));
    }
    if (mSkel.valid && index == mSkel.parentNode) {
        kids.push_back(BuildSkeletonPrim());
    }

    lu::Prim prim;
    switch (info.kind) {
    case NodeKind::SkelRoot: {
        lu::SkelRoot x;
        SetupXformable(x, index, nullptr);
        AddPhysicsApis(x, index);
        prim = lu::Prim(info.name, luv::Value(std::move(x)));
        break;
    }
    case NodeKind::Camera:
        prim = BuildCameraPrim(static_cast<unsigned>(info.typedCamera), info.name, index, true);
        break;
    case NodeKind::Light:
        prim = BuildLightPrim(static_cast<unsigned>(info.typedLight), info.name, index, true);
        break;
    case NodeKind::PhysicsJoint:
        prim = BuildPhysicsJointPrim(index, info.name);
        break;
    case NodeKind::Xform:
    default: {
        lu::Xform x;
        SetupXformable(x, index, nullptr);
        AddPhysicsApis(x, index);
        prim = lu::Prim(info.name, luv::Value(std::move(x)));
        break;
    }
    }
    for (lu::Prim &kid : kids) {
        prim.children().push_back(std::move(kid));
    }
    return prim;
}

bool UsdExporter::ConvertMeshGeometry(const aiMesh *mesh, lu::GeomMesh &usd) {
    const unsigned nv = mesh->mNumVertices;
    std::vector<int32_t> counts;
    std::vector<int32_t> indices;
    counts.reserve(mesh->mNumFaces);
    indices.reserve(size_t(mesh->mNumFaces) * 3);
    unsigned badFaces = 0;
    for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        if (face.mNumIndices < 3) {
            continue;
        }
        bool ok = true;
        for (unsigned i = 0; i < face.mNumIndices && ok; ++i) {
            ok = face.mIndices[i] < nv;
        }
        if (!ok) {
            ++badFaces;
            continue;
        }
        counts.push_back(static_cast<int32_t>(face.mNumIndices));
        for (unsigned i = 0; i < face.mNumIndices; ++i) {
            indices.push_back(static_cast<int32_t>(face.mIndices[i]));
        }
    }
    if (badFaces > 0) {
        ASSIMP_LOG_WARN("USD export: mesh '", mesh->mName.C_Str(), "': ", badFaces, " face(s) with invalid indices skipped");
    }
    if (counts.empty()) {
        return false;
    }
    if (mesh->mNumFaces > 0 && counts.size() < mesh->mNumFaces) {
        ASSIMP_LOG_WARN("USD export: mesh '", mesh->mName.C_Str(), "': point and line primitives are not exported");
    }

    std::vector<luv::point3f> points(nv);
    luv::float3 lo{ { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() } };
    luv::float3 hi{ { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() } };
    for (unsigned v = 0; v < nv; ++v) {
        const aiVector3D &p = mesh->mVertices[v];
        points[v] = { static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) };
        lo[0] = std::min(lo[0], points[v].x);
        lo[1] = std::min(lo[1], points[v].y);
        lo[2] = std::min(lo[2], points[v].z);
        hi[0] = std::max(hi[0], points[v].x);
        hi[1] = std::max(hi[1], points[v].y);
        hi[2] = std::max(hi[2], points[v].z);
    }
    usd.points.set_value(MakeAnimatable(std::move(points)));
    usd.faceVertexCounts.set_value(MakeAnimatable(std::move(counts)));
    usd.faceVertexIndices.set_value(MakeAnimatable(std::move(indices)));
    if (nv > 0) {
        lu::Extent extent;
        extent.lower = lo;
        extent.upper = hi;
        usd.extent.set_value(lu::Animatable<lu::Extent>(extent));
    }
    usd.subdivisionScheme.set_value(lu::GeomMesh::SubdivisionScheme::SubdivisionSchemeNone);

    if (mesh->HasNormals()) {
        std::vector<luv::normal3f> normals(nv);
        unsigned invalid = 0;
        for (unsigned v = 0; v < nv; ++v) {
            const aiVector3D &n = mesh->mNormals[v];
            if (std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z)) {
                normals[v] = { static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z) };
            } else {
                normals[v] = { 0.0f, 0.0f, 1.0f };
                ++invalid;
            }
        }
        if (invalid > 0) {
            ASSIMP_LOG_WARN("USD export: mesh '", mesh->mName.C_Str(), "': ", invalid,
                    " non-finite normal(s) replaced with +Z");
        }
        usd.normals.set_value(MakeAnimatable(std::move(normals)));
        usd.normals.metas().set_interpolation_enum(lu::Interpolation::Vertex);
    }
    // UVs are written unflipped (D22): assimp and USD share a bottom-left origin.
    for (unsigned c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++c) {
        if (!mesh->HasTextureCoords(c)) {
            continue;
        }
        std::vector<luv::texcoord2f> st(nv);
        for (unsigned v = 0; v < nv; ++v) {
            st[v] = { static_cast<float>(mesh->mTextureCoords[c][v].x), static_cast<float>(mesh->mTextureCoords[c][v].y) };
        }
        AddPrimvar(usd.props, c == 0 ? std::string("st") : "st" + std::to_string(c), std::move(st),
                lu::Interpolation::Vertex);
    }
    if (mesh->HasVertexColors(0)) {
        std::vector<luv::color3f> colors(nv);
        std::vector<float> opacity(nv);
        bool translucent = false;
        for (unsigned v = 0; v < nv; ++v) {
            const aiColor4D &col = mesh->mColors[0][v];
            colors[v] = { col.r, col.g, col.b };
            opacity[v] = col.a;
            translucent = translucent || col.a < 1.0f;
        }
        AddPrimvar(usd.props, "displayColor", std::move(colors), lu::Interpolation::Vertex);
        if (translucent) {
            AddPrimvar(usd.props, "displayOpacity", std::move(opacity), lu::Interpolation::Vertex);
        }
    }
    return true;
}

void UsdExporter::ConvertPointsGeometry(const aiMesh *mesh, lu::GeomPoints &usd) {
    const unsigned nv = mesh->mNumVertices;
    std::vector<luv::point3f> points(nv);
    aiVector3D lo(std::numeric_limits<ai_real>::max()), hi(-std::numeric_limits<ai_real>::max());
    for (unsigned v = 0; v < nv; ++v) {
        const aiVector3D &p = mesh->mVertices[v];
        points[v] = { static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) };
        lo.x = std::min(lo.x, p.x);
        lo.y = std::min(lo.y, p.y);
        lo.z = std::min(lo.z, p.z);
        hi.x = std::max(hi.x, p.x);
        hi.y = std::max(hi.y, p.y);
        hi.z = std::max(hi.z, p.z);
    }
    usd.points.set_value(MakeAnimatable(std::move(points)));
    // USD's fallback width is one scene unit; use a size relative to the cloud.
    const float diag = nv > 0 ? static_cast<float>((hi - lo).Length()) : 0.0f;
    const float width = diag > 0 ? diag * 0.002f : 0.01f;
    // per-point widths: LightUSD's USDC writer drops the interpolation metadata
    // of Points attributes, so a constant width would read as a short array
    usd.widths.set_value(MakeAnimatable(std::vector<float>(nv, width)));
    if (nv > 0) {
        lu::Extent extent;
        extent.lower = { { static_cast<float>(lo.x - width), static_cast<float>(lo.y - width), static_cast<float>(lo.z - width) } };
        extent.upper = { { static_cast<float>(hi.x + width), static_cast<float>(hi.y + width), static_cast<float>(hi.z + width) } };
        usd.extent.set_value(lu::Animatable<lu::Extent>(extent));
    }
    if (mesh->HasNormals()) {
        std::vector<luv::normal3f> normals(nv);
        for (unsigned v = 0; v < nv; ++v) {
            const aiVector3D &n = mesh->mNormals[v];
            const bool ok = std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z);
            normals[v] = ok ? luv::normal3f{ static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z) }
                            : luv::normal3f{ 0.0f, 0.0f, 1.0f };
        }
        usd.normals.set_value(MakeAnimatable(std::move(normals)));
    }
    if (mesh->HasVertexColors(0)) {
        std::vector<luv::color3f> colors(nv);
        for (unsigned v = 0; v < nv; ++v) {
            colors[v] = { mesh->mColors[0][v].r, mesh->mColors[0][v].g, mesh->mColors[0][v].b };
        }
        AddPrimvar(usd.props, "displayColor", std::move(colors), lu::Interpolation::Vertex);
    }
}

void UsdExporter::AddSkinning(unsigned meshIndex, lu::GeomMesh &usd) {
    const aiMesh *mesh = mScene->mMeshes[meshIndex];
    const unsigned nv = mesh->mNumVertices;
    std::vector<uint32_t> counts(nv, 0);
    for (unsigned b = 0; b < mesh->mNumBones; ++b) {
        const aiBone *bone = mesh->mBones[b];
        for (unsigned w = 0; w < bone->mNumWeights; ++w) {
            const unsigned v = bone->mWeights[w].mVertexId;
            if (v < nv && bone->mWeights[w].mWeight > 0.0f) {
                ++counts[v];
            }
        }
    }
    uint32_t influences = 1;
    for (uint32_t c : counts) {
        influences = std::max(influences, c);
    }
    std::vector<int32_t> jointIndices(size_t(nv) * influences, 0);
    std::vector<float> jointWeights(size_t(nv) * influences, 0.0f);
    std::fill(counts.begin(), counts.end(), 0u);
    for (unsigned b = 0; b < mesh->mNumBones; ++b) {
        const aiBone *bone = mesh->mBones[b];
        const int joint = mSkel.jointOfNode.at(FindNode(bone->mName.C_Str()));
        for (unsigned w = 0; w < bone->mNumWeights; ++w) {
            const unsigned v = bone->mWeights[w].mVertexId;
            if (v >= nv || bone->mWeights[w].mWeight <= 0.0f) {
                continue;
            }
            const size_t slot = size_t(v) * influences + counts[v]++;
            jointIndices[slot] = joint;
            jointWeights[slot] = bone->mWeights[w].mWeight;
        }
    }
    unsigned unweighted = 0;
    std::vector<std::pair<float, int32_t>> scratch(influences);
    for (unsigned v = 0; v < nv; ++v) {
        const size_t base = size_t(v) * influences;
        float sum = 0.0f;
        for (uint32_t i = 0; i < influences; ++i) {
            scratch[i] = { jointWeights[base + i], jointIndices[base + i] };
            sum += jointWeights[base + i];
        }
        if (sum <= 0.0f) {
            ++unweighted;
            jointIndices[base] = 0;
            jointWeights[base] = 1.0f;
            continue;
        }
        std::stable_sort(scratch.begin(), scratch.end(),
                [](const std::pair<float, int32_t> &a, const std::pair<float, int32_t> &b) { return a.first > b.first; });
        for (uint32_t i = 0; i < influences; ++i) {
            jointWeights[base + i] = scratch[i].first / sum;
            jointIndices[base + i] = scratch[i].first > 0.0f ? scratch[i].second : 0;
        }
    }
    if (unweighted > 0) {
        ASSIMP_LOG_WARN("USD export: mesh '", mesh->mName.C_Str(), "': ", unweighted,
                " vertices without bone weights bound to the first joint");
    }
    AddPrimvar(usd.props, "skel:jointIndices", std::move(jointIndices), lu::Interpolation::Vertex, influences);
    AddPrimvar(usd.props, "skel:jointWeights", std::move(jointWeights), lu::Interpolation::Vertex, influences);
    const aiMatrix4x4 geomBind = Inverse(mNodes[mSkel.parentNode].world) * mNodes[mMeshUsers[meshIndex].front()].world;
    if (!IsIdentity(geomBind)) {
        AddAttribute(usd.props, "primvars:skel:geomBindTransform", ToUsdMatrix(geomBind));
    }
    usd.skeleton = MakeRelationship(mSkel.path);
    AddApiSchema(usd.meta, lu::APISchemas::APIName::SkelBindingAPI);
}

void UsdExporter::AppendBlendShapes(const aiMesh *mesh, const std::string &meshPath,
        const std::vector<std::string> &tokens, lu::GeomMesh &usd, std::vector<lu::Prim> &meshChildren) {
    const std::vector<std::string> names = BlendShapeNames(mesh);
    const unsigned nv = mesh->mNumVertices;
    std::vector<lu::Path> targets;
    for (unsigned k = 0; k < mesh->mNumAnimMeshes; ++k) {
        const aiAnimMesh *am = mesh->mAnimMeshes[k];
        std::vector<luv::vector3f> offsets(nv, luv::vector3f{ 0.0f, 0.0f, 0.0f });
        std::vector<luv::vector3f> normalOffsets;
        const bool withNormals = am != nullptr && am->mNormals != nullptr && mesh->mNormals != nullptr &&
                                 am->mNumVertices == nv;
        if (withNormals) {
            normalOffsets.assign(nv, luv::vector3f{ 0.0f, 0.0f, 0.0f });
        }
        size_t nonZero = 0;
        std::vector<bool> used(nv, false);
        if (am != nullptr && am->mVertices != nullptr && am->mNumVertices == nv) {
            for (unsigned v = 0; v < nv; ++v) {
                const aiVector3D d = am->mVertices[v] - mesh->mVertices[v];
                offsets[v] = { static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z) };
                used[v] = d.x != 0 || d.y != 0 || d.z != 0;
            }
        }
        if (withNormals) {
            for (unsigned v = 0; v < nv; ++v) {
                const aiVector3D d = am->mNormals[v] - mesh->mNormals[v];
                normalOffsets[v] = { static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z) };
                used[v] = used[v] || d.x != 0 || d.y != 0 || d.z != 0;
            }
        }
        for (unsigned v = 0; v < nv; ++v) {
            nonZero += used[v] ? 1 : 0;
        }
        lu::BlendShape bs;
        if (nonZero * 2 < nv) {
            // sparse: only the points the shape moves (one zero offset when none)
            std::vector<int> pointIndices;
            std::vector<luv::vector3f> sparse;
            std::vector<luv::vector3f> sparseNormals;
            for (unsigned v = 0; v < nv; ++v) {
                if (used[v] || (nonZero == 0 && v == 0)) {
                    pointIndices.push_back(static_cast<int>(v));
                    sparse.push_back(offsets[v]);
                    if (withNormals) {
                        sparseNormals.push_back(normalOffsets[v]);
                    }
                }
            }
            bs.pointIndices.set_value(std::move(pointIndices));
            offsets = std::move(sparse);
            normalOffsets = std::move(sparseNormals);
        }
        bs.offsets.set_value(std::move(offsets));
        if (withNormals) {
            bs.normalOffsets.set_value(std::move(normalOffsets));
        }
        meshChildren.emplace_back(names[k], luv::Value(std::move(bs)));
        targets.emplace_back(meshPath + "/" + names[k], "");
    }
    usd.blendShapes.set_value(Tokens(tokens));
    lu::Relationship rel;
    rel.set(std::move(targets));
    usd.blendShapeTargets = rel;
}

void UsdExporter::AppendMeshPrims(int index, std::vector<lu::Prim> &out) {
    NodeInfo &info = mNodes[index];
    for (unsigned k = 0; k < info.node->mNumMeshes; ++k) {
        const unsigned m = info.node->mMeshes[k];
        if (m >= mScene->mNumMeshes || mScene->mMeshes[m] == nullptr) {
            continue;
        }
        const aiMesh *mesh = mScene->mMeshes[m];
        const aiMaterial *mat = mesh->mMaterialIndex < mScene->mNumMaterials ? mScene->mMaterials[mesh->mMaterialIndex] : nullptr;
        aiString diffusePath;
        const bool hasDiffuseTexture = mat != nullptr &&
                (mat->GetTexture(aiTextureType_BASE_COLOR, 0, &diffusePath) == AI_SUCCESS ||
                 mat->GetTexture(aiTextureType_DIFFUSE, 0, &diffusePath) == AI_SUCCESS);
        const bool vertexColor = mesh->HasVertexColors(0) && !hasDiffuseTexture;
        int twoSided = 0;
        const bool doubleSided = mat != nullptr && mat->Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS && twoSided != 0;

        if (!MeshHasPolygons(mesh)) {
            if (!MeshHasPoints(mesh)) {
                ASSIMP_LOG_WARN("USD export: mesh '", mesh->mName.C_Str(), "' has only lines; not exported");
                continue;
            }
            lu::GeomPoints pts;
            ConvertPointsGeometry(mesh, pts);
            if (mat != nullptr) {
                pts.set_materialBinding(MakeRelationship(MaterialPath(mesh->mMaterialIndex, vertexColor)));
                AddApiSchema(pts.meta, lu::APISchemas::APIName::MaterialBindingAPI);
            }
            out.emplace_back(info.names.Claim(mesh->mName.C_Str(), "Points"), luv::Value(std::move(pts)));
            continue;
        }

        const std::string name = info.names.Claim(mesh->mName.C_Str(), "Mesh");
        const std::string path = info.path + "/" + name;
        lu::GeomMesh usd;
        const bool shared = mMeshUsers[m].size() > 1;
        auto cached = shared ? mMeshCache.find(m) : mMeshCache.end();
        if (cached != mMeshCache.end()) {
            if (--mMeshUsesLeft[m] == 0) {
                usd = std::move(cached->second);
                mMeshCache.erase(cached);
            } else {
                usd = cached->second;
            }
        } else {
            if (!ConvertMeshGeometry(mesh, usd)) {
                continue;
            }
            if (IsSkinned(m)) {
                AddSkinning(m, usd);
            }
            if (mat != nullptr) {
                usd.set_materialBinding(MakeRelationship(MaterialPath(mesh->mMaterialIndex, vertexColor)));
                AddApiSchema(usd.meta, lu::APISchemas::APIName::MaterialBindingAPI);
            }
            if (doubleSided) {
                usd.doubleSided.set_value(true);
            }
            if (shared) {
                mMeshUsesLeft[m] = static_cast<unsigned>(mMeshUsers[m].size()) - 1;
                mMeshCache.emplace(m, usd);
            }
        }

        std::vector<lu::Prim> meshChildren;
        bool morphSkeleton = false;
        lu::Prim skeletonPrim;
        if (mesh->mNumAnimMeshes > 0) {
            if (IsSkinned(m)) {
                AppendBlendShapes(mesh, path, mSkel.shapeTokens[m], usd, meshChildren);
            } else {
                // Blend shapes need a skeleton binding to be animated: a
                // one-joint skeleton next to the mesh carries the weights.
                const std::vector<std::string> tokens = BlendShapeNames(mesh);
                const std::string skelName = info.names.Claim(name + "_Skeleton", "Skeleton");
                const std::string skelPath = info.path + "/" + skelName;
                std::vector<float> defaults;
                std::vector<MorphChannel> channels;
                std::vector<unsigned> shapes;
                const MorphChannel channel = FindMorphChannel(index, mesh);
                for (unsigned s = 0; s < mesh->mNumAnimMeshes; ++s) {
                    defaults.push_back(mesh->mAnimMeshes[s] != nullptr ? mesh->mAnimMeshes[s]->mWeight : 0.0f);
                    channels.push_back(channel);
                    shapes.push_back(s);
                }
                lu::Skeleton skel;
                skel.joints.set_value(std::vector<luv::token>{ luv::token("root") });
                luv::matrix4d identity;
                lu::Identity(&identity);
                skel.bindTransforms.set_value(std::vector<luv::matrix4d>{ identity });
                skel.restTransforms.set_value(std::vector<luv::matrix4d>{ identity });
                skel.animationSource = MakeRelationship(skelPath + "/Animation");
                AddApiSchema(skel.meta, lu::APISchemas::APIName::SkelBindingAPI);
                skeletonPrim = lu::Prim(skelName, luv::Value(std::move(skel)));
                skeletonPrim.children().push_back(
                        BuildSkelAnimation("Animation", nullptr, tokens, defaults, channels, shapes));
                morphSkeleton = true;

                AddPrimvar(usd.props, "skel:jointIndices", std::vector<int32_t>{ 0 }, lu::Interpolation::Constant);
                AddPrimvar(usd.props, "skel:jointWeights", std::vector<float>{ 1.0f }, lu::Interpolation::Constant);
                usd.skeleton = MakeRelationship(skelPath);
                AddApiSchema(usd.meta, lu::APISchemas::APIName::SkelBindingAPI);
                AppendBlendShapes(mesh, path, tokens, usd, meshChildren);
            }
        }
        lu::Prim prim(name, luv::Value(std::move(usd)));
        for (lu::Prim &child : meshChildren) {
            prim.children().push_back(std::move(child));
        }
        out.push_back(std::move(prim));
        if (morphSkeleton) {
            out.push_back(std::move(skeletonPrim));
        }
    }
}

lu::Prim UsdExporter::BuildSkeletonPrim() {
    lu::Skeleton skel;
    skel.joints.set_value(Tokens(mSkel.tokens));
    std::vector<luv::matrix4d> bind;
    std::vector<luv::matrix4d> rest;
    bind.reserve(mSkel.joints.size());
    rest.reserve(mSkel.joints.size());
    for (size_t j = 0; j < mSkel.joints.size(); ++j) {
        bind.push_back(ToUsdMatrix(mSkel.bind[j]));
        rest.push_back(ToUsdMatrix(mNodes[mSkel.joints[j]].node->mTransformation));
    }
    skel.bindTransforms.set_value(std::move(bind));
    skel.restTransforms.set_value(std::move(rest));
    const bool animated = mSkel.HasAnimation();
    if (animated) {
        skel.animationSource = MakeRelationship(mSkel.animationPath);
        AddApiSchema(skel.meta, lu::APISchemas::APIName::SkelBindingAPI);
    }
    lu::Prim prim(LastPathElement(mSkel.path), luv::Value(std::move(skel)));
    if (animated) {
        prim.children().push_back(BuildSkelAnimation("Animation", &mSkel, mSkel.allTokens, mSkel.defaultWeights,
                mSkel.tokenChannel, mSkel.tokenShape));
    }
    return prim;
}

lu::Prim UsdExporter::BuildSkelAnimation(const std::string &name, const SkeletonPlan *skel,
        const std::vector<std::string> &tokens, const std::vector<float> &defaults,
        const std::vector<MorphChannel> &channels, const std::vector<unsigned> &shapeOfToken) {
    lu::SkelAnimation anim;
    bool jointAnimation = false;
    if (skel != nullptr) {
        for (const auto &c : skel->channels) {
            jointAnimation = jointAnimation || c.first != nullptr;
        }
    }
    if (jointAnimation) {
        // Samples at the union of the source key times (D25); joints without a
        // channel, or channels without some component, keep the rest pose (D23).
        const size_t nj = skel->joints.size();
        std::vector<double> times;
        for (const auto &c : skel->channels) {
            if (c.first == nullptr) {
                continue;
            }
            for (unsigned k = 0; k < c.first->mNumPositionKeys; ++k) times.push_back(c.first->mPositionKeys[k].mTime * c.second);
            for (unsigned k = 0; k < c.first->mNumRotationKeys; ++k) times.push_back(c.first->mRotationKeys[k].mTime * c.second);
            for (unsigned k = 0; k < c.first->mNumScalingKeys; ++k) times.push_back(c.first->mScalingKeys[k].mTime * c.second);
        }
        SortUnique(times);
        std::vector<aiVector3D> restT(nj), restS(nj);
        std::vector<aiQuaternion> restR(nj);
        for (size_t j = 0; j < nj; ++j) {
            mNodes[skel->joints[j]].node->mTransformation.Decompose(restS[j], restR[j], restT[j]);
        }
        lu::Animatable<std::vector<luv::float3>> translations;
        lu::Animatable<std::vector<luv::quatf>> rotations;
        lu::Animatable<std::vector<luv::half3>> scales;
        std::vector<luv::float3> tv(nj);
        std::vector<luv::quatf> rv(nj);
        std::vector<luv::half3> sv(nj);
        for (double t : times) {
            for (size_t j = 0; j < nj; ++j) {
                const aiNodeAnim *ch = skel->channels[j].first;
                const double ticks = ch != nullptr ? t / skel->channels[j].second : 0.0;
                const aiVector3D p = (ch && ch->mNumPositionKeys) ? SampleVector(ch->mPositionKeys, ch->mNumPositionKeys, ticks) : restT[j];
                const aiQuaternion q = (ch && ch->mNumRotationKeys) ? SampleQuat(ch->mRotationKeys, ch->mNumRotationKeys, ticks) : restR[j];
                const aiVector3D s = (ch && ch->mNumScalingKeys) ? SampleVector(ch->mScalingKeys, ch->mNumScalingKeys, ticks) : restS[j];
                tv[j] = { static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) };
                rv[j] = ToQuatf(q);
                sv[j] = ToHalf3(s);
            }
            translations.add_sample(t, tv);
            rotations.add_sample(t, rv);
            scales.add_sample(t, sv);
        }
        anim.joints.set_value(Tokens(skel->tokens));
        anim.translations.set_value(std::move(translations));
        anim.rotations.set_value(std::move(rotations));
        anim.scales.set_value(std::move(scales));
    }
    if (!tokens.empty()) {
        anim.blendShapes.set_value(Tokens(tokens));
        std::vector<double> times;
        std::set<const aiMeshMorphAnim *> seen;
        for (const MorphChannel &c : channels) {
            if (c.first == nullptr || !seen.insert(c.first).second) {
                continue;
            }
            for (unsigned k = 0; k < c.first->mNumKeys; ++k) {
                times.push_back(c.first->mKeys[k].mTime * c.second);
            }
        }
        SortUnique(times);
        lu::Animatable<std::vector<float>> weights;
        if (times.empty()) {
            weights.set(defaults);
        } else {
            std::vector<float> w(tokens.size());
            for (double t : times) {
                for (size_t i = 0; i < tokens.size(); ++i) {
                    const MorphChannel &c = channels[i];
                    w[i] = c.first != nullptr ? SampleMorphWeight(c.first, shapeOfToken[i], t / c.second) : defaults[i];
                }
                weights.add_sample(t, w);
            }
        }
        anim.blendShapeWeights.set_value(std::move(weights));
    }
    return lu::Prim(name, luv::Value(std::move(anim)));
}

// ------------------------------------------------------------------------------------------------
// Cameras and lights

lu::Prim UsdExporter::BuildCameraPrim(unsigned cameraIndex, const std::string &name, int nodeIndex, bool typedNode) {
    const aiCamera *cam = mScene->mCameras[cameraIndex];
    lu::GeomCamera usd;
    const aiMatrix4x4 frame = FrameFromDirection(cam->mPosition, cam->mLookAt, cam->mUp);
    if (typedNode) {
        SetupXformable(usd, nodeIndex, IsIdentity(frame) ? nullptr : &frame);
        AddPhysicsApis(usd, nodeIndex);
    } else {
        AddMatrixOp(usd, frame);
    }
    const float aspect = cam->mAspect > 0.0f ? cam->mAspect : 1.0f;
    float hAperture = 20.955f; // mm, 35mm academy; any value works with the matching focal length
    if (cam->mOrthographicWidth > 0.0f) {
        // orthographic apertures are in tenths of a scene unit; mOrthographicWidth is half the width
        usd.projection.set_value(lu::Animatable<lu::GeomCamera::Projection>(lu::GeomCamera::Projection::Orthographic));
        hAperture = cam->mOrthographicWidth * 2.0f * 10.0f;
    } else {
        // mHorizontalFOV is the full horizontal angle (as the glTF importer fills it)
        const float fov = cam->mHorizontalFOV > 1e-4f && cam->mHorizontalFOV < 3.14f ? cam->mHorizontalFOV : 0.25f * static_cast<float>(kPi);
        const float focal = 0.5f * hAperture / std::tan(0.5f * fov);
        usd.focalLength.set_value(lu::Animatable<float>(focal));
    }
    usd.horizontalAperture.set_value(lu::Animatable<float>(hAperture));
    usd.verticalAperture.set_value(lu::Animatable<float>(hAperture / aspect));
    if (cam->mClipPlaneNear > 0.0f && cam->mClipPlaneFar > cam->mClipPlaneNear) {
        usd.clippingRange.set_value(lu::Animatable<luv::float2>(luv::float2{ cam->mClipPlaneNear, cam->mClipPlaneFar }));
    }
    return lu::Prim(name, luv::Value(std::move(usd)));
}

template <class L>
lu::Prim FinishLight(L &usd, const aiLight *light, const std::string &name) {
    float intensity = std::max(light->mColorDiffuse.r, std::max(light->mColorDiffuse.g, light->mColorDiffuse.b));
    luv::color3f color{ 1.0f, 1.0f, 1.0f };
    if (intensity > 0.0f) {
        color = { light->mColorDiffuse.r / intensity, light->mColorDiffuse.g / intensity, light->mColorDiffuse.b / intensity };
    } else {
        intensity = 0.0f;
    }
    usd.color.set_value(lu::Animatable<luv::color3f>(color));
    usd.intensity.set_value(lu::Animatable<float>(intensity));
    return lu::Prim(name, luv::Value(std::move(usd)));
}

lu::Prim UsdExporter::BuildLightPrim(unsigned lightIndex, const std::string &name, int nodeIndex, bool typedNode) {
    const aiLight *light = mScene->mLights[lightIndex];
    const bool directional = light->mType == aiLightSource_DIRECTIONAL;
    const bool oriented = light->mType != aiLightSource_POINT;
    // USD lights emit along their local -Z axis.
    const aiMatrix4x4 frame = FrameFromDirection(directional ? aiVector3D() : light->mPosition,
            oriented ? light->mDirection : aiVector3D(0, 0, -1), light->mUp);
    auto place = [&](auto &usd) {
        if (typedNode) {
            SetupXformable(usd, nodeIndex, IsIdentity(frame) ? nullptr : &frame);
            AddPhysicsApis(usd, nodeIndex);
        } else {
            AddMatrixOp(usd, frame);
        }
    };
    switch (light->mType) {
    case aiLightSource_DIRECTIONAL: {
        lu::DistantLight usd;
        place(usd);
        return FinishLight(usd, light, name);
    }
    case aiLightSource_AREA: {
        lu::RectLight usd;
        place(usd);
        usd.width.set_value(lu::Animatable<float>(light->mSize.x > 0 ? light->mSize.x : 1.0f));
        usd.height.set_value(lu::Animatable<float>(light->mSize.y > 0 ? light->mSize.y : 1.0f));
        return FinishLight(usd, light, name);
    }
    case aiLightSource_SPOT: {
        lu::SphereLight usd;
        place(usd);
        usd.radius.set_value(lu::Animatable<float>(0.0f));
        // shaping:cone:angle is the half angle, like mAngleOuterCone
        const float outer = light->mAngleOuterCone > 0.0f ? light->mAngleOuterCone : static_cast<float>(kPi) / 4.0f;
        const float inner = std::min(std::max(light->mAngleInnerCone, 0.0f), outer);
        usd.shapingConeAngle.set_value(lu::Animatable<float>(static_cast<float>(outer * kRadToDeg)));
        usd.shapingConeSoftness.set_value(lu::Animatable<float>(outer > 0.0f ? (outer - inner) / outer : 0.0f));
        AddApiSchema(usd.meta, lu::APISchemas::APIName::ShapingAPI);
        return FinishLight(usd, light, name);
    }
    case aiLightSource_POINT:
    default: {
        lu::SphereLight usd;
        place(usd);
        usd.radius.set_value(lu::Animatable<float>(0.0f));
        return FinishLight(usd, light, name);
    }
    }
}

// ------------------------------------------------------------------------------------------------
// UsdPhysics (node metadata per the importer/exporter contract)

template <class T>
void UsdExporter::AddPhysicsApis(T &prim, int nodeIndex) {
    const aiMetadata *md = mNodes[nodeIndex].node->mMetaData;
    if (md == nullptr) {
        return;
    }
    using API = lu::APISchemas::APIName;
    auto &props = prim.props;
    bool b = false;
    float f = 0.0f;
    aiVector3D v;
    std::string s;
    auto vec3f = [](const aiVector3D &x) { return luv::vector3f{ static_cast<float>(x.x), static_cast<float>(x.y), static_cast<float>(x.z) }; };
    if (MetaBool(md, "usdPhysics:rigidBodyEnabled", b)) {
        AddApiSchema(prim.meta, API::PhysicsRigidBodyAPI);
        AddAttribute(props, "physics:rigidBodyEnabled", b);
    }
    if (MetaBool(md, "usdPhysics:kinematicEnabled", b)) {
        AddApiSchema(prim.meta, API::PhysicsRigidBodyAPI);
        AddAttribute(props, "physics:kinematicEnabled", b);
    }
    if (MetaVector(md, "usdPhysics:velocity", v)) {
        AddApiSchema(prim.meta, API::PhysicsRigidBodyAPI);
        AddAttribute(props, "physics:velocity", vec3f(v));
    }
    if (MetaVector(md, "usdPhysics:angularVelocity", v)) {
        AddApiSchema(prim.meta, API::PhysicsRigidBodyAPI);
        AddAttribute(props, "physics:angularVelocity", vec3f(v));
    }
    if (MetaBool(md, "usdPhysics:collisionEnabled", b)) {
        AddApiSchema(prim.meta, API::PhysicsCollisionAPI);
        AddAttribute(props, "physics:collisionEnabled", b);
    }
    if (MetaString(md, "usdPhysics:approximation", s)) {
        AddApiSchema(prim.meta, API::PhysicsMeshCollisionAPI);
        AddAttribute(props, "physics:approximation", luv::token(s), /*uniform*/ true);
    }
    if (MetaFloat(md, "usdPhysics:mass", f)) {
        AddApiSchema(prim.meta, API::PhysicsMassAPI);
        AddAttribute(props, "physics:mass", f);
    }
    if (MetaFloat(md, "usdPhysics:density", f)) {
        AddApiSchema(prim.meta, API::PhysicsMassAPI);
        AddAttribute(props, "physics:density", f);
    }
    if (MetaVector(md, "usdPhysics:centerOfMass", v)) {
        AddApiSchema(prim.meta, API::PhysicsMassAPI);
        AddAttribute(props, "physics:centerOfMass", luv::point3f{ static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z) });
    }
    if (MetaVector(md, "usdPhysics:diagonalInertia", v)) {
        AddApiSchema(prim.meta, API::PhysicsMassAPI);
        AddAttribute(props, "physics:diagonalInertia", luv::float3{ static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z) });
    }
    const std::string physicsMaterial = PhysicsMaterialPath(nodeIndex);
    if (!physicsMaterial.empty()) {
        AddApiSchema(prim.meta, API::MaterialBindingAPI);
        AddRelationship(props, "material:binding:physics", { physicsMaterial });
    }
}

std::string UsdExporter::PhysicsMaterialPath(int nodeIndex) {
    auto it = mPhysicsMaterialPaths.find(nodeIndex);
    if (it != mPhysicsMaterialPaths.end()) {
        return it->second;
    }
    const aiMetadata *md = mNodes[nodeIndex].node->mMetaData;
    float staticFriction = 0, dynamicFriction = 0, restitution = 0;
    const bool hasStatic = MetaFloat(md, "usdPhysics:staticFriction", staticFriction);
    const bool hasDynamic = MetaFloat(md, "usdPhysics:dynamicFriction", dynamicFriction);
    const bool hasRestitution = MetaFloat(md, "usdPhysics:restitution", restitution);
    std::string path;
    if (hasStatic || hasDynamic || hasRestitution) {
        lu::Material mat;
        AddApiSchema(mat.meta, lu::APISchemas::APIName::PhysicsMaterialAPI);
        if (hasStatic) AddAttribute(mat.props, "physics:staticFriction", staticFriction);
        if (hasDynamic) AddAttribute(mat.props, "physics:dynamicFriction", dynamicFriction);
        if (hasRestitution) AddAttribute(mat.props, "physics:restitution", restitution);
        const std::string name = mMaterialNames.Claim(mNodes[nodeIndex].name + "_PhysicsMaterial", "PhysicsMaterial");
        path = mMaterialsPath + "/" + name;
        mMaterialPrims.emplace_back(name, luv::Value(std::move(mat)));
    }
    mPhysicsMaterialPaths[nodeIndex] = path;
    return path;
}

bool ParseQuat(const std::string &s, luv::quatf &q) {
    // "w x y z"
    float w = 1, x = 0, y = 0, z = 0;
    const char *p = s.c_str();
    char *end = nullptr;
    float *dst[4] = { &w, &x, &y, &z };
    for (float *d : dst) {
        *d = std::strtof(p, &end);
        if (end == p) {
            return false;
        }
        p = end;
    }
    q.real = w;
    q.imag = { x, y, z };
    return true;
}

lu::Prim UsdExporter::BuildPhysicsJointPrim(int nodeIndex, const std::string &name) {
    const NodeInfo &info = mNodes[nodeIndex];
    const aiMetadata *md = info.node->mMetaData;
    std::string type = "joint";
    MetaString(md, "usdPhysics:jointType", type);
    if (!IsIdentity(info.local) && info.kind == NodeKind::PhysicsJoint) {
        ASSIMP_LOG_WARN("USD export: physics joint '", info.node->mName.C_Str(),
                "' has a node transform; joints are not transformable and use localPos/localRot instead");
    }
    auto fillBase = [&](lu::PhysicsJointBase &j) {
        std::string body;
        for (int i = 0; i < 2; ++i) {
            if (!MetaString(md, i == 0 ? "usdPhysics:body0" : "usdPhysics:body1", body)) {
                continue;
            }
            const int target = FindNode(body);
            if (target < 0 || mNodes[target].joint) {
                ASSIMP_LOG_WARN("USD export: physics joint '", info.node->mName.C_Str(), "' targets unknown body '", body, "'");
                continue;
            }
            (i == 0 ? j.body0 : j.body1) = lu::RelationshipProperty(lu::Path(mNodes[target].path, ""));
        }
        aiVector3D v;
        if (MetaVector(md, "usdPhysics:localPos0", v)) j.localPos0.set_value(luv::point3f{ static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z) });
        if (MetaVector(md, "usdPhysics:localPos1", v)) j.localPos1.set_value(luv::point3f{ static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z) });
        std::string rot;
        luv::quatf q;
        if (MetaString(md, "usdPhysics:localRot0", rot) && ParseQuat(rot, q)) j.localRot0.set_value(q);
        if (MetaString(md, "usdPhysics:localRot1", rot) && ParseQuat(rot, q)) j.localRot1.set_value(q);
    };
    std::string axis;
    const bool hasAxis = MetaString(md, "usdPhysics:axis", axis) && (axis == "X" || axis == "Y" || axis == "Z");
    float lower = 0, upper = 0;
    const bool hasLower = MetaFloat(md, "usdPhysics:lowerLimit", lower);
    const bool hasUpper = MetaFloat(md, "usdPhysics:upperLimit", upper);
    if (type == "revolute") {
        lu::PhysicsRevoluteJoint j;
        fillBase(j);
        if (hasAxis) j.axis.set_value(luv::token(axis));
        if (hasLower) j.lowerLimit.set_value(lower);
        if (hasUpper) j.upperLimit.set_value(upper);
        return lu::Prim(name, luv::Value(std::move(j)));
    }
    if (type == "prismatic") {
        lu::PhysicsPrismaticJoint j;
        fillBase(j);
        if (hasAxis) j.axis.set_value(luv::token(axis));
        if (hasLower) j.lowerLimit.set_value(lower);
        if (hasUpper) j.upperLimit.set_value(upper);
        return lu::Prim(name, luv::Value(std::move(j)));
    }
    if (type == "spherical") {
        lu::PhysicsSphericalJoint j;
        fillBase(j);
        if (hasAxis) j.axis.set_value(luv::token(axis));
        return lu::Prim(name, luv::Value(std::move(j)));
    }
    if (type == "fixed") {
        lu::PhysicsFixedJoint j;
        fillBase(j);
        return lu::Prim(name, luv::Value(std::move(j)));
    }
    if (type == "distance") {
        lu::PhysicsDistanceJoint j;
        fillBase(j);
        if (hasLower) j.minDistance.set_value(lower);
        if (hasUpper) j.maxDistance.set_value(upper);
        return lu::Prim(name, luv::Value(std::move(j)));
    }
    lu::PhysicsJoint j;
    fillBase(j);
    return lu::Prim(name, luv::Value(std::move(j)));
}

lu::Prim UsdExporter::BuildPhysicsScene(const std::string &name) {
    lu::PhysicsScene scene;
    aiVector3D dir;
    float magnitude = 0;
    if (MetaVector(mScene->mMetaData, "usdPhysics:gravityDirection", dir)) {
        scene.gravityDirection.set_value(luv::vector3f{ static_cast<float>(dir.x), static_cast<float>(dir.y), static_cast<float>(dir.z) });
    }
    if (MetaFloat(mScene->mMetaData, "usdPhysics:gravityMagnitude", magnitude)) {
        scene.gravityMagnitude.set_value(magnitude);
    }
    return lu::Prim(name, luv::Value(std::move(scene)));
}

// ------------------------------------------------------------------------------------------------
// Materials: UsdPreviewSurface, UsdUVTexture, UsdPrimvarReader_float2, UsdTransform2d only (C6)

struct TexNode {
    std::string key;
    std::string name;
    lu::UsdUVTexture tex;
    std::array<float, 4> scale{ { 1, 1, 1, 1 } };
    std::array<float, 4> bias{ { 0, 0, 0, 0 } };
    std::array<bool, 4> used{ { false, false, false, false } };
    std::set<std::string> outputs;
};

struct ShaderNet {
    std::string path;
    NameRegistry names;
    std::vector<lu::Prim> prims; // readers and transforms
    std::map<int, std::string> readers;
    std::vector<TexNode> textures;
};

std::vector<int> ChannelIndices(const std::string &channels) {
    if (channels == "rgb") return { 0, 1, 2 };
    if (channels == "r") return { 0 };
    if (channels == "g") return { 1 };
    if (channels == "b") return { 2 };
    return { 3 };
}

template <class A>
void ConnectInput(A &attr, const std::string &shaderPath, const std::string &output) {
    attr.set_connection(lu::Path(shaderPath, "outputs:" + output));
    attr.set_value_empty();
}

std::string PrimvarReader(ShaderNet &net, int uv) {
    auto it = net.readers.find(uv);
    if (it != net.readers.end()) {
        return it->second;
    }
    const std::string primvar = uv == 0 ? std::string("st") : "st" + std::to_string(uv);
    const std::string name = net.names.Claim(uv == 0 ? std::string("stReader") : "stReader" + std::to_string(uv), "stReader");
    lu::UsdPrimvarReader_float2 reader;
    reader.varname.set_value(lu::Animatable<std::string>(primvar));
    reader.result.set_authored(true);
    lu::Shader shader;
    shader.name = name;
    shader.info_id = "UsdPrimvarReader_float2";
    shader.value = std::move(reader);
    net.prims.emplace_back(name, luv::Value(std::move(shader)));
    const std::string path = net.path + "/" + name;
    net.readers[uv] = path;
    return path;
}

/// UsdTransform2d parameters (result = R * S * in + T) for an aiUVTransform.
void UvTransformToUsd(const aiUVTransform &xf, bool gltf, luv::float2 &scale, float &rotationDeg, luv::float2 &translation) {
    const float sx = xf.mScaling.x;
    const float sy = xf.mScaling.y;
    scale = { sx, sy };
    if (gltf) {
        // Invert the glTF importer's conversion (glTF2Importer: KHR_texture_transform)
        // back to offset/rotation/scale, then express it on bottom-left st.
        const float theta = -xf.mRotation;
        const float c = std::cos(theta);
        const float s = std::sin(theta);
        const float ox = xf.mTranslation.x - 0.5f * sx * (-c + s + 1.0f);
        const float oy = 1.0f - sy - xf.mTranslation.y + 0.5f * sy * (s + c - 1.0f);
        rotationDeg = static_cast<float>(theta * kRadToDeg);
        translation = { s * sy + ox, 1.0f - c * sy - oy };
        return;
    }
    // assimp convention: uv' = S * R * (uv + t - 0.5) + 0.5
    const float r = xf.mRotation;
    const float dx = xf.mTranslation.x - 0.5f;
    const float dy = xf.mTranslation.y - 0.5f;
    const float rx = std::cos(r) * dx - std::sin(r) * dy;
    const float ry = std::sin(r) * dx + std::cos(r) * dy;
    rotationDeg = static_cast<float>(r * kRadToDeg);
    translation = { sx * rx + 0.5f, sy * ry + 0.5f };
    if (r != 0.0f && sx != sy) {
        ASSIMP_LOG_WARN("USD export: non-uniform UV scale with rotation is approximated (UsdTransform2d scales before rotating)");
    }
}

std::string UsdExporter::MaterialPath(unsigned materialIndex, bool vertexColor) {
    const auto key = std::make_pair(materialIndex, vertexColor);
    auto it = mMaterialPaths.find(key);
    if (it != mMaterialPaths.end()) {
        return it->second;
    }
    aiString matName;
    mScene->mMaterials[materialIndex]->Get(AI_MATKEY_NAME, matName);
    std::string base = matName.length > 0 ? std::string(matName.C_Str()) : "Material";
    if (vertexColor) {
        base += "_vertexColor";
    }
    const std::string name = mMaterialNames.Claim(base, "Material");
    const std::string path = mMaterialsPath + "/" + name;
    mMaterialPaths[key] = path;
    mMaterialPrims.push_back(BuildMaterial(materialIndex, vertexColor, name, path));
    return path;
}

lu::Prim UsdExporter::BuildMaterial(unsigned materialIndex, bool vertexColor, const std::string &name,
        const std::string &path) {
    const aiMaterial *mat = mScene->mMaterials[materialIndex];
    ShaderNet net;
    net.path = path;
    const std::string surfaceName = net.names.Claim("PreviewSurface", "PreviewSurface");
    const std::string surfacePath = path + "/" + surfaceName;
    lu::UsdPreviewSurface surface;

    // ---- textures
    auto useTexture = [&](std::initializer_list<aiTextureType> types, unsigned slot, const std::string &baseName,
                              bool srgb, const std::string &channels, std::array<float, 4> scale,
                              std::array<float, 4> bias, aiString *usedPath = nullptr) -> std::string {
        aiString texPath;
        aiTextureType type = aiTextureType_NONE;
        for (aiTextureType t : types) {
            if (mat->GetTexture(t, slot, &texPath) == AI_SUCCESS && texPath.length > 0) {
                type = t;
                break;
            }
        }
        if (type == aiTextureType_NONE) {
            return std::string();
        }
        if (usedPath != nullptr) {
            *usedPath = texPath;
        }
        const int tex = RequestTexture(texPath.C_Str());
        if (tex < 0) {
            return std::string();
        }
        int uv = 0;
        mat->Get(AI_MATKEY_UVWSRC(type, slot), uv);
        uv = std::max(0, std::min(uv, static_cast<int>(AI_MAX_NUMBER_OF_TEXTURECOORDS) - 1));
        aiUVTransform xf;
        const bool hasXf = mat->Get(AI_MATKEY_UVTRANSFORM(type, slot), xf) == AI_SUCCESS &&
                           (xf.mScaling.x != 1.0f || xf.mScaling.y != 1.0f || xf.mTranslation.x != 0.0f ||
                            xf.mTranslation.y != 0.0f || xf.mRotation != 0.0f);
        int wrapU = aiTextureMapMode_Wrap;
        int wrapV = aiTextureMapMode_Wrap;
        mat->Get(AI_MATKEY_MAPPINGMODE_U(type, slot), wrapU);
        mat->Get(AI_MATKEY_MAPPINGMODE_V(type, slot), wrapV);
        std::string key = std::to_string(tex) + "|" + std::to_string(uv) + "|" + std::to_string(wrapU) + "|" +
                          std::to_string(wrapV) + "|" + (srgb ? "s" : "r");
        if (hasXf) {
            key += "|" + std::to_string(xf.mScaling.x) + "," + std::to_string(xf.mScaling.y) + "," +
                   std::to_string(xf.mTranslation.x) + "," + std::to_string(xf.mTranslation.y) + "," +
                   std::to_string(xf.mRotation);
        }
        const std::vector<int> chans = ChannelIndices(channels);
        TexNode *node = nullptr;
        for (TexNode &candidate : net.textures) {
            if (candidate.key != key) {
                continue;
            }
            bool compatible = true;
            for (int c : chans) {
                compatible = compatible && (!candidate.used[c] || (candidate.scale[c] == scale[c] && candidate.bias[c] == bias[c]));
            }
            if (compatible) {
                node = &candidate;
                break;
            }
        }
        if (node == nullptr) {
            net.textures.emplace_back();
            node = &net.textures.back();
            node->key = key;
            node->name = net.names.Claim(baseName, "texture");
            std::string stSource = PrimvarReader(net, uv);
            if (hasXf) {
                lu::UsdTransform2d t2d;
                luv::float2 s2{}, t2{};
                float rot = 0;
                UvTransformToUsd(xf, mGltfSource, s2, rot, t2);
                t2d.in.set_connection(lu::Path(stSource, "outputs:result"));
                t2d.in.set_value_empty();
                t2d.scale.set_value(lu::Animatable<luv::float2>(s2));
                t2d.rotation.set_value(lu::Animatable<float>(rot));
                t2d.translation.set_value(lu::Animatable<luv::float2>(t2));
                t2d.result.set_authored(true);
                const std::string tname = net.names.Claim(baseName + "_transform", "transform");
                lu::Shader shader;
                shader.name = tname;
                shader.info_id = "UsdTransform2d";
                shader.value = std::move(t2d);
                net.prims.emplace_back(tname, luv::Value(std::move(shader)));
                stSource = path + "/" + tname;
            }
            auto wrap = [](int mode) {
                switch (mode) {
                case aiTextureMapMode_Clamp: return lu::UsdUVTexture::Wrap::Clamp;
                case aiTextureMapMode_Mirror: return lu::UsdUVTexture::Wrap::Mirror;
                case aiTextureMapMode_Decal: return lu::UsdUVTexture::Wrap::Black;
                default: return lu::UsdUVTexture::Wrap::Repeat;
                }
            };
            lu::UsdUVTexture &t = node->tex;
            t.file.set_value(lu::Animatable<luv::AssetPath>(luv::AssetPath(mTextures[tex].assetPath)));
            t.st.set_connection(lu::Path(stSource, "outputs:result"));
            t.st.set_value_empty();
            t.wrapS.set_value(lu::Animatable<lu::UsdUVTexture::Wrap>(wrap(wrapU)));
            t.wrapT.set_value(lu::Animatable<lu::UsdUVTexture::Wrap>(wrap(wrapV)));
            t.sourceColorSpace.set_value(lu::Animatable<lu::UsdUVTexture::SourceColorSpace>(
                    srgb ? lu::UsdUVTexture::SourceColorSpace::SRGB : lu::UsdUVTexture::SourceColorSpace::Raw));
        }
        for (int c : chans) {
            node->used[c] = true;
            node->scale[c] = scale[c];
            node->bias[c] = bias[c];
        }
        node->outputs.insert(channels);
        return path + "/" + node->name;
    };
    const std::array<float, 4> one{ { 1, 1, 1, 1 } };
    const std::array<float, 4> zero{ { 0, 0, 0, 0 } };

    // ---- factors (ported from the original mapping)
    int shadingModel = 0;
    const bool unlit = mat->Get(AI_MATKEY_SHADING_MODEL, shadingModel) == AI_SUCCESS && shadingModel == aiShadingMode_Unlit;
    aiColor3D baseColor(0.8f, 0.8f, 0.8f);
    const bool hasBase = mat->Get(AI_MATKEY_BASE_COLOR, baseColor) == AI_SUCCESS ||
                         mat->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor) == AI_SUCCESS;
    if (hasBase) {
        surface.diffuseColor.set_value(lu::Animatable<luv::color3f>(luv::color3f{ baseColor.r, baseColor.g, baseColor.b }));
    }
    float specularFactor = 1.0f, glossiness = 1.0f;
    aiColor3D specularColor(1.0f, 1.0f, 1.0f);
    const bool hasSpecularFactor = mat->Get(AI_MATKEY_SPECULAR_FACTOR, specularFactor) == AI_SUCCESS;
    const bool specGloss = mat->Get(AI_MATKEY_GLOSSINESS_FACTOR, glossiness) == AI_SUCCESS;
    mat->Get(AI_MATKEY_COLOR_SPECULAR, specularColor); // stays white when absent
    auto clamp01 = [](float v) { return std::min(1.0f, std::max(0.0f, v)); };
    if (specGloss) {
        // KHR_materials_pbrSpecularGlossiness: USD specular workflow
        surface.useSpecularWorkflow.set_value(lu::Animatable<int>(1));
        const float k = hasSpecularFactor ? specularFactor : 1.0f;
        surface.specularColor.set_value(lu::Animatable<luv::color3f>(luv::color3f{
                clamp01(specularColor.r * k), clamp01(specularColor.g * k), clamp01(specularColor.b * k) }));
        surface.roughness.set_value(lu::Animatable<float>(clamp01(1.0f - glossiness)));
    } else if (hasSpecularFactor && specularFactor != 1.0f) {
        // KHR_materials_specular stays metallic: F0 = 0.04 * factor -> ior
        const float sqrtF0 = std::sqrt(0.04f * std::max(0.0f, specularFactor));
        if (sqrtF0 < 1.0f) {
            surface.ior.set_value(lu::Animatable<float>((1.0f + sqrtF0) / (1.0f - sqrtF0)));
        }
    }
    float metallic = 0.0f;
    const bool hasMetallic = mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS;
    if (hasMetallic) {
        surface.metallic.set_value(lu::Animatable<float>(metallic));
    }
    float roughness = 1.0f;
    const bool hasRoughness = !specGloss && mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS;
    if (hasRoughness) {
        surface.roughness.set_value(lu::Animatable<float>(roughness));
    } else if (!specGloss) {
        float shininess = 0.0f;
        if (mat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess >= 0.0f) {
            surface.roughness.set_value(lu::Animatable<float>(std::sqrt(2.0f / (shininess + 2.0f))));
        }
    }
    aiColor3D emissive(0.0f, 0.0f, 0.0f);
    float emissiveIntensity = 1.0f;
    const bool hasEmissive = mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS;
    if (mat->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissiveIntensity) == AI_SUCCESS) {
        emissive = emissive * emissiveIntensity;
    }
    if (hasEmissive) {
        surface.emissiveColor.set_value(lu::Animatable<luv::color3f>(luv::color3f{ emissive.r, emissive.g, emissive.b }));
    }
    float opacity = 1.0f;
    if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 1.0f) {
        surface.opacity.set_value(lu::Animatable<float>(clamp01(opacity)));
    }
    float transmission = 0.0f;
    const bool hasTransmission = mat->Get(AI_MATKEY_TRANSMISSION_FACTOR, transmission) == AI_SUCCESS && transmission > 0.0f;
    if (hasTransmission) {
        // no refraction in UsdPreviewSurface: approximate with opacity (kept visible)
        const float transmissionOpacity = std::max(1.0f - transmission, 0.1f);
        if (transmissionOpacity < opacity) {
            opacity = transmissionOpacity;
            surface.opacity.set_value(lu::Animatable<float>(opacity));
        }
        aiColor3D attenuation(1.0f, 1.0f, 1.0f);
        if (mat->Get(AI_MATKEY_VOLUME_ATTENUATION_COLOR, attenuation) == AI_SUCCESS &&
                (attenuation.r < 0.99f || attenuation.g < 0.99f || attenuation.b < 0.99f)) {
            baseColor.r = baseColor.r * (1.0f - transmission) + attenuation.r * transmission;
            baseColor.g = baseColor.g * (1.0f - transmission) + attenuation.g * transmission;
            baseColor.b = baseColor.b * (1.0f - transmission) + attenuation.b * transmission;
            surface.diffuseColor.set_value(lu::Animatable<luv::color3f>(luv::color3f{ baseColor.r, baseColor.g, baseColor.b }));
        }
    }
    float dtFactor = 0.0f;
    if (mat->Get(AI_MATKEY_DIFFUSE_TRANSMISSION_FACTOR, dtFactor) == AI_SUCCESS && dtFactor > 0.0f) {
        aiColor3D dtColor(1.0f, 1.0f, 1.0f);
        mat->Get(AI_MATKEY_DIFFUSE_TRANSMISSION_COLOR_FACTOR, dtColor);
        baseColor.r = baseColor.r * (1.0f - dtFactor) + dtColor.r * dtFactor;
        baseColor.g = baseColor.g * (1.0f - dtFactor) + dtColor.g * dtFactor;
        baseColor.b = baseColor.b * (1.0f - dtFactor) + dtColor.b * dtFactor;
        surface.diffuseColor.set_value(lu::Animatable<luv::color3f>(luv::color3f{ baseColor.r, baseColor.g, baseColor.b }));
    }
    aiString alphaMode;
    if (mat->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS) {
        const std::string mode(alphaMode.C_Str());
        if (mode == "MASK") {
            float cutoff = 0.5f;
            mat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff);
            surface.opacityThreshold.set_value(lu::Animatable<float>(cutoff)); // D34
        }
    }
    float ior = 1.5f;
    if (mat->Get(AI_MATKEY_REFRACTI, ior) == AI_SUCCESS && ior >= 1.0f) {
        surface.ior.set_value(lu::Animatable<float>(ior));
    }
    if (mOpts.clearcoat) {
        float clearcoat = 0.0f, clearcoatRoughness = 0.0f;
        if (mat->Get(AI_MATKEY_CLEARCOAT_FACTOR, clearcoat) == AI_SUCCESS) {
            surface.clearcoat.set_value(lu::Animatable<float>(clearcoat));
        }
        if (mat->Get(AI_MATKEY_CLEARCOAT_ROUGHNESS_FACTOR, clearcoatRoughness) == AI_SUCCESS) {
            surface.clearcoatRoughness.set_value(lu::Animatable<float>(clearcoatRoughness));
        }
    }

    // ---- textures
    std::string diffuseTex;
    aiString diffusePath;
    const std::array<float, 4> baseScale{ { baseColor.r, baseColor.g, baseColor.b, 1.0f } };
    if (!unlit) {
        diffuseTex = useTexture({ aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE, aiTextureType_MAYA_BASE }, 0,
                "diffuseColor_texture", true, "rgb", hasBase ? baseScale : one, zero, &diffusePath);
        if (!diffuseTex.empty()) {
            ConnectInput(surface.diffuseColor, diffuseTex, "rgb");
        }
    }
    if (vertexColor && diffuseTex.empty()) {
        lu::UsdPrimvarReader_float3 reader;
        reader.varname.set_value(lu::Animatable<std::string>(std::string("displayColor")));
        reader.result.set_authored(true);
        const std::string rname = net.names.Claim("displayColorReader", "displayColorReader");
        lu::Shader shader;
        shader.name = rname;
        shader.info_id = "UsdPrimvarReader_float3";
        shader.value = std::move(reader);
        net.prims.emplace_back(rname, luv::Value(std::move(shader)));
        ConnectInput(unlit ? surface.emissiveColor : surface.diffuseColor, path + "/" + rname, "result");
    }
    std::string normalTex = useTexture({ aiTextureType_NORMALS, aiTextureType_HEIGHT }, 0, "normal_texture", false, "rgb",
            { { 2, 2, 2, 1 } }, { { -1, -1, -1, 0 } });
    if (normalTex.empty() && mOpts.clearcoat) {
        normalTex = useTexture({ aiTextureType_CLEARCOAT }, 2, "normal_texture", false, "rgb", { { 2, 2, 2, 1 } }, { { -1, -1, -1, 0 } });
    }
    if (!normalTex.empty()) {
        ConnectInput(surface.normal, normalTex, "rgb");
    }
    if (!specGloss && !(hasMetallic && metallic <= 0.0f)) {
        const float m = hasMetallic ? metallic : 1.0f;
        const std::string tex = useTexture({ aiTextureType_METALNESS, aiTextureType_GLTF_METALLIC_ROUGHNESS }, 0,
                "metallic_texture", false, "b", { { 1, 1, m, 1 } }, zero);
        if (!tex.empty()) {
            ConnectInput(surface.metallic, tex, "b");
        }
    }
    if (specGloss) {
        const std::string tex = useTexture({ aiTextureType_SPECULAR }, 0, "specularColor_texture", true, "rgb", one, zero);
        if (!tex.empty()) {
            ConnectInput(surface.specularColor, tex, "rgb");
        }
    } else {
        aiString roughPath;
        std::string tex = useTexture({ aiTextureType_DIFFUSE_ROUGHNESS, aiTextureType_GLTF_METALLIC_ROUGHNESS,
                                             aiTextureType_MAYA_SPECULAR_ROUGHNESS },
                0, "roughness_texture", false, "g", { { 1, hasRoughness ? roughness : 1.0f, 1, 1 } }, zero, &roughPath);
        if (tex.empty()) {
            // glossiness map: roughness = 1 - texel
            tex = useTexture({ aiTextureType_SHININESS }, 0, "roughness_texture", false, "g", { { 1, -1, 1, 1 } },
                    { { 0, 1, 0, 0 } });
        }
        if (!tex.empty()) {
            ConnectInput(surface.roughness, tex, "g");
        }
    }
    {
        // occlusion (D34)
        const std::string tex = useTexture({ aiTextureType_AMBIENT_OCCLUSION, aiTextureType_LIGHTMAP }, 0,
                "occlusion_texture", false, "r", one, zero);
        if (!tex.empty()) {
            ConnectInput(surface.occlusion, tex, "r");
        }
    }
    {
        std::array<float, 4> scale = one;
        if (hasEmissive && (emissive.r != 0.0f || emissive.g != 0.0f || emissive.b != 0.0f)) {
            scale = { { emissive.r, emissive.g, emissive.b, 1.0f } };
        }
        std::string tex = useTexture({ aiTextureType_EMISSIVE, aiTextureType_EMISSION_COLOR }, 0, "emissiveColor_texture",
                true, "rgb", scale, zero);
        if (tex.empty() && unlit) {
            tex = useTexture({ aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE }, 0, "emissiveColor_texture", true,
                    "rgb", hasBase ? baseScale : one, zero, &diffusePath);
        }
        if (!tex.empty()) {
            ConnectInput(surface.emissiveColor, tex, "rgb");
        }
    }
    {
        // Opacity: a separate map, or the alpha of the base colour map.
        aiString opacityPath;
        mat->GetTexture(aiTextureType_OPACITY, 0, &opacityPath);
        const bool fromBaseAlpha = opacityPath.length > 0 && diffusePath.length > 0 && opacityPath == diffusePath;
        const std::string channel = fromBaseAlpha ? "a" : "r";
        const int ci = fromBaseAlpha ? 3 : 0;
        std::array<float, 4> scale = one;
        scale[ci] = opacity;
        const std::string tex = useTexture({ aiTextureType_OPACITY }, 0,
                fromBaseAlpha ? "diffuseColor_texture" : "opacity_texture", fromBaseAlpha, channel, scale, zero);
        if (!tex.empty()) {
            ConnectInput(surface.opacity, tex, channel);
        }
    }
    {
        const std::string tex = useTexture({ aiTextureType_DISPLACEMENT }, 0, "displacement_texture", false, "r", one, zero);
        if (!tex.empty()) {
            ConnectInput(surface.displacement, tex, "r");
        }
    }
    if (mOpts.clearcoat) {
        std::string tex = useTexture({ aiTextureType_CLEARCOAT }, 0, "clearcoat_texture", false, "r", one, zero);
        if (!tex.empty()) {
            ConnectInput(surface.clearcoat, tex, "r");
        }
        tex = useTexture({ aiTextureType_CLEARCOAT }, 1, "clearcoatRoughness_texture", false, "g", one, zero);
        if (!tex.empty()) {
            ConnectInput(surface.clearcoatRoughness, tex, "g");
        }
    }
    if (unlit) {
        // flat colour: no diffuse response, base colour drives emission
        surface.diffuseColor = lu::UsdPreviewSurface().diffuseColor;
        surface.diffuseColor.set_value(lu::Animatable<luv::color3f>(luv::color3f{ 0.0f, 0.0f, 0.0f }));
        if (!surface.emissiveColor.has_connections()) {
            surface.emissiveColor.set_value(lu::Animatable<luv::color3f>(luv::color3f{ baseColor.r, baseColor.g, baseColor.b }));
        }
        surface.metallic.set_value(lu::Animatable<float>(0.0f));
        surface.roughness.set_value(lu::Animatable<float>(1.0f));
    }

    // ---- KHR_animation_pointer colour animation
    auto anims = mMaterialAnims.find(materialIndex);
    if (anims != mMaterialAnims.end()) {
        for (const auto &entry : anims->second) {
            const aiPropertyAnim *p = entry.first;
            const std::string prop = p->mTargetProperty.C_Str();
            const bool diffuse = prop == "baseColorFactor" || prop == "diffuseColor";
            const bool emission = prop == "emissiveFactor" || prop == "emissiveColor";
            if (!diffuse && !emission) {
                continue;
            }
            auto &attr = diffuse ? surface.diffuseColor : surface.emissiveColor;
            if (attr.has_connections()) {
                continue;
            }
            lu::Animatable<luv::color3f> color;
            for (unsigned k = 0; k < p->mNumKeys; ++k) {
                color.add_sample(p->mKeys[k].mTime * entry.second,
                        luv::color3f{ p->mKeys[k].mValues[0], p->mKeys[k].mValues[1], p->mKeys[k].mValues[2] });
            }
            attr.set_value(color);
        }
    }

    // ---- assemble
    lu::Material material;
    surface.outputsSurface.set_authored(true);
    material.surface.set(lu::Path(surfacePath, "outputs:surface"));
    if (surface.displacement.has_connections()) {
        surface.outputsDisplacement.set_authored(true);
        material.displacement.set(lu::Path(surfacePath, "outputs:displacement"));
    }
    lu::Shader surfaceShader;
    surfaceShader.name = surfaceName;
    surfaceShader.info_id = "UsdPreviewSurface";
    surfaceShader.value = std::move(surface);

    lu::Prim prim(name, luv::Value(std::move(material)));
    prim.children().emplace_back(surfaceName, luv::Value(std::move(surfaceShader)));
    for (TexNode &node : net.textures) {
        lu::UsdUVTexture &t = node.tex;
        const luv::float4 scale{ node.scale[0], node.scale[1], node.scale[2], node.scale[3] };
        const luv::float4 bias{ node.bias[0], node.bias[1], node.bias[2], node.bias[3] };
        if (scale != luv::float4{ 1, 1, 1, 1 }) {
            t.scale.set_value(scale);
        }
        if (bias != luv::float4{ 0, 0, 0, 0 }) {
            t.bias.set_value(bias);
        }
        for (const std::string &out : node.outputs) {
            if (out == "rgb") t.outputsRGB.set_authored(true);
            else if (out == "r") t.outputsR.set_authored(true);
            else if (out == "g") t.outputsG.set_authored(true);
            else if (out == "b") t.outputsB.set_authored(true);
            else t.outputsA.set_authored(true);
        }
        lu::Shader shader;
        shader.name = node.name;
        shader.info_id = "UsdUVTexture";
        shader.value = std::move(t);
        prim.children().emplace_back(node.name, luv::Value(std::move(shader)));
    }
    for (lu::Prim &p : net.prims) {
        prim.children().push_back(std::move(p));
    }
    return prim;
}

// ------------------------------------------------------------------------------------------------
// Textures: every byte goes through the IOSystem (D27/D28)

struct StreamCloser {
    IOSystem *io;
    void operator()(IOStream *s) const {
        if (s != nullptr) {
            io->Close(s);
        }
    }
};
using StreamPtr = std::unique_ptr<IOStream, StreamCloser>;

bool UsdExporter::ReadExternalTexture(const std::string &path, std::vector<uint8_t> &bytes, std::string &resolved) {
    std::string decoded = UrlDecode(path);
    if (decoded.compare(0, 7, "file://") == 0) {
        decoded = decoded.substr(7);
    }
    const bool absolute = !decoded.empty() &&
                          (decoded[0] == '/' || decoded[0] == '\\' ||
                           (decoded.size() > 2 && IsAsciiAlpha(static_cast<unsigned char>(decoded[0])) && decoded[1] == ':'));
    std::vector<std::string> candidates;
    if (!absolute && !mOpts.sourceDirectory.empty()) {
        std::string dir = mOpts.sourceDirectory;
        if (dir.back() != '/' && dir.back() != '\\') {
            dir.push_back(mIO->getOsSeparator());
        }
        candidates.push_back(dir + decoded);
    }
    candidates.push_back(decoded);
    if (decoded != path) {
        candidates.push_back(path);
    }
    for (const std::string &candidate : candidates) {
        if (!mIO->Exists(candidate.c_str())) {
            continue;
        }
        StreamPtr in(mIO->Open(candidate.c_str(), "rb"), StreamCloser{ mIO });
        if (!in) {
            continue;
        }
        const size_t size = in->FileSize();
        bytes.resize(size);
        if (size > 0 && in->Read(bytes.data(), 1, size) != size) {
            continue;
        }
        resolved = candidate;
        return true;
    }
    return false;
}

int UsdExporter::RequestTexture(const std::string &path) {
    const aiTexture *embedded = mScene->GetEmbeddedTexture(path.c_str());
    std::string key;
    unsigned embeddedIndex = 0;
    if (embedded != nullptr) {
        for (unsigned i = 0; i < mScene->mNumTextures; ++i) {
            if (mScene->mTextures[i] == embedded) {
                embeddedIndex = i;
            }
        }
        key = "*" + std::to_string(embeddedIndex);
    } else {
        key = path;
    }
    auto it = mTextureByKey.find(key);
    if (it != mTextureByKey.end()) {
        return it->second;
    }
    mTextureByKey[key] = -1;

    std::vector<uint8_t> bytes;
    std::string sourceName;
    std::string err;
    if (embedded != nullptr) {
        sourceName = embedded->mFilename.length > 0 ? std::string(embedded->mFilename.C_Str())
                                                    : "texture" + std::to_string(embeddedIndex);
        if (embedded->mHeight == 0) {
            const uint8_t *data = reinterpret_cast<const uint8_t *>(embedded->pcData);
            bytes.assign(data, data + embedded->mWidth);
        } else if (!RawTextureToPng(*embedded, bytes, err)) {
            ASSIMP_LOG_WARN("USD export: embedded texture ", embeddedIndex, " could not be encoded as PNG (", err, "); dropped");
            return -1;
        }
    } else {
        sourceName = path;
        std::string resolved;
        if (!ReadExternalTexture(path, bytes, resolved)) {
            ASSIMP_LOG_WARN("USD export: texture '", path, "' not found (set ", AI_CONFIG_EXPORT_USD_SOURCE_DIRECTORY,
                    " to the source asset's directory); dropped");
            return -1;
        }
    }

    std::string ext;
    if (IsPng(bytes)) {
        ext = "png";
    } else if (IsJpeg(bytes)) {
        ext = "jpg";
    } else if (IsExr(bytes) && mOpts.format != OutputFormat::USDZ) {
        ext = "exr";
    } else {
        // ARKit USDZ allows PNG and JPEG only (C4); other formats are transcoded.
        std::vector<uint8_t> png;
        if (!TranscodeToPng(bytes, sourceName, png, err)) {
            ASSIMP_LOG_WARN("USD export: texture '", sourceName, "' is not PNG/JPEG and could not be transcoded (", err, "); dropped");
            return -1;
        }
        ASSIMP_LOG_INFO("USD export: texture '", sourceName, "' transcoded to PNG");
        bytes = std::move(png);
        ext = "png";
    }

    TextureAsset asset;
    const std::string name = mTextureNames.Claim(SanitizeFileName(FileStem(sourceName), "texture"), "texture");
    if (mOpts.format == OutputFormat::USDZ) {
        asset.fileName = "textures/" + name + "." + ext;
        asset.assetPath = asset.fileName;
    } else {
        asset.fileName = mOutputStem + "_" + name + "." + ext;
        asset.assetPath = "./" + asset.fileName;
    }
    asset.bytes = std::move(bytes);
    const int index = static_cast<int>(mTextures.size());
    mTextures.push_back(std::move(asset));
    mTextureByKey[key] = index;
    return index;
}

// ------------------------------------------------------------------------------------------------
// Output

void UsdExporter::WriteFile(const std::string &name, const uint8_t *data, size_t size, const char *mode) {
    StreamPtr out(mIO->Open(name.c_str(), mode), StreamCloser{ mIO });
    if (!out) {
        throw DeadlyExportError("USD export: cannot open '", name, "' for writing");
    }
    if (size > 0 && out->Write(data, 1, size) != size) {
        throw DeadlyExportError("USD export: short write to '", name, "'");
    }
}

bool UsdExporter::SaveCrate(const lu::Stage &stage, std::vector<uint8_t> &out, std::string &err) {
    // lu::usdc::SaveAsUSDCToMemory with one change: compression is turned off
    // when the stage has long token arrays (see mLongTokenArrays).
    auto stream = std::unique_ptr<lu::experimental::MemoryOutputStream>(new lu::experimental::MemoryOutputStream());
    lu::experimental::MemoryOutputStream *buffer = stream.get();
    lu::experimental::CrateWriter writer(std::unique_ptr<lu::experimental::IOutputStream>(std::move(stream)));
    lu::experimental::CrateWriter::Options opts;
    opts.version_major = 0;
    opts.version_minor = 8;
    opts.version_patch = 0;
    opts.enable_compression = !mLongTokenArrays;
    opts.enable_deduplication = true;
    // lossless (exact-integer and lookup-table codings), readable by OpenUSD (P8)
    opts.enable_float_array_compression = mOpts.compressFloatArrays && !mLongTokenArrays;
    opts.max_file_size_bytes = mOpts.maxFileBytes;
    opts.max_memory_bytes = mOpts.maxMemoryBytes;
    writer.SetOptions(opts);
    if (mLongTokenArrays) {
        ASSIMP_LOG_INFO("USD export: USDC written without array compression (token arrays of 16+ elements)");
    }
    if (!writer.Open(&err) || !writer.ConvertStageToSpecs(stage, &err) || !writer.Finalize(&err)) {
        return false;
    }
    writer.Close();
    out = buffer->TakeBuffer();
    return true;
}

void UsdExporter::WriteOutput(const lu::Stage &stage) {
    std::string warn;
    std::string err;
    if (mOpts.format == OutputFormat::USDA) {
        std::string text;
        if (!lu::usda::ExportToUSDAString(stage, &text, &warn, &err)) {
            throw DeadlyExportError("USD export: USDA serialisation failed: ", err);
        }
        if (!warn.empty()) {
            ASSIMP_LOG_WARN("USD export: ", warn);
        }
        WriteFile(mPath, reinterpret_cast<const uint8_t *>(text.data()), text.size(), "wb");
    } else {
        std::vector<uint8_t> crate;
        if (!SaveCrate(stage, crate, err)) {
            throw DeadlyExportError("USD export: USDC serialisation failed (", AI_CONFIG_EXPORT_USD_MAX_FILE_SIZE_MB,
                    "/", AI_CONFIG_EXPORT_USD_MAX_MEMORY_MB, " raise the limits): ", err);
        }
        if (!warn.empty()) {
            ASSIMP_LOG_WARN("USD export: ", warn);
        }
        if (mOpts.format == OutputFormat::USDC) {
            WriteFile(mPath, crate.data(), crate.size(), "wb");
        } else {
            // USDZ: root layer first, then textures; stored, 64-byte aligned (C1, P2, P3)
            std::vector<USDZ::Entry> entries;
            entries.reserve(mTextures.size() + 1);
            entries.push_back({ mOutputStem + ".usdc", crate.data(), crate.size() });
            for (const TextureAsset &tex : mTextures) {
                entries.push_back({ tex.fileName, tex.bytes.data(), tex.bytes.size() });
            }
            StreamPtr out(mIO->Open(mPath.c_str(), "wb"), StreamCloser{ mIO });
            if (!out) {
                throw DeadlyExportError("USD export: cannot open '", mPath, "' for writing");
            }
            USDZ::WriteArchive(*out, entries);
            return;
        }
    }
    // USDA/USDC: textures become sidecar files next to the layer
    for (const TextureAsset &tex : mTextures) {
        WriteFile(mOutputDir + tex.fileName, tex.bytes.data(), tex.bytes.size(), "wb");
    }
}

void UsdExporter::Run() {
    double upAxis = 1.0;
    MetaNumber(mScene->mMetaData, AI_METADATA_UP_AXIS, upAxis);
    if (upAxis == 2.0) {
        // Z-up to Y-up: (x, y, z) -> (x, z, -y)
        aiMatrix4x4::RotationX(static_cast<ai_real>(-kPi / 2), mAxisCorrection);
        ASSIMP_LOG_INFO("USD export: Z-up source; the root prim carries the rotation to the Y-up stage");
    } else if (upAxis == 0.0) {
        // X-up to Y-up: (x, y, z) -> (-y, x, z)
        aiMatrix4x4::RotationZ(static_cast<ai_real>(kPi / 2), mAxisCorrection);
        ASSIMP_LOG_INFO("USD export: X-up source; the root prim carries the rotation to the Y-up stage");
    } else if (upAxis != 1.0) {
        ASSIMP_LOG_WARN("USD export: unknown up axis ", upAxis, "; assuming Y");
    }
    for (unsigned r = 0; r < 4; ++r) {
        for (unsigned c = 0; c < 4; ++c) {
            if (std::abs(mAxisCorrection[r][c]) < static_cast<ai_real>(1e-7)) {
                mAxisCorrection[r][c] = 0;
            }
        }
    }
    IndexNodes();
    PlanSkeleton();
    PlanExportTree();
    PlanSkelRoots();
    PlanAttachments();
    PlanAnimations();
    PlanSkinnedBlendShapes();

    lu::Prim root = BuildNode(0);
    const aiMetadata *sceneMeta = mScene->mMetaData;
    aiVector3D gravity;
    float magnitude = 0;
    if (MetaVector(sceneMeta, "usdPhysics:gravityDirection", gravity) ||
            MetaFloat(sceneMeta, "usdPhysics:gravityMagnitude", magnitude)) {
        root.children().push_back(BuildPhysicsScene(mNodes[0].names.Claim("PhysicsScene", "PhysicsScene")));
    }
    if (!mMaterialPrims.empty()) {
        lu::Scope scope;
        lu::Prim scopePrim(mMaterialsName, luv::Value(std::move(scope)));
        for (lu::Prim &m : mMaterialPrims) {
            scopePrim.children().push_back(std::move(m));
        }
        mMaterialPrims.clear();
        root.children().push_back(std::move(scopePrim));
    }
    // One root prim, the defaultPrim, is the asset's component model (C2).
    root.metas().set_kind(lu::Kind::Component);

    lu::Stage stage;
    lu::StageMetas &metas = stage.metas();
    metas.defaultPrim = luv::token(mNodes[0].name);
    // D33: units come from scene metadata; geometry is never baked. The stage is
    // always Y-up (ARKit), other source axes are rotated on the root prim.
    double metersPerUnit = 1.0;
    if (!MetaNumber(sceneMeta, AI_METADATA_UNIT_SCALE_TO_METERS, metersPerUnit)) {
        MetaNumber(sceneMeta, "UnitScaleFactor", metersPerUnit);
    }
    if (!(metersPerUnit > 0.0) || !std::isfinite(metersPerUnit)) {
        metersPerUnit = 1.0;
    }
    metas.metersPerUnit.set_value(metersPerUnit);
    metas.upAxis.set_value(lu::Axis::Y);
    if (mStartTime <= mEndTime) {
        metas.timeCodesPerSecond.set_value(mTimeCodesPerSecond);
        metas.startTimeCode.set_value(mStartTime);
        metas.endTimeCode.set_value(mEndTime);
    }
    stage.root_prims().push_back(std::move(root));
    stage.commit();
    WriteOutput(stage);
}

void ExportUsd(const char *pFile, IOSystem *pIOSystem, const aiScene *pScene, const ExportProperties *pProperties,
        OutputFormat format) {
    if (pFile == nullptr || pIOSystem == nullptr || pScene == nullptr || pScene->mRootNode == nullptr) {
        throw DeadlyExportError("USD export: missing output path, IO system or scene");
    }
    UsdExporter exporter(pFile, pIOSystem, pScene, ReadOptions(pProperties, format));
    exporter.Run();
}

} // namespace

void ExportSceneUSDA(const char *pFile, IOSystem *pIOSystem, const aiScene *pScene, const ExportProperties *pProperties) {
    ExportUsd(pFile, pIOSystem, pScene, pProperties, OutputFormat::USDA);
}

void ExportSceneUSDC(const char *pFile, IOSystem *pIOSystem, const aiScene *pScene, const ExportProperties *pProperties) {
    ExportUsd(pFile, pIOSystem, pScene, pProperties, OutputFormat::USDC);
}

void ExportSceneUSDZ(const char *pFile, IOSystem *pIOSystem, const aiScene *pScene, const ExportProperties *pProperties) {
    ExportUsd(pFile, pIOSystem, pScene, pProperties, OutputFormat::USDZ);
}

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_USD_EXPORTER
#endif // ASSIMP_BUILD_NO_EXPORT
