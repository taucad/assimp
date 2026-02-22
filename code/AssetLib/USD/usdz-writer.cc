// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// USDZ(ZIP) writer
//

#include "usdz-writer.hh"

#if !defined(TINYUSDZ_DISABLE_MODULE_USDZ_WRITER)

#include <set>
#include <vector>
#include <map>
#include <memory>
#include <unistd.h>

#include "../../../contrib/tinyusdz/autoclone/tinyusdz_repo-src/src/stage.hh"
#include "../../../contrib/tinyusdz/autoclone/tinyusdz_repo-src/src/external/miniz.h"



namespace tinyusdz {
namespace usdz {

namespace {

///
/// Extract all texture file paths by parsing the USD content
///
bool ExtractTexturePaths(const std::string &usdContent, std::set<std::string> &texturePaths, std::string *err) {
    
    // Parse the USD content to find asset references
    // Look for patterns like: asset inputs:file = @./textures/filename.jpg@
    std::string content = usdContent;
    size_t pos = 0;
    
    while ((pos = content.find("asset inputs:file = @", pos)) != std::string::npos) {
        // Move to start of the asset path
        pos += 21; // Length of "asset inputs:file = @"
        
        // Find the closing @
        size_t endPos = content.find("@", pos);
        if (endPos != std::string::npos) {
            std::string assetPath = content.substr(pos, endPos - pos);
            
            if (!assetPath.empty()) {
                // Remove leading "./" if present for consistency
                if (assetPath.length() > 2 && assetPath.substr(0, 2) == "./") {
                    assetPath = assetPath.substr(2);
                }
                texturePaths.insert(assetPath);
            }
        }
        
        pos = endPos != std::string::npos ? endPos + 1 : std::string::npos;
        if (pos == std::string::npos) break;
    }
    
    return true;
}

///
/// Create USDZ archive with proper 64-byte alignment using OpenUSD approach
///
bool CreateUSDZArchiveWithTextures(const std::string &filename, const std::string &usdContent,
                                   const std::map<std::string, std::vector<uint8_t>> &textureDataMap,
                                   std::string *warn, std::string *err) {

    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    // Initialize ZIP writer without ZIP64 (USDZ files are < 4GB; ZIP64 adds extra fields that break 64-byte alignment)
    if (!mz_zip_writer_init_file_v2(&zip_archive, filename.c_str(), 0, 0)) {
        if (err) {
            (*err) += "Failed to initialize ZIP archive: " + filename + "\n";
        }
        return false;
    }
    
    // Lambda to add files with 64-byte alignment using corrected calculation
    auto AddFileWithAlignment = [&](const std::string& entryName, const void* data, size_t size) -> bool {
        
        // Use working alignment calculation that achieved perfect alignment
        const mz_uint64 headerStart = zip_archive.m_archive_size;
        const mz_uint64 dataOffsetWithoutExtra = headerStart + 30 + entryName.length();
        
        const mz_uint64 remainder = dataOffsetWithoutExtra % 64;
        const mz_uint64 totalPaddingNeeded = (remainder == 0) ? 0 : (64 - remainder);
        
        // Ensure minimum 4 bytes for extra field header
        const mz_uint64 actualPaddingNeeded = (totalPaddingNeeded < 4 && totalPaddingNeeded > 0) ? 
                                             totalPaddingNeeded + 64 : totalPaddingNeeded;
        
        // Create extra field with OpenUSD alignment ID (0x1986)
        std::vector<uint8_t> extraData;
        const char* extraFieldData = nullptr;
        mz_uint extraFieldLength = 0;
        
        // Create 0x1986 extra field with working padding calculation
        if (actualPaddingNeeded > 0) {
            extraData.resize(actualPaddingNeeded, 0);
            const uint16_t extraFieldID = 0x1986;
            const uint16_t paddingDataSize = static_cast<uint16_t>(actualPaddingNeeded - 4);
            
            extraData[0] = static_cast<uint8_t>(extraFieldID & 0xFF);
            extraData[1] = static_cast<uint8_t>((extraFieldID >> 8) & 0xFF);
            extraData[2] = static_cast<uint8_t>(paddingDataSize & 0xFF);
            extraData[3] = static_cast<uint8_t>((paddingDataSize >> 8) & 0xFF);
            
            extraFieldData = reinterpret_cast<const char*>(extraData.data());
            extraFieldLength = static_cast<mz_uint>(extraData.size());
        } else {
            // For perfectly aligned start, include minimal USDZ extra field
            extraData.resize(4, 0);
            const uint16_t extraFieldID = 0x1986;
            
            extraData[0] = static_cast<uint8_t>(extraFieldID & 0xFF);
            extraData[1] = static_cast<uint8_t>((extraFieldID >> 8) & 0xFF);
            extraData[2] = 0; // No padding data
            extraData[3] = 0;
            
            extraFieldData = reinterpret_cast<const char*>(extraData.data());
            extraFieldLength = static_cast<mz_uint>(extraData.size());
        }
        
        // KEY INSIGHT: Use mz_zip_writer_add_read_buf_callback which respects MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE
        // From miniz.c line 6528: gen_flags = (MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE) ? 0 : MZ_ZIP_LDH_BIT_FLAG_HAS_LOCATOR
        
        // Create a simple read callback that reads from our memory buffer
        struct MemoryReadContext {
            const void* data;
            size_t size;
            size_t position;
        };
        
        auto memoryReadContext = std::make_shared<MemoryReadContext>();
        memoryReadContext->data = data;
        memoryReadContext->size = size;
        memoryReadContext->position = 0;
        
        auto memoryReadCallback = [](void *pOpaque, mz_uint64 file_ofs, void *pBuf, size_t n) -> size_t {
            auto* ctx = static_cast<MemoryReadContext*>(pOpaque);
            if (file_ofs >= ctx->size) return 0;
            
            const size_t available = ctx->size - static_cast<size_t>(file_ofs);
            const size_t toRead = (n > available) ? available : n;
            
            memcpy(pBuf, static_cast<const uint8_t*>(ctx->data) + file_ofs, toRead);
            return toRead;
        };
        
        const bool success = mz_zip_writer_add_read_buf_callback(&zip_archive, entryName.c_str(),
                                                               memoryReadCallback, memoryReadContext.get(),
                                                               size, // max_size
                                                               nullptr, // No time 
                                                               nullptr, 0, // No comment
                                                               MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE, // Prevents data descriptors!
                                                               extraFieldData, extraFieldLength, // Local extra fields
                                                               nullptr, 0); // No central extra fields
        
        // With mz_zip_writer_add_read_buf_callback + MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE,
        // most header fields should be correct, but verify USDZ-specific requirements
        if (success && zip_archive.m_pWrite) {
            // Minimal patching for USDZ compatibility
            // Patch version to 10 (offset 4-5) - USD tools require this
            mz_uint8 versionBytes[2] = {10, 0};
            zip_archive.m_pWrite(zip_archive.m_pIO_opaque, headerStart + 4, versionBytes, 2);
            
            // Ensure flags are 0x0000 (offset 6-7) - critical for USDZ
            mz_uint8 flagsBytes[2] = {0, 0};
            zip_archive.m_pWrite(zip_archive.m_pIO_opaque, headerStart + 6, flagsBytes, 2);
        }
        
        return success;
    };

    // Add main USD file first (USDZ specification requirement) with 64-byte alignment
    std::string usdFilename = "model.usda";
    if (!AddFileWithAlignment(usdFilename, usdContent.data(), usdContent.size())) {
        if (err) {
            (*err) += "Failed to add USD file to archive. miniz error: " + std::to_string(zip_archive.m_last_error) + "\n";
        }
        mz_zip_writer_end(&zip_archive);
        return false;
    }

    // Add texture files with proper directory structure and 64-byte alignment
    for (const auto &textureEntry : textureDataMap) {
        const std::string &texturePath = textureEntry.first;
        const std::vector<uint8_t> &textureData = textureEntry.second;

        if (textureData.empty()) {
            if (warn) {
                (*warn) += "Texture data is empty: " + texturePath + "\n";
            }
            continue;
        }

        // Add texture to archive with 64-byte alignment
        if (!AddFileWithAlignment(texturePath, textureData.data(), textureData.size())) {
            if (warn) {
                (*warn) += "Failed to add texture to archive: " + texturePath + ". miniz error: " + std::to_string(zip_archive.m_last_error) + "\n";
            }
            continue; // Continue with other textures
        }
    }

    // Finalize archive
    if (!mz_zip_writer_finalize_archive(&zip_archive)) {
        if (err) {
            (*err) += "Failed to finalize ZIP archive\n";
        }
        mz_zip_writer_end(&zip_archive);
        return false;
    }

    mz_zip_writer_end(&zip_archive);
    
    // Central directory flag patching to match reference (no data descriptors needed)
    {
        FILE* file = fopen(filename.c_str(), "r+b");
        if (file) {
            // Find End of Central Directory record
            fseek(file, -22, SEEK_END);
            uint8_t eocdBuffer[22];
            if (fread(eocdBuffer, 1, 22, file) == 22) {
                const uint32_t centralDirOffset = 
                    eocdBuffer[16] | (eocdBuffer[17] << 8) | (eocdBuffer[18] << 16) | (eocdBuffer[19] << 24);
                
                // Fix flags for USD tool compatibility
                uint64_t currentOffset = centralDirOffset;
                
                while (true) {
                    fseek(file, currentOffset, SEEK_SET);
                    uint8_t signature[4];
                    if (fread(signature, 1, 4, file) != 4) break;
                    
                    if (signature[0] != 0x50 || signature[1] != 0x4b || 
                        signature[2] != 0x01 || signature[3] != 0x02) break;
                    
                    // Set central directory flags to 0x0000 unconditionally (matches reference file)
                    fseek(file, currentOffset + 8, SEEK_SET);
                    uint8_t clearFlags[2] = {0, 0}; // Clear all flags for USDZ compatibility
                    if (fwrite(clearFlags, 1, 2, file) != 2) break;
                    
                    // Read field lengths to move to next entry
                    fseek(file, currentOffset + 28, SEEK_SET);
                    uint8_t lengthFields[6];
                    if (fread(lengthFields, 1, 6, file) != 6) break;
                    
                    const uint16_t filenameLen = lengthFields[0] | (lengthFields[1] << 8);
                    const uint16_t extraLen = lengthFields[2] | (lengthFields[3] << 8);
                    const uint16_t commentLen = lengthFields[4] | (lengthFields[5] << 8);
                    
                    currentOffset += 46 + filenameLen + extraLen + commentLen;
                }
            }
            fclose(file);
        }
    }

    return true;
}

} // namespace

bool SaveAsUSDZ(const std::string &filename, const Stage &stage, std::string *warn, std::string *err) {
    // For now, we don't use the Stage object directly for texture extraction
    // as the current Assimp USDZExporter generates USD content as a string.
    // The texture paths are extracted from this string.
    // This function is kept for API compatibility with tinyusdz's usda::SaveAsUSDA.

    if (stage.root_prims().empty()) {
        if (err) {
            (*err) = "Stage is empty\n";
        }
        return false;
    }

    // Generate USD content from the stage (this would typically be done by tinyusdz's usda-writer)
    // For Assimp's current workflow, USD content is generated by USDZExporter.
    // So, this path is not currently used by Assimp's USDZExporter.
    if (err) {
        (*err) = "SaveAsUSDZ(Stage) not fully implemented for Assimp's workflow. Use SaveAsUSDZWithTextures.\n";
    }
    return false;
}

bool SaveAsUSDZWithTextures(const std::string &filename, const std::string &usdContent,
                            const std::map<std::string, std::vector<uint8_t>> &textureDataMap,
                            std::string *warn, std::string *err) {

    if (usdContent.empty()) {
        if (err) {
            (*err) = "Generated USD content is empty\n";
        }
        return false;
    }

    std::set<std::string> texturePaths;
    if (!ExtractTexturePaths(usdContent, texturePaths, err)) {
        return false;
    }

    return CreateUSDZArchiveWithTextures(filename, usdContent, textureDataMap, warn, err);
}

static void WriteLE16(std::vector<uint8_t> &out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

static void WriteLE32(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

struct USDZFileEntry {
    std::string name;
    uint32_t localHeaderOffset;
    uint32_t crc32;
    uint32_t size;
};

bool SaveAsUSDZToMemory(const std::string &usdContent,
                        const std::map<std::string, std::vector<uint8_t>> &textureDataMap,
                        std::vector<uint8_t> &outData,
                        std::string *warn, std::string *err) {
    if (usdContent.empty()) {
        if (err) (*err) = "Generated USD content is empty\n";
        return false;
    }

    // Collect all files to package
    struct FileInfo {
        std::string name;
        const uint8_t* data;
        size_t size;
    };

    std::vector<FileInfo> files;
    files.push_back({"model.usda", reinterpret_cast<const uint8_t*>(usdContent.data()), usdContent.size()});
    for (const auto &entry : textureDataMap) {
        if (!entry.second.empty()) {
            files.push_back({entry.first, entry.second.data(), entry.second.size()});
        }
    }

    // Build USDZ archive manually with 64-byte alignment
    outData.clear();
    std::vector<USDZFileEntry> entries;

    for (const auto &file : files) {
        USDZFileEntry entry;
        entry.name = file.name;
        entry.localHeaderOffset = static_cast<uint32_t>(outData.size());
        entry.size = static_cast<uint32_t>(file.size);
        entry.crc32 = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, file.data, file.size));

        // Calculate extra field size for 64-byte alignment
        size_t dataOffsetWithoutExtra = outData.size() + 30 + file.name.length();
        size_t remainder = dataOffsetWithoutExtra % 64;
        size_t paddingNeeded = (remainder == 0) ? 0 : (64 - remainder);
        // Extra field needs at least 4 bytes for header (ID + size)
        if (paddingNeeded > 0 && paddingNeeded < 4) {
            paddingNeeded += 64;
        }

        // If already aligned, add a full 64-byte padding block
        if (paddingNeeded == 0) {
            paddingNeeded = 64;
        }

        uint16_t extraLen = static_cast<uint16_t>(paddingNeeded);

        // Local file header (30 bytes fixed)
        WriteLE32(outData, 0x04034b50);  // signature
        WriteLE16(outData, 10);          // version needed (1.0)
        WriteLE16(outData, 0);           // flags (none)
        WriteLE16(outData, 0);           // compression (stored)
        WriteLE16(outData, 0);           // mod time
        WriteLE16(outData, 0);           // mod date
        WriteLE32(outData, entry.crc32); // CRC-32
        WriteLE32(outData, entry.size);  // compressed size
        WriteLE32(outData, entry.size);  // uncompressed size
        WriteLE16(outData, static_cast<uint16_t>(file.name.length()));  // filename length
        WriteLE16(outData, extraLen);    // extra field length

        // Filename
        outData.insert(outData.end(), file.name.begin(), file.name.end());

        // Extra field with OpenUSD alignment tag (0x1986)
        {
            uint16_t paddingDataSize = static_cast<uint16_t>(paddingNeeded - 4);
            // Extra field header
            outData.push_back(0x86); outData.push_back(0x19);  // ID = 0x1986
            outData.push_back(static_cast<uint8_t>(paddingDataSize & 0xFF));
            outData.push_back(static_cast<uint8_t>((paddingDataSize >> 8) & 0xFF));
            // Zero padding
            outData.insert(outData.end(), paddingDataSize, 0);
        }

        // File data (uncompressed)
        outData.insert(outData.end(), file.data, file.data + file.size);

        entries.push_back(entry);
    }

    // Central directory
    uint32_t centralDirOffset = static_cast<uint32_t>(outData.size());
    for (const auto &entry : entries) {
        WriteLE32(outData, 0x02014b50);  // central directory signature
        WriteLE16(outData, 10);          // version made by
        WriteLE16(outData, 10);          // version needed
        WriteLE16(outData, 0);           // flags
        WriteLE16(outData, 0);           // compression
        WriteLE16(outData, 0);           // mod time
        WriteLE16(outData, 0);           // mod date
        WriteLE32(outData, entry.crc32);
        WriteLE32(outData, entry.size);  // compressed size
        WriteLE32(outData, entry.size);  // uncompressed size
        WriteLE16(outData, static_cast<uint16_t>(entry.name.length()));
        WriteLE16(outData, 0);           // extra field length (none in central dir)
        WriteLE16(outData, 0);           // comment length
        WriteLE16(outData, 0);           // disk number start
        WriteLE16(outData, 0);           // internal file attributes
        WriteLE32(outData, 0);           // external file attributes
        WriteLE32(outData, entry.localHeaderOffset);
        outData.insert(outData.end(), entry.name.begin(), entry.name.end());
    }
    uint32_t centralDirSize = static_cast<uint32_t>(outData.size()) - centralDirOffset;

    // End of central directory record
    WriteLE32(outData, 0x06054b50);  // EOCD signature
    WriteLE16(outData, 0);           // disk number
    WriteLE16(outData, 0);           // disk with central dir
    WriteLE16(outData, static_cast<uint16_t>(entries.size()));
    WriteLE16(outData, static_cast<uint16_t>(entries.size()));
    WriteLE32(outData, centralDirSize);
    WriteLE32(outData, centralDirOffset);
    WriteLE16(outData, 0);           // comment length

    return true;
}

#if defined(_WIN32)
bool SaveAsUSDZ(const std::wstring &filename, const Stage &stage, std::string *warn, std::string *err) {

    (void)warn;
    (void)err;
    (void)stage;
    (void)filename;
    // TODO: Implement WideChar filename version
    return false;
}
#endif

} // namespace usdz
} // namespace tinyusdz

#else

namespace tinyusdz {
namespace usdz {

bool SaveAsUSDZ(const std::string &filename, const Stage &stage, std::string *warn, std::string *err) {
    (void)filename;
    (void)stage;
    (void)warn;

    if (err) {
        (*err) = "USDZ Writer feature is disabled in this build.\n";
    }
    return false;
}

} // namespace usdz
} // namespace tinyusdz
#endif
