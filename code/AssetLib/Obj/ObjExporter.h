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

/** @file ObjExporter.h
 * Declares the exporter class to write a scene to a Collada file
 */
#ifndef AI_OBJEXPORTER_H_INC
#define AI_OBJEXPORTER_H_INC

#include <assimp/types.h>
#include <sstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <utility>

#include <assimp/Exporter.hpp>

struct aiScene;
struct aiNode;
struct aiMesh;

namespace Assimp {

// ------------------------------------------------------------------------------------------------
/** Helper class to export a given scene to an OBJ file. */
// ------------------------------------------------------------------------------------------------
class ObjExporter final {
public:
    /// Constructor for a specific scene to export
    ObjExporter(std::streambuf* out, const char* filename, const aiScene* pScene, bool noMtl=false, const ExportProperties* props = nullptr);
    ~ObjExporter();
    std::string GetMaterialLibName();
    std::string GetMaterialLibFileName();

    /// public string-streams to write all output into
    std::ostream mOutput;
    std::ostringstream mOutputMat;

private:
    // intermediate data structures
    struct FaceVertex {
        FaceVertex()
        : vp()
        , vn()
        , vt() {
            // empty
        }

        // one-based, 0 means: 'does not exist'
        unsigned int vp, vn, vt;
    };

    struct Face {
        char kind;
        std::vector<FaceVertex> indices;
    };

    struct MeshInstance {
        std::string name, matname;
        std::vector<Face> faces;
    };

    void WriteHeader(std::ostream& out);
    void WriteMaterialFile();
    void WriteGeometryFile(bool noMtl=false, bool merge_identical_vertices = false);
    std::string GetMaterialName(unsigned int index);
    void AddMesh(const aiString& name, const aiMesh* m, const aiMatrix4x4& mat, bool merge_identical_vertices);
    void AddNode(const aiNode* nd, const aiMatrix4x4& mParent, bool merge_identical_vertices);

private:
    std::string filename;
    const aiScene* const pScene;

    struct vertexData {
        aiVector3D vp;
        aiColor3D vc; // OBJ does not support 4D color
        uint32_t index = 0;
    };

    std::vector<aiVector3D> vn, vt;
    std::vector<aiColor4D> vc;
    std::vector<vertexData> vp;
    bool useVc;

    // Exact-equality keys (+0 and -0 are equal), hashed bit-wise with -0 folded onto +0.
    static uint64_t hashFloat(uint64_t h, ai_real value) {
        uint64_t bits = 0;
        if (value != ai_real(0)) {
            std::memcpy(&bits, &value, sizeof(value));
        }
        h = (h ^ bits) * 0x100000001B3ull;
        return h ^ (h >> 29);
    }
    static uint64_t finishHash(uint64_t h) {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        return h ^ (h >> 33);
    }

    struct vertexDataHash {
        uint64_t operator()(const vertexData &v) const {
            uint64_t h = 0x9E3779B97F4A7C15ull;
            h = hashFloat(h, v.vp.x);
            h = hashFloat(h, v.vp.y);
            h = hashFloat(h, v.vp.z);
            h = hashFloat(h, v.vc.r);
            h = hashFloat(h, v.vc.g);
            h = hashFloat(h, v.vc.b);
            h = (h ^ v.index) * 0x100000001B3ull;
            return finishHash(h);
        }
    };
    struct vertexDataEqual {
        bool operator()(const vertexData &a, const vertexData &b) const {
            return a.vp == b.vp && a.vc.r == b.vc.r && a.vc.g == b.vc.g && a.vc.b == b.vc.b && a.index == b.index;
        }
    };
    struct aiVectorHash {
        uint64_t operator()(const aiVector3D &v) const {
            uint64_t h = 0x9E3779B97F4A7C15ull;
            h = hashFloat(h, v.x);
            h = hashFloat(h, v.y);
            h = hashFloat(h, v.z);
            return finishHash(h);
        }
    };
    struct aiVectorEqual {
        bool operator()(const aiVector3D &a, const aiVector3D &b) const {
            return a == b;
        }
    };

    /// Assigns 1-based indices to distinct keys in first-seen order. An open-addressing table of
    /// indices into the key list: 4 bytes per slot instead of a std::map node per key.
    template <class T, class Hash, class Equal>
    class indexMap {
        std::vector<T> mKeys;
        std::vector<uint32_t> mTable;
        size_t mMask = 0;
        static constexpr uint32_t kEmpty = 0xffffffffu;

        void rehash(size_t capacity) {
            mTable.assign(capacity, kEmpty);
            mMask = capacity - 1;
            for (size_t i = 0; i < mKeys.size(); ++i) {
                size_t slot = static_cast<size_t>(Hash()(mKeys[i])) & mMask;
                while (mTable[slot] != kEmpty) {
                    slot = (slot + 1) & mMask;
                }
                mTable[slot] = static_cast<uint32_t>(i);
            }
        }

    public:
        int getIndex(const T& key) {
            if ((mKeys.size() + 1) * 2 > mTable.size()) {
                rehash(mTable.empty() ? 64 : mTable.size() * 2);
            }
            size_t slot = static_cast<size_t>(Hash()(key)) & mMask;
            for (;;) {
                const uint32_t existing = mTable[slot];
                if (existing == kEmpty) {
                    mTable[slot] = static_cast<uint32_t>(mKeys.size());
                    mKeys.push_back(key);
                    return static_cast<int>(mKeys.size());
                }
                if (Equal()(mKeys[existing], key)) {
                    return static_cast<int>(existing + 1);
                }
                slot = (slot + 1) & mMask;
            }
        }

        /// Hands the keys over in index order; the map is empty afterwards.
        void getKeys( std::vector<T>& keys ) {
            keys = std::move(mKeys);
            mKeys.clear();
            std::vector<uint32_t>().swap(mTable);
            mMask = 0;
        }
    };

    indexMap<aiVector3D, aiVectorHash, aiVectorEqual> mVnMap, mVtMap;
    indexMap<vertexData, vertexDataHash, vertexDataEqual> mVpMap;
    std::vector<MeshInstance> mMeshes;

    // this endl() doesn't flush() the stream
    const std::string endl;
};

}

#endif
