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

#pragma once

#ifndef ASSIMP_BUILD_NO_USD_EXPORTER

#include <assimp/defs.h>
#include <string>
#include <vector>
#include <cstdint>

// Forward declare ZIP archive type
struct zip_t;

namespace Assimp {

class IOSystem;

/**
 * @brief USDZ Archive Writer
 * 
 * Creates USDZ (ZIP-based USD) archives compatible with Apple Quick Look.
 * Uses Assimp's existing contrib/zip library for ZIP creation.
 * 
 * Key features:
 * - Apple Quick Look compatibility with proper 64-byte alignment
 * - Texture embedding with appropriate formats (PNG/JPEG)
 * - Uncompressed storage as required by USDZ specification
 * - Memory-efficient streaming approach for large textures
 */
class ASSIMP_API USDZArchiveWriter {
public:
    /// Asset information for USDZ archive entries
    struct AssetInfo {
        std::string filename;           ///< Entry path in archive
        std::vector<uint8_t> data;     ///< File content (may include padding for alignment)
        size_t originalSize;            ///< Original file size (before padding)
        bool isFirstEntry;              ///< Whether this is the first USD file (Default Layer)
        mutable uint64_t headerOffset; ///< ZIP local header offset (set during writing)
        
        AssetInfo(const std::string& name, const std::vector<uint8_t>& content, bool isFirst = false)
            : filename(name), data(content), originalSize(content.size()), isFirstEntry(isFirst), headerOffset(0) {}
            
        AssetInfo(const std::string& name, const char* content, size_t size, bool isFirst = false)
            : filename(name), data(content, content + size), originalSize(size), isFirstEntry(isFirst), headerOffset(0) {}
    };

    /// Constructor
    /// @param filename Output USDZ file path
    USDZArchiveWriter(const std::string& filename);
    
    /// Destructor - ensures proper cleanup
    ~USDZArchiveWriter();

    /// Add the main USD file as the first entry (required by USDZ spec)
    /// @param usdContent USD file content as string
    /// @param filename Name for USD file in archive (default: "model.usda")
    /// @return true on success
    bool AddMainUSDFile(const std::string& usdContent, const std::string& filename = "model.usda");

    /// Add a texture file to the archive
    /// @param texturePath Path for texture in archive (e.g., "textures/texture.jpg")
    /// @param textureData Raw texture file data
    /// @return true on success
    bool AddTextureFile(const std::string& texturePath, const std::vector<uint8_t>& textureData);
    
    /// Add a texture file from IOSystem
    /// @param texturePath Path for texture in archive
    /// @param sourceFilePath Source file path to read from
    /// @param pIOSystem IOSystem to use for file reading
    /// @return true on success
    bool AddTextureFromFile(const std::string& texturePath, const std::string& sourceFilePath, IOSystem* pIOSystem);

    /// Add arbitrary file to the archive
    /// @param entryPath Path for file in archive
    /// @param fileData File content
    /// @param requiresAlignment Whether file needs 64-byte alignment
    /// @return true on success
    bool AddFile(const std::string& entryPath, const std::vector<uint8_t>& fileData, bool requiresAlignment = false);

    /// Finalize and close the archive
    /// @return true on success
    bool Finalize();

    /// Check if archive is successfully opened
    /// @return true if archive is ready for writing
    bool IsOpen() const { return mZipArchive != nullptr; }

    /// Get list of errors that occurred during archive creation
    /// @return vector of error messages
    const std::vector<std::string>& GetErrors() const { return mErrors; }
    
    /// Get list of warnings that occurred during archive creation
    /// @return vector of warning messages
    const std::vector<std::string>& GetWarnings() const { return mWarnings; }

private:
    
    /// Write asset to ZIP archive with proper error handling
    /// @param asset Asset information to write
    /// @return true on success
    bool WriteAssetToArchive(const AssetInfo& asset);
    
    /// Report error message
    /// @param message Error message
    void ReportError(const std::string& message);
    
    /// Report warning message
    /// @param message Warning message
    void ReportWarning(const std::string& message);

private:
    std::string mFilename;                    ///< Output filename
    struct zip_t* mZipArchive;               ///< ZIP archive handle
    std::vector<AssetInfo> mAssets;          ///< Queued assets for writing
    std::vector<std::string> mErrors;        ///< Error messages
    std::vector<std::string> mWarnings;      ///< Warning messages
    bool mFinalized;                         ///< Whether archive has been finalized

    // Non-copyable
    USDZArchiveWriter(const USDZArchiveWriter&) = delete;
    USDZArchiveWriter& operator=(const USDZArchiveWriter&) = delete;
};

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_USD_EXPORTER
