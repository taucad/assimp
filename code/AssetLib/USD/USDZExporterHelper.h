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

/** @file USDZExporterHelper.h
 *  Helper functions and utilities for USDZ export
 */
#ifndef AI_USDZEXPORTERHELPER_H_INC
#define AI_USDZEXPORTERHELPER_H_INC

#ifndef ASSIMP_BUILD_NO_USD_EXPORTER

#include <string>
#include <vector>

namespace Assimp {

/**
 * @brief Get file extension from filename
 * @param filename The filename to extract extension from
 * @return The file extension in lowercase, or empty string if no extension
 */
std::string GetFileExtension(const std::string& filename);

/**
 * @brief Convert string to lowercase
 * @param str Input string
 * @return Lowercase string
 */
std::string ToLower(const std::string& str);

/**
 * @brief Check if a string starts with another string
 * @param str The string to check
 * @param prefix The prefix to look for
 * @return true if str starts with prefix
 */
bool StartsWith(const std::string& str, const std::string& prefix);

/**
 * @brief Check if a string ends with another string
 * @param str The string to check
 * @param suffix The suffix to look for
 * @return true if str ends with suffix
 */
bool EndsWith(const std::string& str, const std::string& suffix);

/**
 * @brief Replace all occurrences of a substring
 * @param str The string to modify
 * @param from The substring to replace
 * @param to The replacement string
 * @return Modified string
 */
std::string ReplaceAll(std::string str, const std::string& from, const std::string& to);

/**
 * @brief Split string by delimiter
 * @param str The string to split
 * @param delimiter The delimiter character
 * @return Vector of split strings
 */
std::vector<std::string> Split(const std::string& str, char delimiter);

/**
 * @brief Join strings with delimiter
 * @param strings Vector of strings to join
 * @param delimiter The delimiter string
 * @return Joined string
 */
std::string Join(const std::vector<std::string>& strings, const std::string& delimiter);

/**
 * @brief Trim whitespace from string
 * @param str The string to trim
 * @return Trimmed string
 */
std::string Trim(const std::string& str);

} // namespace Assimp

#endif // !ASSIMP_BUILD_NO_USD_EXPORTER

#endif // AI_USDZEXPORTERHELPER_H_INC
