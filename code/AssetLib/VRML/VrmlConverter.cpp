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
/// \file   VrmlImporter.cpp
/// \brief  Convert VRML-formatted (.wrl, .x3dv) files to X3D .xml format
/// \date   2024
/// \author tellypresence

#ifndef ASSIMP_BUILD_NO_X3D_IMPORTER

#include <memory> // std::unique_ptr
#include <fstream>
#include <sstream>
#include <cstdlib> // mbstowcs
#include "VrmlConverter.hpp"

namespace Assimp {

bool isFileWrlVrml97Ext(const std::string &pFile) {
    size_t pos = pFile.find_last_of('.');
    if (pos == std::string::npos) {
        return false;
    }
    std::string ext = pFile.substr(pos + 1);
    if (ext.size() != 3) {
        return false;
    }
    return (ext[0] == 'w' || ext[0] == 'W') && (ext[1] == 'r' || ext[1] == 'R') && (ext[2] == 'l' || ext[2] == 'L');
}

bool isFileX3dvClassicVrmlExt(const std::string &pFile) {
    size_t pos = pFile.find_last_of('.');
    if (pos == std::string::npos) {
        return false;
    }
    std::string ext = pFile.substr(pos + 1);
    if (ext.size() != 4) {
        return false;
    }
    return (ext[0] == 'x' || ext[0] == 'X') && (ext[1] == '3') && (ext[2] == 'd' || ext[2] == 'D') && (ext[3] == 'v' || ext[3] == 'V');
}

#if !defined(ASSIMP_BUILD_NO_VRML_IMPORTER)
static VrmlTranslator::Scanner createScanner(const std::string &fileContent) {
    // Use the buffer constructor, not the filename constructor
    const unsigned char* buffer = reinterpret_cast<const unsigned char*>(fileContent.c_str());
    int len = static_cast<int>(fileContent.length());
    
    return VrmlTranslator::Scanner(buffer, len);
}
#endif // #if !defined(ASSIMP_BUILD_NO_VRML_IMPORTER)

std::stringstream ConvertVrmlFileToX3dXmlFile(const std::string &pFile, IOSystem *pIOHandler) {
    std::stringstream ss;
    if (isFileWrlVrml97Ext(pFile) || isFileX3dvClassicVrmlExt(pFile)) {
#if !defined(ASSIMP_BUILD_NO_VRML_IMPORTER)
        std::string content;
        bool fileLoaded = false;
        
        // Try to read using IOSystem first (for WASM/Emscripten)
        if (pIOHandler) {
            IOStream* pStream = pIOHandler->Open(pFile, "rb");
            if (pStream) {
                size_t fileSize = pStream->FileSize();
                if (fileSize > 0) {
                    content.resize(fileSize);
                    size_t bytesRead = pStream->Read(&content[0], 1, fileSize);
                    if (bytesRead == fileSize) {
                        fileLoaded = true;
                    } else {
                        content.clear();
                    }
                }
                pIOHandler->Close(pStream);
            }
        }
        
        // Fallback to std::ifstream (for native builds)
        if (!fileLoaded) {
            std::ifstream file(pFile, std::ios::in);
            if (file.is_open()) {
                std::stringstream fileContent;
                fileContent << file.rdbuf();
                file.close();
                content = fileContent.str();
                fileLoaded = true;
            }
        }
        
        if (fileLoaded && content.size() > 0) {
            try {
                VrmlTranslator::Scanner scanner = createScanner(content);
                VrmlTranslator::Parser parser(&scanner);
                parser.Parse();
                ss.str("");
                parser.doc_.save(ss);
            } catch (const std::exception& e) {
                // Silent error handling - parsing failed
            } catch (...) {
                // Silent error handling - unknown exception
            }
        }
#endif // #if !defined(ASSIMP_BUILD_NO_VRML_IMPORTER)
    }
    return ss;
}

} // namespace Assimp

#endif // !ASSIMP_BUILD_NO_X3D_IMPORTER
