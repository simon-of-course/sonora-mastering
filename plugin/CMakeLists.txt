cmake_minimum_required(VERSION 3.25)

project(sonora_master_vst
    VERSION 0.1.0
    DESCRIPTION "SONORA natural bus mastering VST3"
    LANGUAGES C CXX
)

include(FetchContent)

set(SMTG_ENABLE_VSTGUI_SUPPORT ON CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_WAYLAND_SUPPORT OFF CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_EXPAT ON)

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

smtg_add_vst3plugin(SONORA_Master
    src/sonora_master.cpp
)
smtg_target_configure_version_file(SONORA_Master)
smtg_target_add_plugin_resources(SONORA_Master
    RESOURCES
        resource/sonora.uidesc
)
target_sources(SONORA_Master
    PRIVATE
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/dllmain.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
)

target_compile_features(SONORA_Master
    PRIVATE
        cxx_std_17
)
target_link_libraries(SONORA_Master
    PRIVATE
        sdk
        vstgui_support
)
target_include_directories(SONORA_Master
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

smtg_add_vst3plugin(SONORA_Shutter
    src/sonora_shutter.cpp
)
smtg_target_configure_version_file(SONORA_Shutter)
smtg_target_add_plugin_resources(SONORA_Shutter
    RESOURCES
        resource/shutter.uidesc
)
target_sources(SONORA_Shutter
    PRIVATE
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/dllmain.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
)
target_compile_features(SONORA_Shutter PRIVATE cxx_std_17)
target_link_libraries(SONORA_Shutter PRIVATE sdk vstgui_support)
target_include_directories(SONORA_Shutter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

smtg_add_vst3plugin(SONORA_Drowner
    src/sonora_drowner.cpp
)
smtg_target_configure_version_file(SONORA_Drowner)
smtg_target_add_plugin_resources(SONORA_Drowner
    RESOURCES
        resource/drowner.uidesc
)
target_sources(SONORA_Drowner
    PRIVATE
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/dllmain.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
)
target_compile_features(SONORA_Drowner PRIVATE cxx_std_17)
target_link_libraries(SONORA_Drowner PRIVATE sdk vstgui_support)
target_include_directories(SONORA_Drowner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
