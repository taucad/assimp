# Web-IFC Integration for Assimp

This directory contains the Web-IFC (engine_web-ifc) library integration that provides IFC file support in Assimp.

## Overview

Web-IFC is a C++ library that provides fast and efficient parsing of Industry Foundation Classes (IFC) files. Web-IFC serves as Assimp's current IFC implementation, offering enhanced performance and comprehensive IFC schema support.

## Repository Information

- **Source**: https://github.com/ThatOpen/engine_web-ifc
- **License**: Mozilla Public License 2.0

## Integration Pattern

This integration follows Assimp's standard third-party library pattern:

```
contrib/web-ifc/
├── README.md              # This file
├── autoclone/             # Auto-cloned Web-IFC repository and dependencies
│   ├── webifc_repo-src/   # Cloned Web-IFC source code
│   ├── glm-src/           # GLM mathematics library
│   ├── spdlog-src/        # Fast C++ logging library
│   ├── fastfloat-src/     # Fast string-to-float conversion
│   ├── cdt-src/           # Constrained Delaunay Triangulation
│   ├── stduuid-src/       # Cross-platform UUID library
│   └── tinynurbs-src/     # NURBS library
└── patches/               # Compatibility patches
    ├── README.md          # Patch documentation
    └── web-ifc-cpp20-compatibility.patch
```

## Build Requirements

- **C++20 compiler** (required by Web-IFC)
- **CMake 3.22+**
- **Git** (for FetchContent operations)
- **Internet connection** (for dependency fetching during configuration)

## Dependencies

Web-IFC requires several third-party libraries that are automatically fetched during CMake configuration:

### Automatically Fetched Dependencies
- **GLM** - OpenGL Mathematics library
- **spdlog** - Fast C++ logging library  
- **fast_float** - Fast string-to-float conversion
- **CDT** - Constrained Delaunay Triangulation
- **stduuid** - Cross-platform UUID library
- **tinynurbs** - NURBS library

### Using Existing Assimp Dependencies
- **earcut-hpp** - Uses `contrib/earcut-hpp/earcut.hpp`

## CMake Configuration

The IFC importer is controlled by the `ASSIMP_BUILD_IFC_IMPORTER` option:

```cmake
# Enable IFC importer (Web-IFC implementation)
cmake -DASSIMP_BUILD_IFC_IMPORTER=ON ..

# Disable IFC importer  
cmake -DASSIMP_BUILD_IFC_IMPORTER=OFF ..
```

When enabled, Web-IFC is **required** - the build will fail if the Web-IFC repository cannot be cloned.

## Supported File Formats

- `.ifc` - Standard IFC files (STEP format)
- Supports IFC schemas: IFC2x3, IFC4, and IFC4x3

## Current Implementation

The Web-IFC integration provides:

### Core Features
- **ModelManager Integration**: Uses `webifc::manager::ModelManager` for IFC file lifecycle management
- **Geometry Processing**: Leverages Web-IFC's `IfcGeometryProcessor` for efficient mesh extraction
- **Material Handling**: Extracts IFC material properties and converts to Assimp material format
- **Spatial Hierarchy**: Preserves IFC spatial structure in the scene graph
- **Element Support**: Handles all standard IFC element types (walls, doors, windows, slabs, etc.)

### Architecture
- **File Loading**: Uses Web-IFC's callback-based loading mechanism for memory efficiency
- **Scene Building**: Creates Assimp scene structure from Web-IFC model data
- **Error Handling**: Comprehensive error handling with proper cleanup
- **Memory Management**: Automatic cleanup of Web-IFC resources

## Compiler Compatibility

Compatibility patches are automatically applied during build:

### `web-ifc-cpp20-compatibility.patch`
- **Apple Clang**: Replaces C++20 `std::views` with traditional loops (ranges library compatibility)
- **General Compatibility**: Replaces `std::format` with `std::to_string` for broader compiler support

## Performance

Web-IFC provides optimized IFC processing:

- **Fast parsing** using "tape reading" parser architecture
- **Efficient geometry processing** with optimized algorithms
- **Memory optimization** through streaming and callback-based loading
- **Configurable processing** with loader settings for performance tuning

## Testing

Comprehensive unit tests are implemented in `assimp/test/unit/utIFCImportExport.cpp`:

### Test Coverage
- **Basic Import**: File loading and scene validation
- **Schema Support**: IFC2x3 and IFC4 format testing
- **Material Extraction**: Material property validation  
- **Geometry Processing**: Mesh and vertex data validation
- **Scene Graph**: Spatial hierarchy validation
- **Performance**: Load time benchmarking

### Test Models
- `AC14-FZK-Haus.ifc` - IFC2x3 test model
- `cube-blender-ifc4.ifc` - IFC4 test model
- `cube-freecad-ifc4.ifc` - Additional IFC4 test model

## Troubleshooting

### Build Issues

1. **Dependency fetch failures**: Check internet connection and ensure git has access to GitHub
   ```bash
   # Test git access
   git clone https://github.com/ThatOpen/engine_web-ifc.git
   ```

2. **C++20 compiler errors**: Verify compiler supports C++20
   ```bash
   # GCC 10+, Clang 10+, MSVC 2019+ required
   gcc --version
   clang --version
   ```

3. **CMake configuration errors**: Ensure CMake 3.22+
   ```bash
   cmake --version
   ```

### Runtime Issues

1. **"Failed to load IFC file"**: Verify file is valid IFC format and contains ISO-10303-21 header
2. **Missing geometry**: Check that IFC file contains geometric representations
3. **Performance issues**: Large IFC files may require significant processing time and memory

### Debug Information

Enable debug logging to troubleshoot issues:
```cpp
// Web-IFC debug output is available through Assimp's logging system
Assimp::DefaultLogger::create("", Assimp::Logger::VERBOSE);
```

## Implementation Notes

- **C++20 Requirement**: Web-IFC requires C++20, which is automatically set when the IFC importer is enabled
- **Warning Suppression**: Extensive warning suppression is applied to Web-IFC source files as a third-party library
- **EMSCRIPTEN Support**: Link-time optimization (LTO) is enabled for WebAssembly builds
- **Thread Safety**: The current implementation is not thread-safe; use separate importer instances for concurrent loading

## License

Web-IFC is licensed under the Mozilla Public License 2.0. See the [Web-IFC repository](https://github.com/ThatOpen/engine_web-ifc) for full license details.
