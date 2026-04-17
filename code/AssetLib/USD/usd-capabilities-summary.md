# USD Capabilities Summary: Spec, TinyUSDZ, and Assimp

## 1. USD Specification Feature Map

The Universal Scene Description (USD) specification, developed by Pixar, defines a comprehensive
set of schema domains for 3D scene interchange. This document maps each domain against what
TinyUSDZ v0.9.1 can handle and what the assimp importer/exporter currently implements.

### 1.1 File Formats

| Format | Description | TinyUSDZ Read | TinyUSDZ Write | Assimp Import | Assimp Export |
|--------|-------------|:---:|:---:|:---:|:---:|
| USDA (.usda) | ASCII | Yes | Yes | Yes | Yes |
| USDC (.usdc) | Binary/Crate | Yes | Experimental | Yes | Yes |
| USD (.usd) | Auto-detect | Yes | N/A | Yes | N/A |
| USDZ (.usdz) | ZIP archive | Yes | Yes | Yes | Yes |

---

## 2. UsdGeom (Geometry)

### 2.1 Prim Types

| Prim Type | Spec | TinyUSDZ Read | TinyUSDZ Write | Assimp Import | Assimp Export |
|-----------|------|:---:|:---:|:---:|:---:|
| Mesh | Polygonal geometry | Yes | Yes | Yes | Yes |
| Xform | Transform node | Yes | Yes | Yes | Yes |
| Scope | Organizational group | Yes | Yes | Yes | Yes |
| Points | Point clouds | Yes | Yes | No | Partial (degenerate tris) |
| BasisCurves | Curves (hair/fur) | Yes | Yes | No | No |
| NurbsCurves | NURBS curves | Yes | Yes | No | No |
| NurbsPatch | NURBS surfaces | No | No | No | No |
| HermiteCurves | Hermite curves | No | No | No | No |
| Cube | Parametric | Yes | Yes | No | No |
| Sphere | Parametric | Yes | Yes | No | No |
| Cylinder | Parametric | Yes | Yes | No | No |
| Cone | Parametric | Yes | Yes | No | No |
| Capsule | Parametric | Yes | Yes | No | No |
| GeomSubset | Mesh subsets | Yes | Yes | Yes | Yes |
| PointInstancer | Instancing | Yes | Yes | No | No |

### 2.2 Geometry Attributes

| Attribute | TinyUSDZ | Assimp Import | Assimp Export |
|-----------|:---:|:---:|:---:|
| points (positions) | Yes | Yes | Yes |
| faceVertexIndices | Yes | Yes | Yes |
| faceVertexCounts | Yes | Yes | Yes |
| normals | Yes | Yes | Yes |
| primvars:st (UVs) | Yes | Yes | Yes |
| Multiple UV sets | Yes | Yes | Yes |
| Vertex colors | Yes | Yes | Yes |
| Tangents | No native | No | Yes |
| extent (bounds) | Yes | No | No |
| doubleSided | Yes | No | No |
| subdivisionScheme | Parse only | No | Detection only |

---

## 3. UsdShade (Materials)

### 3.1 Shader Types

| Shader | TinyUSDZ Read | TinyUSDZ Write | Assimp Import | Assimp Export |
|--------|:---:|:---:|:---:|:---:|
| UsdPreviewSurface | Yes | Yes | Yes | Yes |
| UsdUVTexture | Yes | Yes | Yes | Yes |
| UsdPrimvarReader_float2 | Yes | Yes | Yes | Yes |
| UsdTransform2d | Yes | Yes | Yes | Yes |
| MaterialX (UsdMtlx) | Import only | No | No | No |

### 3.2 Material Properties

| Property | Assimp Import | Assimp Export |
|----------|:---:|:---:|
| diffuseColor | Yes | Yes |
| metallic | Yes | Yes |
| roughness | Yes | Yes |
| opacity | Yes | Yes |
| emissiveColor | Yes | Yes |
| normal map | Yes | Yes |
| occlusion map | Yes | Yes |
| clearcoat | Yes | Yes |
| clearcoatRoughness | Yes | Yes |
| ior | Yes | Yes |
| opacityThreshold | Yes | Yes |
| displacement | Yes | No |
| specularColor | Yes | Yes |

