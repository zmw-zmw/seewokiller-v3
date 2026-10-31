cmake_minimum_required(VERSION 3.21)
project(SeewoKiller LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static link VC++ runtime to avoid requiring vc_redist on target machines
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# vcpkg installed paths (override with -DVCPKG_ROOT=... on configure)
if(NOT DEFINED VCPKG_ROOT)
    set(VCPKG_ROOT "C:/dev/vcpkg/installed/x64-windows")
endif()

# Collect all source files
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h")

# Resource file (app icon)
set(RESOURCES "${CMAKE_SOURCE_DIR}/res/app.rc")

# Main executable
add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${RESOURCES})

# Treat source files as UTF-8 (removes C4819 and fixes Chinese literals)
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
endif()

# Include directories - vcpkg headers + our src
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/res
    ${VCPKG_ROOT}/include
)

# Link directories
target_link_directories(${PROJECT_NAME} PRIVATE
    ${VCPKG_ROOT}/lib
    ${VCPKG_ROOT}/debug/lib
)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    ssh
    WebView2Loader.dll.lib
    ws2_32
    comctl32
    shlwapi
    user32
    kernel32
    gdi32
    advapi32
    ole32
    oleaut32
    shell32
    version
)

# Copy WebView2 DLLs to output
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_ROOT}/bin/WebView2Loader.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_ROOT}/bin/Microsoft.Web.WebView2.Core.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_ROOT}/bin/ssh.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_ROOT}/bin/libcrypto-3-x64.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_ROOT}/bin/libssl-3-x64.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "Copying runtime DLLs"
)

# Copy resources to output directory
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/res"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/res"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/plugins"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/plugins"
    COMMENT "Copying resources and plugins to output directory"
)

# Set output name
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "SeewoKiller"
)
