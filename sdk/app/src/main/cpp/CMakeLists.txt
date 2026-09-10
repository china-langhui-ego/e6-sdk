cmake_minimum_required(VERSION 3.4.1)

# mcap 官方库需要 C++17（std::byte/std::optional/string_view 等）；
# build.gradle 的 cppFlags 里带了 -std=c++14，这里显式要求 C++17，后置的 -std 标志生效。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Declares and names the project.
set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -Wno-unused-variable -Wno-unused-function")
set(QXR_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../../../../)
set(QXR_EXTERNAL_DIR ${QXR_ROOT_PATH}/External/)

add_library(lib_xr_loader SHARED IMPORTED)
set_target_properties(lib_xr_loader PROPERTIES IMPORTED_LOCATION
        ${QXR_EXTERNAL_DIR}/OpenXR-SDK/${ANDROID_ABI}/libopenxr_loader.so)

set(THIRDPARTY_OPENXR_SOURCE_DIR
        ${QXR_EXTERNAL_DIR}/OpenXR-SDK/include
        ${QXR_EXTERNAL_DIR}/OpenXR-SDK/src
        ${QXR_EXTERNAL_DIR}/OpenXR-SDK/src/common)
add_library(qxr-thirdparty-openxr INTERFACE)
target_include_directories(qxr-thirdparty-openxr
        INTERFACE ${THIRDPARTY_OPENXR_SOURCE_DIR})

# build native_app_glue as a static lib
set(${CMAKE_C_FLAGS}, "${CMAKE_C_FLAGS}")
add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

# now build app's shared lib
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

# Export ANativeActivity_onCreate(),
# Refer to: https://github.com/android-ndk/ndk/issues/381.
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate")

# common-log
set(COMMON_LOG_SOURCE_DIR ${QXR_EXTERNAL_DIR}/Common/Log/)
add_library(qxr-common-log STATIC ${COMMON_LOG_SOURCE_DIR}/LogUtils.c)
target_include_directories(qxr-common-log PUBLIC ${COMMON_LOG_SOURCE_DIR})

# png
#set(png_dir ${CMAKE_CURRENT_SOURCE_DIR}/../../../../png/src/main/cpp)
#add_library(png STATIC IMPORTED)
#set_target_properties(png PROPERTIES IMPORTED_LOCATION
#        ${png_dir}/../../outLibs/${CMAKE_BUILD_TYPE}/${CMAKE_ANDROID_ARCH_ABI}/libsxr_png_static.a)
#target_include_directories(png PUBLIC
#                ${png_dir}/include)

# thirdparty-glm module
set(GLM_SOURCE_DIR ${QXR_EXTERNAL_DIR}/ThirdParty/glm/)
add_library(qxr-thirdparty-glm INTERFACE)
target_include_directories(qxr-thirdparty-glm INTERFACE ${GLM_SOURCE_DIR})

# thirdparty-tinyobj module
set(THIRDPARTY_TINYOBJ_SOURCE_DIR
        ${QXR_EXTERNAL_DIR}/ThirdParty/tinyobjloader/)
set(THIRDPARTY_TINYOBJ_SOURCE_FILES
        ${THIRDPARTY_TINYOBJ_SOURCE_DIR}/tiny_obj_loader.cc)
add_library(qxr-thirdparty-tinyobj STATIC ${THIRDPARTY_TINYOBJ_SOURCE_FILES})
target_include_directories(qxr-thirdparty-tinyobj
        INTERFACE ${THIRDPARTY_TINYOBJ_SOURCE_DIR})

# common-data-structures
set(COMMON_DATA_STRUCTURES_SOURCE_DIR
        ${QXR_EXTERNAL_DIR}/Common/DataStructures/cpp)
add_library(qxr-common-data-structures INTERFACE)
target_include_directories(qxr-common-data-structures
        INTERFACE ${COMMON_DATA_STRUCTURES_SOURCE_DIR})

# common-gl
set(COMMON_GL_SOURCE_DIR ${QXR_EXTERNAL_DIR}/Common/GL/cpp)
file(GLOB COMMON_GL_SOURCE_FILES ${COMMON_GL_SOURCE_DIR}/*.cpp)
add_library(qxr-common-gl STATIC ${COMMON_GL_SOURCE_FILES})
target_include_directories(qxr-common-gl PUBLIC ${COMMON_GL_SOURCE_DIR}/)
target_link_libraries(qxr-common-gl PRIVATE
        log
        qxr-common-log
        qxr-thirdparty-glm
        qxr-thirdparty-tinyobj
        qxr-common-data-structures)

# app-common module
set(APPCOMMON_SOURCE_DIR ${QXR_ROOT_PATH}/External/AppCommon/cpp)
add_library(qxr-app-common STATIC ${APPCOMMON_SOURCE_DIR}/AppCommon.cpp)
target_include_directories(qxr-app-common PUBLIC
        ${APPCOMMON_SOURCE_DIR}/)
target_link_libraries(qxr-app-common PRIVATE
        lib_xr_loader
        EGL
        native_app_glue
        qxr-common-log
        qxr-thirdparty-glm
        qxr-thirdparty-openxr
        )
# 官方 mcap 库（vendor thirdparty/mcap，pin releases/cpp/v2.1.3；无压缩依赖）
set(MCAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/mcap)
add_library(mcap-official STATIC ${MCAP_ROOT}/mcap.cpp)
target_include_directories(mcap-official PUBLIC ${MCAP_ROOT})
target_compile_definitions(mcap-official PUBLIC MCAP_PUBLIC= MCAP_COMPRESSION_NO_LZ4 MCAP_COMPRESSION_NO_ZSTD)

file(GLOB main_src "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
# 顶层 GLOB 不覆盖 mcap/ 子目录，显式列出（mcap/test 不移植）
set(MCAP_WRAPPER_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/mcap/McapWriter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mcap/CdrWriter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mcap/McapSchemas.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mcap/McapChunkManager.cpp)
add_library(mixedreality SHARED ${main_src} ${MCAP_WRAPPER_SOURCES})

# SDK 版本号：仓库根 VERSION 文件（打 tag 时同步更新该文件并在同一提交打 tag）。
# 注入为 SDK_VERSION 宏；启动 logcat 与 mcap session metadata 的 sdkVersion 键使用。
file(READ ${QXR_ROOT_PATH}/VERSION SDK_VERSION)
string(STRIP "${SDK_VERSION}" SDK_VERSION)
target_compile_definitions(mixedreality PRIVATE SDK_VERSION="${SDK_VERSION}")
set(LINKED_LIBRARIES
        mixedreality
        android
        native_app_glue
        EGL
        GLESv3
        log
        mediandk
        aaudio
        lib_xr_loader
        qxr-thirdparty-openxr
        qxr-common-log
        qxr-app-common
        qxr-common-gl
        qxr-common-data-structures
        qxr-thirdparty-tinyobj
        mcap-official
        qxr-thirdparty-glm)

target_link_libraries(${LINKED_LIBRARIES})
