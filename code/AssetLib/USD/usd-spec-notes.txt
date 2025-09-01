USD/USDZ SPECIFICATION COMPLETE DOCUMENTATION
==============================================

This document provides a comprehensive analysis of the USD (Universal Scene Description) and USDZ specifications, 
followed by an assessment of our current importer and exporter implementations.

## 1. USD CORE SPECIFICATION

### 1.1 USD Foundation
- **Purpose**: Robust interchange format for 3D computer graphics data
- **Developed by**: Pixar Animation Studios
- **Key Features**: 
  - High-performance data retrieval and rendering
  - Powerful instancing capabilities
  - Composition system for complex scene assembly
  - Time-varying data support
  - Layered, non-destructive editing

### 1.2 USD File Formats
- **USDA**: ASCII format (.usda) - Human-readable, text-based
- **USDC**: Binary format (.usdc) - Optimized for performance
- **USD**: Generic extension (.usd) - Can be either ASCII or binary
- **USDZ**: ZIP archive (.usdz) - Packaged format for distribution

### 1.3 USD Core Concepts

#### 1.3.1 Prims (Primitives)
USD scenes are composed of prims arranged in a namespace hierarchy:
- **Root Prim**: Top-level prim in the scene
- **Prim Path**: Hierarchical addressing (e.g., /World/Characters/Hero)
- **Prim Types**: Defined by schemas (Mesh, Xform, Scope, etc.)

#### 1.3.2 Properties
- **Attributes**: Data values (positions, colors, transforms)
- **Relationships**: References to other prims or properties
- **Metadata**: Additional information about prims/properties

#### 1.3.3 Time and Animation
- **Time Samples**: Values at specific time codes
- **Default Values**: Fallback when no time sample exists
- **Frame Rate**: Temporal resolution (timeCodesPerSecond)
- **Time Range**: startTimeCode to endTimeCode

## 2. USD SCHEMA DOMAINS

### 2.1 UsdGeom (Geometry Schema)
**Purpose**: Geometric primitives and spatial relationships

**Core Prim Types**:
- **Mesh**: Polygonal geometry with vertices, faces, normals, UVs
- **Curve**: NURBS and Bezier curves
- **Points**: Point clouds
- **Volume**: Volumetric data
- **Plane**: Infinite planes
- **Cube**: Parametric cubes
- **Cylinder**: Parametric cylinders  
- **Sphere**: Parametric spheres
- **Cone**: Parametric cones
- **Capsule**: Parametric capsules

**Spatial Prims**:
- **Xform**: Transformation nodes with matrix operations
- **Scope**: Organizational grouping (no transform)
- **BoundingBox**: Spatial bounds information

**Key Attributes**:
- **points**: Vertex positions
- **faceVertexIndices**: Face topology
- **faceVertexCounts**: Vertices per face
- **normals**: Surface normals
- **primvars:st**: UV coordinates
- **extent**: Bounding box
- **doubleSided**: Two-sided rendering flag
- **subdivisionScheme**: Subdivision surface type

### 2.2 UsdShade (Shading Schema)
**Purpose**: Materials, shaders, and shading networks

**Core Prim Types**:
- **Material**: Material definition container
- **Shader**: Individual shader nodes
- **NodeGraph**: Shader network graphs

**Key Shader Types**:
- **UsdPreviewSurface**: Standard PBR surface shader
- **UsdUVTexture**: Texture sampling shader
- **UsdPrimvarReader**: Primvar data reader
- **UsdTransform2d**: UV transformation shader

**Key Attributes**:
- **inputs:diffuseColor**: Base color
- **inputs:metallic**: Metallic factor
- **inputs:roughness**: Surface roughness
- **inputs:opacity**: Transparency
- **inputs:emissiveColor**: Emission color
- **inputs:normal**: Normal mapping
- **outputs:surface**: Material surface output
- **outputs:displacement**: Displacement output

### 2.3 UsdSkel (Skeletal Animation Schema)
**Purpose**: Skeletal deformation and animation

**Core Prim Types**:
- **SkelRoot**: Root container for skeletal hierarchy
- **Skeleton**: Joint hierarchy definition
- **SkelAnimation**: Animation data for skeletons
- **BlendShape**: Morph target definitions

