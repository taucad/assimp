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


/** @file  USDLoaderImplLightUSD.cpp
 *  @brief USD to aiScene conversion on top of LightUSD.
 *
 *  Pipeline:
 *   1. The main file is read once through the IOSystem and classified by
 *      content (USDA, USDC or USDZ).
 *   2. The root layer is loaded and composed explicitly (subLayers, then all
 *      arcs until nothing is unresolved). Every referenced asset - USDA/USDC
 *      layers, MaterialX documents, textures - is served by one wildcard
 *      LightUSD resolver handler backed by USDZ members and the IOSystem.
 *   3. Tydra converts the stage into a RenderScene; the stage is released.
 *   4. The RenderScene is mapped onto the aiScene with bounds checks.
 */

#ifndef ASSIMP_BUILD_NO_USD_IMPORTER

#include "USDLoaderImplLightUSD.h"

#include <assimp/CreateAnimMesh.h>
#include <assimp/DefaultLogger.hpp>
#include <assimp/Exceptional.h>
#include <assimp/GltfMaterial.h>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/material.h>
#include <assimp/metadata.h>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include "asset-resolution.hh"
#include "composition.hh"
#include "layer.hh"
#include "lightusd.hh"
#include "prim-types.hh"
#include "stage.hh"
#include "usdPhysics.hh"
#include "usdSkel.hh"
#include "tydra/render-data-converter.hh"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace Assimp {
namespace USDImport {

namespace lusd = ::lightusd;
namespace tydra = ::lightusd::tydra;

namespace {

// Composition passes are repeated until no arc is left unresolved. Each pass
// resolves at least one level of nesting, so this bounds the reference depth.
constexpr int kMaxCompositionPasses = 32;

// ------------------------------------------------------------------------------------------------
// Diagnostics

/// Forwards LightUSD diagnostic text to the assimp logger, one line per
/// message, without repeating a message within one read.
class Diagnostics {
public:
    void warn(const std::string &text) {
        forEachLine(text, [this](const std::string &line) {
            if (!mSeen.insert(line).second) {
                return;
            }
            // Tydra reports every animatable attribute that is simply not
            // authored (extent, doubleSided, ...) as a warning; that is noise.
            if (line.find("ExtractPrimPropertyAnimation") != std::string::npos &&
                    line.find("does not exist in Prim") != std::string::npos) {
                ASSIMP_LOG_VERBOSE_DEBUG("USD: ", line);
                return;
            }
            ASSIMP_LOG_WARN("USD: ", line);
        });
    }

    void debug(const std::string &text) {
        if (DefaultLogger::get()->getLogSeverity() != Logger::VERBOSE) {
            return;
        }
        forEachLine(text, [](const std::string &line) { ASSIMP_LOG_VERBOSE_DEBUG("USD: ", line); });
    }

private:
    template <typename F>
    static void forEachLine(const std::string &text, F &&f) {
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) {
                end = text.size();
            }
            size_t b = start, e = end;
            while (b < e && (text[b] == ' ' || text[b] == '\t' || text[b] == '\r')) {
                ++b;
            }
            while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r')) {
                --e;
            }
            if (e > b) {
                f(text.substr(b, e - b));
            }
            start = end + 1;
        }
    }

    std::set<std::string> mSeen;
};

// ------------------------------------------------------------------------------------------------
// Paths

bool IsAbsolutePath(const std::string &p) {
    if (p.empty()) {
        return false;
    }
    if (p[0] == '/' || p[0] == '\\') {
        return true;
    }
    return p.size() > 2 && p[1] == ':' && (p[2] == '/' || p[2] == '\\');
}

/// Collapses "." and ".." segments and unifies separators to '/'. A leading
/// ".." that cannot be collapsed is kept, so the IOSystem decides about it.
std::string NormalizePath(const std::string &in) {
    std::string p = in;
    std::replace(p.begin(), p.end(), '\\', '/');
    std::string prefix;
    size_t pos = 0;
    if (p.size() > 2 && p[1] == ':' && p[2] == '/') {
        prefix = p.substr(0, 3);
        pos = 3;
    } else if (!p.empty() && p[0] == '/') {
        prefix = "/";
        pos = 1;
    }
    std::vector<std::string> parts;
    while (pos <= p.size()) {
        size_t next = p.find('/', pos);
        if (next == std::string::npos) {
            next = p.size();
        }
        std::string seg = p.substr(pos, next - pos);
        if (seg.empty() || seg == ".") {
            // skip
        } else if (seg == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (prefix.empty()) {
                parts.push_back(seg);
            }
        } else {
            parts.push_back(seg);
        }
        pos = next + 1;
    }
    std::string out = prefix;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) {
            out += '/';
        }
        out += parts[i];
    }
    return out;
}

std::string JoinPath(const std::string &dir, const std::string &name) {
    if (dir.empty() || dir == "." || dir == "./" || IsAbsolutePath(name)) {
        return name;
    }
    if (dir.back() == '/' || dir.back() == '\\') {
        return dir + name;
    }
    return dir + "/" + name;
}

std::string DirName(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string LowerExtension(const std::string &path) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return std::string();
    }
    std::string ext = path.substr(dot + 1);
    for (char &c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

std::string LeafName(const std::string &primPath) {
    const size_t slash = primPath.find_last_of('/');
    return slash == std::string::npos ? primPath : primPath.substr(slash + 1);
}

std::string ParentPath(const std::string &primPath) {
    const size_t slash = primPath.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return std::string();
    }
    return primPath.substr(0, slash);
}

// ------------------------------------------------------------------------------------------------
// Asset resolution through USDZ members and the IOSystem

/// Userdata of the wildcard resolver handler. Lives for one ReadScene call.
class AssetSource {
public:
    AssetSource(IOSystem *io, std::string baseDir) :
            mIO(io), mBaseDir(std::move(baseDir)) {}

    ~AssetSource() { closeStream(); }

    AssetSource(const AssetSource &) = delete;
    AssetSource &operator=(const AssetSource &) = delete;

    void setPackage(const lusd::USDZAsset *usdz, const uint8_t *data, size_t size) {
        mUsdz = usdz;
        mUsdzData = data;
        mUsdzSize = size;
    }

    const std::string &baseDir() const { return mBaseDir; }

    int resolve(const char *assetName, const std::vector<std::string> &searchPaths, std::string *resolved) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (assetName == nullptr || assetName[0] == '\0' || resolved == nullptr) {
            return -1;
        }
        const std::string name(assetName);
        std::vector<std::string> candidates;
        auto add = [&candidates](const std::string &c) {
            if (!c.empty() && std::find(candidates.begin(), candidates.end(), c) == candidates.end()) {
                candidates.push_back(c);
            }
        };
        if (IsAbsolutePath(name)) {
            add(NormalizePath(name));
        } else {
            // The fork passes the referencing layer's directory first.
            for (const std::string &sp : searchPaths) {
                add(NormalizePath(JoinPath(sp, name)));
            }
            add(NormalizePath(JoinPath(mBaseDir, name)));
            add(NormalizePath(name));
        }
        for (const std::string &c : candidates) {
            size_t off = 0, len = 0;
            if (packageEntry(c, &off, &len) || mIO->Exists(c.c_str())) {
                *resolved = c;
                return 0;
            }
        }
        return -1;
    }

    int size(const char *resolvedName, uint64_t *nbytes, std::string *err) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (resolvedName == nullptr || nbytes == nullptr) {
            return -1;
        }
        size_t off = 0, len = 0;
        if (packageEntry(resolvedName, &off, &len)) {
            *nbytes = len;
            return 0;
        }
        IOStream *stream = openStream(resolvedName);
        if (stream == nullptr) {
            if (err) {
                *err += std::string("Cannot open `") + resolvedName + "`.\n";
            }
            return -1;
        }
        *nbytes = stream->FileSize();
        return 0;
    }

    int read(const char *resolvedName, uint64_t reqBytes, uint8_t *out, uint64_t *nbytes, std::string *err) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (resolvedName == nullptr || out == nullptr || nbytes == nullptr) {
            return -1;
        }
        size_t off = 0, len = 0;
        if (packageEntry(resolvedName, &off, &len)) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(reqBytes, len));
            if (n) {
                std::memcpy(out, mUsdzData + off, n);
            }
            *nbytes = n;
            return 0;
        }
        IOStream *stream = openStream(resolvedName);
        if (stream == nullptr) {
            if (err) {
                *err += std::string("Cannot open `") + resolvedName + "`.\n";
            }
            return -1;
        }
        stream->Seek(0, aiOrigin_SET);
        const size_t want = static_cast<size_t>(std::min<uint64_t>(reqBytes, stream->FileSize()));
        const size_t got = want ? stream->Read(out, 1, want) : 0;
        closeStream();
        if (got != want) {
            if (err) {
                *err += std::string("Short read from `") + resolvedName + "`.\n";
            }
            return -1;
        }
        *nbytes = got;
        return 0;
    }

private:
    /// A USDZ member matches when its archive name equals the candidate
    /// relative to the main file's directory (the package root).
    bool packageEntry(const std::string &candidate, size_t *off, size_t *len) const {
        if (mUsdz == nullptr) {
            return false;
        }
        std::string key = candidate;
        if (!mBaseDir.empty()) {
            const std::string root = NormalizePath(mBaseDir);
            if (!root.empty() && key.size() > root.size() && key.compare(0, root.size(), root) == 0 && key[root.size()] == '/') {
                key = key.substr(root.size() + 1);
            }
        }
        auto it = mUsdz->asset_map.find(key);
        if (it == mUsdz->asset_map.end()) {
            return false;
        }
        const size_t begin = it->second.first;
        const size_t end = it->second.second;
        if (begin > end || end > mUsdzSize) {
            return false;
        }
        *off = begin;
        *len = end - begin;
        return true;
    }

    IOStream *openStream(const std::string &name) {
        if (mStream != nullptr && mStreamName == name) {
            return mStream;
        }
        closeStream();
        mStream = mIO->Open(name, "rb");
        if (mStream != nullptr) {
            mStreamName = name;
        }
        return mStream;
    }

    void closeStream() {
        if (mStream != nullptr) {
            mIO->Close(mStream);
            mStream = nullptr;
            mStreamName.clear();
        }
    }

    IOSystem *mIO;
    std::string mBaseDir;
    const lusd::USDZAsset *mUsdz = nullptr;
    const uint8_t *mUsdzData = nullptr;
    size_t mUsdzSize = 0;
    std::mutex mMutex;
    IOStream *mStream = nullptr;
    std::string mStreamName;
};

int ResolveAssetCallback(const char *assetName, const std::vector<std::string> &searchPaths,
        std::string *resolved, std::string *err, void *userdata) {
    (void)err;
    return static_cast<AssetSource *>(userdata)->resolve(assetName, searchPaths, resolved);
}

int SizeAssetCallback(const char *resolvedName, uint64_t *nbytes, std::string *err, void *userdata) {
    return static_cast<AssetSource *>(userdata)->size(resolvedName, nbytes, err);
}

int ReadAssetCallback(const char *resolvedName, uint64_t reqBytes, uint8_t *out, uint64_t *nbytes,
        std::string *err, void *userdata) {
    return static_cast<AssetSource *>(userdata)->read(resolvedName, reqBytes, out, nbytes, err);
}

/// Tydra texture loader that keeps the encoded file bytes (PNG, JPEG, ...)
/// instead of decoding them; they become compressed aiTextures. A width of
/// zero marks such an image for the scene builder.
bool LoadEncodedTexture(const lusd::value::AssetPath &assetPath, const lusd::AssetInfo &assetInfo,
        const lusd::AssetResolutionResolver &resolver, tydra::TextureImage *image,
        std::vector<uint8_t> *data, void *userdata, std::string *warn, std::string *err) {
    (void)assetInfo;
    (void)userdata;
    if (image == nullptr || data == nullptr) {
        return false;
    }
    std::string path = assetPath.GetAssetPath();
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string resolved = path.empty() ? std::string() : resolver.resolve(path);
    if (resolved.empty() && !assetPath.GetResolvedPath().empty()) {
        resolved = resolver.resolve(assetPath.GetResolvedPath());
    }
    if (resolved.empty()) {
        if (err) {
            *err += "Texture asset not found: `" + path + "`.\n";
        }
        return false;
    }
    lusd::Asset asset;
    if (!resolver.open_asset(resolved, path, &asset, warn, err)) {
        return false;
    }
    *data = asset.release_buffer();
    image->asset_identifier = resolved;
    image->width = 0;
    image->height = 0;
    image->channels = 4;
    image->texelComponentType = tydra::ComponentType::UInt8;
    image->assetTexelComponentType = tydra::ComponentType::UInt8;
    image->decoded = false;
    return true;
}

// ------------------------------------------------------------------------------------------------
// Composition

lusd::AssetResolutionResolver MakeResolver(AssetSource *source, const std::string &cwd) {
    lusd::AssetResolutionResolver resolver;
    lusd::AssetResolutionHandler handler;
    handler.resolve_fun = ResolveAssetCallback;
    handler.size_fun = SizeAssetCallback;
    handler.read_fun = ReadAssetCallback;
    handler.write_fun = nullptr;
    handler.userdata = source;
    resolver.register_wildcard_asset_resolution_handler(handler);
    resolver.set_current_working_path(cwd);
    // The IOSystem is the sandbox: parent-relative assets (`../tex/a.png`) are
    // legitimate in production layouts and are handed to it as is.
    resolver.set_allow_parent_relative_paths(true);
    return resolver;
}