### 3.3 Texture Features

| Feature | TinyUSDZ | Assimp Import | Assimp Export |
|---------|:---:|:---:|:---:|
| Embedded textures (USDZ) | Yes | Yes | Yes |
| External texture refs | Yes | Yes | Partial |
| PNG format | Yes | Yes | Yes |
| JPEG format | Yes | Yes | Yes |
| EXR format | Optional | No | No |
| UDIM textures | No | No | No |
| Color space (sRGB/raw) | Yes | Yes | Yes |
| Wrap modes | Yes | Yes | Yes |
| UV transforms | Yes | Yes | Yes |

---

## 4. UsdSkel (Skeletal Animation)

### 4.1 Prim Types

| Prim Type | TinyUSDZ Read | TinyUSDZ Write | Assimp Import | Assimp Export |
|-----------|:---:|:---:|:---:|:---:|
| SkelRoot | Yes | Yes | Yes | Yes |
| Skeleton | Yes | Yes | Yes | Yes |
| SkelAnimation | Yes | Yes | Stub (see notes) | Yes |
| BlendShape | Yes | Yes | Yes | Yes |

### 4.2 Skeletal Features

| Feature | TinyUSDZ | Assimp Import | Assimp Export |
|---------|:---:|:---:|:---:|
| Joint hierarchy | Yes | Yes (via nodes) | Yes |
| Bind transforms | Yes | Yes | Yes |
| Rest transforms | Yes | Yes | Yes |
| Joint indices | Yes | Yes | Yes |
| Joint weights | Yes | Yes | Yes |
| Joint animation (TRS) | Yes | Stub | Yes |
| Blend shape geometry | Yes | Yes | Yes |
| Blend shape weights anim | Yes | Yes | Yes |
| In-between blend shapes | Yes | No | No |
| skel:skeleton relationship | Yes | Yes | Yes |
| skel:animationSource | Yes | Yes | Yes |
| skel:blendShapeTargets | Yes | Yes | Yes |

**Import note:** The importer creates a stub animation when the Tydra RenderScene has no
animations. The exporter already writes full joint TRS animation via `CreateCentralizedSkelAnimation()`.
The importer needs to parse SkelAnimation prims directly from the Stage to complete the round-trip.

---

## 5. UsdGeom Animation (Transform Animation)

| Feature | TinyUSDZ Read | TinyUSDZ Tydra | Assimp Import | Assimp Export |
|---------|:---:|:---:|:---:|:---:|
| Time-sampled xformOps | Yes | Not converted | Via Tydra channels | Yes (XformOp API) |
| Translation keyframes | Yes | Not converted | Yes | Yes |
| Rotation keyframes | Yes | Not converted | Yes | Yes |
| Scale keyframes | Yes | Not converted | Yes | Yes |
| Matrix animation | Yes | Not converted | No | No |

**Note:** Tydra's RenderSceneConverter does not extract Xform animations. The assimp importer
gets node animations from Tydra's `animation.channels_map` when available. Non-skeletal
animations use the `ConvertAnimation()` exporter path with XformOp time samples.

---

## 6. UsdLux (Lighting)

| Light Type | TinyUSDZ Read | TinyUSDZ Write | Assimp Import | Assimp Export |
|------------|:---:|:---:|:---:|:---:|
| DistantLight | Yes | Yes | Yes | Yes |
| SphereLight | Yes | Yes | Yes | Yes |
| RectLight | Yes | Yes | Yes | Yes |
| DiskLight | Yes | Yes | Yes | No |
| CylinderLight | Yes | Yes | Yes | No |
| DomeLight | Yes | Yes | Yes | No |
| GeometryLight | No | No | No | No |
| PortalLight | No | No | No | No |

