cmake_minimum_required(VERSION 3.20)

project(GT64Recompiled LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Opt out of constexpr mutex constructor on windows to prevent vcredist issues
if (WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
endif()

# Avoid warning about DOWNLOAD_EXTRACT_TIMESTAMP in CMake 3.24:
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(
    HLSL_CPU
    __PRFCHWINTRIN_H
)

# ROM file picker: use the XDG desktop portal (D-Bus) instead of GTK for nativefiledialog-extended. Initializing GTK
# in-process can stall startup for many seconds and delays SDL's audio thread setup.
if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(NFD_PORTAL ON CACHE BOOL "nfd: use xdg-desktop-portal instead of GTK" FORCE)
endif()

# Local fixes to pinned dependencies (patches/deps/<dir>-*.patch, applied once; already-applied patches are skipped).
function(apply_dependency_patch repo_dir patch_file)
    execute_process(COMMAND git apply --reverse --check ${patch_file}
        WORKING_DIRECTORY ${repo_dir} RESULT_VARIABLE already_applied OUTPUT_QUIET ERROR_QUIET)
    if (NOT already_applied EQUAL 0)
        execute_process(COMMAND git apply ${patch_file} WORKING_DIRECTORY ${repo_dir} RESULT_VARIABLE apply_result)
        if (NOT apply_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply ${patch_file} in ${repo_dir}")
        endif()
        message(STATUS "Applied ${patch_file}")
    endif()
endfunction()

# Vsync (FIFO) presentation blocks indefinitely while the window is hidden on some drivers (NVIDIA + Wayland), which
# would stall the game. RT64 skips presenting while the last present hasn't reached the screen (via present wait).
apply_dependency_patch(${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/plume ${CMAKE_SOURCE_DIR}/patches/deps/plume-wait-for-last-present.patch)
apply_dependency_patch(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_SOURCE_DIR}/patches/deps/rt64-skip-present-while-hidden.patch)
# Frame matching read look at entries that don't exist when the game hasn't loaded a look at (crash on environment
# mapped surfaces, e.g. the Embassy right after GT_LEVEL warps into it).
apply_dependency_patch(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_SOURCE_DIR}/patches/deps/rt64-lookat-bounds.patch)
# Keep game memory mapped at quit: the game's threads are never joined and crash if it's unmapped under them.
apply_dependency_patch(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime ${CMAKE_SOURCE_DIR}/patches/deps/N64ModernRuntime-keep-rdram-on-quit.patch)
# Present the framebuffer the VI switches to, not the one it leaves: tasks finish instantly, so the game can already be
# drawing its next frame into the old one (flashing black or partly drawn frames in GT64's races and fades).
apply_dependency_patch(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime ${CMAKE_SOURCE_DIR}/patches/deps/N64ModernRuntime-screen-update-after-vi-swap.patch)
# RecompFrontend never reads mouse buttons ("TODO mouse support"), so the default left/right mouse bindings for
# shoot/aim did nothing. Read them alongside the keyboard state.
apply_dependency_patch(${CMAKE_SOURCE_DIR}/lib/RecompFrontend ${CMAKE_SOURCE_DIR}/patches/deps/RecompFrontend-mouse-buttons.patch)
# Cross-compiling for Windows from Linux (cmake/windows-cross-clang-cl.cmake, docs/WINDOWS_CROSS.md): a host-built
# file_to_c, the Linux DXC for SPIR-V and the Windows dxc.exe under Wine for DXIL.
apply_dependency_patch(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_SOURCE_DIR}/patches/deps/rt64-cross-compile-windows.patch)

# S2DEX 1.x: bgCopy for 4/8-bit images (GT64 title backgrounds drew as noise) and the object commands RT64 left
# unimplemented (G_OBJ_MOVEMEM, G_OBJ_RECTANGLE/SPRITE, the LDTX draws): GT64 draws its menus and text with them.
# Also: unscaled 4/8-bit bg1Cyc (scrolling sky panoramas) and per-task reset of the LOADTXTR skip statuses (stale
# sky palettes).
apply_dependency_patch(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_SOURCE_DIR}/patches/deps/rt64-s2dex1-objects.patch)
# Screen-space triangles (GT64's rotated HUD sprites) keep their 4:3 position in widescreen, like rectangles; draw
# scissors are clamped to the framebuffer (extended origins could push them negative).
apply_dependency_patch(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_SOURCE_DIR}/patches/deps/rt64-raw-triangles-aspect.patch)
# Full-frame race view and 4:3 screens: flags set by the game for a full-frame 3D view (the S2DEX patch then wraps the
# sky out to the window's edges and makes the fade grid cover the frame) and for its "card" screens (2D tasks remapped
# from the 288x208 box at (16, 16) to the full frame); one-pixel-short full-screen scissors widened (a line at the
# right edge); fixRectLR only ever extending rects (it left gaps between wrapped sky pieces); 2D (S2DEX) rectangles
# and sprites drawn with point sampling; filtered, half-strength car shadows.
apply_dependency_patch(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_SOURCE_DIR}/patches/deps/rt64-full-frame-view.patch)
# Crack fill: a pass after each 3D framebuffer fills hairline gaps between polygons (pixels farther than the surfaces
# on both sides, at most an N64 pixel wide) with the colour around them, as the N64's VI coverage filtering does.
# At high resolutions the sky showed through seams in the track geometry.
apply_dependency_patch(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_SOURCE_DIR}/patches/deps/rt64-crack-fill.patch)
# Work in progress (draw-distance branch): race camera far plane moved out with fog kept in place, forced LOD branches,
# and debug logging for the draw distance work.
apply_dependency_patch(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_SOURCE_DIR}/patches/deps/rt64-draw-distance.patch)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)