void ComposeLayer(lusd::Layer &layer, lusd::AssetResolutionResolver &resolver, uint32_t maxMemoryMB,
        Diagnostics &diag) {
    const std::string layerDir = layer.get_current_working_path();

    std::string warn, err;
    if (!layer.metas().subLayers.empty()) {
        lusd::SublayersCompositionOptions options;
        options.error_when_asset_not_found = false;
        options.allow_parent_relative_paths = true;
        options.max_memory_limit_mb = maxMemoryMB;
        lusd::Layer composed;
        resolver.set_current_working_path(layerDir);
        if (lusd::CompositeSublayers(resolver, layer, &composed, &warn, &err, options)) {
            layer = std::move(composed);
        } else {
            diag.warn("Sublayer composition failed, continuing without sublayers: " + err);
        }
        diag.warn(warn);
        warn.clear();
        err.clear();
    }

    std::map<std::string, lusd::Layer> layerCache;
    lusd::AllArcsCompositionOptions arcOptions;
    arcOptions.references.error_when_asset_not_found = false;
    arcOptions.references.allow_parent_relative_paths = true;
    arcOptions.references.max_memory_limit_mb = maxMemoryMB;
    arcOptions.references.layer_cache = &layerCache;
    arcOptions.payload.error_when_asset_not_found = false;
    arcOptions.payload.allow_parent_relative_paths = true;
    arcOptions.payload.max_memory_limit_mb = maxMemoryMB;
    arcOptions.payload.layer_cache = &layerCache;

    auto unresolved = [&layer]() {
        return layer.check_unresolved_references() || layer.check_unresolved_payload() ||
               layer.check_unresolved_inherits() || layer.check_unresolved_variant() ||
               layer.check_unresolved_specializes();
    };

    int pass = 0;
    for (; pass < kMaxCompositionPasses && unresolved(); ++pass) {
        lusd::Layer composed;
        resolver.set_current_working_path(layerDir);
        const bool ok = lusd::CompositeAllArcs(resolver, layer, &composed, &warn, &err, arcOptions);
        diag.warn(warn);
        warn.clear();
        if (!ok) {
            diag.warn("Composition stopped early: " + err);
            err.clear();
            break;
        }
        layer = std::move(composed);
    }
    if (pass == kMaxCompositionPasses && unresolved()) {
        diag.warn("Composition did not converge; some arcs remain unresolved.");
    }
    resolver.set_current_working_path(layerDir);
}

// ------------------------------------------------------------------------------------------------
// Conversions

aiMatrix4x4 ToAiMatrix(const lusd::value::matrix4d &m) {
    // USD matrices are row-major with row vectors; assimp uses column vectors.
    return aiMatrix4x4(
            static_cast<ai_real>(m.m[0][0]), static_cast<ai_real>(m.m[1][0]), static_cast<ai_real>(m.m[2][0]), static_cast<ai_real>(m.m[3][0]),
            static_cast<ai_real>(m.m[0][1]), static_cast<ai_real>(m.m[1][1]), static_cast<ai_real>(m.m[2][1]), static_cast<ai_real>(m.m[3][1]),
            static_cast<ai_real>(m.m[0][2]), static_cast<ai_real>(m.m[1][2]), static_cast<ai_real>(m.m[2][2]), static_cast<ai_real>(m.m[3][2]),
            static_cast<ai_real>(m.m[0][3]), static_cast<ai_real>(m.m[1][3]), static_cast<ai_real>(m.m[2][3]), static_cast<ai_real>(m.m[3][3]));
}

aiColor3D ToAiColor(const tydra::vec3 &c) {
    return aiColor3D(c[0], c[1], c[2]);
}

std::string FormatFloat(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    return buf;
}

// ------------------------------------------------------------------------------------------------
// UsdPhysics, read from the stage before it is released

struct MetaEntry {
    enum class Kind { Bool, Float, Vector, String };
    std::string key;
    Kind kind = Kind::Bool;
    bool b = false;
    float f = 0.0f;
    aiVector3D v;
    std::string s;
};

struct PhysicsJointPrim {
    std::string path;
    std::vector<MetaEntry> meta;
};

struct PhysicsData {
    std::unordered_map<std::string, std::vector<MetaEntry>> primMeta;
    std::vector<PhysicsJointPrim> joints;
    bool hasScene = false;
    bool hasGravityDirection = false;
    aiVector3D gravityDirection;
    bool hasGravityMagnitude = false;
    float gravityMagnitude = 0.0f;
};

void AddBool(std::vector<MetaEntry> &m, const char *key, bool b) {
    MetaEntry e;
    e.key = key;
    e.kind = MetaEntry::Kind::Bool;
    e.b = b;
    m.push_back(std::move(e));
}

void AddFloat(std::vector<MetaEntry> &m, const char *key, float f) {
    MetaEntry e;
    e.key = key;
    e.kind = MetaEntry::Kind::Float;
    e.f = f;
    m.push_back(std::move(e));
}

void AddVector(std::vector<MetaEntry> &m, const char *key, float x, float y, float z) {
    MetaEntry e;
    e.key = key;
    e.kind = MetaEntry::Kind::Vector;
    e.v = aiVector3D(x, y, z);
    m.push_back(std::move(e));
}

void AddString(std::vector<MetaEntry> &m, const char *key, std::string s) {
    MetaEntry e;
    e.key = key;
    e.kind = MetaEntry::Kind::String;
    e.s = std::move(s);
    m.push_back(std::move(e));
}

bool HasKey(const std::vector<MetaEntry> &m, const char *key) {
    for (const MetaEntry &e : m) {
        if (e.key == key) {
            return true;
        }
    }
    return false;
}

std::string RelTargetName(const lusd::RelationshipProperty &rp) {
    if (!rp.authored()) {
        return std::string();
    }
    const lusd::Relationship &rel = rp.relationship();
    if (rel.is_path()) {
        return LeafName(rel.targetPath.full_path_name());
    }
    if (rel.is_pathvector() && !rel.targetPathVector.empty()) {
        return LeafName(rel.targetPathVector.front().full_path_name());
    }
    return std::string();
}

void AddMaterialPhysics(const lusd::PhysicsMaterialAPI &mat, std::vector<MetaEntry> &m) {
    float f = 0.0f;
    if (mat.staticFriction.get_value(&f) && !HasKey(m, "usdPhysics:staticFriction")) {
        AddFloat(m, "usdPhysics:staticFriction", f);
    }
    if (mat.dynamicFriction.get_value(&f) && !HasKey(m, "usdPhysics:dynamicFriction")) {
        AddFloat(m, "usdPhysics:dynamicFriction", f);
    }
    if (mat.restitution.get_value(&f) && !HasKey(m, "usdPhysics:restitution")) {
        AddFloat(m, "usdPhysics:restitution", f);
    }
}

template <typename Joint>
void AddJointBase(const Joint &j, std::vector<MetaEntry> &m) {
    const std::string b0 = RelTargetName(j.body0);
    if (!b0.empty()) {
        AddString(m, "usdPhysics:body0", b0);
    }
    const std::string b1 = RelTargetName(j.body1);
    if (!b1.empty()) {
        AddString(m, "usdPhysics:body1", b1);
    }
    if (j.localPos0.authored()) {
        const auto &p = j.localPos0.get_value();
        AddVector(m, "usdPhysics:localPos0", p.x, p.y, p.z);
    }
    if (j.localPos1.authored()) {
        const auto &p = j.localPos1.get_value();
        AddVector(m, "usdPhysics:localPos1", p.x, p.y, p.z);
    }
    if (j.localRot0.authored()) {
        const auto &q = j.localRot0.get_value();
        AddString(m, "usdPhysics:localRot0", FormatFloat(q.real) + " " + FormatFloat(q.imag[0]) + " " + FormatFloat(q.imag[1]) + " " + FormatFloat(q.imag[2]));
    }
    if (j.localRot1.authored()) {
        const auto &q = j.localRot1.get_value();
        AddString(m, "usdPhysics:localRot1", FormatFloat(q.real) + " " + FormatFloat(q.imag[0]) + " " + FormatFloat(q.imag[1]) + " " + FormatFloat(q.imag[2]));
    }
}

template <typename Joint>
void AddAxisAndLimits(const Joint &j, std::vector<MetaEntry> &m) {
    lusd::value::token axis;
    if (j.axis.get_value(&axis) && !axis.str().empty()) {
        AddString(m, "usdPhysics:axis", axis.str());
    }
    float f = 0.0f;
    if (j.lowerLimit.get_value(&f)) {
        AddFloat(m, "usdPhysics:lowerLimit", f);
    }
    if (j.upperLimit.get_value(&f)) {
        AddFloat(m, "usdPhysics:upperLimit", f);
    }
}

class PhysicsCollector {
public:
    PhysicsCollector(const lusd::Stage &stage, PhysicsData &out) :
            mStage(stage), mOut(out) {}

    void run() {
        for (const lusd::Prim &prim : mStage.root_prims()) {
            visit(prim, std::string(), 0);
        }
    }

private:
    void visit(const lusd::Prim &prim, const std::string &parentPath, int depth) {
        if (depth > 4096) {
            return;
        }
        const std::string path = parentPath + "/" + std::string(prim.element_name());
        collect(prim, path);
        for (const lusd::Prim &child : prim.children()) {
            visit(child, path, depth + 1);
        }
    }

    void collect(const lusd::Prim &prim, const std::string &path) {
        if (prim.as<lusd::Material>() || prim.as<lusd::Shader>() || prim.as<lusd::NodeGraph>()) {
            return;
        }
        if (const auto *scene = prim.as<lusd::PhysicsScene>()) {
            if (!mOut.hasScene) {
                mOut.hasScene = true;
                lusd::value::vector3f dir;
                if (scene->gravityDirection.get_value(&dir)) {
                    mOut.hasGravityDirection = true;
                    mOut.gravityDirection = aiVector3D(dir.x, dir.y, dir.z);
                }
                float mag = 0.0f;
                if (scene->gravityMagnitude.get_value(&mag)) {
                    mOut.hasGravityMagnitude = true;
                    mOut.gravityMagnitude = mag;
                }
            }
            return;
        }

        if (collectJoint(prim, path)) {
            return;
        }

        std::vector<MetaEntry> m;
        lusd::PhysicsRigidBodyAPI rigid;
        if (lusd::GetPhysicsRigidBodyAPI(prim, &rigid)) {
            AddBool(m, "usdPhysics:rigidBodyEnabled", rigid.rigidBodyEnabled.get_value());
            if (rigid.kinematicEnabled.authored()) {
                AddBool(m, "usdPhysics:kinematicEnabled", rigid.kinematicEnabled.get_value());
            }
            lusd::value::vector3f vel;
            if (rigid.velocity.authored()) {
                const auto value = rigid.velocity.get_value();
                if (value.has_value() && value.value().get_scalar(&vel)) {
                    AddVector(m, "usdPhysics:velocity", vel.x, vel.y, vel.z);
                }
            }
            if (rigid.angularVelocity.authored()) {
                const auto value = rigid.angularVelocity.get_value();
                if (value.has_value() && value.value().get_scalar(&vel)) {
                    AddVector(m, "usdPhysics:angularVelocity", vel.x, vel.y, vel.z);
                }
            }
        }
        lusd::PhysicsCollisionAPI collision;
        if (lusd::GetPhysicsCollisionAPI(prim, &collision)) {
            AddBool(m, "usdPhysics:collisionEnabled", collision.collisionEnabled.get_value());
        }
        lusd::PhysicsMeshCollisionAPI meshCollision;
        if (lusd::GetPhysicsMeshCollisionAPI(prim, &meshCollision) && meshCollision.approximation.authored()) {
            AddString(m, "usdPhysics:approximation", meshCollision.approximation.get_value().str());
        }
        lusd::PhysicsMassAPI mass;
        if (lusd::GetPhysicsMassAPI(prim, &mass)) {
            if (mass.mass.authored()) {
                AddFloat(m, "usdPhysics:mass", mass.mass.get_value());
            }
            if (mass.density_.authored()) {
                AddFloat(m, "usdPhysics:density", mass.density_.get_value());
            }
            if (mass.centerOfMass.authored()) {
                const auto &c = mass.centerOfMass.get_value();
                AddVector(m, "usdPhysics:centerOfMass", c.x, c.y, c.z);
            }
            if (mass.diagonalInertia.authored()) {
                const auto &d = mass.diagonalInertia.get_value();
                AddVector(m, "usdPhysics:diagonalInertia", d[0], d[1], d[2]);
            }
        }
        // Physics material: the bound physics material wins over the prim's own.
        lusd::Relationship rel;
        std::string relErr;
        if (tydra::GetRelationship(prim, "material:binding:physics", &rel, &relErr)) {
            std::string target;
            if (rel.is_path()) {
                target = rel.targetPath.full_path_name();
            } else if (rel.is_pathvector() && !rel.targetPathVector.empty()) {
                target = rel.targetPathVector.front().full_path_name();
            }
            const lusd::Prim *matPrim = nullptr;
            std::string findErr;
            if (!target.empty() && mStage.find_prim_at_path(lusd::Path(target, ""), matPrim, &findErr) && matPrim) {
                lusd::PhysicsMaterialAPI mat;
                if (lusd::GetPhysicsMaterialAPI(*matPrim, &mat)) {
                    AddMaterialPhysics(mat, m);
                }
            }
        }
        lusd::PhysicsMaterialAPI ownMaterial;
        if (lusd::GetPhysicsMaterialAPI(prim, &ownMaterial)) {
            AddMaterialPhysics(ownMaterial, m);
        }
        if (!m.empty()) {
            mOut.primMeta[path] = std::move(m);
        }
    }

