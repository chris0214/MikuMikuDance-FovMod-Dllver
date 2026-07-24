cmake_minimum_required(VERSION 3.20)
project(mmd_float_fov_mod LANGUAGES CXX)

add_library(MMEffect SHARED
    src/float_fov_patch.cpp
    src/mme_forwarder.cpp
    src/mme_forwarder.def
)

target_compile_features(MMEffect PRIVATE cxx_std_17)
target_compile_definitions(MMEffect PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(MMEffect PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

set_target_properties(MMEffect PROPERTIES
    PREFIX ""
    OUTPUT_NAME "MMEffect"
)

if(MSVC)
    set_property(TARGET MMEffect PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(MMEffect PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(MMEffect PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(MINGW)
        target_link_options(MMEffect PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(float_fov_patch_fixture
        src/float_fov_patch.cpp
        tests/float_fov_patch_fixture.cpp
    )
    target_compile_features(float_fov_patch_fixture PRIVATE cxx_std_17)
    target_compile_definitions(
        float_fov_patch_fixture PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(float_fov_patch_fixture PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    if(MSVC)
        set_property(TARGET float_fov_patch_fixture PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(
            float_fov_patch_fixture PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(
            float_fov_patch_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
        if(MINGW)
            target_link_options(
                float_fov_patch_fixture PRIVATE
                -static -static-libgcc -static-libstdc++)
        endif()
    endif()
    add_test(NAME float_fov_patch_fixture COMMAND float_fov_patch_fixture)
endif()

install(TARGETS MMEffect
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
)