set(BUILD_SHARED_LIBS OFF)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)

target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/rt64/src)

# RecompiledFuncs - the N64Recomp output for the game (generated by scripts/recomp.sh)
add_library(RecompiledFuncs STATIC)

target_compile_options(RecompiledFuncs PRIVATE
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-implicit-function-declaration
    -falign-functions=16
)

target_include_directories(RecompiledFuncs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)

file(GLOB FUNC_C_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c)
file(GLOB FUNC_CXX_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.cpp)

target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES})


# Main executable
add_executable(GT64Recompiled)

set(SOURCES
    ${CMAKE_SOURCE_DIR}/src/main/main.cpp
    ${CMAKE_SOURCE_DIR}/src/main/register_overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/main/debug_stats.cpp

    ${CMAKE_SOURCE_DIR}/src/game/config.cpp
    ${CMAKE_SOURCE_DIR}/src/game/enhancements.cpp


    ${CMAKE_SOURCE_DIR}/rsp/n_aspMain.cpp
)

target_include_directories(GT64Recompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty/sse2neon
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/inc
    ${CMAKE_SOURCE_DIR}/lib/rt64/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/rhi
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/render
    ${CMAKE_SOURCE_DIR}/lib/RecompFrontend/recompinput/include
    ${CMAKE_SOURCE_DIR}/lib/RecompFrontend/recompui/include
    ${CMAKE_SOURCE_DIR}/lib/RecompFrontend/recompui/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/nativefiledialog-extended/src/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    target_compile_options(GT64Recompiled PRIVATE
        -march=nehalem
        -fno-strict-aliasing
        -fms-extensions
    )
else()
    target_compile_options(GT64Recompiled PRIVATE
        -fno-strict-aliasing
        -fms-extensions
    )
endif()

if (MSVC)
    # Disable identical code folding, since this breaks mod function patching as multiple functions can get merged into one.
    target_link_options(GT64Recompiled PRIVATE /OPT:NOICF)
endif()

if (WIN32)
    include(FetchContent)

    if (DEFINED ENV{SDL2_VERSION})
        set(SDL2_VERSION $ENV{SDL2_VERSION})
    else()
        set(SDL2_VERSION "2.30.3")
    endif()

    # Fetch SDL2 on windows
    FetchContent_Declare(
        sdl2
        URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip
    )
    FetchContent_MakeAvailable(sdl2)
    target_include_directories(GT64Recompiled PRIVATE
        ${sdl2_SOURCE_DIR}/include
    )
    target_link_directories(GT64Recompiled PRIVATE
        ${sdl2_SOURCE_DIR}/lib/x64
    )

    # Copy SDL2 and dxc DLLs to output folder as post build step
    add_custom_command(TARGET GT64Recompiled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:GT64Recompiled>)

    target_link_libraries(GT64Recompiled PRIVATE SDL2 Winmm.lib)
endif()

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    find_package(SDL2 REQUIRED)

    add_compile_definitions("PLUME_SDL_VULKAN_ENABLED")
    add_compile_definitions("RT64_SDL_WINDOW_VULKAN")

    target_include_directories(GT64Recompiled PRIVATE ${SDL2_INCLUDE_DIRS})

    find_package(Freetype REQUIRED)

    include_directories(${FREETYPE_LIBRARIES})
    target_link_libraries(GT64Recompiled PRIVATE ${FREETYPE_LIBRARIES} SDL2::SDL2)

    set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)

    target_link_libraries(GT64Recompiled PRIVATE "-latomic -static-libstdc++" ${CMAKE_DL_LIBS} Threads::Threads)

    # Export symbols so the crash handler's backtraces show function names.
    target_link_options(GT64Recompiled PRIVATE -rdynamic)
