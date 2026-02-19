#pragma once

#include <map>
#include <vector>
#include <string>

#include "../../../contrib/tinyusdz/autoclone/tinyusdz_repo-src/src/stage.hh"

namespace tinyusdz {
namespace usdz {

///
/// Save scene as USDZ(ZIP)
///
/// @param[in] filename USDZ filename(UTF-8). WideChar(Unicode) represented as std::string is supported on Windows.
/// @param[in] stage Stage(scene graph).
/// @param[out] warn Warning message
/// @param[out] err Error message
///
/// @return true upon success.
///
bool SaveAsUSDZ(const std::string &filename, const Stage &stage, std::string *warn, std::string *err);

///
/// Save scene as USDZ(ZIP) with provided texture data
///
/// @param[in] filename USDZ filename(UTF-8).
/// @param[in] usdContent USD content as string.
/// @param[in] textureDataMap Map of texture paths to texture data.
/// @param[out] warn Warning message
/// @param[out] err Error message
///
/// @return true upon success.
///
bool SaveAsUSDZWithTextures(const std::string &filename, const std::string &usdContent, 
                            const std::map<std::string, std::vector<uint8_t>> &textureDataMap,
                            std::string *warn, std::string *err);

bool SaveAsUSDZToMemory(const std::string &usdContent,
                        const std::map<std::string, std::vector<uint8_t>> &textureDataMap,
                        std::vector<uint8_t> &outData,
                        std::string *warn, std::string *err);

#if defined(_WIN32)
// WideChar(UNICODE) filename version.
bool SaveAsUSDZ(const std::wstring &filename, const Stage &stage, std::string *warn, std::string *err);
#endif

} // namespace usdz
} // namespace tinyusdz
