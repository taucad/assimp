# Web-IFC Patches

This directory contains patches required to build Web-IFC as part of Assimp.

## Current Patches

### `web-ifc-cpp20-compatibility.patch`
Fixes C++20 compatibility issues with Apple Clang and other compilers:

1. **shared-position.h**: Replaces `std::views` usage with traditional loops to avoid dependency on `<ranges>` which is not available in Apple Clang
2. **IfcLoader.cpp**: Replaces `std::format` with `std::to_string` as `std::format` is not widely available

These patches ensure Web-IFC compiles across different compiler versions while maintaining the same functionality.

## Applying Patches

Patches are automatically applied during the CMake configure step via:
```cmake
set(WEBIFC_PATCH_CMD git apply ${WebIFC_BASE_ABSPATH}/patches/web-ifc-cpp20-compatibility.patch)
```

## Creating New Patches

To create new patches:

1. Make changes in the autocloned Web-IFC repository
2. Generate patch with: `git diff > ../patches/new-patch.patch`  
3. Update the PATCH_COMMAND in CMakeLists.txt
4. Update this README