| Property | Assimp Import | Assimp Export |
|----------|:---:|:---:|
| intensity | Yes | Yes |
| color | Yes | Yes |
| exposure | Yes | No |
| radius | Yes | Yes |
| angle | Yes | No |
| width/height | Yes | Yes |

---

## 7. UsdGeomCamera (Cameras)

| Feature | TinyUSDZ | Assimp Import | Assimp Export |
|---------|:---:|:---:|:---:|
| Perspective projection | Yes | Yes | Yes |
| Orthographic projection | Yes | Yes | Yes |
| Focal length | Yes | Yes | Yes |
| Horizontal aperture | Yes | Yes | Yes |
| Vertical aperture | Yes | Yes | Yes |
| Clipping range | Yes | Yes | Yes |
| FOV calculation | Yes | Yes | Yes |

---

## 8. Composition System

| Arc Type | TinyUSDZ Read | TinyUSDZ Write | Assimp Usage |
|----------|:---:|:---:|:---:|
| Sublayers | Yes | Yes | Not used |
| References | Yes | Yes | Not used |
| Payloads | Yes (eager) | Yes | Not used |
| Variants | Yes | Yes | Not used |
| Inherits | Yes | Yes | Not used |
| Specializes | Parse only | No | Not used |

The composition system is handled entirely within TinyUSDZ during stage loading.
Assimp operates on the flattened/composed stage and does not create composition arcs on export.

---

## 9. Unsupported USD Domains

| Domain | TinyUSDZ Status | Assimp Status |
|--------|-----------------|---------------|
| UsdMedia (Audio) | Not supported | Not supported |
| UsdPhysics | Parse only | Not supported |
| UsdVol (Volumes) | Data structures only | Placeholder |
| UsdAR (Spatial/AR) | Not supported | Metadata only |
| UsdUI | Not supported | Not supported |
| UsdRender (Settings) | Partial | Not supported |
| Value Clips | Not supported | Not supported |
| USD Instancing | Partial | Not supported |

---

## 10. Gap Analysis: TinyUSDZ Capabilities Not Yet Used by Assimp

These are features TinyUSDZ supports that the assimp integration could leverage:

1. **Parametric primitives** (Cube, Sphere, Cylinder, Cone, Capsule) - TinyUSDZ can read/write
   these but assimp only handles Mesh prims. Could convert parametrics to meshes on import.
2. **BasisCurves/NurbsCurves** - TinyUSDZ reads/writes these; assimp has no curve representation.
3. **PointInstancer** - Efficient instancing; assimp could flatten to individual meshes.
4. **Composition arcs** - TinyUSDZ handles sublayers/references/variants; assimp could leverage
   for multi-file USD import.
5. **SkelAnimation joint TRS** - TinyUSDZ parses time-sampled joint animations; the importer
   needs to extract these directly from Stage prims rather than relying on Tydra.
6. **DiskLight, CylinderLight, DomeLight export** - TinyUSDZ write support exists; assimp
   exporter only creates DistantLight, SphereLight, RectLight.
7. **Exposure, angle** properties on lights - TinyUSDZ supports; exporter doesn't write them.

---

## 11. Current Implementation Architecture

```
Import: USD File -> tinyusdz::Stage -> tydra::RenderScene -> aiScene
                                    \-> Direct Stage access (cameras, lights)

Export: aiScene -> tinyusdz prims (GeomMesh, Material, Skeleton, etc.)
              -> tinyusdz::Stage -> USDA/USDC/USDZ file
```

The importer uses a hybrid approach: Tydra converts meshes, materials, textures, and
basic animations, while cameras and lights are read directly from the Stage. Skeletal
animation import needs the same direct-Stage approach since Tydra does not convert
SkelAnimation prims.

The exporter constructs tinyusdz prim objects directly and assembles them into a Stage,
then serializes via USDA/USDC writers or the custom USDZ ZIP writer.
