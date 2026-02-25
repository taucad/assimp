/*
 * Standalone miniz compilation unit for use when kuba-zip (zip.c) is excluded
 * due to symbol conflicts with lib3mf's bundled libzip.
 *
 * Only compiles the mz_* prefixed functions (deflate, archive APIs) without
 * creating zlib-compatible name aliases that would conflict with Assimp's zlib.
 */
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"
