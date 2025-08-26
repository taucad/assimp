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

#ifndef ASSIMP_BUILD_NO_USD_EXPORTER

#include "USDZArchiveWriter.h"

#include <assimp/DefaultLogger.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/ai_assert.h>

// Use Assimp's existing ZIP library (same as 3MF exporter)
#ifdef ASSIMP_USE_HUNTER
#include <zip/zip.h>
#else
#include <contrib/zip/src/zip.h>
#endif

#include <memory>

namespace Assimp {

namespace {
    [[maybe_unused]] static const char* TAG = "USDZArchiveWriter";
}

// ------------------------------------------------------------------------------------------------
USDZArchiveWriter::USDZArchiveWriter(const std::string& filename) :
    mFilename(filename),
    mZipArchive(nullptr),
    mFinalized(false) {
    
    // Create ZIP archive for writing with no compression (USDZ requirement)
    mZipArchive = zip_open(filename.c_str(), 0, 'w'); // 0 = no compression
    
    if (!mZipArchive) {
        ReportError("Failed to create USDZ archive: " + filename);
    } else {
        ASSIMP_LOG_INFO("USDZArchiveWriter: Created archive ", filename);
    }
}

// ------------------------------------------------------------------------------------------------
USDZArchiveWriter::~USDZArchiveWriter() {
    if (mZipArchive && !mFinalized) {
        // Force finalization if not done explicitly
        ReportWarning("Archive was not finalized explicitly, forcing finalization in destructor");
        Finalize();
    }
}

// ------------------------------------------------------------------------------------------------
bool USDZArchiveWriter::AddMainUSDFile(const std::string& usdContent, const std::string& filename) {
    if (!IsOpen()) {
        ReportError("Archive is not open");
        return false;
    }
    
    if (mFinalized) {
        ReportError("Cannot add files to finalized archive");
        return false;
    }

    // USD file should be first in the archive per USDZ specification
    if (!mAssets.empty()) {
        ReportWarning("USD file should be added first, but archive already contains " + 
                     std::to_string(mAssets.size()) + " assets");
    }

    // Convert string to bytes
    std::vector<uint8_t> usdBytes(usdContent.begin(), usdContent.end());
    
    // USD files don't typically need alignment, but we'll add it anyway for consistency
    mAssets.emplace_back(filename, usdBytes, false);
    
    ASSIMP_LOG_INFO("USDZArchiveWriter: Added USD file ", filename, " (", usdBytes.size(), " bytes)");
    return true;
}

// ------------------------------------------------------------------------------------------------
bool USDZArchiveWriter::AddTextureFile(const std::string& texturePath, const std::vector<uint8_t>& textureData) {
    if (!IsOpen()) {
        ReportError("Archive is not open");
        return false;
    }
    
    if (mFinalized) {
        ReportError("Cannot add files to finalized archive");
        return false;
    }

    if (textureData.empty()) {
        ReportWarning("Empty texture data for: " + texturePath);
        return false;
    }

    // Add texture file to archive
    mAssets.emplace_back(texturePath, textureData, false);
    
    ASSIMP_LOG_INFO("USDZArchiveWriter: Added texture ", texturePath, " (", textureData.size(), " bytes)");
    return true;
}

// ------------------------------------------------------------------------------------------------
bool USDZArchiveWriter::AddTextureFromFile(const std::string& texturePath, 
                                          const std::string& sourceFilePath, 
                                          IOSystem* pIOSystem) {
    if (!pIOSystem) {
        ReportError("IOSystem is null");
        return false;
    }
    
    std::unique_ptr<IOStream> fileStream(pIOSystem->Open(sourceFilePath, "rb"));
    if (!fileStream) {
        ReportError("Failed to open texture file: " + sourceFilePath);
        return false;
    }
    
    // Get file size
    fileStream->Seek(0, aiOrigin_END);
    const size_t fileSize = fileStream->Tell();
    fileStream->Seek(0, aiOrigin_SET);
    
    if (fileSize == 0) {
        ReportWarning("Empty texture file: " + sourceFilePath);
        return false;
    }
    
    // Read entire file into memory
    std::vector<uint8_t> textureData(fileSize);
    const size_t bytesRead = fileStream->Read(textureData.data(), 1, fileSize);
    
    if (bytesRead != fileSize) {
        ReportError("Failed to read complete texture file: " + sourceFilePath);
        return false;
    }
    
    return AddTextureFile(texturePath, textureData);
}

// ------------------------------------------------------------------------------------------------
bool USDZArchiveWriter::AddFile(const std::string& entryPath, 
                                const std::vector<uint8_t>& fileData, 
                                bool requiresAlignment) {
    if (!IsOpen()) {
        ReportError("Archive is not open");
        return false;
    }
    
    if (mFinalized) {
        ReportError("Cannot add files to finalized archive");
        return false;
    }

    if (fileData.empty()) {
        ReportWarning("Empty file data for: " + entryPath);
        return false;
    }

    mAssets.emplace_back(entryPath, fileData, requiresAlignment);
    
    ASSIMP_LOG_INFO("USDZArchiveWriter: Added file ", entryPath, " (", fileData.size(), " bytes, alignment: ", 
                   requiresAlignment ? "required" : "not required", ")");
    return true;
}

// ------------------------------------------------------------------------------------------------
bool USDZArchiveWriter::Finalize() {
    if (!IsOpen()) {
        ReportError("Archive is not open");
        return false;
    }
    
    if (mFinalized) {
        ReportWarning("Archive already finalized");
        return true;
    }

    bool success = true;
    
    // Write all assets to the archive
    for (const auto& asset : mAssets) {
        if (!WriteAssetToArchive(asset)) {
            success = false;
        }
    }
    
    // Close the archive
    if (mZipArchive) {
        zip_close(mZipArchive);
        mZipArchive = nullptr;
    }
    
    mFinalized = true;
    
    if (success) {
        ASSIMP_LOG_INFO("USDZArchiveWriter: Successfully finalized USDZ archive ", mFilename, 
                       " with ", mAssets.size(), " assets");
    } else {
        ReportError("Failed to finalize USDZ archive");
    }
    
    return success;
}



// ------------------------------------------------------------------------------------------------
bool USDZArchiveWriter::WriteAssetToArchive(const AssetInfo& asset) {
    if (!mZipArchive) {
        ReportError("ZIP archive not initialized");
        return false;
    }
    
    // Open ZIP entry
    if (zip_entry_open(mZipArchive, asset.filename.c_str()) != 0) {
        ReportError("Failed to open ZIP entry: " + asset.filename);
        return false;
    }
    
    // Write file data
    if (!asset.data.empty()) {
        if (zip_entry_write(mZipArchive, asset.data.data(), asset.data.size()) != 0) {
            ReportError("Failed to write data to ZIP entry: " + asset.filename);
            zip_entry_close(mZipArchive);
            return false;
        }
    }
    
    // Close ZIP entry
    if (zip_entry_close(mZipArchive) != 0) {
        ReportError("Failed to close ZIP entry: " + asset.filename);
        return false;
    }
    
    return true;
}

// ------------------------------------------------------------------------------------------------
void USDZArchiveWriter::ReportError(const std::string& message) {
    mErrors.push_back(message);
    ASSIMP_LOG_ERROR("USDZArchiveWriter: ", message);
}

// ------------------------------------------------------------------------------------------------
void USDZArchiveWriter::ReportWarning(const std::string& message) {
    mWarnings.push_back(message);
    ASSIMP_LOG_WARN("USDZArchiveWriter: ", message);
}

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_USD_EXPORTER
