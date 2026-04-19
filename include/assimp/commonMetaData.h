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

/** @file commonMetaData.h
 *  @brief Defines a set of common scene metadata keys.
 */
#pragma once
#ifndef AI_COMMONMETADATA_H_INC
#define AI_COMMONMETADATA_H_INC

#ifdef __GNUC__
#pragma GCC system_header
#endif

/// Scene metadata holding the name of the importer which loaded the source asset.
/// This is always present if the scene was created from an imported asset.
#define AI_METADATA_SOURCE_FORMAT "SourceAsset_Format"

/// Scene metadata holding the version of the source asset as a string, if available.
/// Not all formats add this metadata.
#define AI_METADATA_SOURCE_FORMAT_VERSION "SourceAsset_FormatVersion"

/// Scene metadata holding the name of the software which generated the source asset, if available.
/// Not all formats add this metadata.
#define AI_METADATA_SOURCE_GENERATOR "SourceAsset_Generator"

/// Scene metadata holding the source asset copyright statement, if available.
/// Not all formats add this metadata.
#define AI_METADATA_SOURCE_COPYRIGHT "SourceAsset_Copyright"

/// Scene metadata holding the scale factor that converts scene-space distances
/// into meters: `scene_meters = vertex_value * UnitScaleToMeters`.
/// Examples: 1.0 (meters), 0.001 (millimeters), 0.0254 (inches).
///
/// Importers SHOULD set this whenever the source format defines a unit; the
/// value type is `double`. Exporters MAY consume this to rescale geometry for
/// spec-compliant output (e.g. the 3MF exporter rescales to millimetres).
///
/// Disjoint from the legacy `"UnitScaleFactor"` key written by the FBX importer
/// with cm-relative semantics — the two MUST NOT be conflated. New importers
/// and exporters added under this contract use only `UnitScaleToMeters`.
#define AI_METADATA_UNIT_SCALE_TO_METERS "UnitScaleToMeters"

/// Scene metadata holding the up-axis of the post-import vertex coordinates.
/// 0 = +X, 1 = +Y, 2 = +Z. Value type is `int32_t` (matches the existing
/// FBX-populated `UpAxis` encoding, so this key is shared rather than parallel).
///
/// Importers SHOULD set this whenever the source format defines an up-axis;
/// exporters MAY rotate the scene into their own format's required up-axis on
/// write (e.g. the 3MF exporter rotates to +Z per the 3MF Core Specification).
#define AI_METADATA_UP_AXIS "UpAxis"

#endif
