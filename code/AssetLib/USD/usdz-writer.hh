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

/** @file usdz-writer.hh
 *  @brief Streaming USDZ package writer.
 *
 *  A USDZ package is a classic ZIP archive whose members are stored
 *  uncompressed, with every member's data aligned to 64 bytes and the root
 *  layer first (https://openusd.org/release/spec_usdz.html). Members are
 *  written straight to the output stream, so the archive is never assembled
 *  in memory.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Assimp {

class IOStream;

namespace USDZ {

/// One package member. `data` must stay valid until WriteArchive returns.
struct Entry {
    std::string name;
    const uint8_t *data = nullptr;
    size_t size = 0;
};

/// Classic ZIP addresses at most 4 GiB; ZIP64 is not part of the USDZ spec.
constexpr bool FitsClassicZipArchive(std::uint64_t size) {
    return size < UINT32_MAX;
}

/// CRC-32 (IEEE 802.3), as ZIP requires.
uint32_t Crc32(const uint8_t *data, size_t size, uint32_t crc = 0);

/// Validates `entries` and returns the exact archive size in bytes.
/// Throws DeadlyExportError when the package cannot be a classic ZIP or a
/// member name is not a safe relative path.
uint64_t ArchiveSize(const std::vector<Entry> &entries);

/// Writes the package to `out`. The first entry is the root layer.
/// Throws DeadlyExportError on invalid input or a short write.
void WriteArchive(IOStream &out, const std::vector<Entry> &entries);

} // namespace USDZ
} // namespace Assimp
