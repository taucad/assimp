# LightUSD

[LightUSD](https://github.com/lighttransport/LightUSD) (formerly TinyUSDZ,
Apache-2.0) provides USDA, USDC and USDZ reading and writing, composition,
MaterialX and UsdPhysics for the USD importer and exporter.

It is fetched at configure time by `code/CMakeLists.txt` (only when the USD
importer or exporter is enabled) and built as a static library with
`add_subdirectory`. To build against a local checkout, configure with
`-DFETCHCONTENT_SOURCE_DIR_LIGHTUSD=/path/to/LightUSD`.

## Pinned revision

`LIGHTUSD_GIT_TAG` in `code/CMakeLists.txt` pins a commit on the `lightusd`
branch of [taucad/tinyusdz](https://github.com/taucad/tinyusdz/tree/lightusd),
GitHub's fork of LightUSD. The branch is LightUSD `1fdd60a6` plus these fixes,
each a separate commit intended for upstream:

- Degenerate faces (fewer than 3 vertices) are skipped instead of failing the
  whole mesh.
- `UsdUVTexture inputs:st` is typed `float2`, as the UsdPreviewSurface spec
  requires.
- Custom asset-resolver callbacks receive the current working path, so
  relative sublayers resolve through in-memory resolvers.
- Missing-asset warnings are deduplicated per load, not per process.
- No per-load threading warning.
- USDZ packages with a USDA root get a base directory, not a filename.
- The MaterialX `vector3` type is accepted for `normal`.
- `.mtlx` documents converted to USD keep their shader inputs and connect
  each Material's `outputs:mtlx:surface` to its surface shader.
- An unresolved subLayer honours `error_when_asset_not_found`.
- Tydra skips an invalid geometry prim, or a malformed BlendShape, with a
  warning instead of failing the scene (`MeshConverterConfig::skip_invalid_geometry`).
- Material, Shader and NodeGraph nodes are tagged `NodeCategory::Material`.
- Dense BlendShapes (no `pointIndices`) convert, and `ShapeTarget::abs_path`
  holds the full prim path.
- LightUSD's C sources build as C11.
- The USDC writer stores token, string and asset index arrays uncompressed,
  as OpenUSD does, and keeps attribute metadata (interpolation, elementSize)
  on Points and BasisCurves.

When a fix lands upstream, rebase the branch and drop it here.