endif()

# TODO fix the rt64 CMake script so that this doesn't need to be duplicated here
# For DXC
set (DXC_COMMON_OPTS "-I${PROJECT_SOURCE_DIR}/src")
set (DXC_DXIL_OPTS "-Wno-ignored-attributes")
set (DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
set (DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_0" "-D DYNAMIC_RENDER_PARAMS")
set (DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_0" "-D DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
set (DXC_CS_OPTS "${DXC_COMMON_OPTS}" "-E" "CSMain" "-T cs_6_0")
set (DXC_GS_OPTS "${DXC_COMMON_OPTS}" "-E" "GSMain" "-T gs_6_0")
set (DXC_RT_OPTS "${DXC_COMMON_OPTS}" "-D" "RT_SHADER" "-T" "lib_6_3" "-fspv-target-env=vulkan1.1spirv1.4" "-fspv-extension=SPV_KHR_ray_tracing" "-fspv-extension=SPV_EXT_descriptor_indexing")

if (WIN32 AND CMAKE_HOST_WIN32)
    set (DXC "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc.exe")
    add_compile_definitions(NOMINMAX)
elseif (WIN32)
    # Cross-compiling for Windows from Linux: Linux DXC for SPIR-V, the Windows dxc.exe under Wine for DXIL so it matches
    # the shipped dxcompiler.dll (see patches/deps/rt64-cross-compile-windows.patch).
    set (DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/x64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc-linux")
    find_program(RT64_WINE NAMES wine64 wine HINTS /usr/lib/wine REQUIRED)
    set (DXC_FOR_DXIL sh "${PROJECT_SOURCE_DIR}/lib/rt64/src/tools/dxc-wine.sh" "${RT64_WINE}" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc.exe")
    add_compile_definitions(NOMINMAX)
else()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
        set (DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/x64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc-linux")
    else()
        set (DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/arm64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/arm64/dxc-linux")
    endif()
endif()

set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)
set(RECOMP_FRONTEND_RT64_PATH ${CMAKE_SOURCE_DIR}/lib/rt64)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/RecompFrontend)

target_link_libraries(GT64Recompiled PRIVATE
    RecompiledFuncs
    recompui
    recompinput
    librecomp
    ultramodern
    rt64
    nfd
)

target_sources(GT64Recompiled PRIVATE ${SOURCES})

if (WIN32)
    # App icon for the exe, its shortcuts and the game window (scripts/release/icon.ico, made from icon.png).
    target_sources(GT64Recompiled PRIVATE ${CMAKE_SOURCE_DIR}/scripts/release/GT64Recompiled.rc)
endif()


# Run the executable from the build dir with assets alongside it.
add_custom_command(TARGET GT64Recompiled POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:GT64Recompiled>/assets
    COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/recompcontrollerdb.txt $<TARGET_FILE_DIR:GT64Recompiled>/recompcontrollerdb.txt
)