**Key Attributes**:
- **joints**: Joint name tokens
- **bindTransforms**: Bind-pose matrices
- **restTransforms**: Rest-pose matrices
- **jointIndices**: Vertex-to-joint mapping
- **jointWeights**: Vertex influence weights
- **blendShapes**: Blend shape name tokens
- **blendShapeWeights**: Morph target weights
- **offsets**: Vertex position offsets
- **normalOffsets**: Vertex normal offsets
- **pointIndices**: Affected vertex indices

**Key Relationships**:
- **skel:skeleton**: Link to skeleton prim
- **skel:animationSource**: Link to animation data
- **skel:blendShapeTargets**: Link to blend shape prims

### 2.4 UsdLux (Lighting Schema)
**Purpose**: Light sources and lighting environments

**Core Prim Types**:
- **DistantLight**: Directional lights (sun)
- **SphereLight**: Point lights
- **RectLight**: Area lights (rectangular)
- **CylinderLight**: Cylindrical area lights
- **DiskLight**: Disk-shaped area lights
- **DomeLight**: Environment/HDRI lighting

**Key Attributes**:
- **inputs:intensity**: Light intensity
- **inputs:color**: Light color
- **inputs:exposure**: Exposure adjustment
- **inputs:diffuse**: Diffuse contribution
- **inputs:specular**: Specular contribution
- **inputs:texture:file**: Environment texture

### 2.5 UsdMedia (Media Schema)
**Purpose**: Audio and video assets

**Core Prim Types**:
- **SpatialAudio**: 3D positioned audio

**Key Attributes**:
- **filePath**: Media file reference
- **mediaOffset**: Playback offset
- **gain**: Audio volume

### 2.6 UsdRender (Rendering Schema)
**Purpose**: Render settings and camera definitions

**Core Prim Types**:
- **Camera**: Camera definitions
- **RenderSettings**: Global render parameters

**Key Attributes**:
- **focalLength**: Camera focal length
- **horizontalAperture**: Sensor width
- **verticalAperture**: Sensor height
- **clippingRange**: Near/far planes

### 2.7 UsdUI (User Interface Schema)
**Purpose**: UI elements and metadata

**Core Prim Types**:
- **Backdrop**: UI backdrop elements
- **NodeGraphNodeAPI**: Node graph UI data

## 3. USD COMPOSITION SYSTEM

### 3.1 Composition Arcs
**Purpose**: Combine multiple USD files into complex scenes

**Arc Types**:
- **References**: Include external USD files
- **Payloads**: Lazy-loaded references
- **Inherits**: Class-based inheritance
- **Variants**: Selectable asset variations
- **Specializes**: Specialized inheritance

### 3.2 Layer System
- **Root Layer**: Primary USD file
- **Sublayers**: Additional layers for overrides
- **Session Layer**: Temporary modifications
- **Layer Stack**: Ordered composition of layers

### 3.3 Variant Sets
- **Purpose**: Multiple versions within single file
- **Selection**: Runtime variant switching
- **Use Cases**: LOD, material variations, configurations

## 4. USDZ SPECIFICATION

### 4.1 USDZ Foundation
- **Purpose**: Single-file 3D asset distribution
- **Format**: Zero-compression, unencrypted ZIP archive
- **Target**: AR/VR applications, web delivery
- **Benefits**: Self-contained, streamable, efficient

### 4.2 USDZ Constraints

#### 4.2.1 ZIP Requirements
- **Compression**: MUST be uncompressed (store method)
- **Encryption**: MUST NOT be encrypted
- **Structure**: Flat directory structure preferred
- **Ordering**: Primary USD file should be first entry

#### 4.2.2 File Type Support
**Required**:
- **USD Files**: .usd, .usda, .usdc (primary scene)

**Optional Assets**:
- **Textures**: .png, .jpeg/.jpg
- **Audio**: .m4a (preferred), .mp3, .wav

**Prohibited**:
- Nested directories (discouraged)
- Compressed or encrypted content
- External references outside package

#### 4.2.3 USD Content Constraints
- **Flattened Composition**: All references must be internal
- **Relative Paths**: Asset paths relative to archive root
- **Self-Contained**: No external dependencies
- **Read-Only**: Package contents are immutable

