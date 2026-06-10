# Cross-platform CMake build for libdart_bridge — the Dart↔Python FFI bridge
# binary consumed by serious_python_{linux,windows,android}.
#
# Builds one shared library per (platform × arch) combination. Apple uses a
# separate xcodebuild flow in apple/build_xcframework.sh because Apple needs
# a multi-slice xcframework, not a single dylib.
#
# Inputs (set by the CI build script or the consuming project):
#   DART_BRIDGE_PYTHON_INCLUDE_DIRS — path to Python headers (Python.h)
#   DART_BRIDGE_PYTHON_LIBRARIES     — link target for Python (abi3 stub)
#                                       Linux:   Python3::Module
#                                       Windows: <python-pkg>/libs/python3.lib
#                                       Android: <mobile-forge>/.../libpython3.so
#
# If neither is provided, falls back to find_package(Python3 Development.Module)
# — useful for local desktop development. CI sets them explicitly.

cmake_minimum_required(VERSION 3.15)

project(dart_bridge VERSION 1.0.0 LANGUAGES C)

if(NOT DEFINED DART_BRIDGE_PYTHON_INCLUDE_DIRS)
  find_package(Python3 REQUIRED COMPONENTS Development.Module)
  set(DART_BRIDGE_PYTHON_INCLUDE_DIRS ${Python3_INCLUDE_DIRS})
  if(WIN32)
    # python-build-standalone Windows doesn't ship a Debug pythonXY_d.lib, so
    # link the abi3 stable-ABI stub python3.lib instead — version-agnostic,
    # no debug-variant requirement (matches our Py_LIMITED_API choice).
    get_filename_component(_python_root "${Python3_INCLUDE_DIRS}" DIRECTORY)
    set(DART_BRIDGE_PYTHON_LIBRARIES "${_python_root}/libs/python3.lib")
  else()
    set(DART_BRIDGE_PYTHON_LIBRARIES Python3::Module)
  endif()
endif()

add_library(dart_bridge SHARED
  src/dart_bridge.c
  src/serious_python_run.c
  src/dart_api/dart_api_dl.c
)

target_include_directories(dart_bridge PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  ${DART_BRIDGE_PYTHON_INCLUDE_DIRS}
)

target_link_libraries(dart_bridge PRIVATE ${DART_BRIDGE_PYTHON_LIBRARIES})

set_target_properties(dart_bridge PROPERTIES
  C_VISIBILITY_PRESET hidden
  OUTPUT_NAME "dart_bridge"
)

target_compile_definitions(dart_bridge PRIVATE
  DART_SHARED_LIB
  Py_LIMITED_API=0x030c0000   # abi3 — one binary works for any CPython 3.12+
)

if(WIN32)
  # pyconfig.h's `#pragma comment(lib, "pythonXY(_d).lib")` auto-link directive
  # would otherwise pull in a Debug pythonXY_d.lib that python-build-standalone
  # doesn't ship. Py_NO_LINK_LIB suppresses the pragma in Python 3.14+;
  # 3.12/3.13 don't honor it so we also use /NODEFAULTLIB to explicitly
  # exclude the request at link time.
  target_compile_definitions(dart_bridge PRIVATE Py_NO_LINK_LIB)
  if(Python3_VERSION_MAJOR AND Python3_VERSION_MINOR)
    target_link_options(dart_bridge PRIVATE
      "/NODEFAULTLIB:python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}_d.lib"
    )
  endif()
endif()

if(ANDROID)
  # Android 15: 16 KB page size support.
  set_target_properties(dart_bridge PROPERTIES
    LINK_FLAGS "-Wl,-z,max-page-size=16384"
  )
endif()
