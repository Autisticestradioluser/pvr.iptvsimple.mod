# Findlzma.cmake
# Resolve from the ABI-specific addon depends path so the same module works for
# both armv7 and aarch64 builds (ADDON_DEPENDS_PATH points at the correct arch).
set(_depends_path "${ADDON_DEPENDS_PATH}")
if(NOT _depends_path)
  set(_depends_path "${CMAKE_PREFIX_PATH}")
endif()

set(LZMA_INCLUDE_DIR "${_depends_path}/include")
set(LZMA_LIBRARY "${_depends_path}/lib/liblzma.a")

if(LZMA_INCLUDE_DIR AND EXISTS "${LZMA_INCLUDE_DIR}/lzma.h" AND LZMA_LIBRARY AND EXISTS "${LZMA_LIBRARY}")
  if(NOT LZMA_INCLUDE_DIRS)
    set(LZMA_INCLUDE_DIRS ${LZMA_INCLUDE_DIR})
  endif()
  if(NOT LZMA_LIBRARIES)
    set(LZMA_LIBRARIES ${LZMA_LIBRARY})
  endif()
  add_library(lzma STATIC IMPORTED)
  set_target_properties(lzma PROPERTIES
    IMPORTED_LOCATION ${LZMA_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${LZMA_INCLUDE_DIRS}
  )
  set(lzma_FOUND TRUE)
endif()

mark_as_advanced(LZMA_INCLUDE_DIRS LZMA_LIBRARIES)
