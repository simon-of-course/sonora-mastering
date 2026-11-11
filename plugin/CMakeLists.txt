cmake_minimum_required(VERSION 3.25)

project(sonora_vst
    VERSION 0.2.0
    DESCRIPTION "SONORA rap mastering / FX VST3 plug-ins"
    LANGUAGES C CXX
)

include(FetchContent)

#------------------------------------------------------------------------
# Options
#------------------------------------------------------------------------
if(MSVC)
    set(SONORA_BUILD_TESTS_DEFAULT OFF)
else()
    set(SONORA_BUILD_TESTS_DEFAULT ON)
endif()
if(WIN32)
    set(SONORA_BUILD_HOST_TOOLS_DEFAULT OFF)
else()
    set(SONORA_BUILD_HOST_TOOLS_DEFAULT ON)
endif()

option(SONORA_BUILD_TESTS "Build SDK-free unit tests (plugin/tests/test_*.cpp) and register them with CTest" ${SONORA_BUILD_TESTS_DEFAULT})
option(SONORA_BUILD_HOST_TOOLS "Build the VST3 SDK hosting examples (editorhost, audiohost)" ${SONORA_BUILD_HOST_TOOLS_DEFAULT})
option(SONORA_MSVC_STATIC_RUNTIME "Link the MSVC runtime statically (/MT) so release bundles need no VC++ redistributable" ON)

#------------------------------------------------------------------------
# VST3 SDK
#------------------------------------------------------------------------
set(SMTG_ENABLE_VSTGUI_SUPPORT ON CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ${SONORA_BUILD_HOST_TOOLS} CACHE BOOL "" FORCE)
set(SMTG_ENABLE_WAYLAND_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_AUV2_BUILDS OFF CACHE BOOL "" FORCE)
# Symlinking into the system VST3 folder needs admin rights on Windows; opt in with -DSMTG_CREATE_PLUGIN_LINK=ON.
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "Create symbolic link for each Plug-in")
set(CMAKE_DISABLE_FIND_PACKAGE_EXPAT ON)

if(MSVC AND SONORA_MSVC_STATIC_RUNTIME)
    set(SMTG_USE_STATIC_CRT ON CACHE BOOL "" FORCE)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96
    GIT_SHALLOW FALSE
    GIT_SUBMODULES
        base
        cmake
        pluginterfaces
        public.sdk
        vstgui4
)
FetchContent_MakeAvailable(vst3sdk)

#------------------------------------------------------------------------
# Plug-ins
#------------------------------------------------------------------------
set(SONORA_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
)

# public_sdk_SOURCE_DIR is not visible in this scope, so smtg_add_vst3plugin does not add the module entry point.
if(WIN32)
    set(SONORA_SDK_MAIN_SOURCE ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/dllmain.cpp)
elseif(APPLE)
    set(SONORA_SDK_MAIN_SOURCE ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/macmain.cpp)
else()
    set(SONORA_SDK_MAIN_SOURCE ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/linuxmain.cpp)
endif()

function(sonora_apply_common_settings target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_include_directories(${target} PRIVATE ${SONORA_INCLUDE_DIRS})
    target_compile_definitions(${target} PRIVATE
        SONORA_VERSION_STRING="${PROJECT_VERSION}"
        SONORA_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        SONORA_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        SONORA_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    )
    if(MSVC)
        # The SDK's toolset flags only apply inside its own directory scope, so set ours explicitly.
        target_compile_options(${target} PRIVATE /utf-8 /W3)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

function(sonora_add_plugin target source uidesc)
    smtg_add_vst3plugin(${target} ${source})
    smtg_target_configure_version_file(${target})
    smtg_target_add_plugin_resources(${target}
        RESOURCES
            ${uidesc}
    )
    target_sources(${target}
        PRIVATE
            ${SONORA_SDK_MAIN_SOURCE}
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
    )
    target_link_libraries(${target}
        PRIVATE
            sdk
            vstgui_support
    )
    sonora_apply_common_settings(${target})
endfunction()

sonora_add_plugin(SONORA_Master  src/sonora_master.cpp  resource/sonora.uidesc)
sonora_add_plugin(SONORA_Shutter src/sonora_shutter.cpp resource/shutter.uidesc)
sonora_add_plugin(SONORA_Drowner src/sonora_drowner.cpp resource/drowner.uidesc)

add_custom_target(sonora_plugins DEPENDS SONORA_Master SONORA_Shutter SONORA_Drowner)

#------------------------------------------------------------------------
# SDK-free unit tests and tools
#------------------------------------------------------------------------
if(SONORA_BUILD_TESTS)
    enable_testing()
    file(GLOB SONORA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp)
    foreach(test_source IN LISTS SONORA_TEST_SOURCES)
        get_filename_component(test_name ${test_source} NAME_WE)
        add_executable(sonora_${test_name} ${test_source})
        sonora_apply_common_settings(sonora_${test_name})
        target_include_directories(sonora_${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        add_test(NAME ${test_name} COMMAND sonora_${test_name} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endforeach()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/sonora_render.cpp)
        add_executable(sonora_render ${CMAKE_CURRENT_SOURCE_DIR}/tools/sonora_render.cpp)
        sonora_apply_common_settings(sonora_render)
        target_include_directories(sonora_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    endif()

    # Loads the built bundles like a host: state migration, round trip, latency, processing.
    add_executable(sonora_hostcheck ${CMAKE_CURRENT_SOURCE_DIR}/tools/sonora_hostcheck.cpp
                                    ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/memorystream.cpp
                                    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp)
    if(WIN32)
        target_sources(sonora_hostcheck PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp)
    elseif(APPLE)
        target_sources(sonora_hostcheck PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_mac.mm)
    else()
        target_sources(sonora_hostcheck PRIVATE ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
    endif()
    target_link_libraries(sonora_hostcheck PRIVATE sdk_hosting)
    target_compile_features(sonora_hostcheck PRIVATE cxx_std_17)
    add_dependencies(sonora_hostcheck SONORA_Master SONORA_Shutter SONORA_Drowner)
    add_test(NAME hostcheck
             COMMAND sonora_hostcheck
                     $<TARGET_PROPERTY:SONORA_Master,SMTG_PLUGIN_PACKAGE_PATH>
                     $<TARGET_PROPERTY:SONORA_Shutter,SMTG_PLUGIN_PACKAGE_PATH>
                     $<TARGET_PROPERTY:SONORA_Drowner,SMTG_PLUGIN_PACKAGE_PATH>)
endif()
