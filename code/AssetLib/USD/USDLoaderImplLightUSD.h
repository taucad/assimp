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

/** @file  USDLoaderImplLightUSD.h
 *  @brief USD (USDA/USDC/USDZ) to aiScene conversion on top of LightUSD.
 *
 *  LightUSD headers are deliberately not included here so that they stay
 *  private to USDLoaderImplLightUSD.cpp.
 */
#pragma once
#ifndef AI_USDLOADER_IMPL_LIGHTUSD_H_INCLUDED
#define AI_USDLOADER_IMPL_LIGHTUSD_H_INCLUDED

#include <cstddef>
#include <cstdint>
#include <string>

struct aiScene;

namespace Assimp {

class IOSystem;

namespace USDImport {

/// Detected container format of a USD byte stream.
enum class SourceFormat {
    Unknown,
    USDA,
    USDC,
    USDZ
};

/// Classifies `data` by its magic bytes ("#usda", "PXR-USDC", "PK\3\4").
/// A ZIP archive only counts as USDZ when its first member is a USD layer.
SourceFormat DetectFormat(const uint8_t *data, size_t size);

/// Per-read settings, taken from the importer properties.
struct ImportConfig {
    /// Memory budget (MiB) handed to LightUSD's parser, composition and Tydra.
    uint32_t maxMemoryMB = 16384;
};

/// Stage metadata that the caller turns into the unit/axis contract.
struct SourceFrame {
    double metersPerUnit = 1.0;
    int32_t upAxis = 1; ///< 0 = X, 1 = Y, 2 = Z
};

/// Reads `file` through `io`, composes it (sublayers, references, payloads,
/// inherits, specializes, variants; sidecars and USDZ members go through `io`)
/// and fills `scene`. Throws DeadlyImportError on failure. Holds no state
/// between calls.
void ReadScene(const std::string &file, aiScene *scene, IOSystem *io,
        const ImportConfig &config, SourceFrame *frame);

} // namespace USDImport
} // namespace Assimp

#endif // AI_USDLOADER_IMPL_LIGHTUSD_H_INCLUDED