    bool collectJoint(const lusd::Prim &prim, const std::string &path) {
        PhysicsJointPrim joint;
        joint.path = path;
        std::vector<MetaEntry> &m = joint.meta;
        if (const auto *j = prim.as<lusd::PhysicsRevoluteJoint>()) {
            AddString(m, "usdPhysics:jointType", "revolute");
            AddJointBase(*j, m);
            AddAxisAndLimits(*j, m);
        } else if (const auto *j2 = prim.as<lusd::PhysicsPrismaticJoint>()) {
            AddString(m, "usdPhysics:jointType", "prismatic");
            AddJointBase(*j2, m);
            AddAxisAndLimits(*j2, m);
        } else if (const auto *j3 = prim.as<lusd::PhysicsSphericalJoint>()) {
            AddString(m, "usdPhysics:jointType", "spherical");
            AddJointBase(*j3, m);
            lusd::value::token axis;
            if (j3->axis.get_value(&axis) && !axis.str().empty()) {
                AddString(m, "usdPhysics:axis", axis.str());
            }
        } else if (const auto *j4 = prim.as<lusd::PhysicsFixedJoint>()) {
            AddString(m, "usdPhysics:jointType", "fixed");
            AddJointBase(*j4, m);
        } else if (const auto *j5 = prim.as<lusd::PhysicsDistanceJoint>()) {
            AddString(m, "usdPhysics:jointType", "distance");
            AddJointBase(*j5, m);
            float f = 0.0f;
            if (j5->minDistance.get_value(&f)) {
                AddFloat(m, "usdPhysics:lowerLimit", f);
            }
            if (j5->maxDistance.get_value(&f)) {
                AddFloat(m, "usdPhysics:upperLimit", f);
            }
        } else if (const auto *j6 = prim.as<lusd::PhysicsJoint>()) {
            AddString(m, "usdPhysics:jointType", "joint");
            AddJointBase(*j6, m);
        } else {
            return false;
        }
        mOut.joints.push_back(std::move(joint));
        return true;
    }

    const lusd::Stage &mStage;
    PhysicsData &mOut;
};

