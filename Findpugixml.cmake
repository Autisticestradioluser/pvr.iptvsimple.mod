# Findpugixml.cmake
# Resolve from the ABI-specific addon depends path so the same module works for
# both armv7 and aarch64 builds (ADDON_DEPENDS_PATH points at the correct arch).
set(_depends_path "${ADDON_DEPENDS_PATH}")
if(NOT _depends_path)
  set(_depends_path "${CMAKE_PREFIX_PATH}")
endif()

set(PUGIXML_INCLUDE_DIR "${_depends_path}/include")
set(PUGIXML_LIBRARY "${_depends_path}/lib/libpugixml.a")

if(PUGIXML_INCLUDE_DIR AND EXISTS "${PUGIXML_INCLUDE_DIR}/pugixml.hpp" AND PUGIXML_LIBRARY AND EXISTS "${PUGIXML_LIBRARY}")
  if(NOT PUGIXML_INCLUDE_DIRS)
    set(PUGIXML_INCLUDE_DIRS ${PUGIXML_INCLUDE_DIR})
  endif()
  if(NOT PUGIXML_LIBRARIES)
    set(PUGIXML_LIBRARIES ${PUGIXML_LIBRARY})
  endif()
  add_library(pugixml STATIC IMPORTED)
  set_target_properties(pugixml PROPERTIES
    IMPORTED_LOCATION ${PUGIXML_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${PUGIXML_INCLUDE_DIRS}
  )
  set(PUGIXML_FOUND TRUE)
endif()

mark_as_advanced(PUGIXML_INCLUDE_DIRS PUGIXML_LIBRARIES)
