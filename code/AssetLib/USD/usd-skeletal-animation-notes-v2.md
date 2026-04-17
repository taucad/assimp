# USD Skeletal Animation Technical Comparison Analysis
## CesiumMan-blender-trunc.usda vs CesiumMan_out-trunc.usda

### FILE HEADER AND METADATA DIFFERENCES

**Coordinate Systems:**
- **Reference (Blender):** `upAxis = "Z"` (line 9)
- **Generated:** `upAxis = "Y"` (line 4)

**Time Code Handling:**
- **Reference:** `startTimeCode = 1, endTimeCode = 48` (lines 7-8)
- **Generated:** `startTimeCode = 0, endTimeCode = 48` (lines 6-7)
- **Impact:** Animation frame indexing differs by 1 frame offset

**Default Prim:**
- **Reference:** `defaultPrim = "root"` (line 3)
- **Generated:** `defaultPrim = "CesiumMan_out"` (line 8)

**Additional Metadata:**
- **Reference:** `doc = "Blender v4.5.0", metersPerUnit = 1, timeCodesPerSecond = 24` (lines 4, 6, 8)
- **Generated:** `customLayerData = {string generator = "Assimp"}` (lines 9-11)

### HIERARCHY AND TRANSFORM STRUCTURE DIFFERENCES

**Root Prim Structure:**
- **Reference:** `def Xform "root"` → `def Xform "Z_UP"` → `def SkelRoot "Armature"`
- **Generated:** `def Xform "CesiumMan_out"` → `def Xform "Z_UP"` → `def SkelRoot "Armature"`

**Transform Operations - Z_UP Prim:**
- **Reference:** 
  ```usda
  float3 xformOp:rotateXYZ = (-90.000015, -0, 0)
  float3 xformOp:scale = (1, 1.0000001, 1.0000001)
  double3 xformOp:translate = (0, 0, 0)
  uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ", "xformOp:scale"]
  ```
- **Generated:**
  ```usda
  quatf xformOp:orient = (0.70710679, -0.70710679, 0, 0)
  uniform token[] xformOpOrder = ["xformOp:orient"]
  ```
- **Critical Issue:** Generated file uses quaternion orientation instead of Euler rotations, and lacks scale/translate operations

**SkelRoot Transform Operations:**
- **Reference:** Has time-sampled transforms:
  ```usda
  float3 xformOp:rotateXYZ.timeSamples = {1: (-0, 89.99999, 0)}
  float3 xformOp:scale.timeSamples = {1: (1.0000001, 1, 1.0000001)}
  double3 xformOp:translate.timeSamples = {1: (0, 0, 0)}
  ```
- **Generated:** **MISSING** - No transform operations on SkelRoot at all
- **Critical Issue:** Time-sampled transforms on SkelRoot are completely missing

### SKELETAL STRUCTURE DIFFERENCES

**Skeleton Prim Path:**
- **Reference:** `</root/Z_UP/Armature/Armature>` (line 42)
- **Generated:** `</CesiumMan_out/Z_UP/Armature/Armature>` (line 23)

**Animation Source Reference:**
- **Reference:** `rel skel:animationSource = </root/Z_UP/Armature/Armature/Anim_0>` (line 49)
- **Generated:** `rel skel:animationSource = </CesiumMan_out/Z_UP/Armature/Armature/SkelAnim>` (line 31)

**Animation Prim Name:**
- **Reference:** `def SkelAnimation "Anim_0"` (line 51)
- **Generated:** `def SkelAnimation "SkelAnim"` (line 33)

**Joint Arrays (truncated comparison):**
- **Reference:** `uniform token[] joints = ["Skeleton_torso_joint_1", "Skeleton_torso_joint_1/Skeleton...` (line 47)
- **Generated:** `uniform token[] joints = ["Skeleton_torso_joint_1", "Skeleton_torso_joint_1/Skeleton...` (line 29)
- **Note:** Joint names appear consistent (full data truncated)

### ANIMATION DATA DIFFERENCES

**Time Sample Frame Indexing:**
- **Reference:** Animation data starts at frame 1: `rotations.timeSamples = {1: [...], 2: [...], ...}` (line 56)
- **Generated:** Animation data starts at frame 2: `rotations.timeSamples = {2: [...], 3: [...], ...}` (line 38)
- **Critical Issue:** Frame offset by +1 in generated file