void ApplyMeta(aiNode *node, const std::vector<MetaEntry> &entries) {
    if (entries.empty()) {
        return;
    }
    if (node->mMetaData == nullptr) {
        node->mMetaData = new aiMetadata();
    }
    for (const MetaEntry &e : entries) {
        switch (e.kind) {
        case MetaEntry::Kind::Bool:
            node->mMetaData->Add(e.key, e.b);
            break;
        case MetaEntry::Kind::Float:
            node->mMetaData->Add(e.key, e.f);
            break;
        case MetaEntry::Kind::Vector:
            node->mMetaData->Add(e.key, e.v);
            break;
        case MetaEntry::Kind::String:
            node->mMetaData->Add(e.key, aiString(e.s));
            break;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Vertex attribute access with bounds checks

/// Read-only view of a Tydra VertexAttribute as `components` floats per item.
struct AttributeView {
    const float *data = nullptr;
    size_t count = 0;      ///< number of items
    unsigned components = 0;
    tydra::VertexVariability variability = tydra::VertexVariability::Vertex;
    const std::vector<uint32_t> *indices = nullptr;
    bool indexedPerPoint = false;

    bool valid() const { return data != nullptr && count > 0; }
};

/// Validates `attr` for a mesh with `numPoints` points, `numFaceVertices`
/// face-vertices and `numFaces` faces. Returns an invalid view (and logs)
/// when the attribute cannot be used safely.
AttributeView MakeView(const tydra::VertexAttribute &attr, const char *what, const std::string &meshName,
        size_t numPoints, size_t numFaceVertices, size_t numFaces, Diagnostics &diag) {
    AttributeView view;
    if (attr.empty()) {
        return view;
    }
    unsigned components = 0;
    switch (attr.format) {
    case tydra::VertexAttributeFormat::Float: components = 1; break;
    case tydra::VertexAttributeFormat::Vec2: components = 2; break;
    case tydra::VertexAttributeFormat::Vec3: components = 3; break;
    case tydra::VertexAttributeFormat::Vec4: components = 4; break;
    default:
        diag.warn(std::string("Unsupported ") + what + " format on mesh " + meshName + "; ignored.");
        return view;
    }
    if (attr.elementSize != 1 || (attr.stride != 0 && attr.stride != components * sizeof(float))) {
        diag.warn(std::string("Unsupported ") + what + " layout on mesh " + meshName + "; ignored.");
        return view;
    }
    const size_t count = attr.data.size() / (components * sizeof(float));
    size_t required = 0;
    switch (attr.variability) {
    case tydra::VertexVariability::Constant: required = 1; break;
    case tydra::VertexVariability::Uniform: required = numFaces; break;
    case tydra::VertexVariability::Varying:
    case tydra::VertexVariability::Vertex: required = numPoints; break;
    case tydra::VertexVariability::FaceVarying: required = numFaceVertices; break;
    case tydra::VertexVariability::Indexed:
        if (attr.indices.size() == numFaceVertices) {
            view.indexedPerPoint = false;
        } else if (attr.indices.size() == numPoints) {
            view.indexedPerPoint = true;
        } else {
            diag.warn(std::string("Indexed ") + what + " of mesh " + meshName + " has a mismatching index count; ignored.");
            return view;
        }
        view.indices = &attr.indices;
        required = 1;
        break;
    }
    if (count < required || count == 0) {
        diag.warn(std::string("Mesh ") + meshName + " has " + std::to_string(count) + " " + what +
                  " values but needs " + std::to_string(required) + "; ignored.");
        return view;
    }
    view.data = reinterpret_cast<const float *>(attr.data.data());
    view.count = count;
    view.components = components;
    view.variability = attr.variability;
    return view;
}

/// True when the attribute can be addressed by point index alone.
bool IsPointAddressable(const AttributeView &v) {
    if (!v.valid()) {
        return true;
    }
    switch (v.variability) {
    case tydra::VertexVariability::Constant:
    case tydra::VertexVariability::Varying:
    case tydra::VertexVariability::Vertex:
        return true;
    case tydra::VertexVariability::Indexed:
        return v.indexedPerPoint;
    default:
        return false;
    }
}

/// Returns the item for (point, face-vertex, face) or nullptr when out of range.
const float *Fetch(const AttributeView &v, size_t point, size_t faceVertex, size_t face) {
    size_t item = 0;
    switch (v.variability) {
    case tydra::VertexVariability::Constant: item = 0; break;
    case tydra::VertexVariability::Uniform: item = face; break;
    case tydra::VertexVariability::Varying:
    case tydra::VertexVariability::Vertex: item = point; break;
    case tydra::VertexVariability::FaceVarying: item = faceVertex; break;
    case tydra::VertexVariability::Indexed: {
        const size_t at = v.indexedPerPoint ? point : faceVertex;
        if (at >= v.indices->size()) {
            return nullptr;
        }
        item = (*v.indices)[at];
        break;
    }
    }
    if (item >= v.count) {
        return nullptr;
    }
    return v.data + item * v.components;
}

// ------------------------------------------------------------------------------------------------
// Scene builder

struct SkeletonInfo {
    std::vector<std::string> jointNames; ///< indexed by joint id
    std::vector<aiMatrix4x4> bindWorld;  ///< skeleton-space bind transforms
};

class SceneBuilder {
public:
    SceneBuilder(tydra::RenderScene &rs, aiScene *scene, PhysicsData &physics, Diagnostics &diag) :
            mRS(rs), mScene(scene), mPhysics(physics), mDiag(diag) {}

    void build() {
        buildSkeletons();
        buildMaterials();
        buildMeshes();
        buildCameras();
        buildLights();
        buildNodes();
        buildAnimations();
        finishMaterials();
        applySceneMeta();
    }

private:
    // --- skeletons ------------------------------------------------------------------------------
    void buildSkeletons() {
        mSkeletons.resize(mRS.skeletons.size());
        for (size_t s = 0; s < mRS.skeletons.size(); ++s) {
            const tydra::SkelHierarchy &skel = mRS.skeletons[s];
            SkeletonInfo &info = mSkeletons[s];
            std::vector<const tydra::SkelNode *> stack{ &skel.root_node };
            while (!stack.empty()) {
                const tydra::SkelNode *n = stack.back();
                stack.pop_back();
                if (n->joint_id >= 0 && n->joint_id < 1 << 24) {
                    const size_t id = static_cast<size_t>(n->joint_id);
                    if (info.jointNames.size() <= id) {
                        info.jointNames.resize(id + 1);
                        info.bindWorld.resize(id + 1);
                    }
                    info.jointNames[id] = n->joint_path;
                    info.bindWorld[id] = ToAiMatrix(n->bind_transform);
                }
                for (const tydra::SkelNode &c : n->children) {
                    stack.push_back(&c);
                }
            }
            for (size_t j = 0; j < info.bindWorld.size() && j < skel.bind_transforms.size(); ++j) {
                info.bindWorld[j] = ToAiMatrix(skel.bind_transforms[j]);
            }
        }
    }

    // --- materials -----------------------------------------------------------------------------
    void buildMaterials() {
        mMaterials.reserve(mRS.materials.size() + 1);
        for (const tydra::RenderMaterial &rm : mRS.materials) {
            mMaterials.push_back(convertMaterial(rm));
        }
    }

    unsigned defaultMaterial(const tydra::RenderMesh &mesh) {
        std::array<float, 4> key{ { 0.18f, 0.18f, 0.18f, 1.0f } };
        if (mesh.has_authored_displayColor) {
            key = { { mesh.displayColor[0], mesh.displayColor[1], mesh.displayColor[2], mesh.displayOpacity } };
        }
        auto it = mDefaultMaterials.find(key);
        if (it != mDefaultMaterials.end()) {
            return it->second;
        }
        auto *mat = new aiMaterial();
        const aiString name(mesh.has_authored_displayColor ? "displayColor" : AI_DEFAULT_MATERIAL_NAME);
        mat->AddProperty(&name, AI_MATKEY_NAME);
        const aiColor4D color(key[0], key[1], key[2], key[3]);
        mat->AddProperty(&color, 1, AI_MATKEY_COLOR_DIFFUSE);
        mat->AddProperty(&color, 1, AI_MATKEY_BASE_COLOR);
        if (key[3] < 1.0f) {
            mat->AddProperty(&key[3], 1, AI_MATKEY_OPACITY);
        }
        const unsigned index = static_cast<unsigned>(mMaterials.size());
        mMaterials.push_back(mat);
        mDefaultMaterials.emplace(key, index);
        return index;
    }

    void setTexture(aiMaterial *mat, aiTextureType type, int32_t textureId, unsigned &slotCounter) {
        (void)slotCounter;
        if (textureId < 0 || static_cast<size_t>(textureId) >= mRS.textures.size()) {
            if (textureId >= 0) {
                mDiag.warn("Material references texture " + std::to_string(textureId) + " which does not exist; ignored.");
            }
            return;
        }
        const tydra::UVTexture &tex = mRS.textures[static_cast<size_t>(textureId)];
        aiString path;
        const int64_t imageId = tex.texture_image_id;
        if (imageId >= 0 && static_cast<size_t>(imageId) < mRS.images.size()) {
            const int embedded = embeddedTexture(static_cast<size_t>(imageId));
            if (embedded >= 0) {
                path.Set("*" + std::to_string(embedded));
            } else {
                path.Set(mRS.images[static_cast<size_t>(imageId)].asset_identifier);
            }
        } else {
            return;
        }
        if (path.length == 0) {
            return;
        }
        const unsigned slot = mat->GetTextureCount(type);
        mat->AddProperty(&path, _AI_MATKEY_TEXTURE_BASE, type, slot);

        auto wrap = [](tydra::UVTexture::WrapMode w) {
            switch (w) {
            case tydra::UVTexture::WrapMode::REPEAT: return aiTextureMapMode_Wrap;
            case tydra::UVTexture::WrapMode::MIRROR: return aiTextureMapMode_Mirror;
            case tydra::UVTexture::WrapMode::CLAMP_TO_BORDER: return aiTextureMapMode_Decal;
            case tydra::UVTexture::WrapMode::CLAMP_TO_EDGE:
            default: return aiTextureMapMode_Clamp;
            }
        };
        const int ws = wrap(tex.wrapS);
        const int wt = wrap(tex.wrapT);
        mat->AddProperty(&ws, 1, _AI_MATKEY_MAPPINGMODE_U_BASE, type, slot);
        mat->AddProperty(&wt, 1, _AI_MATKEY_MAPPINGMODE_V_BASE, type, slot);
        if (tex.has_transform2d) {
            aiUVTransform transform;
            transform.mScaling = aiVector2D(tex.tx_scale[0], tex.tx_scale[1]);
            transform.mTranslation = aiVector2D(tex.tx_translation[0], tex.tx_translation[1]);
            // UsdTransform2d rotation is in degrees.
            transform.mRotation = tex.tx_rotation * static_cast<float>(AI_MATH_PI) / 180.0f;
            mat->AddProperty(&transform, 1, _AI_MATKEY_UVTRANSFORM_BASE, type, slot);
        }
        if (!tex.varname_uv.empty()) {
            mPendingUVSources.push_back({ mat, type, slot, tex.varname_uv });
        }
    }

    aiMaterial *convertMaterial(const tydra::RenderMaterial &rm) {
        auto *mat = new aiMaterial();
        const aiString name(rm.name);
        mat->AddProperty(&name, AI_MATKEY_NAME);
        const int shading = aiShadingMode_PBR_BRDF;
        mat->AddProperty(&shading, 1, AI_MATKEY_SHADING_MODEL);
        unsigned slots = 0;

        if (rm.surfaceShader.has_value()) {
            const tydra::PreviewSurfaceShader &s = *rm.surfaceShader;
            const aiColor4D base(s.diffuseColor.value[0], s.diffuseColor.value[1], s.diffuseColor.value[2], s.opacity.value);
            mat->AddProperty(&base, 1, AI_MATKEY_BASE_COLOR);
            mat->AddProperty(&base, 1, AI_MATKEY_COLOR_DIFFUSE);
            setTexture(mat, aiTextureType_BASE_COLOR, s.diffuseColor.texture_id, slots);
            setTexture(mat, aiTextureType_DIFFUSE, s.diffuseColor.texture_id, slots);

            const aiColor3D emissive = ToAiColor(s.emissiveColor.value);
            mat->AddProperty(&emissive, 1, AI_MATKEY_COLOR_EMISSIVE);
            setTexture(mat, aiTextureType_EMISSIVE, s.emissiveColor.texture_id, slots);

            if (s.useSpecularWorkflow) {
                const aiColor3D specular = ToAiColor(s.specularColor.value);
                mat->AddProperty(&specular, 1, AI_MATKEY_COLOR_SPECULAR);
                setTexture(mat, aiTextureType_SPECULAR, s.specularColor.texture_id, slots);
            } else {
                mat->AddProperty(&s.metallic.value, 1, AI_MATKEY_METALLIC_FACTOR);
                setTexture(mat, aiTextureType_METALNESS, s.metallic.texture_id, slots);
            }
            mat->AddProperty(&s.roughness.value, 1, AI_MATKEY_ROUGHNESS_FACTOR);
            setTexture(mat, aiTextureType_DIFFUSE_ROUGHNESS, s.roughness.texture_id, slots);

            mat->AddProperty(&s.opacity.value, 1, AI_MATKEY_OPACITY);
            setTexture(mat, aiTextureType_OPACITY, s.opacity.texture_id, slots);
            if (s.opacityThreshold.value > 0.0f) {
                const aiString mode("MASK");
                mat->AddProperty(&mode, AI_MATKEY_GLTF_ALPHAMODE);
                mat->AddProperty(&s.opacityThreshold.value, 1, AI_MATKEY_GLTF_ALPHACUTOFF);
            } else if (s.opacity.value < 1.0f || s.opacity.is_texture()) {
                const aiString mode("BLEND");
                mat->AddProperty(&mode, AI_MATKEY_GLTF_ALPHAMODE);
            }
            mat->AddProperty(&s.ior.value, 1, AI_MATKEY_REFRACTI);
            if (s.clearcoat.value > 0.0f || s.clearcoat.is_texture()) {
                mat->AddProperty(&s.clearcoat.value, 1, AI_MATKEY_CLEARCOAT_FACTOR);
                mat->AddProperty(&s.clearcoatRoughness.value, 1, AI_MATKEY_CLEARCOAT_ROUGHNESS_FACTOR);
            }
            setTexture(mat, aiTextureType_NORMALS, s.normal.texture_id, slots);
            setTexture(mat, aiTextureType_AMBIENT_OCCLUSION, s.occlusion.texture_id, slots);
            setTexture(mat, aiTextureType_LIGHTMAP, s.occlusion.texture_id, slots);
            setTexture(mat, aiTextureType_DISPLACEMENT, s.displacement.texture_id, slots);
        } else if (rm.openPBRShader.has_value()) {
            const tydra::OpenPBRSurfaceShader &s = *rm.openPBRShader;
            const float w = s.base_weight.value;
            const aiColor4D base(s.base_color.value[0] * w, s.base_color.value[1] * w, s.base_color.value[2] * w, s.opacity.value);
            mat->AddProperty(&base, 1, AI_MATKEY_BASE_COLOR);
            mat->AddProperty(&base, 1, AI_MATKEY_COLOR_DIFFUSE);
            setTexture(mat, aiTextureType_BASE_COLOR, s.base_color.texture_id, slots);
            setTexture(mat, aiTextureType_DIFFUSE, s.base_color.texture_id, slots);
            mat->AddProperty(&s.base_metalness.value, 1, AI_MATKEY_METALLIC_FACTOR);
            setTexture(mat, aiTextureType_METALNESS, s.base_metalness.texture_id, slots);
            mat->AddProperty(&s.specular_roughness.value, 1, AI_MATKEY_ROUGHNESS_FACTOR);
            setTexture(mat, aiTextureType_DIFFUSE_ROUGHNESS, s.specular_roughness.texture_id, slots);
            if (s.emission_luminance.value > 0.0f || s.emission_color.is_texture()) {
                const aiColor3D emissive = ToAiColor(s.emission_color.value);
                mat->AddProperty(&emissive, 1, AI_MATKEY_COLOR_EMISSIVE);
                mat->AddProperty(&s.emission_luminance.value, 1, AI_MATKEY_EMISSIVE_INTENSITY);
                setTexture(mat, aiTextureType_EMISSIVE, s.emission_color.texture_id, slots);
            } else {
                const aiColor3D black(0.0f, 0.0f, 0.0f);
                mat->AddProperty(&black, 1, AI_MATKEY_COLOR_EMISSIVE);
            }
            mat->AddProperty(&s.opacity.value, 1, AI_MATKEY_OPACITY);
            setTexture(mat, aiTextureType_OPACITY, s.opacity.texture_id, slots);
            if (s.opacity.value < 1.0f || s.opacity.is_texture()) {
                const aiString mode("BLEND");
                mat->AddProperty(&mode, AI_MATKEY_GLTF_ALPHAMODE);
            }
            mat->AddProperty(&s.specular_ior.value, 1, AI_MATKEY_REFRACTI);
            mat->AddProperty(&s.specular_weight.value, 1, AI_MATKEY_SPECULAR_FACTOR);
            const aiColor3D specular = ToAiColor(s.specular_color.value);
            mat->AddProperty(&specular, 1, AI_MATKEY_COLOR_SPECULAR);
            if (s.coat_weight.value > 0.0f) {
                mat->AddProperty(&s.coat_weight.value, 1, AI_MATKEY_CLEARCOAT_FACTOR);
                mat->AddProperty(&s.coat_roughness.value, 1, AI_MATKEY_CLEARCOAT_ROUGHNESS_FACTOR);
            }
            if (s.sheen_weight.value > 0.0f) {
                const float sw = s.sheen_weight.value;
                const aiColor3D sheen(s.sheen_color.value[0] * sw, s.sheen_color.value[1] * sw, s.sheen_color.value[2] * sw);
                mat->AddProperty(&sheen, 1, AI_MATKEY_SHEEN_COLOR_FACTOR);
                mat->AddProperty(&s.sheen_roughness.value, 1, AI_MATKEY_SHEEN_ROUGHNESS_FACTOR);
            }
            if (s.transmission_weight.value > 0.0f) {
                mat->AddProperty(&s.transmission_weight.value, 1, AI_MATKEY_TRANSMISSION_FACTOR);
            }
            setTexture(mat, aiTextureType_NORMALS, s.normal.texture_id, slots);
        } else {
            const aiColor4D grey(0.18f, 0.18f, 0.18f, 1.0f);
            mat->AddProperty(&grey, 1, AI_MATKEY_COLOR_DIFFUSE);
            mat->AddProperty(&grey, 1, AI_MATKEY_BASE_COLOR);
        }
        return mat;
    }

    /// Returns the aiTexture index for an image, creating it on first use,
    /// or -1 when the image bytes were not loaded.
    int embeddedTexture(size_t imageId) {
        auto it = mImageToTexture.find(imageId);
        if (it != mImageToTexture.end()) {
            return it->second;
        }
        int result = -1;
        tydra::TextureImage &img = mRS.images[imageId];
        if (img.buffer_id >= 0 && static_cast<size_t>(img.buffer_id) < mRS.buffers.size()) {
            tydra::BufferData &buf = mRS.buffers[static_cast<size_t>(img.buffer_id)];
            std::unique_ptr<aiTexture> tex(img.width <= 0 ? encodedTexture(img, buf) : decodedTexture(img, buf));
            // The bytes now live in the aiTexture.
            std::vector<uint8_t>().swap(buf.data);
            if (tex) {
                result = static_cast<int>(mTextures.size());
                mTextures.push_back(tex.release());
            }
        }
        mImageToTexture.emplace(imageId, result);
        return result;
    }

    static std::string FormatHint(const std::vector<uint8_t> &d, const std::string &name) {
        auto starts = [&d](const char *magic, size_t n, size_t at = 0) {
            return d.size() >= at + n && std::memcmp(d.data() + at, magic, n) == 0;
        };
        if (starts("\x89PNG", 4)) return "png";
        if (starts("\xFF\xD8\xFF", 3)) return "jpg";
        if (starts("RIFF", 4) && starts("WEBP", 4, 8)) return "webp";
        if (starts("\xABKTX 20\xBB", 8)) return "ktx2";
        if (starts("\x76\x2F\x31\x01", 4)) return "exr";
        if (starts("#?RADIANCE", 10) || starts("#?RGBE", 6)) return "hdr";
        if (starts("ftypavif", 8, 4)) return "avif";
        if (starts("BM", 2)) return "bmp";
        if (starts("GIF8", 4)) return "gif";
        if (starts("DDS ", 4)) return "dds";
        if (starts("II*\0", 4) || starts("MM\0*", 4)) return "tif";
        std::string ext = LowerExtension(name);
        if (ext == "jpeg") ext = "jpg";
        if (ext.size() >= HINTMAXTEXTURELEN) ext.clear();
        return ext;
    }

    aiTexture *encodedTexture(const tydra::TextureImage &img, const tydra::BufferData &buf) {
        if (buf.data.empty() || buf.data.size() > std::numeric_limits<unsigned int>::max()) {
            return nullptr;
        }
        auto *tex = new aiTexture();
        const size_t size = buf.data.size();
        tex->mWidth = static_cast<unsigned int>(size);
        tex->mHeight = 0;
        tex->pcData = new aiTexel[(size + sizeof(aiTexel) - 1) / sizeof(aiTexel)];
        std::memcpy(tex->pcData, buf.data.data(), size);
        const std::string hint = FormatHint(buf.data, img.asset_identifier);
        std::strncpy(tex->achFormatHint, hint.c_str(), HINTMAXTEXTURELEN - 1);
        tex->achFormatHint[HINTMAXTEXTURELEN - 1] = '\0';
        tex->mFilename.Set(NormalizePath(img.asset_identifier));
        return tex;
    }

    aiTexture *decodedTexture(const tydra::TextureImage &img, const tydra::BufferData &buf) {
        const int64_t w = img.width, h = img.height, c = img.channels;
        if (w <= 0 || h <= 0 || c <= 0 || c > 4 || w > 65536 || h > 65536) {
            mDiag.warn("Texture " + img.asset_identifier + " has unsupported dimensions; ignored.");
            return nullptr;
        }
        const size_t texels = static_cast<size_t>(w) * static_cast<size_t>(h);
        const bool isFloat = buf.componentType == tydra::ComponentType::Float;
        const bool isByte = buf.componentType == tydra::ComponentType::UInt8;
        const size_t compSize = isFloat ? sizeof(float) : 1;
        if ((!isFloat && !isByte) || buf.data.size() < texels * static_cast<size_t>(c) * compSize) {
            mDiag.warn("Texture " + img.asset_identifier + " has an unsupported texel format; ignored.");
            return nullptr;
        }
        auto *tex = new aiTexture();
        tex->mWidth = static_cast<unsigned int>(w);
        tex->mHeight = static_cast<unsigned int>(h);
        tex->pcData = new aiTexel[texels];
        tex->mFilename.Set(NormalizePath(img.asset_identifier));
        auto component = [&](size_t i, int k) -> uint8_t {
            const size_t at = i * static_cast<size_t>(c) + static_cast<size_t>(k);
            if (isByte) {
                return buf.data[at];
            }
            float f;
            std::memcpy(&f, buf.data.data() + at * sizeof(float), sizeof(float));
            f = std::min(1.0f, std::max(0.0f, f));
            return static_cast<uint8_t>(f * 255.0f + 0.5f);
        };
        for (size_t i = 0; i < texels; ++i) {
            aiTexel &t = tex->pcData[i];
            const uint8_t r = component(i, 0);
            t.r = r;
            t.g = c >= 3 ? component(i, 1) : r;
            t.b = c >= 3 ? component(i, 2) : r;
            t.a = c == 4 ? component(i, 3) : (c == 2 ? component(i, 1) : 255);
        }
        return tex;
    }

    void finishMaterials() {
        // UV sources: the channel of the named primvar on the first mesh using
        // the material (USD binds UV sets by name, assimp by index).
        for (const PendingUV &p : mPendingUVSources) {
            int channel = -1;
            auto it = mMaterialUVNames.find(p.mat);
            if (it != mMaterialUVNames.end()) {
                auto ch = it->second.find(p.varname);
                if (ch != it->second.end()) {
                    channel = ch->second;
                }
            }
            if (channel > 0) {
                mat_uvsrc(p.mat, p.type, p.slot, channel);
            }
        }
        for (aiMaterial *mat : mTwoSided) {
            const int one = 1;
            mat->AddProperty(&one, 1, AI_MATKEY_TWOSIDED);
        }
        if (mMaterials.empty()) {
            auto *mat = new aiMaterial();
            const aiString name(AI_DEFAULT_MATERIAL_NAME);
            mat->AddProperty(&name, AI_MATKEY_NAME);
            mMaterials.push_back(mat);
        }
        mScene->mNumMaterials = static_cast<unsigned>(mMaterials.size());
        mScene->mMaterials = new aiMaterial *[mMaterials.size()];
        std::copy(mMaterials.begin(), mMaterials.end(), mScene->mMaterials);
        mMaterials.clear();
        if (!mTextures.empty()) {
            mScene->mNumTextures = static_cast<unsigned>(mTextures.size());
            mScene->mTextures = new aiTexture *[mTextures.size()];
            std::copy(mTextures.begin(), mTextures.end(), mScene->mTextures);
            mTextures.clear();
        }
    }

    static void mat_uvsrc(aiMaterial *mat, aiTextureType type, unsigned slot, int channel) {
        mat->AddProperty(&channel, 1, _AI_MATKEY_UVWSRC_BASE, type, slot);
    }

    // --- meshes --------------------------------------------------------------------------------
    struct FaceGroup {
        unsigned material = 0;
        std::vector<uint32_t> faces;
    };

    void buildMeshes() {
        mMeshMap.resize(mRS.meshes.size());
        for (size_t i = 0; i < mRS.meshes.size(); ++i) {
            convertMesh(i);
            // Release the Tydra copy as soon as it has been mapped.
            tydra::RenderMesh &rm = mRS.meshes[i];
            rm = tydra::RenderMesh();
        }
        if (!mMeshes.empty()) {
            mScene->mNumMeshes = static_cast<unsigned>(mMeshes.size());
            mScene->mMeshes = new aiMesh *[mMeshes.size()];
            std::copy(mMeshes.begin(), mMeshes.end(), mScene->mMeshes);
            mMeshes.clear();
        }
    }

    void convertMesh(size_t meshIndex) {
        const tydra::RenderMesh &rm = mRS.meshes[meshIndex];
        const std::string &name = rm.prim_name;
        const std::vector<uint32_t> &fvi = rm.faceVertexIndices();
        const std::vector<uint32_t> &fvc = rm.faceVertexCounts();
        const size_t numPoints = rm.points.size();
        const size_t numFaces = fvc.size();
        if (numPoints == 0 || numFaces == 0) {
            return;
        }
        if (numPoints > std::numeric_limits<unsigned int>::max() / 2) {
            mDiag.warn("Mesh " + name + " is too large; skipped.");
            return;
        }

        // Face offsets, and validation of the topology.
        std::vector<uint32_t> faceStart(numFaces);
        {
            uint64_t offset = 0;
            for (size_t f = 0; f < numFaces; ++f) {
                faceStart[f] = static_cast<uint32_t>(offset);
                offset += fvc[f];
                if (offset > fvi.size()) {
                    mDiag.warn("Mesh " + name + " has inconsistent faceVertexCounts; skipped.");
                    return;
                }
            }
        }

        // Faces that can be emitted: at least three corners, all in range.
        std::vector<uint8_t> faceOk(numFaces, 1);
        size_t degenerate = 0, outOfRange = 0;
        for (size_t f = 0; f < numFaces; ++f) {
            const uint32_t n = fvc[f];
            if (n < 3) {
                faceOk[f] = 0;
                ++degenerate;
                continue;
            }
            for (uint32_t k = 0; k < n; ++k) {
                if (fvi[faceStart[f] + k] >= numPoints) {
                    faceOk[f] = 0;
                    ++outOfRange;
                    break;
                }
            }
        }
        if (degenerate) {
            mDiag.warn("Mesh " + name + ": skipped " + std::to_string(degenerate) + " face(s) with fewer than 3 vertices.");
        }
        if (outOfRange) {
            mDiag.warn("Mesh " + name + ": skipped " + std::to_string(outOfRange) + " face(s) with out-of-range vertex indices.");
        }

        // Per-face material (GeomSubsets with familyName materialBind).
        const unsigned baseMaterial = materialIndex(rm, rm.material_id);
        std::vector<unsigned> faceMaterial;
        if (!rm.material_subsetMap.empty()) {
            faceMaterial.assign(numFaces, baseMaterial);
            for (const auto &entry : rm.material_subsetMap) {
                const tydra::MaterialSubset &subset = entry.second;
                const unsigned m = materialIndex(rm, subset.material_id);
                for (int idx : subset.indices()) {
                    if (idx >= 0 && static_cast<size_t>(idx) < numFaces) {
                        faceMaterial[static_cast<size_t>(idx)] = m;
                    }
                }
            }
        }
        std::vector<FaceGroup> groups;
        if (faceMaterial.empty()) {
            groups.push_back({ baseMaterial, {} });
        } else {
            std::map<unsigned, size_t> groupOf;
            for (size_t f = 0; f < numFaces; ++f) {
                if (!faceOk[f]) {
                    continue;
                }
                auto it = groupOf.find(faceMaterial[f]);
                if (it == groupOf.end()) {
                    it = groupOf.emplace(faceMaterial[f], groups.size()).first;
                    groups.push_back({ faceMaterial[f], {} });
                }
                groups[it->second].faces.push_back(static_cast<uint32_t>(f));
            }
        }
        const bool wholeMesh = faceMaterial.empty();

        // Attributes.
        const size_t numFV = fvi.size();
        AttributeView normals = MakeView(rm.normals, "normal", name, numPoints, numFV, numFaces, mDiag);
        if (normals.valid() && normals.components != 3) {
            normals = AttributeView();
        }
        std::vector<std::pair<uint32_t, const tydra::VertexAttribute *>> uvSlots;
        for (const auto &uv : rm.texcoords) {
            uvSlots.emplace_back(uv.first, &uv.second);
        }
        std::sort(uvSlots.begin(), uvSlots.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        std::vector<AttributeView> uvs;
        std::vector<std::string> uvNames;
        size_t validUvSets = 0;
        for (const auto &slot : uvSlots) {
            AttributeView v = MakeView(*slot.second, "texcoord", name, numPoints, numFV, numFaces, mDiag);
            if (!v.valid() || v.components < 2) {
                continue;
            }
            if (++validUvSets > AI_MAX_NUMBER_OF_TEXTURECOORDS) {
                continue;
            }
            std::string uvName = slot.second->name;
            if (rm.texcoordSlotIdMap.count(uint64_t(slot.first))) {
                uvName = rm.texcoordSlotIdMap.at(uint64_t(slot.first));
            }
            uvs.push_back(v);
            uvNames.push_back(uvName);
        }
        if (validUvSets > AI_MAX_NUMBER_OF_TEXTURECOORDS) {
            mDiag.warn("Mesh " + name + " has " + std::to_string(validUvSets) + " UV sets; only the first " +
                       std::to_string(AI_MAX_NUMBER_OF_TEXTURECOORDS) + " are kept.");
        }
        AttributeView colors = MakeView(rm.vertex_colors, "color", name, numPoints, numFV, numFaces, mDiag);
        if (colors.valid() && colors.components < 3) {
            colors = AttributeView();
        }
        AttributeView opacities = MakeView(rm.vertex_opacities, "opacity", name, numPoints, numFV, numFaces, mDiag);
        // A constant colour carries no per-vertex information.
        if (colors.valid() && colors.variability == tydra::VertexVariability::Constant) {
            colors = AttributeView();
        }
        if (!colors.valid() || opacities.variability == tydra::VertexVariability::Constant) {
            opacities = AttributeView();
        }

        bool pointAddressable = IsPointAddressable(normals) && IsPointAddressable(colors) && IsPointAddressable(opacities);
        for (const AttributeView &uv : uvs) {
            pointAddressable = pointAddressable && IsPointAddressable(uv);
        }

        std::vector<unsigned> &outMeshes = mMeshMap[meshIndex];
        for (FaceGroup &group : groups) {
            if (wholeMesh) {
                group.faces.reserve(numFaces);
                for (size_t f = 0; f < numFaces; ++f) {
                    if (faceOk[f]) {
                        group.faces.push_back(static_cast<uint32_t>(f));
                    }
                }
            }
            if (group.faces.empty()) {
                continue;
            }
            aiMesh *mesh = buildGroup(rm, group, faceStart, numPoints, wholeMesh && degenerate == 0 && outOfRange == 0,
                    pointAddressable, normals, uvs, uvNames, colors, opacities);
            if (mesh != nullptr) {
                outMeshes.push_back(static_cast<unsigned>(mMeshes.size()));
                mMeshes.push_back(mesh);
            }
        }
    }

    unsigned materialIndex(const tydra::RenderMesh &rm, int materialId) {
        unsigned index;
        if (materialId >= 0 && static_cast<size_t>(materialId) < mRS.materials.size()) {
            index = static_cast<unsigned>(materialId);
        } else {
            if (materialId >= 0) {
                mDiag.warn("Mesh " + rm.prim_name + " references material " + std::to_string(materialId) + " which does not exist.");
            }
            index = defaultMaterial(rm);
        }
        if (rm.doubleSided && index < mMaterials.size()) {
            mTwoSided.insert(mMaterials[index]);
        }
        return index;
    }

    aiMesh *buildGroup(const tydra::RenderMesh &rm, const FaceGroup &group, const std::vector<uint32_t> &faceStart,
            size_t numPoints, bool identity, bool pointAddressable, const AttributeView &normals,
            const std::vector<AttributeView> &uvs, const std::vector<std::string> &uvNames,
            const AttributeView &colors, const AttributeView &opacities) {
        const std::vector<uint32_t> &fvi = rm.faceVertexIndices();
        const std::vector<uint32_t> &fvc = rm.faceVertexCounts();

        // Output vertices: either points (shared between faces) or one vertex
        // per face corner when some attribute varies per face-vertex.
        std::vector<uint32_t> vtxPoint;     // output vertex -> point (empty when identity)
        std::vector<uint32_t> vtxCorner;    // output vertex -> face-vertex index
        std::vector<uint32_t> vtxFace;      // output vertex -> face
        std::vector<int32_t> pointRemap;    // point -> output vertex
        size_t numVertices = 0;
        const bool useIdentity = pointAddressable && identity;
        if (useIdentity) {
            numVertices = numPoints;
        } else if (pointAddressable) {
            pointRemap.assign(numPoints, -1);
            for (uint32_t f : group.faces) {
                for (uint32_t k = 0; k < fvc[f]; ++k) {
                    const uint32_t p = fvi[faceStart[f] + k];
                    if (pointRemap[p] < 0) {
                        pointRemap[p] = static_cast<int32_t>(vtxPoint.size());
                        vtxPoint.push_back(p);
                    }
                }
            }
            numVertices = vtxPoint.size();
        } else {
            size_t corners = 0;
            for (uint32_t f : group.faces) {
                corners += fvc[f];
            }
            vtxPoint.reserve(corners);
            vtxCorner.reserve(corners);
            vtxFace.reserve(corners);
            for (uint32_t f : group.faces) {
                for (uint32_t k = 0; k < fvc[f]; ++k) {
                    vtxPoint.push_back(fvi[faceStart[f] + k]);
                    vtxCorner.push_back(faceStart[f] + k);
                    vtxFace.push_back(f);
                }
            }
            numVertices = vtxPoint.size();
        }
        if (numVertices == 0 || numVertices > std::numeric_limits<unsigned int>::max() / 2) {
            return nullptr;
        }

        std::unique_ptr<aiMesh> mesh(new aiMesh());
        mesh->mName.Set(rm.prim_name.empty() ? LeafName(rm.abs_path) : rm.prim_name);
        mesh->mMaterialIndex = group.material;
        mesh->mNumVertices = static_cast<unsigned>(numVertices);

        auto pointOf = [&](size_t v) -> size_t { return useIdentity ? v : vtxPoint[v]; };
        auto cornerOf = [&](size_t v) -> size_t { return vtxCorner.empty() ? 0 : vtxCorner[v]; };
        auto faceOf = [&](size_t v) -> size_t { return vtxFace.empty() ? 0 : vtxFace[v]; };

        mesh->mVertices = new aiVector3D[numVertices];
        for (size_t v = 0; v < numVertices; ++v) {
            const auto &p = rm.points[pointOf(v)];
            mesh->mVertices[v].Set(p[0], p[1], p[2]);
        }
        if (normals.valid()) {
            mesh->mNormals = new aiVector3D[numVertices];
            for (size_t v = 0; v < numVertices; ++v) {
                const float *n = Fetch(normals, pointOf(v), cornerOf(v), faceOf(v));
                mesh->mNormals[v] = n ? aiVector3D(n[0], n[1], n[2]) : aiVector3D(0.0f, 0.0f, 0.0f);
            }
        }
        for (size_t t = 0; t < uvs.size(); ++t) {
            mesh->mTextureCoords[t] = new aiVector3D[numVertices];
            mesh->mNumUVComponents[t] = 2;
            for (size_t v = 0; v < numVertices; ++v) {
                const float *uv = Fetch(uvs[t], pointOf(v), cornerOf(v), faceOf(v));
                mesh->mTextureCoords[t][v] = uv ? aiVector3D(uv[0], uv[1], 0.0f) : aiVector3D(0.0f, 0.0f, 0.0f);
            }
            if (!uvNames[t].empty()) {
                mesh->SetTextureCoordsName(static_cast<unsigned>(t), aiString(uvNames[t]));
            }
        }
        if (colors.valid()) {
            mesh->mColors[0] = new aiColor4D[numVertices];
            for (size_t v = 0; v < numVertices; ++v) {
                const float *c = Fetch(colors, pointOf(v), cornerOf(v), faceOf(v));
                const float *o = opacities.valid() ? Fetch(opacities, pointOf(v), cornerOf(v), faceOf(v)) : nullptr;
                float alpha = o ? o[0] : (c && colors.components == 4 ? c[3] : 1.0f);
                mesh->mColors[0][v] = c ? aiColor4D(c[0], c[1], c[2], alpha) : aiColor4D(1.0f, 1.0f, 1.0f, 1.0f);
            }
        }

        // Faces.
        mesh->mNumFaces = static_cast<unsigned>(group.faces.size());
        mesh->mFaces = new aiFace[group.faces.size()];
        unsigned primitiveTypes = 0;
        size_t corner = 0;
        for (size_t i = 0; i < group.faces.size(); ++i) {
            const uint32_t f = group.faces[i];
            const uint32_t n = fvc[f];
            aiFace &face = mesh->mFaces[i];
            face.mNumIndices = n;
            face.mIndices = new unsigned int[n];
            for (uint32_t k = 0; k < n; ++k) {
                const uint32_t p = fvi[faceStart[f] + k];
                if (useIdentity) {
                    face.mIndices[k] = p;
                } else if (pointAddressable) {
                    face.mIndices[k] = static_cast<unsigned>(pointRemap[p]);
                } else {
                    face.mIndices[k] = static_cast<unsigned>(corner++);
                }
            }
            primitiveTypes |= n == 3 ? aiPrimitiveType_TRIANGLE : aiPrimitiveType_POLYGON;
        }
        mesh->mPrimitiveTypes = primitiveTypes;

        // UV-set names per material, for the texture UV source lookup.
        if (group.material < mMaterials.size()) {
            auto &names = mMaterialUVNames[mMaterials[group.material]];
            for (size_t t = 0; t < uvNames.size(); ++t) {
                names.emplace(uvNames[t], static_cast<int>(t));
            }
        }

        buildBones(rm, mesh.get(), numPoints, pointOf);
        buildBlendShapes(rm, mesh.get(), numPoints, pointOf, cornerOf, faceOf, normals);
        return mesh.release();
    }

    template <typename PointOf>
    void buildBones(const tydra::RenderMesh &rm, aiMesh *mesh, size_t numPoints, PointOf pointOf) {
        if (rm.skel_id < 0) {
            return;
        }
        if (static_cast<size_t>(rm.skel_id) >= mSkeletons.size()) {
            mDiag.warn("Mesh " + rm.prim_name + " references a missing skeleton; skinning ignored.");
            return;
        }
        const SkeletonInfo &skel = mSkeletons[static_cast<size_t>(rm.skel_id)];
        const tydra::JointAndWeight &jw = rm.joint_and_weights;
        const size_t numBones = skel.jointNames.size();
        const int es = jw.elementSize;
        if (numBones == 0 || es <= 0) {
            return;
        }
        const size_t elementSize = static_cast<size_t>(es);
        if (jw.jointIndices.size() != numPoints * elementSize || jw.jointWeights.size() != numPoints * elementSize) {
            mDiag.warn("Mesh " + rm.prim_name + " has skin weights that do not match its points; skinning ignored.");
            return;
        }
        const aiMatrix4x4 geomBind = ToAiMatrix(jw.geomBindTransform);

        // Two passes: count, then fill, so each bone gets one exact allocation.
        std::vector<unsigned> counts(numBones, 0);
        size_t badIndices = 0;
        const size_t numVertices = mesh->mNumVertices;
        for (size_t v = 0; v < numVertices; ++v) {
            const size_t base = pointOf(v) * elementSize;
            for (size_t k = 0; k < elementSize; ++k) {
                const float w = jw.jointWeights[base + k];
                if (!(w > 0.0f)) {
                    continue;
                }
                const int j = jw.jointIndices[base + k];
                if (j < 0 || static_cast<size_t>(j) >= numBones) {
                    ++badIndices;
                    continue;
                }
                ++counts[static_cast<size_t>(j)];
            }
        }
        if (badIndices) {
            mDiag.warn("Mesh " + rm.prim_name + ": ignored " + std::to_string(badIndices) + " skin weight(s) with out-of-range joint indices.");
        }
        mesh->mNumBones = static_cast<unsigned>(numBones);
        mesh->mBones = new aiBone *[numBones];
        for (size_t b = 0; b < numBones; ++b) {
            auto *bone = new aiBone();
            bone->mName.Set(skel.jointNames[b]);
            aiMatrix4x4 invBind = skel.bindWorld[b];
            invBind.Inverse();
            // USD: p_skel = p_mesh * geomBind * inverse(bind) * joint (row vectors).
            bone->mOffsetMatrix = invBind * geomBind;
            bone->mNumWeights = counts[b];
            bone->mWeights = counts[b] ? new aiVertexWeight[counts[b]] : nullptr;
            mesh->mBones[b] = bone;
            counts[b] = 0;
        }
        for (size_t v = 0; v < numVertices; ++v) {
            const size_t base = pointOf(v) * elementSize;
            for (size_t k = 0; k < elementSize; ++k) {
                const float w = jw.jointWeights[base + k];
                const int j = jw.jointIndices[base + k];
                if (!(w > 0.0f) || j < 0 || static_cast<size_t>(j) >= numBones) {
                    continue;
                }
                aiBone *bone = mesh->mBones[j];
                bone->mWeights[counts[static_cast<size_t>(j)]++] = aiVertexWeight(static_cast<unsigned>(v), w);
            }
        }
    }

    template <typename PointOf, typename CornerOf, typename FaceOf>
    void buildBlendShapes(const tydra::RenderMesh &rm, aiMesh *mesh, size_t numPoints, PointOf pointOf,
            CornerOf cornerOf, FaceOf faceOf, const AttributeView &normals) {
        (void)cornerOf;
        (void)faceOf;
        (void)normals;
        if (rm.targets.empty()) {
            return;
        }
        std::vector<aiAnimMesh *> animMeshes;
        std::vector<aiVector3D> offsets;
        std::vector<aiVector3D> normalOffsets;
        bool inbetweens = false;
        for (const auto &entry : rm.targets) {
            const tydra::ShapeTarget &target = entry.second;
            inbetweens = inbetweens || !target.inbetweens.empty();
            // Tydra gives dense shapes (no authored pointIndices) the indices
            // 0..N-1 and drops shapes whose offsets do not fit the points.
            const size_t count = target.pointOffsets.size();
            if (target.pointIndices.size() != count) {
                mDiag.warn("Blend shape " + entry.first + " of mesh " + rm.prim_name + " has mismatching pointIndices and offsets; skipped.");
                continue;
            }
            const bool hasNormals = mesh->HasNormals() && target.normalOffsets.size() == count;
            offsets.assign(numPoints, aiVector3D(0.0f, 0.0f, 0.0f));
            if (hasNormals) {
                normalOffsets.assign(numPoints, aiVector3D(0.0f, 0.0f, 0.0f));
            }
            size_t bad = 0;
            for (size_t i = 0; i < count; ++i) {
                const size_t p = target.pointIndices[i];
                if (p >= numPoints) {
                    ++bad;
                    continue;
                }
                const auto &o = target.pointOffsets[i];
                offsets[p] = aiVector3D(o[0], o[1], o[2]);
                if (hasNormals) {
                    const auto &n = target.normalOffsets[i];
                    normalOffsets[p] = aiVector3D(n[0], n[1], n[2]);
                }
            }
            if (bad) {
                mDiag.warn("Blend shape " + entry.first + " of mesh " + rm.prim_name + ": ignored " + std::to_string(bad) + " out-of-range point index(es).");
            }
            aiAnimMesh *anim = aiCreateAnimMesh(mesh, true, hasNormals, false, false, false);
            anim->mName.Set(entry.first);
            anim->mWeight = 0.0f;
            for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
                const size_t p = pointOf(v);
                anim->mVertices[v] += offsets[p];
                if (hasNormals) {
                    anim->mNormals[v] += normalOffsets[p];
                    anim->mNormals[v].NormalizeSafe();
                }
            }
            animMeshes.push_back(anim);
        }
        if (inbetweens) {
            mDiag.warn("Mesh " + rm.prim_name + ": blend shape in-betweens are not supported and were ignored.");
        }
        if (!animMeshes.empty()) {
            mesh->mNumAnimMeshes = static_cast<unsigned>(animMeshes.size());
            mesh->mAnimMeshes = new aiAnimMesh *[animMeshes.size()];
            std::copy(animMeshes.begin(), animMeshes.end(), mesh->mAnimMeshes);
            mesh->mMethod = aiMorphingMethod_MORPH_RELATIVE;
            // Remember the target order for the weight animation.
            std::vector<std::string> &order = mMorphTargets[mesh];
            for (aiAnimMesh *a : animMeshes) {
                order.emplace_back(a->mName.C_Str());
            }
        }
    }

    // --- cameras and lights --------------------------------------------------------------------
    void buildCameras() {
        if (mRS.cameras.empty()) {
            return;
        }
        mScene->mNumCameras = static_cast<unsigned>(mRS.cameras.size());
        mScene->mCameras = new aiCamera *[mRS.cameras.size()];
        for (size_t i = 0; i < mRS.cameras.size(); ++i) {
            const tydra::RenderCamera &rc = mRS.cameras[i];
            auto *cam = new aiCamera();
            cam->mName.Set(rc.name);
            // Camera space: looks down -Z with +Y up, like assimp; the node
            // carries the transform.
            cam->mPosition = aiVector3D(0.0f, 0.0f, 0.0f);
            cam->mLookAt = aiVector3D(0.0f, 0.0f, -1.0f);
            cam->mUp = aiVector3D(0.0f, 1.0f, 0.0f);
            cam->mClipPlaneNear = rc.znear;
            cam->mClipPlaneFar = rc.zfar;
            if (rc.verticalAperture > 0.0f) {
                cam->mAspect = rc.horizontalAperture / rc.verticalAperture;
            }
            if (rc.projection == lusd::GeomCamera::Projection::Orthographic) {
                cam->mHorizontalFOV = 0.0f;
                // Apertures are in tenths of a scene unit; assimp wants the half width.
                cam->mOrthographicWidth = rc.horizontalAperture / 10.0f / 2.0f;
            } else if (rc.focalLength > 0.0f) {
                cam->mHorizontalFOV = 2.0f * std::atan(0.5f * rc.horizontalAperture / rc.focalLength);
            }
            mScene->mCameras[i] = cam;
        }
    }

    void buildLights() {
        std::vector<aiLight *> lights;
        for (const tydra::RenderLight &rl : mRS.lights) {
            auto *light = new aiLight();
            light->mName.Set(rl.name);
            light->mPosition = aiVector3D(0.0f, 0.0f, 0.0f);
            light->mDirection = aiVector3D(0.0f, 0.0f, -1.0f);
            light->mUp = aiVector3D(0.0f, 1.0f, 0.0f);
            const float scale = rl.intensity * std::pow(2.0f, rl.exposure);
            const aiColor3D color(rl.color[0] * scale, rl.color[1] * scale, rl.color[2] * scale);
            light->mColorDiffuse = color;
            light->mColorSpecular = color;
            light->mAttenuationConstant = 1.0f;
            light->mAttenuationLinear = 0.0f;
            light->mAttenuationQuadratic = 0.0f;
            switch (rl.type) {
            case tydra::RenderLight::Type::Distant:
                light->mType = aiLightSource_DIRECTIONAL;
                break;
            case tydra::RenderLight::Type::Dome:
                light->mType = aiLightSource_AMBIENT;
                light->mColorAmbient = color;
                break;
            case tydra::RenderLight::Type::Rect:
                light->mType = aiLightSource_AREA;
                light->mSize = aiVector2D(rl.width, rl.height);
                break;
            case tydra::RenderLight::Type::Disk:
                light->mType = aiLightSource_AREA;
                light->mSize = aiVector2D(2.0f * rl.radius, 2.0f * rl.radius);
                break;
            case tydra::RenderLight::Type::Point:
            case tydra::RenderLight::Type::Sphere:
            case tydra::RenderLight::Type::Cylinder:
            default:
                if (rl.shapingConeAngle < 90.0f) {
                    light->mType = aiLightSource_SPOT;
                    light->mAngleOuterCone = rl.shapingConeAngle * static_cast<float>(AI_MATH_PI) / 180.0f;
                    light->mAngleInnerCone = light->mAngleOuterCone * (1.0f - std::min(1.0f, std::max(0.0f, rl.shapingConeSoftness)));
                } else {
                    light->mType = aiLightSource_POINT;
                }
                // Physical inverse-square falloff.
                light->mAttenuationConstant = 0.0f;
                light->mAttenuationQuadratic = 1.0f;
                break;
            }
            mLightIndex.emplace(rl.abs_path, lights.size());
            lights.push_back(light);
        }
        if (!lights.empty()) {
            mScene->mNumLights = static_cast<unsigned>(lights.size());
            mScene->mLights = new aiLight *[lights.size()];
            std::copy(lights.begin(), lights.end(), mScene->mLights);
        }
    }

    // --- nodes ---------------------------------------------------------------------------------
    void buildNodes() {
        std::unique_ptr<aiNode> root;
        if (mRS.nodes.size() == 1) {
            root.reset(convertNode(mRS.nodes[0], nullptr, aiMatrix4x4()));
        } else {
            root.reset(new aiNode("root"));
            std::vector<aiNode *> children;
            for (const tydra::Node &n : mRS.nodes) {
                if (isShadingNode(n)) {
                    skipNode(n);
                    continue;
                }
                children.push_back(convertNode(n, root.get(), aiMatrix4x4()));
            }
            attachChildren(root.get(), children);
        }
        mScene->mRootNode = root.release();
        attachSkeletons();
        attachInstances();
        attachPhysics();
        disambiguateCameraAndLightNodes();
    }

    static void collectNodes(aiNode *node, std::vector<aiNode *> &out) {
        out.push_back(node);
        for (unsigned i = 0; i < node->mNumChildren; ++i) {
            collectNodes(node->mChildren[i], out);
        }
    }

    /// assimp binds cameras and lights to nodes by name, so those names must
    /// be unique in the node graph. Cameras and lights keep their prim name;
    /// another node with the same name (Blender writes Xform "/Camera" over
    /// Camera "/Camera/Camera") gets a numeric suffix. Only a second camera or
    /// light with an already used name is renamed itself.
    void disambiguateCameraAndLightNodes() {
        if (mScene->mNumCameras == 0 && mScene->mNumLights == 0) {
            return;
        }
        std::vector<aiNode *> nodes;
        collectNodes(mScene->mRootNode, nodes);
        std::set<std::string> used;
        for (const aiNode *n : nodes) {
            used.insert(n->mName.C_Str());
        }
        auto uniqueName = [&](const std::string &base) {
            for (unsigned n = 1;; ++n) {
                std::string candidate = base + "_" + std::to_string(n);
                if (used.insert(candidate).second) {
                    return candidate;
                }
            }
        };
        std::set<const aiNode *> bound;
        std::set<std::string> claimed;
        auto bind = [&](aiString &objName, const std::string &absPath) {
            auto it = mNodeByPath.find(absPath);
            if (it == mNodeByPath.end()) {
                return;
            }
            aiNode *node = it->second;
            if (!claimed.insert(node->mName.C_Str()).second) {
                const std::string renamed = uniqueName(node->mName.C_Str());
                mDiag.warn("camera/light name '" + std::string(node->mName.C_Str()) + "' of " + absPath + " is not unique; renamed to '" + renamed + "'");
                node->mName.Set(renamed);
                claimed.insert(renamed);
            }
            bound.insert(node);
            objName = node->mName;
        };
        for (unsigned i = 0; i < mScene->mNumCameras && i < mRS.cameras.size(); ++i) {
            bind(mScene->mCameras[i]->mName, mRS.cameras[i].abs_path);
        }
        for (unsigned i = 0; i < mScene->mNumLights && i < mRS.lights.size(); ++i) {
            bind(mScene->mLights[i]->mName, mRS.lights[i].abs_path);
        }
        for (aiNode *n : nodes) {
            if (bound.count(n) == 0 && claimed.count(n->mName.C_Str()) != 0) {
                n->mName.Set(uniqueName(n->mName.C_Str()));
            }
        }
    }

    static void attachChildren(aiNode *parent, std::vector<aiNode *> &children) {
        if (children.empty()) {
            return;
        }
        aiNode **merged = new aiNode *[parent->mNumChildren + children.size()];
        for (unsigned i = 0; i < parent->mNumChildren; ++i) {
            merged[i] = parent->mChildren[i];
        }
        for (size_t i = 0; i < children.size(); ++i) {
            children[i]->mParent = parent;
            merged[parent->mNumChildren + i] = children[i];
        }
        delete[] parent->mChildren;
        parent->mChildren = merged;
        parent->mNumChildren += static_cast<unsigned>(children.size());
        children.clear();
    }

    /// Material, Shader and NodeGraph prims are not scene objects; their
    /// content is already in mMaterials.
    static bool isShadingNode(const tydra::Node &n) {
        return n.category == tydra::NodeCategory::Material;
    }

    /// Keeps the pre-order numbering (animation targets) for a skipped subtree.
    void skipNode(const tydra::Node &n) {
        mNodeOrder.push_back(nullptr);
        for (const tydra::Node &c : n.children) {
            skipNode(c);
        }
    }

    /// Converts a Tydra node subtree. Ownership: the returned node owns its
    /// children as they are attached, so an exception cannot leak or
    /// double-free (the caller owns the returned node).
    aiNode *convertNode(const tydra::Node &n, aiNode *parent, const aiMatrix4x4 &parentGlobal) {
        std::unique_ptr<aiNode> node(new aiNode(n.prim_name));
        node->mParent = parent;
        aiMatrix4x4 global;
        if (n.has_resetXform) {
            global = ToAiMatrix(n.global_matrix);
            aiMatrix4x4 inv = parentGlobal;
            inv.Inverse();
            node->mTransformation = inv * global;
        } else {
            node->mTransformation = ToAiMatrix(n.local_matrix);
            global = parentGlobal * node->mTransformation;
        }
        mNodeOrder.push_back(node.get());
        if (!n.abs_path.empty()) {
            mNodeByPath.emplace(n.abs_path, node.get());
        }
        if (n.nodeType == tydra::NodeType::Mesh && n.id >= 0 && static_cast<size_t>(n.id) < mMeshMap.size()) {
            const std::vector<unsigned> &meshes = mMeshMap[static_cast<size_t>(n.id)];
            if (!meshes.empty()) {
                node->mNumMeshes = static_cast<unsigned>(meshes.size());
                node->mMeshes = new unsigned[meshes.size()];
                std::copy(meshes.begin(), meshes.end(), node->mMeshes);
            }
        }
        std::vector<aiNode *> children;
        children.reserve(n.children.size());
        try {
            for (const tydra::Node &c : n.children) {
                if (isShadingNode(c)) {
                    skipNode(c);
                    continue;
                }
                children.push_back(convertNode(c, node.get(), global));
            }
        } catch (...) {
            for (aiNode *c : children) {
                delete c;
            }
            throw;
        }
        attachChildren(node.get(), children);
        return node.release();
    }

    void attachSkeletons() {
        for (size_t s = 0; s < mRS.skeletons.size(); ++s) {
            const tydra::SkelHierarchy &skel = mRS.skeletons[s];
            aiNode *parent = nullptr;
            auto it = mNodeByPath.find(skel.abs_path);
            if (it != mNodeByPath.end()) {
                parent = it->second;
            } else {
                parent = mScene->mRootNode;
            }
            std::vector<aiNode *> children{ convertJoint(skel.root_node, parent, 0) };
            attachChildren(parent, children);
        }
    }

    aiNode *convertJoint(const tydra::SkelNode &joint, aiNode *parent, int depth) {
        std::unique_ptr<aiNode> node(new aiNode(joint.joint_path));
        node->mParent = parent;
        node->mTransformation = ToAiMatrix(joint.rest_transform);
        if (depth > 4096) {
            return node.release();
        }
        std::vector<aiNode *> children;
        try {
            for (const tydra::SkelNode &c : joint.children) {
                children.push_back(convertJoint(c, node.get(), depth + 1));
            }
        } catch (...) {
            for (aiNode *c : children) {
                delete c;
            }
            throw;
        }
        attachChildren(node.get(), children);
        return node.release();
    }

    /// Point-instancer instances share the prototype's aiMesh.
    void attachInstances() {
        if (mRS.instances.empty()) {
            return;
        }
        aiMatrix4x4 rootInverse = mScene->mRootNode->mTransformation;
        rootInverse.Inverse();
        std::vector<aiNode *> children;
        for (const tydra::RenderInstance &inst : mRS.instances) {
            if (!inst.visible || inst.mesh_id < 0 || static_cast<size_t>(inst.mesh_id) >= mMeshMap.size()) {
                continue;
            }
            const std::vector<unsigned> &meshes = mMeshMap[static_cast<size_t>(inst.mesh_id)];
            if (meshes.empty()) {
                continue;
            }
            auto *node = new aiNode(inst.prim_name);
            node->mTransformation = rootInverse * ToAiMatrix(inst.global_matrix);
            node->mNumMeshes = static_cast<unsigned>(meshes.size());
            node->mMeshes = new unsigned[meshes.size()];
            std::copy(meshes.begin(), meshes.end(), node->mMeshes);
            children.push_back(node);
        }
        attachChildren(mScene->mRootNode, children);
    }

    void attachPhysics() {
        for (const auto &entry : mPhysics.primMeta) {
            auto it = mNodeByPath.find(entry.first);
            if (it != mNodeByPath.end()) {
                ApplyMeta(it->second, entry.second);
            }
        }
        for (const PhysicsJointPrim &joint : mPhysics.joints) {
            auto it = mNodeByPath.find(joint.path);
            aiNode *node = nullptr;
            if (it != mNodeByPath.end()) {
                node = it->second;
            } else {
                auto parentIt = mNodeByPath.find(ParentPath(joint.path));
                aiNode *parent = parentIt != mNodeByPath.end() ? parentIt->second : mScene->mRootNode;
                node = new aiNode(LeafName(joint.path));
                std::vector<aiNode *> children{ node };
                attachChildren(parent, children);
                mNodeByPath.emplace(joint.path, node);
            }
            ApplyMeta(node, joint.meta);
        }
    }

    // --- animations ----------------------------------------------------------------------------
    struct NodeTrack {
        std::vector<aiVectorKey> position, scaling;
        std::vector<aiQuatKey> rotation;
    };

    static bool SamplerValues(const tydra::KeyframeSampler &s, size_t components, std::vector<std::pair<float, const float *>> &out) {
        const size_t keys = s.times.size();
        if (keys == 0 || components == 0) {
            return false;
        }
        size_t stride = components;
        size_t offset = 0;
        if (s.interpolation == tydra::AnimationInterpolation::CubicSpline) {
            stride = components * 3;
            offset = components; // [in-tangent, value, out-tangent]
        }
        if (s.values.size() != keys * stride) {
            return false;
        }
        out.clear();
        out.reserve(keys);
        for (size_t k = 0; k < keys; ++k) {
            out.emplace_back(s.times[k], s.values.data() + k * stride + offset);
        }
        return true;
    }

    void buildAnimations() {
        if (mRS.animations.empty()) {
            return;
        }
        // Morph targets per node, for weight channels.
        std::vector<aiAnimation *> anims;
        const double tps = mRS.meta.timeCodesPerSecond > 0.0 ? mRS.meta.timeCodesPerSecond : 24.0;
        std::vector<std::pair<float, const float *>> keys;
        for (const tydra::AnimationClip &clip : mRS.animations) {
            std::map<std::string, NodeTrack> tracks;
            std::map<aiNode *, std::vector<aiMeshMorphKey>> morphs;
            double maxTime = 0.0;
            size_t skipped = 0;
            for (const tydra::AnimationChannel &ch : clip.channels) {
                if (ch.sampler < 0 || static_cast<size_t>(ch.sampler) >= clip.samplers.size()) {
                    ++skipped;
                    continue;
                }
                const tydra::KeyframeSampler &sampler = clip.samplers[static_cast<size_t>(ch.sampler)];
                if (ch.path == tydra::AnimationPath::Weights) {
                    aiNode *node = channelNode(ch);
                    if (node == nullptr || !addMorphKeys(node, ch, sampler, morphs[node], maxTime)) {
                        ++skipped;
                    }
                    continue;
                }
                if (ch.path == tydra::AnimationPath::CustomProperty) {
                    continue;
                }
                std::string target;
                if (ch.target_type == tydra::ChannelTargetType::SkeletonJoint) {
                    if (ch.skeleton_id >= 0 && static_cast<size_t>(ch.skeleton_id) < mSkeletons.size() && ch.joint_id >= 0 &&
                            static_cast<size_t>(ch.joint_id) < mSkeletons[static_cast<size_t>(ch.skeleton_id)].jointNames.size()) {
                        target = mSkeletons[static_cast<size_t>(ch.skeleton_id)].jointNames[static_cast<size_t>(ch.joint_id)];
                    }
                } else if (aiNode *node = channelNode(ch)) {
                    target = node->mName.C_Str();
                }
                if (target.empty()) {
                    ++skipped;
                    continue;
                }
                const size_t components = ch.path == tydra::AnimationPath::Rotation ? 4 : 3;
                if (!SamplerValues(sampler, components, keys)) {
                    ++skipped;
                    continue;
                }
                NodeTrack &track = tracks[target];
                for (const auto &k : keys) {
                    const double t = k.first;
                    maxTime = std::max(maxTime, t);
                    const float *v = k.second;
                    if (ch.path == tydra::AnimationPath::Translation) {
                        track.position.emplace_back(t, aiVector3D(v[0], v[1], v[2]));
                    } else if (ch.path == tydra::AnimationPath::Scale) {
                        track.scaling.emplace_back(t, aiVector3D(v[0], v[1], v[2]));
                    } else {
                        // Tydra stores quaternions as (x, y, z, w).
                        track.rotation.emplace_back(t, aiQuaternion(v[3], v[0], v[1], v[2]));
                    }
                }
            }
            if (skipped) {
                mDiag.warn("Animation " + clip.name + ": skipped " + std::to_string(skipped) + " channel(s) with invalid targets or samplers.");
            }
            if (tracks.empty() && morphs.empty()) {
                continue;
            }
            std::unique_ptr<aiAnimation> anim(new aiAnimation());
            anim->mName.Set(clip.name.empty() ? clip.prim_name : clip.name);
            anim->mTicksPerSecond = tps;
            anim->mDuration = maxTime;
            if (!tracks.empty()) {
                anim->mNumChannels = static_cast<unsigned>(tracks.size());
                anim->mChannels = new aiNodeAnim *[tracks.size()]();
                unsigned c = 0;
                for (auto &entry : tracks) {
                    anim->mChannels[c++] = makeNodeAnim(entry.first, entry.second);
                }
            }
            if (!morphs.empty()) {
                anim->mNumMorphMeshChannels = static_cast<unsigned>(morphs.size());
                anim->mMorphMeshChannels = new aiMeshMorphAnim *[morphs.size()]();
                unsigned c = 0;
                for (auto &entry : morphs) {
                    auto *morph = new aiMeshMorphAnim();
                    anim->mMorphMeshChannels[c++] = morph;
                    morph->mName = entry.first->mName;
                    morph->mNumKeys = static_cast<unsigned>(entry.second.size());
                    morph->mKeys = new aiMeshMorphKey[entry.second.size()];
                    for (size_t k = 0; k < entry.second.size(); ++k) {
                        aiMeshMorphKey &src = entry.second[k];
                        aiMeshMorphKey &dst = morph->mKeys[k];
                        dst.mTime = src.mTime;
                        dst.mNumValuesAndWeights = src.mNumValuesAndWeights;
                        dst.mValues = src.mValues;
                        dst.mWeights = src.mWeights;
                        src.mValues = nullptr;
                        src.mWeights = nullptr;
                        src.mNumValuesAndWeights = 0;
                    }
                }
            }
            anims.push_back(anim.release());
        }
        if (!anims.empty()) {
            mScene->mNumAnimations = static_cast<unsigned>(anims.size());
            mScene->mAnimations = new aiAnimation *[anims.size()];
            std::copy(anims.begin(), anims.end(), mScene->mAnimations);
        }
    }

    aiNode *channelNode(const tydra::AnimationChannel &ch) {
        if (!ch.target_prim_path.empty()) {
            auto it = mNodeByPath.find(ch.target_prim_path);
            if (it != mNodeByPath.end()) {
                return it->second;
            }
        }
        if (ch.target_node >= 0 && static_cast<size_t>(ch.target_node) < mNodeOrder.size()) {
            return mNodeOrder[static_cast<size_t>(ch.target_node)];
        }
        return nullptr;
    }

    bool addMorphKeys(aiNode *node, const tydra::AnimationChannel &ch, const tydra::KeyframeSampler &sampler,
            std::vector<aiMeshMorphKey> &out, double &maxTime) {
        if (node->mNumMeshes == 0) {
            return false;
        }
        const aiMesh *mesh = mScene->mMeshes[node->mMeshes[0]];
        auto order = mMorphTargets.find(mesh);
        if (order == mMorphTargets.end()) {
            return false;
        }
        const std::vector<std::string> &targetOrder = order->second;
        std::vector<std::string> names = ch.blendshape_target_names;
        if (names.empty()) {
            names = targetOrder;
        }
        const size_t numWeights = names.size();
        const size_t keys = sampler.times.size();
        if (numWeights == 0 || keys == 0 || sampler.values.size() != keys * numWeights) {
            return false;
        }
        std::vector<int> animIndex(numWeights, -1);
        size_t mapped = 0;
        for (size_t w = 0; w < numWeights; ++w) {
            auto it = std::find(targetOrder.begin(), targetOrder.end(), names[w]);
            if (it != targetOrder.end()) {
                animIndex[w] = static_cast<int>(it - targetOrder.begin());
                ++mapped;
            }
        }
        if (mapped == 0) {
            return false;
        }
        out.reserve(out.size() + keys);
        for (size_t k = 0; k < keys; ++k) {
            out.emplace_back();
            aiMeshMorphKey &key = out.back();
            key.mTime = sampler.times[k];
            maxTime = std::max(maxTime, key.mTime);
            key.mNumValuesAndWeights = static_cast<unsigned>(mapped);
            key.mValues = new unsigned int[mapped];
            key.mWeights = new double[mapped];
            unsigned m = 0;
            for (size_t w = 0; w < numWeights; ++w) {
                if (animIndex[w] < 0) {
                    continue;
                }
                key.mValues[m] = static_cast<unsigned>(animIndex[w]);
                key.mWeights[m] = sampler.values[k * numWeights + w];
                ++m;
            }
        }
        return true;
    }

    aiNodeAnim *makeNodeAnim(const std::string &name, NodeTrack &track) {
        auto *na = new aiNodeAnim();
        na->mNodeName.Set(name);
        // Missing tracks hold the rest pose, so a rotation-only channel keeps
        // its translation and scale.
        aiVector3D restT(0.0f, 0.0f, 0.0f), restS(1.0f, 1.0f, 1.0f);
        aiQuaternion restR;
        if (aiNode *node = mScene->mRootNode->FindNode(name.c_str())) {
            node->mTransformation.Decompose(restS, restR, restT);
        }
        if (track.position.empty()) {
            track.position.emplace_back(0.0, restT);
        }
        if (track.scaling.empty()) {
            track.scaling.emplace_back(0.0, restS);
        }
        if (track.rotation.empty()) {
            track.rotation.emplace_back(0.0, restR);
        }
        na->mNumPositionKeys = static_cast<unsigned>(track.position.size());
        na->mPositionKeys = new aiVectorKey[track.position.size()];
        std::copy(track.position.begin(), track.position.end(), na->mPositionKeys);
        na->mNumScalingKeys = static_cast<unsigned>(track.scaling.size());
        na->mScalingKeys = new aiVectorKey[track.scaling.size()];
        std::copy(track.scaling.begin(), track.scaling.end(), na->mScalingKeys);
        na->mNumRotationKeys = static_cast<unsigned>(track.rotation.size());
        na->mRotationKeys = new aiQuatKey[track.rotation.size()];
        std::copy(track.rotation.begin(), track.rotation.end(), na->mRotationKeys);
        return na;
    }

    // --- scene metadata -----------------------------------------------------------------------
    void applySceneMeta() {
        if (!mPhysics.hasScene) {
            return;
        }
        if (mScene->mMetaData == nullptr) {
            mScene->mMetaData = new aiMetadata();
        }
        if (mPhysics.hasGravityDirection) {
            mScene->mMetaData->Add("usdPhysics:gravityDirection", mPhysics.gravityDirection);
        }
        if (mPhysics.hasGravityMagnitude) {
            mScene->mMetaData->Add("usdPhysics:gravityMagnitude", mPhysics.gravityMagnitude);
        }
    }

    struct PendingUV {
        aiMaterial *mat;
        aiTextureType type;
        unsigned slot;
        std::string varname;
    };

    tydra::RenderScene &mRS;
    aiScene *mScene;
    PhysicsData &mPhysics;
    Diagnostics &mDiag;

    std::vector<SkeletonInfo> mSkeletons;
    std::vector<aiMaterial *> mMaterials;
    std::map<std::array<float, 4>, unsigned> mDefaultMaterials;
    std::set<aiMaterial *> mTwoSided;
    std::vector<PendingUV> mPendingUVSources;
    std::map<aiMaterial *, std::map<std::string, int>> mMaterialUVNames;
    std::vector<aiTexture *> mTextures;
    std::map<size_t, int> mImageToTexture;
    std::vector<aiMesh *> mMeshes;
    std::vector<std::vector<unsigned>> mMeshMap; ///< Tydra mesh -> aiMesh indices
    std::map<const aiMesh *, std::vector<std::string>> mMorphTargets;
    std::map<std::string, size_t> mLightIndex;
    std::vector<aiNode *> mNodeOrder; ///< Tydra pre-order node index -> aiNode
    std::unordered_map<std::string, aiNode *> mNodeByPath;

public:
    ~SceneBuilder() {
        // Anything not yet handed to the scene is released here.
        for (aiMaterial *m : mMaterials) delete m;
        for (aiTexture *t : mTextures) delete t;
        for (aiMesh *m : mMeshes) delete m;
    }
};

int32_t UpAxisIndex(const std::string &axis) {
    if (axis == "X") return 0;
    if (axis == "Z") return 2;
    return 1;
}

} // namespace

// ------------------------------------------------------------------------------------------------

SourceFormat DetectFormat(const uint8_t *data, size_t size) {
    if (data == nullptr) {
        return SourceFormat::Unknown;
    }
    if (size >= 8 && std::memcmp(data, "PXR-USDC", 8) == 0) {
        return SourceFormat::USDC;
    }
    if (size >= 5 && std::memcmp(data, "#usda", 5) == 0) {
        return SourceFormat::USDA;
    }
    if (size >= 30 && std::memcmp(data, "PK\x03\x04", 4) == 0) {
        // The first member of a USDZ package is its root layer.
        const size_t nameLen = static_cast<size_t>(data[26]) | (static_cast<size_t>(data[27]) << 8);
        if (nameLen == 0 || 30 + nameLen > size) {
            return SourceFormat::Unknown;
        }
        const std::string ext = LowerExtension(std::string(reinterpret_cast<const char *>(data + 30), nameLen));
        if (ext == "usd" || ext == "usda" || ext == "usdc") {
            return SourceFormat::USDZ;
        }
    }
    return SourceFormat::Unknown;
}

void ReadScene(const std::string &file, aiScene *scene, IOSystem *io, const ImportConfig &config, SourceFrame *frame) {
    if (scene == nullptr || io == nullptr) {
        throw DeadlyImportError("USD: invalid arguments.");
    }
    Diagnostics diag;
    const uint32_t maxMB = std::max<uint32_t>(config.maxMemoryMB, 1);
    const uint64_t maxBytes = static_cast<uint64_t>(maxMB) * 1024ull * 1024ull;

    // 1. Read the main file once.
    std::vector<uint8_t> buffer;
    {
        std::unique_ptr<IOStream, std::function<void(IOStream *)>> stream(io->Open(file, "rb"),
                [io](IOStream *s) { io->Close(s); });
        if (!stream) {
            throw DeadlyImportError("USD: failed to open file ", file, ".");
        }
        const size_t size = stream->FileSize();
        if (size == 0) {
            throw DeadlyImportError("USD: file ", file, " is empty.");
        }
        if (size > maxBytes) {
            throw DeadlyImportError("USD: file ", file, " exceeds the memory limit (", maxMB, " MiB, see AI_CONFIG_IMPORT_USD_MAX_MEMORY_MB).");
        }
        buffer.resize(size);
        if (stream->Read(buffer.data(), 1, size) != size) {
            throw DeadlyImportError("USD: failed to read file ", file, ".");
        }
    }

    // 2. Dispatch on content, not on the extension.
    const SourceFormat format = DetectFormat(buffer.data(), buffer.size());
    if (format == SourceFormat::Unknown) {
        throw DeadlyImportError("USD: ", file, " is not a USDA, USDC or USDZ file.");
    }

    lusd::USDLoadOptions loadOptions;
    loadOptions.max_memory_limit_in_mb = static_cast<int32_t>(std::min<uint32_t>(maxMB, static_cast<uint32_t>(std::numeric_limits<int32_t>::max())));
    loadOptions.max_allowed_asset_size_in_mb = maxMB;

    const std::string baseDir = DirName(file);
    AssetSource source(io, baseDir);
    lusd::USDZAsset usdz;
    lusd::Layer layer;
    {
        std::string warn, err;
        bool ok = false;
        if (format == SourceFormat::USDZ) {
            // Members are served straight from `buffer` (kept alive below).
            if (!lusd::ReadUSDZAssetInfoFromMemory(buffer.data(), buffer.size(), /* asset_on_memory */ true, &usdz, &warn, &err)) {
                throw DeadlyImportError("USD: invalid USDZ package ", file, ": ", err);
            }
            source.setPackage(&usdz, buffer.data(), buffer.size());
            auto root = usdz.asset_map.find(usdz.root_asset_name);
            if (usdz.root_asset_name.empty() || root == usdz.asset_map.end() || root->second.first > root->second.second ||
                    root->second.second > buffer.size()) {
                throw DeadlyImportError("USD: USDZ package ", file, " has no valid root layer.");
            }
            const size_t begin = root->second.first;
            ok = lusd::LoadLayerFromMemory(buffer.data() + begin, root->second.second - begin,
                    JoinPath(baseDir, usdz.root_asset_name), &layer, &warn, &err, loadOptions);
        } else {
            ok = lusd::LoadLayerFromMemory(buffer.data(), buffer.size(), file, &layer, &warn, &err, loadOptions);
            // The layer owns its data now.
            std::vector<uint8_t>().swap(buffer);
        }
        diag.warn(warn);
        if (!ok) {
            throw DeadlyImportError("USD: failed to load ", file, ": ", err);
        }
    }

    // 3. Compose through the IOSystem-backed resolver.
    lusd::AssetResolutionResolver resolver = MakeResolver(&source, layer.get_current_working_path());
    ComposeLayer(layer, resolver, maxMB, diag);

    std::unique_ptr<lusd::Stage> stage(new lusd::Stage());
    {
        std::string warn, err;
        if (!lusd::LayerToStage(std::move(layer), stage.get(), &warn, &err)) {
            throw DeadlyImportError("USD: failed to build the stage of ", file, ": ", err);
        }
        diag.warn(warn);
    }

    // 4. UsdPhysics lives on the stage only.
    PhysicsData physics;
    PhysicsCollector(*stage, physics).run();

    // 5. Tydra.
    tydra::RenderScene renderScene;
    {
        tydra::RenderSceneConverterEnv env(*stage);
        env.usd_filename = file;
        env.asset_resolver = resolver;
        env.asset_resolver.set_current_working_path(resolver.current_working_path());
        env.scene_config.load_texture_assets = true;
        env.mesh_config.triangulate = true;
        env.mesh_config.compute_normals = false;
        env.mesh_config.compute_tangents_and_binormals = false;
        env.mesh_config.normal_storage = tydra::MeshConverterConfig::NormalStorageFormat::Float3;
        env.mesh_config.tangent_storage = tydra::MeshConverterConfig::TangentStorageFormat::Float3;
        env.mesh_config.extract_all_texcoords = true;
        env.mesh_config.lowmem = true;
        env.material_config.texture_image_loader_function = LoadEncodedTexture;
        env.material_config.texture_image_loader_function_userdata = nullptr;
        env.material_config.preserve_texel_bitdepth = true;
        env.material_config.linearize_color_space = false;
        env.material_config.allow_texture_load_failure = true;
        env.material_config.allow_missing_asset = true;

        tydra::RenderSceneConverter converter;
        const bool ok = converter.ConvertToRenderScene(env, &renderScene);
        diag.warn(converter.GetWarning());
        diag.debug(converter.GetInfo());
        if (!ok) {
            throw DeadlyImportError("USD: failed to convert ", file, ": ", converter.GetError());
        }
    }
    // The stage and the package bytes are no longer needed.
    stage.reset();
    source.setPackage(nullptr, nullptr, 0);
    usdz = lusd::USDZAsset();
    std::vector<uint8_t>().swap(buffer);

    if (frame) {
        frame->metersPerUnit = renderScene.meta.metersPerUnit > 0.0 ? renderScene.meta.metersPerUnit : 1.0;
        frame->upAxis = UpAxisIndex(renderScene.meta.upAxis);
    }

    // 6. aiScene.
    SceneBuilder(renderScene, scene, physics, diag).build();
    if (scene->mNumMeshes == 0) {
        scene->mFlags |= AI_SCENE_FLAGS_INCOMPLETE;
    }
}

} // namespace USDImport
} // namespace Assimp

#endif // !! ASSIMP_BUILD_NO_USD_IMPORTER
