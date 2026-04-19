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

/** @file  UnitAxisContract.h
 *  @brief Shared infrastructure for the cross-importer/exporter
 *  `AI_METADATA_UNIT_SCALE_TO_METERS` / `AI_METADATA_UP_AXIS` contract.
 *
 *  Every importer that adopts the contract uses this header to:
 *    1. Read source-file unit/axis declarations.
 *    2. Honour `AI_CONFIG_IMPORT_<FMT>_UNIT_SCALE_TO_METERS` /
 *       `AI_CONFIG_IMPORT_<FMT>_UP_AXIS` overrides.
 *    3. Fall back to a per-format default for genuinely unitless files.
 *    4. Write the resolved values into `aiScene::mMetaData` so exporters
 *       can bake the inverse transform at write time.
 *
 *  Every exporter that adopts the contract uses this header to read
 *  the post-import scene frame and bake a single linear transform
 *  (vertices + normals + tangents + anim-meshes) into the meshes when
 *  the source frame differs from the exporter's spec target.
 *
 *  See repos/assimpjs/assimp/include/assimp/commonMetaData.h for the
 *  metadata key definitions and docs/research/3mf-export-scale-orientation-manifold.md
 *  for the architectural rationale.
 */

#pragma once
#ifndef ASSIMP_COMMON_UNIT_AXIS_CONTRACT_H_INC
#define ASSIMP_COMMON_UNIT_AXIS_CONTRACT_H_INC

#include <assimp/defs.h>
#include <assimp/matrix4x4.h>
#include <assimp/vector3.h>

#include <cstdint>

struct aiScene;