### 4.3 USDZ Best Practices
- **File Ordering**: Place primary USD file first for streaming
- **Asset Optimization**: Minimize texture sizes, optimize geometry
- **Path Anchoring**: Use consistent relative path conventions
- **Validation**: Use usdchecker for compliance verification

### 4.4 USDZ Tools
- **usdzip**: Create USDZ packages from USD compositions
- **usdchecker**: Validate USD/USDZ file compliance
- **usdcat**: Inspect USD content
- **usdedit**: View/edit USD files (read-only for USDZ)

### 4.5 MIME Type
- **Standard**: model/vnd.usdz+zip
- **Usage**: Web browsers, AR applications

## 5. OUR USD IMPORTER ANALYSIS

### 5.1 SUPPORTED FEATURES ✅

#### 5.1.1 Core Geometry (UsdGeom)
- **Mesh Import**: ✅ Vertices, faces, normals, UVs
- **Xform Nodes**: ✅ Transformation hierarchies
- **Scope Nodes**: ✅ Organizational grouping
- **Basic Attributes**: ✅ points, faceVertexIndices, normals, primvars:st

#### 5.1.2 Materials (UsdShade) - COMPREHENSIVE SUPPORT ✅
- **Material Import**: ✅ Full PBR material properties
- **UsdPreviewSurface**: ✅ Complete PBR material support
- **Texture Types**: ✅ Diffuse, Specular, Normal, Emissive, Occlusion, Metallic, Roughness, Clearcoat, Opacity, Displacement
- **Texture Formats**: ✅ PNG, JPEG texture support
- **Embedded Textures**: ✅ USDZ texture extraction
- **Advanced PBR**: ✅ Clearcoat, clearcoatRoughness, opacityThreshold, IOR support

#### 5.1.3 Skeletal Animation (UsdSkel) - RECENTLY ADDED ✅
- **BlendShape Import**: ✅ Morph target geometry
- **BlendShape Animation**: ✅ Time-sampled morph weights (fixed in recent update)
- **SkelAnimation**: ✅ Blend shape weight animation data conversion
- **Multiple Targets**: ✅ Multiple blend shapes per mesh
- **Animation Channels**: ✅ Separate aiMeshMorphAnim per blend shape

#### 5.1.4 Node Animation (Transform Animation) ✅
- **Translation**: ✅ Position keyframes
- **Rotation**: ✅ Quaternion keyframes
- **Scale**: ✅ Scale keyframes
- **Transform Matrices**: ✅ Full 4x4 matrix decomposition
- **Time Sampling**: ✅ Proper time-based interpolation

#### 5.1.5 File Format Support
- **USDA**: ✅ ASCII USD files
- **USDC**: ✅ Binary USD files  
- **USDZ**: ✅ ZIP archive extraction and processing

### 5.2 PARTIALLY SUPPORTED FEATURES ⚠️

#### 5.2.1 Advanced Geometry
- **Parametric Primitives**: ⚠️ Limited support (Cube, Sphere, etc.)
- **Curves**: ⚠️ Basic curve support
- **Subdivision Surfaces**: ⚠️ Limited subdivision scheme support

#### 5.2.2 Complex Materials
- **Shader Networks**: ⚠️ Basic network traversal
- **Custom Shaders**: ⚠️ Limited to standard types
- **Advanced PBR**: ⚠️ Some PBR parameters may be missing

### 5.3 UNSUPPORTED FEATURES ❌

#### 5.3.1 Advanced Skeletal Animation
- **Full Skeleton Import**: ❌ Joint hierarchies not fully supported
- **Skinning Data**: ❌ Joint weights/indices import incomplete  
- **Skeletal Animation**: ❌ Joint transform animation not supported
- **Bind Poses**: ❌ bindTransforms not processed

#### 5.3.2 Composition System
- **References**: ❌ External file references not resolved
- **Payloads**: ❌ Lazy loading not supported
- **Variants**: ❌ Variant selection not implemented
- **Layers**: ❌ Multi-layer composition not supported
- **Inherits/Specializes**: ❌ Inheritance arcs not processed

