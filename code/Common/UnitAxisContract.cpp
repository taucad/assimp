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

#include "Common/UnitAxisContract.h"

#include <assimp/Exceptional.h>
#include <assimp/Importer.hpp>
#include <assimp/commonMetaData.h>
#include <assimp/metadata.h>
#include <assimp/scene.h>
#include <assimp/mesh.h>

#include <cmath>
#include <cstring>
#include <string>

namespace Assimp {

namespace {

constexpr float kIdentityEps = 1e-6f;
constexpr double kUnitMatchEps = 1e-12;

// Build the canonical AI_CONFIG_IMPORT_<FMT>_UNIT_SCALE_TO_METERS string at
// runtime so we don't have to define a per-format macro for every format up
// front. Identifier is constrained to ASCII uppercase by convention.
std::string buildConfigKey(const char *fmtKey, const char *suffix) {
    std::string result = "IMPORT_";
    result += fmtKey;
    result += '_';
    result += suffix;
    return result;
}

} // anonymous namespace

// ---- Validation ------------------------------------------------------------

int32_t validateUpAxisInt(int32_t axis, const char *propertyName) {
    if (axis < 0 || axis > 2) {
        // We throw DeadlyExportError because every current call site is in
        // an exporter context (post-import scene metadata validation, or an
        // export-property validation). The Exporter::Export pipeline catches
        // DeadlyExportError specifically to capture the message into
        // GetErrorString(); throwing DeadlyImportError here would still
        // surface as AI_FAILURE via the catch-all but would lose the message.
        // Importer-side callers (resolveImporterContract) validate inline
        // and throw DeadlyImportError themselves to keep semantics correct.
        throw DeadlyExportError(
            std::string("Invalid ") + (propertyName ? propertyName : "axis") +
            " value " + std::to_string(axis) +
            "; expected one of [0, 2] (0=X, 1=Y, 2=Z)."
        );
    }
    return axis;
}

aiMatrix4x4 buildAxisRotationMatrix(int32_t fromAxis, int32_t toAxis) {
    aiMatrix4x4 m; // identity
    if (fromAxis == toAxis) {
        return m;
    }
    // Common CAD case: Y-up to Z-up — rotate -90 about +X (so +Y maps to +Z, +Z to -Y).
    if (fromAxis == 1 && toAxis == 2) {
        m.a1 = 1; m.a2 = 0; m.a3 = 0;
        m.b1 = 0; m.b2 = 0; m.b3 = -1;
        m.c1 = 0; m.c2 = 1; m.c3 = 0;
        return m;
    }
    if (fromAxis == 2 && toAxis == 1) {
        m.a1 = 1; m.a2 = 0; m.a3 = 0;
        m.b1 = 0; m.b2 = 0; m.b3 = 1;
        m.c1 = 0; m.c2 = -1; m.c3 = 0;
        return m;
    }
    if (fromAxis == 0 && toAxis == 2) {
        m.a1 = 0; m.a2 = 0; m.a3 = -1;
        m.b1 = 0; m.b2 = 1; m.b3 = 0;
        m.c1 = 1; m.c2 = 0; m.c3 = 0;
        return m;
    }
    if (fromAxis == 2 && toAxis == 0) {
        m.a1 = 0; m.a2 = 0; m.a3 = 1;
        m.b1 = 0; m.b2 = 1; m.b3 = 0;
        m.c1 = -1; m.c2 = 0; m.c3 = 0;
        return m;
    }
    if (fromAxis == 0 && toAxis == 1) {
        m.a1 = 0; m.a2 = -1; m.a3 = 0;
        m.b1 = 1; m.b2 = 0; m.b3 = 0;
        m.c1 = 0; m.c2 = 0; m.c3 = 1;
        return m;
    }
    if (fromAxis == 1 && toAxis == 0) {
        m.a1 = 0; m.a2 = 1; m.a3 = 0;
        m.b1 = -1; m.b2 = 0; m.b3 = 0;
        m.c1 = 0; m.c2 = 0; m.c3 = 1;
        return m;
    }
    return m;
}

// ---- Scene-metadata reads --------------------------------------------------

double readSceneUnitScaleToMeters(const aiScene *scene) {
    if (!scene || !scene->mMetaData) {
        return 0.0;
    }
    double value = 0.0;
    if (scene->mMetaData->Get(AI_METADATA_UNIT_SCALE_TO_METERS, value)) {
        return value;
    }
    return 0.0;
}

int32_t readSceneUpAxis(const aiScene *scene) {
    if (!scene || !scene->mMetaData) {
        return -1;
    }
    int32_t value = -1;
    if (scene->mMetaData->Get(AI_METADATA_UP_AXIS, value)) {
        return value;
    }
    return -1;
}

// ---- Linear-transform helpers ----------------------------------------------

void applyLinearTransform(const aiMatrix4x4 &m, aiVector3D &v) {
    const float x = v.x, y = v.y, z = v.z;
    v.x = m.a1 * x + m.a2 * y + m.a3 * z;
    v.y = m.b1 * x + m.b2 * y + m.b3 * z;
    v.z = m.c1 * x + m.c2 * y + m.c3 * z;
}

bool isApproxIdentity(const aiMatrix4x4 &m) {
    return std::fabs(m.a1 - 1.0f) < kIdentityEps && std::fabs(m.a2) < kIdentityEps && std::fabs(m.a3) < kIdentityEps &&
           std::fabs(m.b1) < kIdentityEps && std::fabs(m.b2 - 1.0f) < kIdentityEps && std::fabs(m.b3) < kIdentityEps &&
           std::fabs(m.c1) < kIdentityEps && std::fabs(m.c2) < kIdentityEps && std::fabs(m.c3 - 1.0f) < kIdentityEps;
}

// ---- Importer-side writer --------------------------------------------------

void writeContractMetadata(aiScene *scene,
                           double unitScaleToMeters,
                           int32_t upAxis,
                           const char *sourceFormat) {
    if (!scene) {
        return;
    }
    if (!scene->mMetaData) {
        scene->mMetaData = new aiMetadata();
    }
    // Use Set when the key already exists (from a prior contract write or
    // from FBX's legacy `UpAxis` write) so the contract value wins. Add
    // when absent. aiMetadata's Add asserts on duplicate keys.
    if (scene->mMetaData->HasKey(AI_METADATA_UNIT_SCALE_TO_METERS)) {
        scene->mMetaData->Set(AI_METADATA_UNIT_SCALE_TO_METERS, unitScaleToMeters);
    } else {
        scene->mMetaData->Add(AI_METADATA_UNIT_SCALE_TO_METERS, unitScaleToMeters);
    }
    if (scene->mMetaData->HasKey(AI_METADATA_UP_AXIS)) {
        scene->mMetaData->Set(AI_METADATA_UP_AXIS, upAxis);
    } else {
        scene->mMetaData->Add(AI_METADATA_UP_AXIS, upAxis);
    }
    // Note: AI_METADATA_SOURCE_FORMAT is intentionally NOT written here.
    // Importer::ReadFile fills it from the importer's desc->mName when no
    // importer pre-populated it; pre-populating with a short identifier
    // here would clobber that canonical "<Format> Importer" string and
    // break downstream consumers that expect the desc-derived value.
    (void)sourceFormat;
}

// ---- Importer-side resolver ------------------------------------------------

ContractDefaults resolveImporterContract(const Importer *importer,
                                         const char *fmtKey,
                                         ContractDefaults defaults) {
    if (importer == nullptr || fmtKey == nullptr || fmtKey[0] == '\0') {
        return defaults;
    }
    ContractDefaults out = defaults;

    // Floating-point override. Importer::GetPropertyFloat uses 10e10 as the
    // sentinel for "not set" (cf. Importer.hpp:289-290) — we reuse it instead
    // of relying on a raw HasProperty check to keep parity with the rest of
    // the codebase.
    constexpr ai_real kNotSet = static_cast<ai_real>(10e10);
    const std::string unitKey = buildConfigKey(fmtKey, "UNIT_SCALE_TO_METERS");
    const ai_real unitOverride = importer->GetPropertyFloat(unitKey.c_str(), kNotSet);
    if (unitOverride < kNotSet * 0.5) { // any value < 5e10 is a real override
        out.unit = static_cast<double>(unitOverride);
    }

    // Integer override. Importer::GetPropertyInteger uses 0xffffffff (i.e.
    // -1 when interpreted as int32) as the sentinel for "not set". We accept
    // any value in [0, 2] as an explicit override.
    const std::string axisKey = buildConfigKey(fmtKey, "UP_AXIS");
    const int axisOverride = importer->GetPropertyInteger(axisKey.c_str(), -1);
    if (axisOverride >= 0) {
        // Inline validation throws DeadlyImportError so the importer's
        // exception region surfaces a clean error string (cf. ReadFile catch
        // at Importer.cpp:509). Equivalent to validateUpAxisInt's body but
        // with the import-side exception type.
        if (axisOverride > 2) {
            throw DeadlyImportError(
                std::string("Invalid ") + axisKey + " value " +
                std::to_string(axisOverride) +
                "; expected one of [0, 2] (0=X, 1=Y, 2=Z)."
            );
        }
        out.upAxis = static_cast<int32_t>(axisOverride);
    }

    return out;
}

// ---- Exporter-side bake ----------------------------------------------------

namespace {

// Apply `vertexScale` (uniform) and/or `axisRotation` (linear) to a single
// vertex in-place. Caller has already proven that at least one is non-trivial.
inline void bakeOne(float scaleF, bool needsScale, bool needsRotation,
                    const aiMatrix4x4 &axisRotation, aiVector3D &v) {
    if (needsScale) {
        v.x *= scaleF;
        v.y *= scaleF;
        v.z *= scaleF;
    }
    if (needsRotation) {
        applyLinearTransform(axisRotation, v);
    }
}

void bakeTransformIntoMesh(aiMesh *mesh, double vertexScale,
                           const aiMatrix4x4 &axisRotation,
                           bool needsScale, bool needsRotation) {
    if (mesh == nullptr) {
        return;
    }
    const float scaleF = static_cast<float>(vertexScale);

    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        bakeOne(scaleF, needsScale, needsRotation, axisRotation, mesh->mVertices[i]);
    }
    if (needsRotation && mesh->mNormals != nullptr) {
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            applyLinearTransform(axisRotation, mesh->mNormals[i]);
        }
    }
    if (needsRotation && mesh->mTangents != nullptr) {
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            applyLinearTransform(axisRotation, mesh->mTangents[i]);
        }
    }
    if (needsRotation && mesh->mBitangents != nullptr) {
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            applyLinearTransform(axisRotation, mesh->mBitangents[i]);
        }
    }
    for (unsigned int am = 0; am < mesh->mNumAnimMeshes; ++am) {
        aiAnimMesh *pAnimMesh = mesh->mAnimMeshes[am];
        if (pAnimMesh == nullptr) {
            continue;
        }
        if (pAnimMesh->mVertices != nullptr) {
            for (unsigned int i = 0; i < pAnimMesh->mNumVertices; ++i) {
                bakeOne(scaleF, needsScale, needsRotation, axisRotation, pAnimMesh->mVertices[i]);
            }
        }
        if (needsRotation && pAnimMesh->mNormals != nullptr) {
            for (unsigned int i = 0; i < pAnimMesh->mNumVertices; ++i) {
                applyLinearTransform(axisRotation, pAnimMesh->mNormals[i]);
            }
        }
        if (needsRotation && pAnimMesh->mTangents != nullptr) {
            for (unsigned int i = 0; i < pAnimMesh->mNumVertices; ++i) {
                applyLinearTransform(axisRotation, pAnimMesh->mTangents[i]);
            }
        }
        if (needsRotation && pAnimMesh->mBitangents != nullptr) {
            for (unsigned int i = 0; i < pAnimMesh->mNumVertices; ++i) {
                applyLinearTransform(axisRotation, pAnimMesh->mBitangents[i]);
            }
        }
    }
}

} // anonymous namespace

