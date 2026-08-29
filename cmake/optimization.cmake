# optimization.cmake — CMAKE_USER_MAKE_RULES_OVERRIDE fragment
# This file is included by the Kodi superbuild via
# CMAKE_USER_MAKE_RULES_OVERRIDE so that builds get proper optimization
# and dead-code elimination instead of the default -g with no optimization.
#
# The NDK toolchain sets CMAKE_C_FLAGS/CMAKE_CXX_FLAGS to include -g (debug
# info) and CMAKE_C_FLAGS_RELEASE/CMAKE_CXX_FLAGS_RELEASE are empty by default.
# This override forces -O3 -DNDEBUG and adds --gc-sections to the linker flags.
#
# NOTE: This file is processed during project()/enable_language(), which runs
# BEFORE CMAKE_BUILD_TYPE is guaranteed to be set in all generator paths.
# Guard with if(Release) and the flags silently vanish. Apply unconditionally.

string(REGEX REPLACE "-g" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REGEX REPLACE "-g" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -DNDEBUG -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -DNDEBUG -ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -ffunction-sections -fdata-sections")

# Linker flags: add --gc-sections to drop unused code and --icf=all for
# identical code folding (merges duplicate functions/data across translation units)
if(CMAKE_SHARED_LINKER_FLAGS)
  string(REGEX REPLACE "(^| )-Wl,--gc-sections( |$)" " " CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections -Wl,--icf=all")
else()
  set(CMAKE_SHARED_LINKER_FLAGS "-Wl,--gc-sections -Wl,--icf=all")
endif()

if(CMAKE_MODULE_LINKER_FLAGS)
  string(REGEX REPLACE "(^| )-Wl,--gc-sections( |$)" " " CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS}")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--gc-sections")
else()
  set(CMAKE_MODULE_LINKER_FLAGS "-Wl,--gc-sections")
endif()

if(CMAKE_EXE_LINKER_FLAGS)
  string(REGEX REPLACE "(^| )-Wl,--gc-sections( |$)" " " CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
else()
  set(CMAKE_EXE_LINKER_FLAGS "-Wl,--gc-sections")
endif()

# Hide internal symbols so the linker can dead-strip them
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")
