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

#include "usdz-writer.hh"

#include <assimp/Exceptional.h>
#include <assimp/IOStream.hpp>

#include <array>
#include <set>

namespace Assimp {
namespace USDZ {

namespace {

constexpr size_t kLocalHeaderSize = 30;
constexpr size_t kCentralHeaderSize = 46;
constexpr size_t kEndOfCentralDirSize = 22;
constexpr size_t kAlignment = 64;
// Extra-field id OpenUSD's own writer (UsdZipFileWriter) uses for padding.
constexpr uint16_t kPaddingExtraId = 0x1986;
constexpr size_t kExtraHeaderSize = 4;

std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

// Bytes of padding extra field needed so that member data starting after a
// local header at `offset` with a `nameSize`-byte name lands on 64 bytes. The
// field always carries its 4-byte header.
size_t PaddingFor(uint64_t offset, size_t nameSize) {
    const uint64_t dataStart = offset + kLocalHeaderSize + nameSize + kExtraHeaderSize;
    return kExtraHeaderSize + static_cast<size_t>((kAlignment - dataStart % kAlignment) % kAlignment);
}

bool IsSafeMemberName(const std::string &name) {
    if (name.empty() || name.size() > UINT16_MAX || name.front() == '/' ||
            name.find('\\') != std::string::npos || name.find(':') != std::string::npos) {
        return false;
    }
    size_t start = 0;
    while (start <= name.size()) {
        const size_t end = name.find('/', start);
        const std::string part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

class LittleEndianWriter {
public:
    explicit LittleEndianWriter(IOStream &out) : mOut(out) {}

    void U16(uint16_t v) {
        const uint8_t b[2] = { static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8) };
        Bytes(b, sizeof(b));
    }
    void U32(uint32_t v) {
        const uint8_t b[4] = { static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
            static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24) };
        Bytes(b, sizeof(b));
    }
    void Zeros(size_t n) {
        static const uint8_t zeros[kAlignment] = {};
        while (n > 0) {
            const size_t chunk = n < sizeof(zeros) ? n : sizeof(zeros);
            Bytes(zeros, chunk);
            n -= chunk;
        }
    }
    void Bytes(const void *data, size_t size) {
        if (size == 0) {
            return;
        }
        if (mOut.Write(data, 1, size) != size) {
            throw DeadlyExportError("USDZ: short write to the output stream");
        }
        mWritten += size;
    }
    uint64_t Written() const { return mWritten; }

private:
    IOStream &mOut;
    uint64_t mWritten = 0;
};

} // namespace

uint32_t Crc32(const uint8_t *data, size_t size, uint32_t crc) {
    static const std::array<uint32_t, 256> table = MakeCrcTable();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

uint64_t ArchiveSize(const std::vector<Entry> &entries) {
    if (entries.empty()) {
        throw DeadlyExportError("USDZ: a package needs at least the root layer");
    }
    if (entries.size() >= UINT16_MAX) {
        throw DeadlyExportError("USDZ: ", entries.size(), " members exceed the classic ZIP limit");
    }
    std::set<std::string> names;
    uint64_t offset = 0;
    uint64_t central = 0;
    for (const Entry &entry : entries) {
        if (!IsSafeMemberName(entry.name)) {
            throw DeadlyExportError("USDZ: invalid member name '", entry.name, "'");
        }
        if (!names.insert(entry.name).second) {
            throw DeadlyExportError("USDZ: duplicate member name '", entry.name, "'");
        }
        if (entry.size > 0 && entry.data == nullptr) {
            throw DeadlyExportError("USDZ: member '", entry.name, "' has no data");
        }
        if (!FitsClassicZipArchive(offset) || !FitsClassicZipArchive(entry.size)) {
            throw DeadlyExportError("USDZ: package exceeds the 4 GiB classic ZIP limit");
        }
        offset += kLocalHeaderSize + entry.name.size() + PaddingFor(offset, entry.name.size()) + entry.size;
        central += kCentralHeaderSize + entry.name.size();
    }
    if (!FitsClassicZipArchive(offset) || !FitsClassicZipArchive(central)) {
        throw DeadlyExportError("USDZ: package exceeds the 4 GiB classic ZIP limit");
    }
    const uint64_t total = offset + central + kEndOfCentralDirSize;
    if (!FitsClassicZipArchive(total)) {
        throw DeadlyExportError("USDZ: package exceeds the 4 GiB classic ZIP limit");
    }
    return total;
}

void WriteArchive(IOStream &out, const std::vector<Entry> &entries) {
    // Validate the whole layout before writing a byte.
    const uint64_t total = ArchiveSize(entries);

    struct CentralRecord {
        uint32_t crc;
        uint32_t size;
        uint32_t offset;
    };
    std::vector<CentralRecord> records;
    records.reserve(entries.size());

    LittleEndianWriter w(out);
    for (const Entry &entry : entries) {
        const uint64_t offset = w.Written();
        const size_t padding = PaddingFor(offset, entry.name.size());
        const uint32_t crc = Crc32(entry.data, entry.size);
        const uint32_t size = static_cast<uint32_t>(entry.size);
        records.push_back({ crc, size, static_cast<uint32_t>(offset) });

        w.U32(0x04034b50); // local file header signature
        w.U16(10);         // version needed to extract (1.0, stored)
        w.U16(0);          // flags
        w.U16(0);          // compression: stored
        w.U16(0);          // mod time
        w.U16(0x21);       // mod date: 1980-01-01, the DOS epoch
        w.U32(crc);
        w.U32(size);       // compressed size
        w.U32(size);       // uncompressed size
        w.U16(static_cast<uint16_t>(entry.name.size()));
        w.U16(static_cast<uint16_t>(padding));
        w.Bytes(entry.name.data(), entry.name.size());
        w.U16(kPaddingExtraId);
        w.U16(static_cast<uint16_t>(padding - kExtraHeaderSize));
        w.Zeros(padding - kExtraHeaderSize);
        w.Bytes(entry.data, entry.size);
    }

    const uint64_t centralOffset = w.Written();
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry &entry = entries[i];
        const CentralRecord &record = records[i];
        w.U32(0x02014b50); // central directory header signature
        w.U16(10);         // version made by
        w.U16(10);         // version needed to extract
        w.U16(0);          // flags
        w.U16(0);          // compression: stored
        w.U16(0);          // mod time
        w.U16(0x21);       // mod date
        w.U32(record.crc);
        w.U32(record.size);
        w.U32(record.size);
        w.U16(static_cast<uint16_t>(entry.name.size()));
        w.U16(0);          // extra field length
        w.U16(0);          // comment length
        w.U16(0);          // disk number start
        w.U16(0);          // internal attributes
        w.U32(0);          // external attributes
        w.U32(record.offset);
        w.Bytes(entry.name.data(), entry.name.size());
    }
    const uint64_t centralSize = w.Written() - centralOffset;

    w.U32(0x06054b50); // end of central directory signature
    w.U16(0);          // this disk
    w.U16(0);          // disk with the central directory
    w.U16(static_cast<uint16_t>(entries.size()));
    w.U16(static_cast<uint16_t>(entries.size()));
    w.U32(static_cast<uint32_t>(centralSize));
    w.U32(static_cast<uint32_t>(centralOffset));
    w.U16(0);          // comment length

    if (w.Written() != total) {
        throw DeadlyExportError("USDZ: internal size mismatch (", w.Written(), " != ", total, ")");
    }
}

} // namespace USDZ
} // namespace Assimp