**Rotation Data Format:**
- **Reference:** `quatf[] rotations = [(0.706595, 0.70666844, 0.025898935, 0.02593308), ...]` (line 54)
- **Generated:** `quatf[] rotations = [(-0.99932816, -0.00005194215, -0.036650763, -0.000024140946), ...]` (line 36)
- **Critical Issue:** Completely different quaternion values - coordinate system conversion issue

**Translation Data Coordinate Differences:**
- **Reference:** `float3[] translations = [(1.9324942e-8, -0.64399713, -0.020000108), ...]` (line 106)
- **Generated:** `float3[] translations = [(0.0000000197135, -0.02000011, 0.64399716), ...]` (line 88)
- **Critical Issue:** Y and Z coordinates appear swapped/negated due to coordinate system conversion

**Scale Data:**
- **Reference:** `half3[] scales = [(1, 1, 1), ...]` (line 105)
- **Generated:** **MISSING** - No scale arrays in animation data
- **Critical Issue:** Scale animation data completely missing

### MESH HIERARCHY AND BINDING DIFFERENCES

**Mesh Container Structure:**
- **Reference:** `def Xform "Cesium_Man"` → `def Mesh "Cesium_Man"` (lines 160-164)
- **Generated:** `def Mesh "Cesium_Man"` directly under SkelRoot (line 143)
- **Issue:** Generated file missing XForm wrapper around mesh

**Mesh Points Coordinate Differences:**
- **Reference:** `point3f[] points = [(0.09342921, -0.9735751, 0.04871457), ...]` (line 177)
- **Generated:** `point3f[] points = [(0.0934292, 0.04871457, 0.973575), ...]` (line 148)
- **Critical Issue:** Coordinate system conversion applied to vertex positions

**Normal Vectors:**
- **Reference:** `normal3f[] normals = [(0.96666527, -0.08140432, 0.24275842), ...]` with `interpolation = "faceVarying"` (lines 174-176)
- **Generated:** `normal3f[] normals = [(0.9666681, 0.2427504, 0.08139491), ...]` with `interpolation = "vertex"` (lines 154-156)
- **Critical Issues:** 1) Coordinate conversion applied 2) Interpolation method changed from faceVarying to vertex

**Texture Coordinates:**
- **Reference:** `texCoord2f[] primvars:st = [(0.273657, 0.19638199), ...]` with `interpolation = "faceVarying"` (lines 187-189)
- **Generated:** `float2[] primvars:st = [(0.273657, 0.803618), ...]` with `interpolation = "vertex"` (lines 166-168)
- **Critical Issues:** 1) V coordinates flipped (0.19638199 vs 0.803618) 2) Type changed from texCoord2f to float2 3) Interpolation changed

**Skeletal Binding:**
- **Reference:** `matrix4d primvars:skel:geomBindTransform = ( (1, -5.293955920339377e-23, -2.0896...` (line 178)
- **Generated:** `matrix4d primvars:skel:geomBindTransform = ( (1.0, 0.0, 0.0, 0.0), (0.0, 1.0, 0.0, 0...` (line 157)
- **Critical Issue:** Bind transform matrix completely different - coordinate system conversion

**Skeleton Reference:**
- **Reference:** `rel skel:skeleton = </root/Z_UP/Armature/Armature>` (line 190)
- **Generated:** `prepend rel skel:skeleton = </SkelRoot/Skeleton>` (line 170)
- **Critical Issue:** Incorrect skeleton path reference

**Joint Data:**
- **Reference:** Joint indices and weights have `interpolation = "vertex"` and `elementSize = 4` (lines 179-186)
- **Generated:** Same structure but different attribute ordering (lines 158-165)

### MATERIAL SYSTEM DIFFERENCES

**Material Scope/Container:**
- **Reference:** `def Scope "_materials"` (line 198)
- **Generated:** `def "Materials"` (line 177)
- **Issue:** Different container type and name

**Material Path References:**
- **Reference:** `rel material:binding = </root/_materials/Cesium_Man_effect>` (line 173)
- **Generated:** `rel material:binding = </CesiumMan_out/Materials/Cesium_Man_effect>` (line 151)

