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

project(dart_bridge VERSION 1.2.2 LANGUAGES C)

if(NOT DEFINED DART_BRIDGE_PYTHON_INCLUDE_DIRS)
  find_package(Python3 REQUIRED COMPONENTS Development.Module)
  set(DART_BRIDGE_PYTHON_INCLUDE_DIRS ${Python3_INCLUDE_DIRS})
  if(WIN32)
    # python-build-standalone Windows doesn't ship a Debug pythonXY_d.lib, so
    # link the abi3 stable-ABI stub python3.lib instead — version-agnostic,
    # no debug-variant requirement (matches our Py_LIMITED_API choice).
    get_filename_component(_python_root "${Python3_INCLUDE_DIRS}" DIRECTORY)
    set(DART_BRIDGE_PYTHON_LIBRARIES "${_python_root}/libs/python3.lib")
  elseif(UNIX AND NOT APPLE)
    # Linux: Python3::Module would be the "extension module" target which
    # intentionally leaves libpython unlinked (the Python interpreter that
    # dlopen()s the module is expected to provide the symbols). dart_bridge
    # is the OPPOSITE — Dart loads it to embed Python, so libpython MUST be
    # in DT_NEEDED. Without this, the resulting .so has no libpython
    # reference, Py_* symbols stay unresolved at dlopen, and the embedded
    # interpreter crashes on first Py_Initialize.
    #
    # Prefer the abi3 stub libpython3.so (version-agnostic DT_NEEDED).
    # python-build-standalone ships it; Ubuntu's python3-dev typically does
    # not. CI overrides via -DDART_BRIDGE_PYTHON_LIBRARIES=<path> to
    # python-linux-dart's libpython3.so. Local desktop dev falls back to
    # the version-specific libpython (DT_NEEDED becomes libpython3.X.so).
    find_library(_libpython3_so python3 HINTS ${Python3_LIBRARY_DIRS})
    if(_libpython3_so)
      message(STATUS "Linking abi3 libpython3.so: ${_libpython3_so}")
      set(DART_BRIDGE_PYTHON_LIBRARIES "${_libpython3_so}")
    else()
      message(WARNING
        "abi3 libpython3.so not found; falling back to version-specific "
        "Python3_LIBRARIES — the resulting .so will only run against "
        "matching Python ${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}")
      set(DART_BRIDGE_PYTHON_LIBRARIES ${Python3_LIBRARIES})
    endif()
  else()
    # Apple host build (local dev only — CI uses build_xcframework.sh).
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