namespace Assimp {

class Importer;

// ---- Validation / construction helpers -------------------------------------

/** Range-validates an axis index. Returns the input value when valid.
 *  Throws DeadlyImportError / DeadlyExportError-equivalent (a runtime_error
 *  derived exception) with full diagnostic context when out of bounds.
 *  Both importers and exporters share the same encoding (0=+X, 1=+Y, 2=+Z).
 */
ASSIMP_API int32_t validateUpAxisInt(int32_t axis, const char *propertyName);

/** Build an axis-rotation matrix that maps `fromAxis`-up vertices into
 *  `toAxis`-up vertices via a 90 deg rotation about the perpendicular axis.
 *  Returns identity when from==to. Both axes MUST be pre-validated to lie
 *  in [0, 2]. Translation column is zero (linear transform only).
 *
 *  Convention table (post-rotation +Y always points to the new up axis):
 *    1 -> 2 (Y-up to Z-up):   rotate -90 about +X
 *    2 -> 1 (Z-up to Y-up):   rotate +90 about +X
 *    0 -> 2 (X-up to Z-up):   rotate +90 about +Y
 *    2 -> 0 (Z-up to X-up):   rotate -90 about +Y
 *    0 -> 1 (X-up to Y-up):   rotate +90 about +Z
 *    1 -> 0 (Y-up to X-up):   rotate -90 about +Z
 */
ASSIMP_API aiMatrix4x4 buildAxisRotationMatrix(int32_t fromAxis, int32_t toAxis);

// ---- Scene-metadata read helpers -------------------------------------------

/** Reads `AI_METADATA_UNIT_SCALE_TO_METERS` (double) from the scene metadata.
 *  Returns 0.0 (sentinel "not declared") when the key is absent — callers
 *  typically interpret 0.0 as "skip rescaling entirely".
 *  Strictly disjoint from the legacy FBX `UnitScaleFactor` key — the latter
 *  is cm-relative and intentionally NOT consulted here.
 */
ASSIMP_API double readSceneUnitScaleToMeters(const aiScene *scene);

/** Reads `AI_METADATA_UP_AXIS` (int32) from the scene metadata. Returns -1
 *  when the key is absent so the caller can decide how to fall back
 *  (typically: skip rotation entirely).
 */
ASSIMP_API int32_t readSceneUpAxis(const aiScene *scene);

// ---- Linear-transform helpers ----------------------------------------------

/** Apply a 3x3 linear transform (the upper-left submatrix of `m`) to a
 *  vector in-place. Translation column is deliberately ignored because
 *  positions in `aiMesh::mVertices` are mesh-local; the bake combines pure
 *  scale and pure rotation only — both translation-free.
 */
ASSIMP_API void applyLinearTransform(const aiMatrix4x4 &m, aiVector3D &v);

/** True iff `m` is the identity to within float epsilon. Used to short-circuit
 *  the bake when the resolver settles on a no-op transform (the common path
 *  for scenes already authored in spec coords).
 */
ASSIMP_API bool isApproxIdentity(const aiMatrix4x4 &m);

// ---- Importer-side writer --------------------------------------------------

/** One-call writer for importers. Allocates `scene->mMetaData` if needed,
 *  records the (post-import) vertex frame, and optionally records the source
 *  format string (the importer-registry fallback already covers most paths,
 *  so passing nullptr is fine when the format is registered with
 *  `aiImporterDesc`).
 *
 *  Pre-conditions: `upAxis` MUST already be in [0, 2] (caller is expected
 *  to use `validateUpAxisInt` first); `unitScaleToMeters` MUST be > 0.
 */
ASSIMP_API void writeContractMetadata(aiScene *scene,
                                      double unitScaleToMeters,
                                      int32_t upAxis,
                                      const char *sourceFormat = nullptr);

// ---- Importer-side resolver ------------------------------------------------

/** Defaults used when the format authors no unit/axis metadata in the file
 *  AND no `AI_CONFIG_IMPORT_<FMT>_*` override is set on the Importer.
 */
struct ContractDefaults {
    double unit;     ///< meters per source unit
    int32_t upAxis;  ///< 0=X, 1=Y, 2=Z
};

/** Reads `AI_CONFIG_IMPORT_<FMT>_UNIT_SCALE_TO_METERS` and
 *  `AI_CONFIG_IMPORT_<FMT>_UP_AXIS` from the Importer, falling back to
 *  the supplied per-format defaults. Mirrors the existing `AI_CONFIG_*`
 *  pattern (cf. `AI_CONFIG_IMPORT_COLLADA_IGNORE_UP_DIRECTION`).
 *
 *  This resolver covers the "config override" + "format default" rungs of
 *  the precedence hierarchy. The "source-file metadata" rung is the
 *  importer's responsibility to detect first, then either use directly OR
 *  forward via the defaults parameter to this helper.
 *
 *  Importers should construct the property keys using the canonical
 *  `AI_CONFIG_IMPORT_<FMT>_UNIT_SCALE_TO_METERS` / `..._UP_AXIS` macros
 *  exposed via assimp/config.h.in. Pass the FMT identifier (e.g. "STL")
 *  as `fmtKey`; the helper builds the full property names internally.
 *
 *  When `importer` is null (no Importer back-reference, e.g. unit-test
 *  shortcuts), returns the defaults verbatim.
 */
ASSIMP_API ContractDefaults resolveImporterContract(const Importer *importer,
                                                    const char *fmtKey,
                                                    ContractDefaults defaults);

// ---- Exporter-side bake ----------------------------------------------------

/** Outcome of `bakeContractTransformIntoMeshes`. Lets callers write
 *  provenance metadata or skip a downstream NormalizeSafe pass when they
 *  know the bake didn't run.
 */
enum class BakeOutcome {
    Skipped_NoOptIn,    ///< Scene metadata didn't declare `UnitScaleToMeters`; legacy callers preserved unchanged.
    Skipped_Identity,   ///< Source frame already matches target frame — bitwise zero-touch.
    Applied             ///< Linear transform was baked into vertices/normals/tangents/anim-meshes.
};

/** Shared exporter bake. Mirrors the glTF2 exporter loop so 3DS / DAE /
 *  FBX / OBJ / PLY / STL / X / X3D / USDA / USDZ exporters all share one
 *  implementation instead of cargo-culting the loop.
 *
 *  Behaviour:
 *    * If `requireOptIn` is true (default) AND the scene does not declare
 *      `AI_METADATA_UNIT_SCALE_TO_METERS`, returns BakeOutcome::Skipped_NoOptIn
 *      without touching any vertex data. This is the backward-compatibility
 *      gate — legacy callers and unmigrated importers produce byte-for-byte
 *      identical exports to pre-contract Assimp.
 *    * Otherwise resolves `(sourceUnit, sourceAxis)` from scene metadata.
 *    * Identity short-circuit: when sourceUnit == targetUnitToMeters AND
 *      sourceAxis == targetUpAxis, returns BakeOutcome::Skipped_Identity
 *      without touching any vertex data (no copies, no float drift).
 *    * Otherwise multiplies a single (scale x rotation) linear transform
 *      into every mesh's positions, normals, tangents and anim-mesh
 *      positions/normals, returning BakeOutcome::Applied.
 *
 *  This helper is only valid for exporters that target a single, fixed
 *  spec frame. Exporters whose target frame is configurable via
 *  `ExportProperty` must resolve the target up-front and pass the result.
 */
ASSIMP_API BakeOutcome bakeContractTransformIntoMeshes(aiScene *scene,
                                                       double targetUnitToMeters,
                                                       int32_t targetUpAxis,
                                                       bool requireOptIn = true);

} // namespace Assimp

#endif // ASSIMP_COMMON_UNIT_AXIS_CONTRACT_H_INC
