cmake_minimum_required(VERSION 3.21)
project(foo_taglookup LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Point this to your local foobar2000 SDK root.
set(FOOBAR2000_SDK_ROOT "" CACHE PATH "Path to foobar2000 SDK")

if(NOT FOOBAR2000_SDK_ROOT)
  set(_default_sdk_root "${CMAKE_CURRENT_SOURCE_DIR}/sdk/foobar2000-sdk-2025-03-07")
  if(EXISTS "${_default_sdk_root}/foobar2000/SDK")
    set(FOOBAR2000_SDK_ROOT "${_default_sdk_root}" CACHE PATH "Path to foobar2000 SDK" FORCE)
  else()
    file(GLOB _sdk_candidates LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/sdk/foobar2000-sdk-*")
    list(SORT _sdk_candidates ORDER DESCENDING)
    foreach(_candidate IN LISTS _sdk_candidates)
      if(EXISTS "${_candidate}/foobar2000/SDK")
        set(FOOBAR2000_SDK_ROOT "${_candidate}" CACHE PATH "Path to foobar2000 SDK" FORCE)
        break()
      endif()
    endforeach()
  endif()
endif()

if(NOT FOOBAR2000_SDK_ROOT OR NOT EXISTS "${FOOBAR2000_SDK_ROOT}/foobar2000/SDK")
  message(FATAL_ERROR "Set FOOBAR2000_SDK_ROOT to the extracted foobar2000 SDK directory")
endif()

set(FOOBAR2000_HELPERS_DIR "${FOOBAR2000_SDK_ROOT}/foobar2000/helpers")
if(APPLE AND EXISTS "${FOOBAR2000_SDK_ROOT}/foobar2000/helpers-mac")
  set(FOOBAR2000_HELPERS_DIR "${FOOBAR2000_SDK_ROOT}/foobar2000/helpers-mac")
endif()

include(FetchContent)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(CURL REQUIRED)

file(GLOB FB2K_PFC_SOURCES "${FOOBAR2000_SDK_ROOT}/pfc/*.cpp")
if(APPLE)
  list(APPEND FB2K_PFC_SOURCES
    ${FOOBAR2000_SDK_ROOT}/pfc/obj-c.mm
  )
endif()
add_library(fb2k_pfc STATIC ${FB2K_PFC_SOURCES})
target_include_directories(fb2k_pfc PUBLIC
  ${FOOBAR2000_SDK_ROOT}
  ${FOOBAR2000_SDK_ROOT}/pfc
)

set(FB2K_SHARED_SOURCES
  ${FOOBAR2000_SDK_ROOT}/foobar2000/shared/audio_math.cpp
  ${FOOBAR2000_SDK_ROOT}/foobar2000/shared/shared-nix.cpp
  ${FOOBAR2000_SDK_ROOT}/foobar2000/shared/stdafx.cpp
  ${FOOBAR2000_SDK_ROOT}/foobar2000/shared/utf8.cpp
)
if(APPLE)
  list(APPEND FB2K_SHARED_SOURCES
    ${FOOBAR2000_SDK_ROOT}/foobar2000/shared/shared-apple.mm
  )
endif()
add_library(fb2k_shared STATIC ${FB2K_SHARED_SOURCES})
target_include_directories(fb2k_shared PUBLIC
  ${FOOBAR2000_SDK_ROOT}
  ${FOOBAR2000_SDK_ROOT}/pfc
  ${FOOBAR2000_SDK_ROOT}/foobar2000
  ${FOOBAR2000_SDK_ROOT}/foobar2000/SDK
  ${FOOBAR2000_SDK_ROOT}/foobar2000/shared
)
target_link_libraries(fb2k_shared PUBLIC fb2k_pfc)

file(GLOB FB2K_SDK_SOURCES "${FOOBAR2000_SDK_ROOT}/foobar2000/SDK/*.cpp")
if(APPLE)
  list(APPEND FB2K_SDK_SOURCES
    ${FOOBAR2000_SDK_ROOT}/foobar2000/SDK/commonObjects-Apple.mm
  )
endif()
add_library(fb2k_sdk STATIC ${FB2K_SDK_SOURCES})
target_include_directories(fb2k_sdk PUBLIC
  ${FOOBAR2000_SDK_ROOT}
  ${FOOBAR2000_SDK_ROOT}/pfc
  ${FOOBAR2000_SDK_ROOT}/foobar2000
  ${FOOBAR2000_SDK_ROOT}/foobar2000/SDK
  ${FOOBAR2000_SDK_ROOT}/foobar2000/shared
)
target_link_libraries(fb2k_sdk PUBLIC fb2k_shared fb2k_pfc)

add_library(fb2k_component_client STATIC
  ${FOOBAR2000_SDK_ROOT}/foobar2000/foobar2000_component_client/component_client.cpp
)
target_include_directories(fb2k_component_client PUBLIC
  ${FOOBAR2000_SDK_ROOT}
  ${FOOBAR2000_SDK_ROOT}/pfc
  ${FOOBAR2000_SDK_ROOT}/foobar2000
  ${FOOBAR2000_SDK_ROOT}/foobar2000/SDK
  ${FOOBAR2000_SDK_ROOT}/foobar2000/shared
  ${FOOBAR2000_SDK_ROOT}/foobar2000/foobar2000_component_client
)
target_link_libraries(fb2k_component_client PUBLIC fb2k_sdk fb2k_shared fb2k_pfc)

add_library(foo_taglookup SHARED
  src/component_main.cpp
  src/match_selector_mac.mm
  src/tag_lookup_service.cpp
)

target_include_directories(foo_taglookup PRIVATE
  ${FOOBAR2000_SDK_ROOT}
  ${FOOBAR2000_SDK_ROOT}/foobar2000/SDK
  ${FOOBAR2000_HELPERS_DIR}
  src
)

target_link_libraries(foo_taglookup PRIVATE
  CURL::libcurl
  nlohmann_json::nlohmann_json
  fb2k_component_client
  fb2k_sdk
  fb2k_shared
  fb2k_pfc
)

# The SDK ships its own helper/static libraries and project files.
# For an actual build, add the SDK libs here according to SDK docs.
# Example (names vary by SDK release):
# target_link_libraries(foo_taglookup PRIVATE foobar2000_sdk foobar2000_helpers pfc)

set_target_properties(foo_taglookup PROPERTIES
  OUTPUT_NAME "foo_taglookup"
  PREFIX ""
)

if(APPLE)
  target_compile_definitions(foo_taglookup PRIVATE _FILE_OFFSET_BITS=64)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
  find_library(FOUNDATION_FRAMEWORK Foundation)
  find_library(APPKIT_FRAMEWORK AppKit)
  if(COREFOUNDATION_FRAMEWORK)
    target_link_libraries(foo_taglookup PRIVATE ${COREFOUNDATION_FRAMEWORK})
  endif()
  if(FOUNDATION_FRAMEWORK)
    target_link_libraries(foo_taglookup PRIVATE ${FOUNDATION_FRAMEWORK})
  endif()
  if(APPKIT_FRAMEWORK)
    target_link_libraries(foo_taglookup PRIVATE ${APPKIT_FRAMEWORK})
  endif()
endif()
