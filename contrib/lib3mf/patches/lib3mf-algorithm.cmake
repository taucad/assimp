FILE(READ "${SOURCE_FILE}" LIB3MF_SOURCE)
IF(LIB3MF_SOURCE MATCHES "#include <algorithm>")
    RETURN()
ENDIF()

SET(LIB3MF_INCLUDE "#include \"Model/Writer/v100/NMR_ResourceDependencySorter.h\"")
STRING(REPLACE
    "${LIB3MF_INCLUDE}"
    "${LIB3MF_INCLUDE}\n\n#include <algorithm>"
    LIB3MF_PATCHED_SOURCE
    "${LIB3MF_SOURCE}"
)
IF(LIB3MF_PATCHED_SOURCE STREQUAL LIB3MF_SOURCE)
    MESSAGE(FATAL_ERROR "lib3mf dependency sorter include anchor was not found")
ENDIF()
FILE(WRITE "${SOURCE_FILE}" "${LIB3MF_PATCHED_SOURCE}")