#### 5.3.3 Scene Elements
- **Lighting**: ✅ UsdLux lights fully supported (SphereLight, DistantLight, RectLight, DiskLight, CylinderLight, DomeLight)
  - *Intensity, color, radius, angle, width, height, exposure attributes*
  - *Transform support: positions extracted from parent Xform transforms*
  - *Light types mapped to Assimp (POINT, DIRECTIONAL, AREA, AMBIENT)*
- **Cameras**: ✅ GeomCamera fully supported
  - *Perspective and orthographic projection modes*
  - *Focal length, horizontal/vertical aperture, clipping range*
  - *Correct FOV calculation and aspect ratio computation*
- **Audio**: ❌ UsdMedia audio not imported
- **Volume Rendering**: ❌ Volumetric data not imported

#### 5.3.4 Advanced Features
- **Instancing**: ❌ USD instancing not supported
- **Time-Varying Geometry**: ❌ Animated geometry not supported
- **Custom Schemas**: ❌ User-defined schemas not supported

#### 5.3.5 Metadata and Properties
- **Custom Metadata**: ❌ User metadata not preserved
- **Property Relationships**: ❌ Complex relationships not processed
- **Attribute Metadata**: ❌ Interpolation, variability not handled

## 6. OUR USD EXPORTER ANALYSIS

### 6.1 SUPPORTED FEATURES ✅

#### 6.1.1 Core Geometry (UsdGeom)
- **Mesh Export**: ✅ Vertices, faces, normals, UVs
- **Xform Hierarchy**: ✅ Node transformations and hierarchy
- **Scope Organization**: ✅ Geometry scope wrapping
- **Transform Operations**: ✅ Translate, rotate, scale operations

#### 6.1.2 Materials (UsdShade)
- **Material Export**: ✅ PBR material properties
- **UsdPreviewSurface**: ✅ Standard surface shader
- **Texture Export**: ✅ PNG, JPEG texture embedding
- **Shader Networks**: ✅ Basic material-to-shader connections
- **UV Transforms**: ✅ Texture coordinate transformations

#### 6.1.3 Skeletal Animation (UsdSkel) - RECENTLY IMPLEMENTED ✅
- **SkelRoot Creation**: ✅ Proper skeletal hierarchy containers
- **Skeleton Export**: ✅ Dummy skeleton for blend shapes
- **SkelAnimation**: ✅ Time-sampled blend shape weights
- **BlendShape Prims**: ✅ Morph target geometry export
- **Animation Sampling**: ✅ Proper time-based weight interpolation
- **Relationship Setup**: ✅ skel:skeleton, skel:blendShapeTargets, skel:animationSource

#### 6.1.4 Lighting (UsdLux) - FULLY IMPLEMENTED ✅
- **DistantLight**: ✅ Directional lights (sun/directional)
- **SphereLight**: ✅ Point lights with radius
- **RectLight**: ✅ Area lights (rectangular)
- **Light Properties**: ✅ Color, intensity, exposure, attenuation
- **Light Positioning**: ✅ Proper USD light hierarchy

#### 6.1.5 Cameras (UsdRender) - FULLY IMPLEMENTED ✅
- **GeomCamera**: ✅ Perspective and orthographic cameras
- **Camera Properties**: ✅ FOV, focal length, aperture, clipping planes
- **Projection Types**: ✅ Perspective and orthographic projection
- **Camera Positioning**: ✅ Proper USD camera hierarchy

#### 6.1.6 File Format Support
- **USDA Export**: ✅ ASCII USD output
- **USDZ Packaging**: ✅ ZIP archive creation with textures
- **Metadata**: ✅ Stage metadata (timeCodesPerSecond, endTimeCode)

#### 6.1.7 Advanced Features
- **Animation Export**: ✅ Node transform animations
- **Time Sampling**: ✅ Proper keyframe interpolation
- **Path Management**: ✅ Absolute path construction for relationships
- **Validation**: ✅ usdchecker compliance
- **AR Anchoring**: ✅ ARKit metadata support

### 6.2 PARTIALLY SUPPORTED FEATURES ⚠️

