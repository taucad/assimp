#!/usr/bin/env bash
# Exports a fixture matrix to USDA, USDC and USDZ with the assimp command-line
# tool and validates every output with OpenUSD.
#
# Usage: scripts/usd/validate-exports.sh <assimp binary> <output dir>
#
# Environment:
#   USD_PYTHON_ARKIT   python with usd-core 25.x (ComplianceChecker --arkit)
#   USD_PYTHON_LATEST  python with usd-core 26.x (all UsdValidation validators)
#   USD_EXTRA_FIXTURES space-separated extra input files (e.g. large grids)
#
# Every USDZ must pass both checkers. A USDA/USDC output may fail only the
# ARKit prim-type rule, and only for UsdLux lights (ARKit allows no lights).
# Each USDC is also opened with Usd.Stage.Open and its Mesh/Points counts
# must match the USDA export of the same input.
set -u

if [ $# -ne 2 ]; then
    sed -n '2,15p' "$0"
    exit 2
fi
ASSIMP=$1
OUT=$2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CHECK="$ROOT/scripts/usd/strict_check.py"
PY_ARKIT=${USD_PYTHON_ARKIT:-python3}
PY_LATEST=${USD_PYTHON_LATEST:-python3}
M="$ROOT/test/models"

FIXTURES=(
    "$M/glTF2/DamagedHelmet.glb"
    "$M/glTF2/BoxTextured-glTF/BoxTextured.gltf"
    "$M/glTF2/BoxTextured-glTF-Binary/BoxTextured.glb"
    "$M/glTF2/BoxTextured-glTF-pbrSpecularGlossiness/BoxTextured.gltf"
    "$M/glTF2/CesiumMan/CesiumMan.glb"
    "$M/glTF2/AnimatedMorphCube/glTF/AnimatedMorphCube.gltf"
    "$M/glTF2/SimpleMorph/glTF/SimpleMorph.gltf"
    "$M/glTF2/simple_skin/simple_skin.gltf"
    "$M/glTF2/simple_skin/quad_skin.glb"
    "$M/glTF2/cameras/Cameras.gltf"
    "$M/glTF2/textureTransform/TextureTransformTest.gltf"
    "$M/glTF2/2CylinderEngine-glTF-Binary/2CylinderEngine.glb"
    "$M/glTF2/ClearCoat-glTF/ClearCoatTest.gltf"
    "$M/glTF2/AlphaBlend-glTF/Material_AlphaBlend_00.gltf"
    "$M/OBJ/box.obj"
    "$M/OBJ/cube_with_vertexcolors.obj"
    "$M/OBJ/point_cloud.obj"
    "$M/PLY/cube.ply"
    "$M/PLY/points.ply"
    "$M/STL/Spider_binary.stl"
    "$M/Collada/COLLADA.dae"
    "$M/Collada/duck.dae"
    "$M/FBX/animation_with_skeleton.fbx"
    "$M/FBX/huesitos.fbx"
    "$M/FBX/maxPbrMaterial_metalRough.fbx"
    "$M/SMD/holy_grailref.smd"
)
# shellcheck disable=SC2206
FIXTURES+=(${USD_EXTRA_FIXTURES:-})

mkdir -p "$OUT"
fail=0
summary=()

check() { # python, file -> prints problems, returns checker status
    "$1" "$CHECK" "$2" 2>&1
}

only_light_prim_type_failures() { # checker output on stdin
    local problems
    problems=$(grep -E '^    ' || true)
    [ -n "$problems" ] || return 1
    ! grep -vE 'type .*(SphereLight|DistantLight|RectLight|DiskLight|CylinderLight|DomeLight)' <<<"$problems" >/dev/null
}

prim_counts() { # usd file -> "meshes points"
    "$PY_LATEST" - "$1" <<'EOF'
import sys
from pxr import Usd, UsdGeom
stage = Usd.Stage.Open(sys.argv[1])
prims = list(stage.Traverse())
print(sum(p.IsA(UsdGeom.Mesh) for p in prims), sum(p.IsA(UsdGeom.Points) for p in prims))
EOF
}

for input in "${FIXTURES[@]}"; do
    if [ ! -f "$input" ]; then
        echo "missing fixture: $input"
        fail=1
        continue
    fi
    stem=$(basename "$(dirname "$input")")_$(basename "${input%.*}")
    for ext in usda usdc usdz; do
        dir="$OUT/$stem"
        mkdir -p "$dir"
        out="$dir/$stem.$ext"
        if ! "$ASSIMP" export "$input" "$out" >"$out.log" 2>&1; then
            summary+=("FAIL export $stem.$ext (see $out.log)")
            fail=1
            continue
        fi
        status=ok
        for py in "$PY_ARKIT" "$PY_LATEST"; do
            if ! res=$(check "$py" "$out"); then
                if [ "$ext" != usdz ] && only_light_prim_type_failures <<<"$res"; then
                    status="ok (lights: ARKit prim type)"
                else
                    status=FAIL
                    echo "$res" >>"$out.check.log"
                fi
            fi
        done
        [ "$status" = FAIL ] && fail=1
        summary+=("$status $stem.$ext")
    done
    if [ -f "$OUT/$stem/$stem.usda" ] && [ -f "$OUT/$stem/$stem.usdc" ]; then
        a=$(prim_counts "$OUT/$stem/$stem.usda")
        c=$(prim_counts "$OUT/$stem/$stem.usdc")
        if [ "$a" != "$c" ] || [ -z "$c" ]; then
            summary+=("FAIL counts $stem: usda=($a) usdc=($c)")
            fail=1
        else
            summary+=("ok counts $stem: meshes/points $c")
        fi
    fi
done

printf '%s\n' "${summary[@]}"
exit $fail
