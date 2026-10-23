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

/** @file  USDLoader.cpp
 *  @brief Implementation of the USD importer front end.
 */

#ifndef ASSIMP_BUILD_NO_USD_IMPORTER

#include "USDLoader.h"
#include "USDLoaderImplLightUSD.h"
#include "Common/UnitAxisContract.h"

#include <assimp/commonMetaData.h>
#include <assimp/config.h>
#include <assimp/importerdesc.h>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>

#include <functional>
#include <memory>

namespace Assimp {

namespace {

constexpr aiImporterDesc kUsdImporterDesc = {
    "USD Object Importer",
    "",
    "",
    "https://openusd.org/",
    aiImporterFlags_SupportTextFlavour | aiImporterFlags_SupportBinaryFlavour,
    0,
    0,
    0,
    0,
    "usd usda usdc usdz"
};

// Large enough for a ZIP local header plus a reasonable first member name.
constexpr size_t kUsdSniffBytes = 512;

} // namespace

bool USDImporter::CanRead(const std::string &pFile, IOSystem *pIOHandler, bool /*checkSig*/) const {
    if (SimpleExtensionCheck(pFile, "usd", "usda", "usdc", "usdz")) {
        return true;
    }
    if (pIOHandler == nullptr) {
        return false;
    }
    // Content sniffing, so that e.g. a USDC payload with a foreign extension
    // is still recognised (and then dispatched on content, not extension).
    std::unique_ptr<IOStream, std::function<void(IOStream *)>> stream(
            pIOHandler->Open(pFile, "rb"), [pIOHandler](IOStream *s) { pIOHandler->Close(s); });
    if (!stream) {
        return false;
    }
    uint8_t head[kUsdSniffBytes];
    const size_t got = stream->Read(head, 1, sizeof(head));
    return USDImport::DetectFormat(head, got) != USDImport::SourceFormat::Unknown;
}

const aiImporterDesc *USDImporter::GetInfo() const {
    return &kUsdImporterDesc;
}

void USDImporter::SetupProperties(const Importer *pImp) {
    mImporter = pImp;
    const int maxMemory = pImp->GetPropertyInteger(AI_CONFIG_IMPORT_USD_MAX_MEMORY_MB, 16384);
    mMaxMemoryMB = maxMemory > 0 ? static_cast<uint32_t>(maxMemory) : 16384u;
}

void USDImporter::InternReadFile(const std::string &pFile, aiScene *pScene, IOSystem *pIOHandler) {
    USDImport::ImportConfig config;
    config.maxMemoryMB = mMaxMemoryMB;
    USDImport::SourceFrame frame;
    USDImport::ReadScene(pFile, pScene, pIOHandler, config, &frame);

    // Unit/up-axis contract. The importer applies no axis conversion: the
    // metadata reflects the stage values (USD defaults: 1 metre, Y up) unless
    // the caller overrides them through AI_CONFIG_IMPORT_USD_*.
    const ContractDefaults defaults{ frame.metersPerUnit, frame.upAxis };
    const ContractDefaults resolved = resolveImporterContract(mImporter, "USD", defaults);
    writeContractMetadata(pScene, resolved.unit, resolved.upAxis, "USD");

    // Shared USD contract: `UnitScaleFactor` carries metersPerUnit and
    // `SourceAsset_Format` names the format family.
    if (pScene->mMetaData->HasKey("UnitScaleFactor")) {
        pScene->mMetaData->Set("UnitScaleFactor", resolved.unit);
    } else {
        pScene->mMetaData->Add("UnitScaleFactor", resolved.unit);
    }
    const aiString format("USD");
    if (pScene->mMetaData->HasKey(AI_METADATA_SOURCE_FORMAT)) {
        pScene->mMetaData->Set(AI_METADATA_SOURCE_FORMAT, format);
    } else {
        pScene->mMetaData->Add(AI_METADATA_SOURCE_FORMAT, format);
    }
}

} // namespace Assimp

#endif // !! ASSIMP_BUILD_NO_USD_IMPORTER