BakeOutcome bakeContractTransformIntoMeshes(aiScene *scene,
                                            double targetUnitToMeters,
                                            int32_t targetUpAxis,
                                            bool requireOptIn) {
    if (scene == nullptr) {
        return BakeOutcome::Skipped_NoOptIn;
    }
    validateUpAxisInt(targetUpAxis, "bakeContractTransformIntoMeshes(targetUpAxis)");

    const bool optedIn = scene->mMetaData != nullptr &&
        scene->mMetaData->HasKey(AI_METADATA_UNIT_SCALE_TO_METERS);
    if (requireOptIn && !optedIn) {
        return BakeOutcome::Skipped_NoOptIn;
    }

    const double sourceUnit = readSceneUnitScaleToMeters(scene);
    const int32_t sourceAxis = readSceneUpAxis(scene);

    // Identity short-circuit: when the declared source frame already matches
    // the exporter's target spec, do not touch any vertex data. Float drift
    // is bitwise-zero in this branch, satisfying the same-format identity
    // round-trip property tested by utContractRoundTripMatrix.
    const bool unitDeclared = sourceUnit > 0.0;
    const bool axisDeclared = sourceAxis >= 0;
    const bool unitMatches = !unitDeclared || std::fabs(sourceUnit - targetUnitToMeters) < kUnitMatchEps;
    const bool axisMatches = !axisDeclared || sourceAxis == targetUpAxis;
    if (unitMatches && axisMatches) {
        return BakeOutcome::Skipped_Identity;
    }

    double vertexScale = 1.0;
    if (unitDeclared && targetUnitToMeters > 0.0) {
        vertexScale = sourceUnit / targetUnitToMeters;
    }
    aiMatrix4x4 axisRotation; // identity by default
    if (axisDeclared) {
        validateUpAxisInt(sourceAxis, AI_METADATA_UP_AXIS " (scene metadata)");
        axisRotation = buildAxisRotationMatrix(sourceAxis, targetUpAxis);
    }

    const bool needsScale = std::fabs(vertexScale - 1.0) > kUnitMatchEps;
    const bool needsRotation = !isApproxIdentity(axisRotation);
    if (!needsScale && !needsRotation) {
        return BakeOutcome::Skipped_Identity;
    }

    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        bakeTransformIntoMesh(scene->mMeshes[i], vertexScale, axisRotation,
                              needsScale, needsRotation);
    }
    return BakeOutcome::Applied;
}

} // namespace Assimp
