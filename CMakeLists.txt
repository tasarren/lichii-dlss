cmake_minimum_required(VERSION 3.22)
project(lichii-dlss VERSION 1.0.0 LANGUAGES CXX)

# Root of the NVIDIA NGX SDK (has include/ and lib/libnvsdk_ngx.a).
# The SDK is proprietary; it is never distributed with this repository.
set(NGX_SDK_DIR "" CACHE PATH "NVIDIA NGX SDK root")
if(NOT NGX_SDK_DIR)
    message(FATAL_ERROR "Set NGX_SDK_DIR to the NVIDIA NGX SDK root.")
endif()
set(NGX_STATIC_LIB "${NGX_SDK_DIR}/lib/libnvsdk_ngx.a")
if(NOT EXISTS "${NGX_STATIC_LIB}")
    message(FATAL_ERROR "${NGX_STATIC_LIB} not found.")
endif()

find_package(Vulkan REQUIRED)

add_library(lichii-dlss-module MODULE src/ngx_module.cpp)
target_compile_features(lichii-dlss-module PRIVATE cxx_std_17)
target_include_directories(lichii-dlss-module
    PRIVATE include "${NGX_SDK_DIR}/include")
target_compile_definitions(lichii-dlss-module PRIVATE
    NGX_MODULE_VERSION_STRING="${PROJECT_VERSION}"
    NGX_MODULE_DLSS_VERSION="310.9.16")
# The module calls no Vulkan functions; only the headers are needed.
target_include_directories(lichii-dlss-module PRIVATE ${Vulkan_INCLUDE_DIRS})
target_compile_options(lichii-dlss-module PRIVATE -fvisibility=hidden -fno-exceptions -fno-rtti)
target_link_libraries(lichii-dlss-module PRIVATE "${NGX_STATIC_LIB}")
# CMake prefixes modules with "lib" on Linux; the loader and the panel
# both look for the exact name lichii-dlss-module.so.
set_target_properties(lichii-dlss-module PROPERTIES PREFIX "" OUTPUT_NAME "lichii-dlss-module")
