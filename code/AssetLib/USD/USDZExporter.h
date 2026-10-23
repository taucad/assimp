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


/** @file USDZExporter.h
 *  @brief USD exporters (USDA, USDC, USDZ) built on LightUSD.
 *
 *  All output goes through the IOSystem passed by Assimp::Exporter. The
 *  exporters never touch the file system directly, so they work with the
 *  blob exporter, custom IOSystems and WebAssembly.
 */
#pragma once
#ifndef AI_USDZ_EXPORTER_H_INC
#define AI_USDZ_EXPORTER_H_INC

#ifndef ASSIMP_BUILD_NO_EXPORT
#ifndef ASSIMP_BUILD_NO_USD_EXPORTER

struct aiScene;

namespace Assimp {

class IOSystem;
class ExportProperties;

/// USDA text layer. External textures are written next to the output file.
void ExportSceneUSDA(const char *pFile, IOSystem *pIOSystem, const aiScene *pScene,
        const ExportProperties *pProperties);

/// USDC (Crate) binary layer. External textures are written next to the output.
void ExportSceneUSDC(const char *pFile, IOSystem *pIOSystem, const aiScene *pScene,
        const ExportProperties *pProperties);

/// ARKit-compliant USDZ package: `<stem>.usdc` root layer plus PNG/JPEG textures.
void ExportSceneUSDZ(const char *pFile, IOSystem *pIOSystem, const aiScene *pScene,
        const ExportProperties *pProperties);

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_USD_EXPORTER
#endif // ASSIMP_BUILD_NO_EXPORT
#endif // AI_USDZ_EXPORTER_H_INC