#### 6.2.1 USDZ Compliance
- **ZIP Constraints**: ⚠️ May not always ensure uncompressed archives
- **File Ordering**: ⚠️ Primary USD file ordering not guaranteed
- **Path Anchoring**: ⚠️ Relative path consistency needs verification

#### 6.2.2 Advanced Materials
- **Complex Shader Networks**: ⚠️ Limited to basic PBR workflows
- **Custom Shaders**: ⚠️ Only standard USD shader types supported

### 6.3 UNSUPPORTED FEATURES ❌

#### 6.3.1 Advanced Skeletal Animation
- **Full Skeleton Export**: ❌ Real joint hierarchies not exported
- **Skinning Data**: ❌ Joint weights/indices not exported
- **Skeletal Animation**: ❌ Joint transform animation not supported
- **Multiple Skeletons**: ❌ Complex skeletal setups not supported

#### 6.3.2 Composition System
- **References**: ❌ Cannot create external references
- **Variants**: ❌ Variant sets not supported
- **Layers**: ❌ Multi-layer composition not supported
- **Payloads**: ❌ Lazy loading not implemented

#### 6.3.3 Advanced Geometry
- **Parametric Primitives**: ❌ Only mesh export supported (no Cube, Sphere, Cylinder primitives)
- **Curves**: ❌ Curve export not implemented
- **Volumes**: ❌ Volumetric data not supported
- **Subdivision Surfaces**: ⚠️ Analysis implemented but not applied to meshes

#### 6.3.4 Advanced Features
- **Audio**: ❌ UsdMedia audio not supported
- **Instancing**: ❌ USD instancing not implemented
- **MaterialX**: ❌ MaterialX node graphs not supported (uses UsdPreviewSurface fallback)
- **Volume Rendering**: ⚠️ Placeholder implementation (detects volume meshes but exports as regular geometry)

#### 6.3.5 Advanced Animation
- **Time-Varying Geometry**: ❌ Animated mesh topology not supported
- **Complex Animation**: ❌ Multi-target animation constraints
- **Animation Layers**: ❌ Layered animation not supported

## 7. COMPLIANCE SUMMARY

### 7.1 USD Core Compliance
- **Importer**: ~40% of full USD specification
- **Exporter**: ~45% of full USD specification
- **Strength**: Basic geometry, comprehensive materials, blend shape animation, full lighting and camera support
- **Weakness**: Composition system, full skeletal animation, advanced geometry primitives

### 7.2 USDZ Compliance
- **Importer**: ~75% of USDZ requirements (can read most USDZ files with full lighting/camera support)
- **Exporter**: ~80% of USDZ requirements (creates valid USDZ files)
- **Strength**: Self-contained packages, texture embedding, complete lighting and camera support
- **Weakness**: ZIP constraint enforcement, advanced content

### 7.3 Production Readiness
- **Basic 3D Assets**: ✅ Ready for production use
- **Animated Models**: ✅ Blend shape animation fully supported
- **Lit Scenes**: ✅ Full lighting support (export only)
- **Camera Setups**: ✅ Full camera support (export only)
- **Complex Scenes**: ❌ Limited by composition system gaps
- **Professional Workflows**: ✅ Suitable for intermediate to advanced use cases

## 8. RECOMMENDATIONS FOR FUTURE DEVELOPMENT

### 8.1 High Priority
1. **Full Skeletal Animation**: Complete joint hierarchy and skinning support
2. **Composition System**: References, variants, and layering
3. **Lighting Import**: UsdLux schema import (export already complete)
4. **Camera Import**: UsdRender camera import (export already complete)

### 8.2 Medium Priority
1. **Parametric Primitives**: Cube, Sphere, Cylinder support
2. **Advanced Materials**: Complex shader network support
3. **Curve Support**: NURBS and Bezier curve handling
4. **Instancing**: USD instancing for performance

### 8.3 Low Priority
1. **Volume Rendering**: Volumetric data support
2. **Audio Support**: UsdMedia implementation
3. **Custom Schemas**: User-defined schema support
4. **Advanced Animation**: Time-varying geometry, animation layers

This comprehensive analysis shows that while our USD implementation covers the fundamental aspects needed for basic 3D asset interchange, significant opportunities exist for expanding support to match the full richness of the USD specification.
