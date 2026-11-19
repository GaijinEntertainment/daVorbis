cmake_minimum_required(VERSION 3.13)
project(daVorbis CXX)

set(DAVORBIS_VECMATH_DEFAULT ${CMAKE_CURRENT_SOURCE_DIR}/external)
if(NOT EXISTS ${DAVORBIS_VECMATH_DEFAULT}/vecmath/dag_vecMath.h)
  set(DAVORBIS_VECMATH_DEFAULT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
endif()
set(VECMATH_INCLUDE_DIR ${DAVORBIS_VECMATH_DEFAULT} CACHE PATH "The folder that holds vecmath/")
if(NOT EXISTS ${VECMATH_INCLUDE_DIR}/vecmath/dag_vecMath.h)
  message(FATAL_ERROR "No vecmath/dag_vecMath.h in VECMATH_INCLUDE_DIR (${VECMATH_INCLUDE_DIR}). "
    "Run 'git submodule update --init' or set VECMATH_INCLUDE_DIR to the folder that holds vecmath/.")
endif()
set(DAVORBIS_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/std CACHE PATH "The folder of daVorbisConfig.h")

add_library(daVorbis STATIC src/daVorbisSetup.cpp src/daVorbisFloorTable.cpp src/daVorbisDecodeBase.cpp)
target_include_directories(daVorbis PUBLIC include PRIVATE ${DAVORBIS_CONFIG_DIR})
target_include_directories(daVorbis SYSTEM PRIVATE ${VECMATH_INCLUDE_DIR}) # SYSTEM hides the vecmath warnings
target_compile_features(daVorbis PUBLIC cxx_std_17)
if(NOT MSVC) # vecmath uses __forceinline when this is not set
  target_compile_definitions(daVorbis PRIVATE "VECMATH_FINLINE=inline __attribute__((always_inline))")
endif()

set(DAVORBIS_ARCH ${CMAKE_SYSTEM_PROCESSOR})
if(CMAKE_OSX_ARCHITECTURES)
  set(DAVORBIS_ARCH "${CMAKE_OSX_ARCHITECTURES}")
endif()
if(DAVORBIS_ARCH MATCHES ";") # a universal binary compiles each file for each arch
  target_compile_options(daVorbis PRIVATE -Xarch_x86_64 -msse4.1)
  target_sources(daVorbis PRIVATE src/daVorbisIsaGeneric.cpp)
elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64" OR (NOT WIN32 AND DAVORBIS_ARCH MATCHES "x86_64|AMD64"))
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(daVorbis PRIVATE /D_TARGET_SIMD_SSE=4)
    set_source_files_properties(src/daVorbisDecodeAvx.cpp PROPERTIES COMPILE_OPTIONS /arch:AVX)
    set_source_files_properties(src/daVorbisDecodeAvx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/fp:contract")
  else()
    target_compile_options(daVorbis PRIVATE -msse4.1)
    set_source_files_properties(src/daVorbisDecodeAvx.cpp PROPERTIES COMPILE_OPTIONS -mavx)
    set_source_files_properties(src/daVorbisDecodeAvx2.cpp PROPERTIES COMPILE_OPTIONS
      "-mavx2;-mfma;-mbmi;-mbmi2;-mlzcnt;$<$<BOOL:${MSVC}>:/clang:>-ffp-contract=fast") # clang-cl takes it only after /clang:
  endif()
  target_sources(daVorbis PRIVATE src/daVorbisDecodeAvx.cpp src/daVorbisDecodeAvx2.cpp src/daVorbisIsaX86.cpp)
else()
  target_sources(daVorbis PRIVATE src/daVorbisIsaGeneric.cpp)
endif()