**Material Structure - Reference (Blender):**
```usda
def Material "Cesium_Man_effect" {
    token outputs:surface.connect = </root/_materials/Cesium_Man_effect/Principled_BSDF.outputs:surface>
    def Shader "Principled_BSDF" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </root/_materials/Cesium_Man_effect/Image_Texture.outputs:rgb>
        // ... other PBR parameters
    }
    def Shader "Image_Texture" {
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @./textures/Image_0.jpg@
        float2 inputs:st.connect = </root/_materials/Cesium_Man_effect/uvmap.outputs:result>
    }
    def Shader "uvmap" {
        uniform token info:id = "UsdPrimvarReader_float2"
        string inputs:varname = "st"
    }
}
```

**Material Structure - Generated:**
```usda
def Material "Cesium_Man_effect" {
    token outputs:surface.connect = </CesiumMan_out/Materials/Cesium_Man_effect/UsdPreviewSurface.outputs:surface>
    token outputs:displacement.connect = </CesiumMan_out/Materials/Cesium_Man_effect/UsdPreviewSurface.outputs:displacement>
    token inputs:stPrimvarName = "st"
    
    def Shader "UsdPreviewSurface" { /* ... */ }
    def Shader "texCoordReader" { /* ... */ }
    def Shader "diffuseColor_stTransform" { /* ... */ }
    def Shader "diffuseColor" { /* ... */ }
}
```

**Key Material Differences:**
1. **Shader Names:** "Principled_BSDF" vs "UsdPreviewSurface"
2. **Texture Shader:** "Image_Texture" vs "diffuseColor" 
3. **UV Reader:** "uvmap" vs "texCoordReader"
4. **Generated adds:** displacement output, stPrimvarName input, UV transform shader
5. **Texture File:** `@./textures/Image_0.jpg@` vs `@./textures/Default_albedo.jpg@`
6. **UV Transform:** Generated adds explicit UV transform with `float2 inputs:scale = (1, -1)` and `float2 inputs:translation = (0, 1)` for V-flip
7. **Additional Material:** Generated includes "DefaultMaterial" not present in reference

### MISSING ELEMENTS IN GENERATED FILE

**Scene Elements:**
- **DomeLight:** Reference has `def DomeLight "env_light"` with HDR texture - completely missing in generated file

**Transform Operations:**
- Missing time-sampled transforms on SkelRoot
- Missing XForm wrapper around mesh
- Simplified transform operations (quaternion vs Euler + scale/translate)

**Animation Data:**
- Missing scale arrays in skeletal animation
- Frame offset issue (starts at frame 2 vs frame 1)

## HIGH PRIORITY FIXES FOR ANIMATION FUNCTIONALITY

### Priority 1 (Critical - Animation Breaking):
1. **Frame Indexing Offset:** Animation starts at frame 2 instead of frame 1
2. **Rotation Quaternion Values:** Completely different rotation values due to coordinate conversion issues
3. **Translation Coordinate Conversion:** Y/Z axis swapping in translation data
4. **Missing Scale Animation:** No scale data in skeletal animation
5. **Skeleton Path Reference:** Incorrect path `</SkelRoot/Skeleton>` vs `</root/Z_UP/Armature/Armature>`

### Priority 2 (Structural Issues):
1. **Missing SkelRoot Transforms:** Time-sampled transform operations not generated
2. **Bind Transform Matrix:** Different geomBindTransform values
3. **Missing Mesh XForm Wrapper:** Mesh directly under SkelRoot instead of wrapped in XForm
4. **Normal Interpolation:** Changed from faceVarying to vertex

### Priority 3 (Rendering/Material Issues):
1. **UV Coordinate Flipping:** V coordinates flipped, requiring UV transform shader
2. **Texture Coordinate Type:** texCoord2f vs float2
3. **Material Shader Structure:** Different shader naming and organization
4. **Missing Environment Lighting:** No DomeLight equivalent

### Priority 4 (Metadata/Organization):
1. **Default Prim Path:** Different root prim name
2. **Material Container:** Scope vs unnamed def
3. **Animation Prim Name:** "Anim_0" vs "SkelAnim"

The most critical issues are the animation data problems (frame offset, quaternion values, coordinate conversion) which will prevent the skeletal animation from working correctly.
