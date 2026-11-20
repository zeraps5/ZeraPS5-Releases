# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.21)
project(ps5c VERSION 0.1.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|amd64)$")
    message(FATAL_ERROR "PS5C targets x86-64 hosts only (found ${CMAKE_SYSTEM_PROCESSOR})")
endif()

option(PS5C_BUILD_TESTS "Build unit tests" ON)
option(PS5C_RUN_GPU_TESTS "Enable native Vulkan/display fixtures in CTest" OFF)
option(PS5C_BUILD_BENCHMARKS "Build standalone CPU benchmarks (Windows)" OFF)
option(PS5C_WERROR "Treat warnings as errors" OFF)
option(PS5C_MARCH_V3 "Target x86-64-v3 (AVX2/BMI2/FMA) in optimized builds" OFF)
# Where launchers look for new builds (tools/serve_updates.ps1 on the local
# network today); [atualizacao] fonte in the user settings overrides it.
set(PS5C_UPDATE_URL "https://github.com/zeraps5/ZeraPS5-Releases/releases/latest/download" CACHE STRING "Update source built into the launcher")
set(PS5C_LOG_MIN_LEVEL "0" CACHE STRING "Compile out log calls below this level (0=trace .. 5=fatal)")

function(ps5c_target_defaults target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(PS5C_MARCH_V3)
            target_compile_options(${target} PRIVATE $<$<NOT:$<CONFIG:Debug>>:/arch:AVX2>)
        endif()
        if(PS5C_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow
                                                 -Wstrict-prototypes -Wmissing-prototypes)
        if(PS5C_MARCH_V3)
            target_compile_options(${target} PRIVATE $<$<NOT:$<CONFIG:Debug>>:-march=x86-64-v3>)
        endif()
        target_compile_options(${target} PRIVATE
            "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=/ZeraPS5"
            "-fdebug-prefix-map=${CMAKE_BINARY_DIR}=/ZeraPS5/build")
        if(PS5C_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    target_compile_definitions(${target} PRIVATE PS5C_LOG_MIN_LEVEL=${PS5C_LOG_MIN_LEVEL})
endfunction()

# Executables hosting guest memory: the guest owns 0x800000000-0x10000000000 at
# the same host address. High-entropy ASLR may place heaps/DLLs there before
# main() runs; keep host allocations low.
function(ps5c_guest_host target)
    if(WIN32)
        if(MSVC)
            target_link_options(${target} PRIVATE /HIGHENTROPYVA:NO)
        else()
            target_link_options(${target} PRIVATE -Wl,--disable-high-entropy-va)
        endif()
    endif()
endfunction()

configure_file(include/ps5c/version.h.in ${CMAKE_BINARY_DIR}/generated/ps5c/version.h @ONLY)
add_custom_target(ps5c_build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/generated/ps5c/build_info.h
            -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/ps5c/build_info.h ${CMAKE_BINARY_DIR}/generated/ps5c/shader_cache_version.h
    COMMENT "Build information (git commit, shader translator hash)")

# ---- base: portable C11, no OS calls ----
add_library(ps5c_base STATIC
    src/base/arena.c
    src/base/dynarray.c
    src/base/env.c
    src/base/error.c
    src/base/hashmap.c
    src/base/ini.c
    src/base/json.c
    src/base/log.c
    src/base/log_record.c
    src/base/log_record_file.c
    src/base/log_session.c
    src/base/panic.c
    src/base/sha1.c
    src/base/crypto.c
    src/core/compat_catalog.c
    src/base/kraken.c
    src/base/inflate.c
    src/base/rsa.c
)
target_sources(ps5c_base PRIVATE src/base/log_record.c)
target_include_directories(ps5c_base PUBLIC src include ${CMAKE_BINARY_DIR}/generated)
target_include_directories(ps5c_base SYSTEM PUBLIC third_party/vulkan/include)
ps5c_target_defaults(ps5c_base)

# ---- pal: host operating system services ----
if(WIN32)
    add_library(ps5c_pal STATIC src/pal_win32/report_session.c src/pal_win32/report_archive.c src/pal/pal_exfat.c src/pal/pal_pkg.c src/pal/pal_pfs.c src/pal/pal_pfs_volume.c src/pal/pal_pkg_inner.c src/pal/pal_pkg_ps4.c src/pal/pal_common.c src/pal/pal_media.c src/pal_win32/pal_win32.c
                src/pal_win32/pal_media_win32.c src/pal_win32/dualsense.c src/pal_win32/pal_bundled_assets.c)
    target_link_libraries(ps5c_pal PUBLIC user32 gdi32 shell32 shlwapi ole32 mfuuid uuid synchronization winmm setupapi
                          hid advapi32)
    target_sources(ps5c_pal PRIVATE src/pal_win32/pal_host_info.c)
    target_sources(ps5c_pal PRIVATE src/pal_win32/udp_lan.c)
    target_link_libraries(ps5c_pal PUBLIC ws2_32)
else()
    add_library(ps5c_pal STATIC src/pal/pal_exfat.c src/pal/pal_pkg.c src/pal/pal_pfs.c src/pal/pal_pfs_volume.c src/pal/pal_pkg_inner.c src/pal/pal_pkg_ps4.c src/pal/pal_common.c src/pal/pal_media.c src/pal_posix/pal_posix.c)
    find_package(Threads REQUIRED)
    target_link_libraries(ps5c_pal PUBLIC Threads::Threads)
    target_compile_definitions(ps5c_pal PRIVATE _GNU_SOURCE)
    target_sources(ps5c_pal PRIVATE src/pal_posix/pal_host_info.c)
endif()
target_sources(ps5c_pal PRIVATE src/pal/module_paths.c src/pal/native_input.c src/core/game_patch.c)
if(WIN32)
    target_sources(ps5c_pal PRIVATE src/pal_win32/native_input_win32.c)
endif()
target_link_libraries(ps5c_pal PUBLIC ps5c_base)
target_sources(ps5c_pal PRIVATE src/pal/module_paths.c)
ps5c_target_defaults(ps5c_pal)

# FFmpeg (movies and game audio): the static LGPL build tools/build_ffmpeg.sh
# makes in third_party/ffmpeg/<platform>. Without it movies fall back to Media
# Foundation on Windows and are skipped elsewhere.
if(WIN32)
    set(PS5C_FFMPEG_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/win64 CACHE PATH "FFmpeg build (include/, lib/)")
else()
    set(PS5C_FFMPEG_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/linux64 CACHE PATH "FFmpeg build (include/, lib/)")
endif()
if(EXISTS ${PS5C_FFMPEG_DIR}/lib/libavcodec.a)
    message(STATUS "FFmpeg: ${PS5C_FFMPEG_DIR}")
    target_sources(ps5c_pal PRIVATE src/pal/pal_media_ffmpeg.c src/pal/pal_vdec_ffmpeg.c)
    target_compile_definitions(ps5c_pal PUBLIC PS5C_HAVE_FFMPEG)
    target_include_directories(ps5c_pal SYSTEM PUBLIC ${PS5C_FFMPEG_DIR}/include)
    foreach(lib avformat avcodec swscale swresample avutil)
        target_link_libraries(ps5c_pal PUBLIC ${PS5C_FFMPEG_DIR}/lib/lib${lib}.a)
    endforeach()
    if(WIN32)
        target_link_libraries(ps5c_pal PUBLIC bcrypt ole32 user32)
        if(NOT MSVC)
            # FFmpeg's threads use winpthread: link it in, testers have no libwinpthread-1.dll
            target_link_libraries(ps5c_pal PUBLIC -Wl,-Bstatic winpthread -Wl,-Bdynamic)
        endif()
    else()
        target_link_libraries(ps5c_pal PUBLIC m)
    endif()
else()
    message(STATUS "FFmpeg: not built (tools/build_ffmpeg.sh); movies use the host decoder if any")
endif()

# ---- core: emulation (loader, recorder, ...) ----
add_library(ps5c_core STATIC
    src/core/runtime.c
    src/core/settings.c
    src/core/game_fixes.c
    src/core/ue_scale.c
    src/core/user_config.c
    src/cpu/x64_compat.c
    src/cpu/x64_native.c
    src/gpu/gpu_cp.c
    src/gpu/gcn.c
    src/gpu/rdna2_decode.c
    src/gpu/shader/shader_analyze.c
    src/gpu/shader/shader_dynamic.c
    src/gpu/shader/shader_corpus.c
    src/gpu/gpu_sampler.c
    src/gpu/gpu_watch.c
    src/gpu/shader/shader_spirv.c
    src/gpu/shader/shader_uniform.c
    src/gpu/shader/spv.c
    src/gpu/vk/vk_api.c
    src/gpu/vk/vk_descriptor_plan.c
    src/gpu/vk/vk_spirv_bindings.c
    src/gpu/vk/vk_pipeline.c
    src/gpu/vk/vk_pipeline_async.c
    src/gpu/vk/vk_renderer.c
    src/gpu/vk/vk_sparse_image.c
    src/gpu/vk/vk_shader_cache.c
    src/gpu/vk/vk_surface.c
    src/hle/hle.c
    src/hle/lib_input.c
    src/hle/equeue.c
    src/hle/guest_printf.c
    src/hle/lib_agc_driver.c
    src/hle/audio_mix.c
    src/hle/lib_audio_out.c
    src/hle/lib_audio_out2.c
    src/hle/lib_av_player.c
    src/hle/lib_videodec2.c
    src/hle/lib_vdecsw.c
    src/hle/lib_ampr.c
    src/hle/lib_png_enc.c
    src/hle/lib_png_dec.c
    src/hle/lib_audiodec.c
    src/hle/lib_compat_stubs.c
    src/hle/png_decode.c
    src/hle/lib_ajm.c
    src/hle/lib_acm.c
    src/hle/lib_ngs2.c
    src/hle/font_raster.c
    src/hle/lib_font.c
    src/hle/lib_font_text.c
    src/hle/lib_voice.c
    src/hle/lib_np_trophy.c
    src/hle/lib_fiber.c
    src/hle/lib_net_offline.c
    src/hle/lib_ult.c
    src/hle/lib_platform.c
    src/hle/lib_json2.c
    src/hle/lib_rtc.c
    src/hle/addcont.c
    src/hle/lib_playgo.c
    src/hle/playgo_metadata.c
    src/hle/lib_save_data.c
    src/hle/lib_save_data_dir.c
    src/hle/lib_dialogs.c
    src/hle/lib_jpeg_dec.c
    src/hle/lib_services_offline.c
    src/hle/lib_kernel.c
    src/hle/kernel_objects.c
    src/hle/lib_kernel_apr.c
    src/hle/lib_kernel_fs.c
    src/hle/net_loopback.c
    src/hle/lib_kernel_memory.c
    src/hle/lib_kernel_sync.c
    src/hle/lib_kernel_thread.c
    src/hle/lib_libc_internal.c
    src/hle/lib_libc_memory.c
    src/hle/lib_libc_process.c
    src/hle/lib_libc_jumps.c
    src/hle/lib_services.c
    src/hle/lib_system.c
    src/hle/lib_video_out.c
    src/loader/linker.c
    src/loader/module_set.c
    src/loader/nid.c
    src/loader/nid_db.c
    src/loader/sce_image.c
    src/mem/gmem.c
    src/mem/gmem_access.c
    src/recorder/profile.c
)
target_sources(ps5c_core PRIVATE src/hle/net_loopback.c)
target_link_libraries(ps5c_core PUBLIC ps5c_pal)
ps5c_target_defaults(ps5c_core)
add_dependencies(ps5c_core ps5c_build_info) # shader_cache_version.h

# ---- command-line frontend ----
add_executable(ps5c frontends/cli/main.c)
if(WIN32)
    enable_language(RC)
    target_sources(ps5c PRIVATE frontends/cli/ps5c.rc frontends/assets/bundled.rc)
    set_property(SOURCE frontends/assets/bundled.rc APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_SOURCE_DIR}/data/fonts/NotoSans-Regular.ttf"
        "${CMAKE_SOURCE_DIR}/data/intro.mp4")
endif()
target_link_libraries(ps5c PRIVATE ps5c_core)
add_dependencies(ps5c ps5c_build_info)
ps5c_target_defaults(ps5c)
ps5c_guest_host(ps5c)

if(PS5C_BUILD_BENCHMARKS AND WIN32)
    add_executable(bench_sh_evaluate tools/bench_sh_evaluate.c)
    target_link_libraries(bench_sh_evaluate PRIVATE ps5c_core)
    ps5c_target_defaults(bench_sh_evaluate)
endif()

# ---- launcher (Windows): ZeraPS5.exe next to ps5c.exe ----
if(WIN32)
    enable_language(RC)
    add_executable(zeraps5 WIN32 frontends/launcher/launcher.c frontends/launcher/launcher.rc src/core/user_config.c)
    set_target_properties(zeraps5 PROPERTIES OUTPUT_NAME ZeraPS5)
    target_include_directories(zeraps5 PRIVATE src ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(zeraps5 PRIVATE UNICODE _UNICODE)
    target_link_libraries(zeraps5 PRIVATE ps5c_pal comctl32 gdiplus shell32 advapi32 ole32 winhttp bcrypt setupapi hid winmm)
    if(NOT MSVC)
        target_link_options(zeraps5 PRIVATE -municode)
    endif()
    target_compile_definitions(zeraps5 PRIVATE PS5C_UPDATE_URL="${PS5C_UPDATE_URL}")
    add_dependencies(zeraps5 ps5c_build_info)
    ps5c_target_defaults(zeraps5)
endif()

# Bundled font for libSceFont (see data/README.md)
configure_file(data/fonts/NotoSans-Regular.ttf ${CMAKE_BINARY_DIR}/data/fonts/NotoSans-Regular.ttf COPYONLY)
# The intro video played in the game window before each game
configure_file(data/intro.mp4 ${CMAKE_BINARY_DIR}/data/intro.mp4 COPYONLY)
# NID name database next to the binary (see data/README.md)
if(EXISTS ${CMAKE_SOURCE_DIR}/data/nid_names.txt)
    configure_file(data/nid_names.txt ${CMAKE_BINARY_DIR}/data/nid_names.txt COPYONLY)
endif()

if(PS5C_BUILD_TESTS)
    enable_testing()
    add_executable(test_module_paths tests/unit/test_module_paths.c)
    target_link_libraries(test_module_paths PRIVATE ps5c_pal)
    ps5c_target_defaults(test_module_paths)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/module-paths")
    add_test(NAME module_paths COMMAND test_module_paths "${CMAKE_CURRENT_BINARY_DIR}/fixtures/module-paths")
    add_executable(test_log_record tests/unit/test_log_record.c)
    target_link_libraries(test_log_record PRIVATE ps5c_base)
    ps5c_target_defaults(test_log_record)
    add_test(NAME log_record COMMAND test_log_record)
    if(WIN32)
        add_executable(test_launcher_update tests/unit/test_launcher_update.c src/core/user_config.c)
        add_executable(test_log_record_file tests/unit/test_log_record_file.c)
        target_link_libraries(test_log_record_file PRIVATE ps5c_pal)
        if(NOT MSVC)
            target_link_options(test_log_record_file PRIVATE -municode)
        endif()
        ps5c_target_defaults(test_log_record_file)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/record-file")
        add_test(NAME log_record_file COMMAND test_log_record_file "${CMAKE_CURRENT_BINARY_DIR}/fixtures/record-file")
        target_include_directories(test_launcher_update PRIVATE src ${CMAKE_BINARY_DIR}/generated)
        target_compile_definitions(test_launcher_update PRIVATE UNICODE _UNICODE PS5C_UPDATE_URL="${PS5C_UPDATE_URL}")
        target_link_libraries(test_launcher_update PRIVATE ps5c_pal comctl32 gdiplus shell32 advapi32 ole32 winhttp bcrypt setupapi hid winmm)
        if(NOT MSVC)
            target_link_options(test_launcher_update PRIVATE -municode)
        endif()
        add_dependencies(test_launcher_update ps5c_build_info)
        add_test(NAME launcher_privacy COMMAND test_launcher_update)
        add_executable(test_launcher_reports tests/unit/test_launcher_reports.c src/core/user_config.c)
        target_include_directories(test_launcher_reports PRIVATE src ${CMAKE_BINARY_DIR}/generated)
        target_compile_definitions(test_launcher_reports PRIVATE UNICODE _UNICODE PS5C_UPDATE_URL="${PS5C_UPDATE_URL}")
        target_link_libraries(test_launcher_reports PRIVATE ps5c_pal comctl32 gdiplus shell32 advapi32 ole32 winhttp bcrypt setupapi hid winmm)
        if(NOT MSVC)
            target_link_options(test_launcher_reports PRIVATE -municode)
        endif()
        add_dependencies(test_launcher_reports ps5c_build_info)
        ps5c_target_defaults(test_launcher_reports)
        add_executable(test_launcher_quality tests/unit/test_launcher_quality.c src/core/user_config.c)
        target_include_directories(test_launcher_quality PRIVATE src ${CMAKE_BINARY_DIR}/generated)
        target_compile_definitions(test_launcher_quality PRIVATE UNICODE _UNICODE PS5C_UPDATE_URL="")
        target_link_libraries(test_launcher_quality PRIVATE ps5c_pal comctl32 gdiplus shell32 advapi32 ole32 winhttp bcrypt setupapi hid winmm)
        if(NOT MSVC)
            target_link_options(test_launcher_quality PRIVATE -municode)
        endif()
        add_dependencies(test_launcher_quality ps5c_build_info)
        ps5c_target_defaults(test_launcher_quality)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/report-quality")
        add_test(NAME launcher_report_quality COMMAND test_launcher_quality "${CMAKE_CURRENT_BINARY_DIR}/fixtures/report-quality")
        add_executable(test_launcher_sessions tests/unit/test_launcher_sessions.c src/core/user_config.c)
        target_include_directories(test_launcher_sessions PRIVATE src ${CMAKE_BINARY_DIR}/generated)
        target_compile_definitions(test_launcher_sessions PRIVATE UNICODE _UNICODE PS5C_UPDATE_URL="")
        target_link_libraries(test_launcher_sessions PRIVATE ps5c_pal comctl32 gdiplus shell32 advapi32 ole32 winhttp bcrypt setupapi hid winmm)
        if(NOT MSVC)
            target_link_options(test_launcher_sessions PRIVATE -municode)
        endif()
        add_dependencies(test_launcher_sessions ps5c_build_info)
        add_executable(test_launcher_patches tests/unit/test_launcher_patches.c src/core/user_config.c)
        target_include_directories(test_launcher_patches PRIVATE src ${CMAKE_BINARY_DIR}/generated)
        target_compile_definitions(test_launcher_patches PRIVATE UNICODE _UNICODE PS5C_UPDATE_URL="")
        target_link_libraries(test_launcher_patches PRIVATE ps5c_pal comctl32 gdiplus shell32 advapi32 ole32 winhttp bcrypt setupapi hid winmm)
        if(NOT MSVC)
            target_link_options(test_launcher_patches PRIVATE -municode)
        endif()
        add_dependencies(test_launcher_patches ps5c_build_info)
        ps5c_target_defaults(test_launcher_patches)
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/fixtures/launcher-patches")
        add_test(NAME launcher_patches COMMAND test_launcher_patches "${CMAKE_BINARY_DIR}/fixtures/launcher-patches")
        ps5c_target_defaults(test_launcher_sessions)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/c46")
        add_test(NAME launcher_sessions COMMAND test_launcher_sessions "${CMAKE_CURRENT_BINARY_DIR}/fixtures/c46")

        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/ui-languages")
        add_test(NAME launcher_localization COMMAND test_launcher_update language-tests "${CMAKE_CURRENT_BINARY_DIR}/fixtures/ui-languages")
    endif()
    add_executable(test_shader_global tests/unit/test_shader_global.c
                   src/gpu/shader/shader_analyze.c src/gpu/rdna2_decode.c)
    target_link_libraries(test_shader_global PRIVATE ps5c_base)
    ps5c_target_defaults(test_shader_global)
    add_test(NAME shader_global COMMAND test_shader_global)
    add_executable(test_inflate_rsa tests/unit/test_inflate_rsa.c)
    target_include_directories(test_inflate_rsa PRIVATE tests)
    target_link_libraries(test_inflate_rsa PRIVATE ps5c_base)
    ps5c_target_defaults(test_inflate_rsa)
    add_test(NAME inflate_rsa COMMAND test_inflate_rsa)
    add_executable(test_pkg_ps4 tests/unit/test_pkg_ps4.c)
    target_include_directories(test_pkg_ps4 PRIVATE tests)
    target_link_libraries(test_pkg_ps4 PRIVATE ps5c_pal ps5c_base)
    ps5c_target_defaults(test_pkg_ps4)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/pkg-ps4")
    add_test(NAME pkg_ps4 COMMAND test_pkg_ps4 "${CMAKE_CURRENT_BINARY_DIR}/fixtures/pkg-ps4")
    add_executable(test_texture_hash tests/unit/test_texture_hash.c)
    target_include_directories(test_texture_hash PRIVATE src)
    ps5c_target_defaults(test_texture_hash)
    add_test(NAME texture_hash COMMAND test_texture_hash)
    add_library(ps5c_test_support STATIC tests/support/sce_builder.c)
    target_include_directories(ps5c_test_support PUBLIC tests)
    target_link_libraries(ps5c_test_support PUBLIC ps5c_core)
    add_executable(test_vdecsw tests/unit/test_vdecsw.c)
    target_link_libraries(test_vdecsw PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_vdecsw)
    add_test(NAME vdecsw COMMAND test_vdecsw "${CMAKE_SOURCE_DIR}/tests/fixtures/vdecsw-blue16x18.h264" "${CMAKE_SOURCE_DIR}/tests/fixtures/vdecsw-bframes16x18.h264")
    set_tests_properties(vdecsw PROPERTIES TIMEOUT 30)
    add_executable(test_shader_calls tests/unit/test_shader_calls.c)
    target_link_libraries(test_shader_calls PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_shader_calls)
    add_test(NAME shader_calls COMMAND test_shader_calls)
    add_executable(test_image_atomic_raw tests/unit/test_image_atomic_raw.c)
    target_link_libraries(test_image_atomic_raw PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_image_atomic_raw)
    add_test(NAME image_atomic_raw COMMAND test_image_atomic_raw)
    add_executable(test_image_rgb10_store tests/unit/test_image_rgb10_store.c)
    target_link_libraries(test_image_rgb10_store PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_image_rgb10_store)
    add_test(NAME image_rgb10_store COMMAND test_image_rgb10_store)
    if(PS5C_RUN_GPU_TESTS)
        add_test(NAME image_rgb10_store_gpu COMMAND test_image_rgb10_store)
        set_tests_properties(image_rgb10_store_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU ENVIRONMENT "PS5C_TEST_GPU=1")
        add_test(NAME image_atomic_raw_gpu COMMAND test_image_atomic_raw)
        set_tests_properties(image_atomic_raw_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU ENVIRONMENT "PS5C_TEST_GPU=1")
        add_test(NAME image_linear_writeback_gpu COMMAND test_image_atomic_raw)
        set_tests_properties(image_linear_writeback_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_LINEAR_IMAGE_WB=1;PS5C_TEST_LINEAR_IMAGE_WB=1")
    endif()
    add_executable(test_shader_aperture tests/unit/test_shader_aperture.c)
    target_link_libraries(test_shader_aperture PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_shader_aperture)
    add_test(NAME shader_aperture COMMAND test_shader_aperture)
    ps5c_target_defaults(ps5c_test_support)

    if(WIN32)
        add_executable(test_bundled_assets tests/unit/test_bundled_assets.c frontends/assets/bundled.rc)
        target_link_libraries(test_bundled_assets PRIVATE ps5c_test_support)
        ps5c_target_defaults(test_bundled_assets)
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/fixtures/bundled-assets")
        add_test(NAME bundled_assets COMMAND test_bundled_assets
            "${CMAKE_SOURCE_DIR}/data/fonts/NotoSans-Regular.ttf"
            "${CMAKE_SOURCE_DIR}/data/intro.mp4")
        set_tests_properties(bundled_assets PROPERTIES
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/fixtures/bundled-assets" TIMEOUT 45)
    endif()

    foreach(t writeback_merge uniform_layout spv_prologue spv_intern descriptor_plan avplayer_timing base pal loader mem mem_access exfat vertex_ranges texture_layout texture_tiling texture_3d resident_mips sparse_plan ngg_group image_views color_target dcc_sample fill_clear htile_clear shader_resinfo shader_options shader_corpus shader_gaps shader_inputs shader_lod shader_mip shader_half shader_half_sdwa shader_masks shader_dpp8 shader_gds shader_gather_compare compute_dispatch buffer_pairs shader_resources store_range sbuffer audio_out ajm_resample agc_suspend agc_queues gpu_waits kernel_memory resource_limits runtime_faults thread_start thread_cancel kernel_pipe kernel_cwd libc_internal libc_callbacks process_params pad_script pad_motion mesh_indices program_storage playgo apr_events net_loopback acm snapshot_refresh shader_fp16_overflow shader_isa shader_isa_coverage json2 crypto png_decode rtc stencil_state overlay packed_formats shader_structured font_raster gcn ue_scale)
        add_executable(test_${t} tests/unit/test_${t}.c)
        target_link_libraries(test_${t} PRIVATE ps5c_test_support)
        ps5c_target_defaults(test_${t})
        ps5c_guest_host(test_${t})
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()
    add_executable(test_kernel_readv tests/unit/test_kernel_readv.c)
    target_link_libraries(test_kernel_readv PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_kernel_readv)
    ps5c_guest_host(test_kernel_readv)
    add_test(NAME kernel_readv COMMAND test_kernel_readv)
    add_executable(test_kernel_pwritev tests/unit/test_kernel_pwritev.c)
    target_link_libraries(test_kernel_pwritev PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_kernel_pwritev)
    ps5c_guest_host(test_kernel_pwritev)
    add_test(NAME kernel_pwritev COMMAND test_kernel_pwritev)
    add_executable(test_hrtimer tests/unit/test_hrtimer.c)
    target_link_libraries(test_hrtimer PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_hrtimer)
    ps5c_guest_host(test_hrtimer)
    add_test(NAME hrtimer COMMAND test_hrtimer)
    add_executable(test_cpu_compat tests/unit/test_cpu_compat.c)
    if(PS5C_RUN_GPU_TESTS)
        add_test(NAME sbuffer_local_gpu COMMAND test_sbuffer)
        set_tests_properties(sbuffer_local_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU
            ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_LOCAL_READONLY=1;PS5C_LOCAL_COMPUTE=1")
    endif()
    add_executable(test_compat_patches tests/unit/test_compat_patches.c)
    target_link_libraries(test_compat_patches PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_compat_patches)
    ps5c_guest_host(test_compat_patches)
    add_test(NAME compat_patches COMMAND test_compat_patches)
    if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
        add_executable(test_fiber_handoff tests/unit/test_fiber_handoff.c)
        target_link_libraries(test_fiber_handoff PRIVATE ps5c_test_support)
        target_link_options(test_fiber_handoff PRIVATE -Wl,--wrap=x64_native_switch)
        ps5c_target_defaults(test_fiber_handoff)
        ps5c_guest_host(test_fiber_handoff)
        add_test(NAME fiber_handoff COMMAND test_fiber_handoff)
        set_tests_properties(fiber_handoff PROPERTIES TIMEOUT 15)
        add_executable(test_native_input tests/unit/test_native_input.c)
        target_link_libraries(test_native_input PRIVATE ps5c_test_support)
        target_link_options(test_native_input PRIVATE -Wl,--wrap=pal_input_has_controller)
        ps5c_target_defaults(test_native_input)
        ps5c_guest_host(test_native_input)
        add_test(NAME native_input COMMAND test_native_input)
    endif()
    target_link_libraries(test_cpu_compat PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_cpu_compat)
    ps5c_guest_host(test_cpu_compat)
    add_test(NAME cpu_compat COMMAND test_cpu_compat)
    add_test(NAME cpu_compat_off COMMAND test_cpu_compat --off)
    add_test(NAME cpu_compat_auto COMMAND test_cpu_compat --auto)
    add_test(NAME cpu_compat_vector_fault COMMAND test_cpu_compat --trap)
    set_tests_properties(cpu_compat cpu_compat_off cpu_compat_auto cpu_compat_vector_fault PROPERTIES TIMEOUT 30)
    add_executable(test_apr_pending tests/unit/test_apr_pending.c)
    target_link_libraries(test_apr_pending PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_apr_pending)
    ps5c_guest_host(test_apr_pending)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC AND NOT APPLE)
        target_compile_definitions(test_apr_pending PRIVATE PS5C_TEST_APR_WAIT_HANDSHAKE=1)
        target_link_options(test_apr_pending PRIVATE -Wl,--wrap=pal_cond_wait -Wl,--wrap=pal_sleep_ns -Wl,--wrap=malloc)
        target_compile_definitions(test_kernel_memory PRIVATE PS5C_TEST_MAP_COMMIT_FAILURE=1)
        target_link_options(test_kernel_memory PRIVATE -Wl,--wrap=pal_vm_commit)
    endif()
    add_test(NAME apr_pending COMMAND test_apr_pending)
    add_test(NAME apr_pending_sync COMMAND test_apr_pending --sync)
    set_tests_properties(apr_pending PROPERTIES TIMEOUT 15 ENVIRONMENT "PS5C_APR_LATENCY_US=0")
    set_tests_properties(apr_pending_sync PROPERTIES TIMEOUT 15 ENVIRONMENT "PS5C_APR_SYNC=1")
    add_test(NAME flexible_budget_reclaim COMMAND test_kernel_memory --flex-reclaim)
    add_test(NAME agc_suspend_empty_runtime COMMAND test_agc_suspend --empty-runtime)
    set_tests_properties(agc_suspend_empty_runtime PROPERTIES TIMEOUT 20 ENVIRONMENT "PS5C_CP_WAITS=1")
    if(WIN32)
        add_executable(test_pal_read_recovery tests/unit/test_pal_read_recovery.c)
        target_link_libraries(test_pal_read_recovery PRIVATE ps5c_test_support)
        ps5c_target_defaults(test_pal_read_recovery)
        add_test(NAME pal_read_recovery COMMAND test_pal_read_recovery)
        set_tests_properties(pal_read_recovery PROPERTIES TIMEOUT 20)
    endif()
    if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
        add_executable(test_writeback_oom tests/unit/test_writeback_oom.c)
        target_link_libraries(test_writeback_oom PRIVATE ps5c_test_support)
        ps5c_target_defaults(test_writeback_oom)
        ps5c_guest_host(test_writeback_oom)
        target_link_options(test_writeback_oom PRIVATE -Wl,--wrap=malloc -Wl,--wrap=pal_exit_process)
        if(PS5C_RUN_GPU_TESTS)
            foreach(pool 0 1)
                add_test(NAME writeback_oom_pool${pool} COMMAND test_writeback_oom)
                set_tests_properties(writeback_oom_pool${pool} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU
                    ENVIRONMENT "PS5C_WB_POOL=${pool};PS5C_STORE_WINDOWS=0;PS5C_NO_CPU_FILL=1;PS5C_READONLY_CACHE=0;PS5C_WORK_OVERLAP=0;PS5C_TEST_GPU=1")
            endforeach()
            add_test(NAME writeback_scope_gpu COMMAND test_writeback_oom)
            set_tests_properties(writeback_scope_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU
                ENVIRONMENT "PS5C_WB_POOL=1;PS5C_STORE_FOOTPRINT=1;PS5C_TEST_SCOPE_MULTI=1;PS5C_STORE_WINDOWS=0;PS5C_NO_CPU_FILL=1;PS5C_READONLY_CACHE=0;PS5C_WORK_OVERLAP=0;PS5C_TEST_GPU=1")
            add_test(NAME writeback_scope_check_gpu COMMAND test_writeback_oom)
            set_tests_properties(writeback_scope_check_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU
                ENVIRONMENT "PS5C_WB_POOL=1;PS5C_STORE_FOOTPRINT=check;PS5C_STORE_WINDOWS=0;PS5C_NO_CPU_FILL=1;PS5C_READONLY_CACHE=0;PS5C_WORK_OVERLAP=0;PS5C_TEST_GPU=1")
        endif()
    endif()
    foreach(t gpu_shader_size eval_cache held_labels shader_execz)
        add_executable(test_${t} tests/unit/test_${t}.c)
        target_link_libraries(test_${t} PRIVATE ps5c_test_support)
        ps5c_target_defaults(test_${t})
        ps5c_guest_host(test_${t})
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()
    add_executable(test_buffer_prefix tests/unit/test_buffer_prefix.c)
    target_link_libraries(test_buffer_prefix PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_buffer_prefix)
    ps5c_guest_host(test_buffer_prefix)
    if(PS5C_RUN_GPU_TESTS)
        foreach(mode 0 1 2)
            add_test(NAME buffer_prefix_gpu_${mode} COMMAND test_buffer_prefix)
            set_tests_properties(buffer_prefix_gpu_${mode} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU
                ENVIRONMENT "PS5C_STORE_FOOTPRINT=${mode};PS5C_NO_CPU_FILL=1;PS5C_WORK_OVERLAP=0;PS5C_TEST_GPU=1;PS5C_GPU_STATS=1")
        endforeach()
    endif()
    foreach(t ds_swizzle shader_scalar_reference)
        add_executable(test_${t} tests/unit/test_${t}.c)
        target_link_libraries(test_${t} PRIVATE ps5c_test_support)
        ps5c_target_defaults(test_${t})
        ps5c_guest_host(test_${t})
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()
    add_executable(test_shader_class tests/unit/test_shader_class.c)
    target_link_libraries(test_shader_class PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_shader_class)
    ps5c_guest_host(test_shader_class)
    add_test(NAME shader_class COMMAND test_shader_class)
    add_executable(test_buffer_d16 tests/unit/test_buffer_d16.c)
    target_link_libraries(test_buffer_d16 PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_buffer_d16)
    ps5c_guest_host(test_buffer_d16)
    add_test(NAME buffer_d16 COMMAND test_buffer_d16)
    if(WIN32)
        add_executable(test_image_physical_alias tests/unit/test_image_physical_alias.c)
        target_link_libraries(test_image_physical_alias PRIVATE ps5c_test_support)
        ps5c_target_defaults(test_image_physical_alias)
        ps5c_guest_host(test_image_physical_alias)
        add_test(NAME image_physical_alias COMMAND test_image_physical_alias)
        if(PS5C_RUN_GPU_TESTS)
            add_test(NAME image_physical_alias_gpu COMMAND test_image_physical_alias)
            set_tests_properties(image_physical_alias_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU ENVIRONMENT "PS5C_TEST_GPU=1")
        endif()
    endif()
    if(PS5C_RUN_GPU_TESTS)
        add_test(NAME held_labels_gpu COMMAND test_held_labels)
        add_test(NAME shader_execz_gpu COMMAND test_shader_execz)
        add_test(NAME shader_execz_dispatch_gpu COMMAND test_shader_execz)
        set_tests_properties(held_labels_gpu shader_execz_gpu shader_execz_dispatch_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU
            ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_NO_CPU_FILL=1;PS5C_WORK_OVERLAP=0")
        set_tests_properties(shader_execz_dispatch_gpu PROPERTIES ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_NO_CPU_FILL=1;PS5C_WORK_OVERLAP=0;PS5C_NO_STRUCTURED=1")
        add_test(NAME shader_class_gpu COMMAND test_shader_class)
        add_test(NAME buffer_d16_gpu COMMAND test_buffer_d16)
        set_tests_properties(buffer_d16_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU ENVIRONMENT "PS5C_TEST_GPU=1")
        set_tests_properties(shader_class_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU
            ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_CS_WAVE_FROM_PACKET=1;PS5C_CS_NATIVE32=1;PS5C_CS_WAVE64=1")
        add_test(NAME constant_blend_serial COMMAND test_image_views --constant-blend)
        add_test(NAME constant_blend_overlap COMMAND test_image_views --constant-blend)
        set_tests_properties(constant_blend_serial constant_blend_overlap PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE LABELS GPU)
        set_tests_properties(constant_blend_serial PROPERTIES ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_WORK_OVERLAP=0")
        set_tests_properties(constant_blend_overlap PROPERTIES ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_WORK_OVERLAP=1")
        add_test(NAME ds_swizzle_native32 COMMAND test_ds_swizzle)
        add_test(NAME ds_swizzle_paired64 COMMAND test_ds_swizzle)
        set_tests_properties(ds_swizzle_native32 ds_swizzle_paired64 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE LABELS GPU)
        set_tests_properties(ds_swizzle_native32 PROPERTIES ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_CS_WAVE_FROM_PACKET=1;PS5C_CS_NATIVE32=1;PS5C_TEST_SWIZZLE_NATIVE32=1")
        set_tests_properties(ds_swizzle_paired64 PROPERTIES ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_CS_WAVE_FROM_PACKET=1;PS5C_CS_WAVE64=1")
        add_test(NAME dpp8_native32 COMMAND test_shader_dpp8)
        add_test(NAME dpp8_paired64 COMMAND test_shader_dpp8)
        set_tests_properties(dpp8_native32 dpp8_paired64 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE LABELS GPU)
        set_tests_properties(dpp8_native32 PROPERTIES ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_CS_WAVE_FROM_PACKET=1;PS5C_CS_NATIVE32=1;PS5C_TEST_DPP_NATIVE32=1")
        set_tests_properties(dpp8_paired64 PROPERTIES ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_CS_WAVE_FROM_PACKET=1;PS5C_CS_WAVE64=1;PS5C_TEST_DPP_PAIRED64=1")
        add_test(NAME pseudoflags_gpu COMMAND test_shader_masks)
        set_tests_properties(pseudoflags_gpu PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE LABELS GPU
            ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_TEST_PSEUDOFLAGS=1;PS5C_CS_WAVE_FROM_PACKET=1;PS5C_CS_NATIVE32=1;PS5C_CS_WAVE64=1")
        add_test(NAME resident_mips_gpu COMMAND test_resident_mips)
        set_tests_properties(resident_mips_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU
            ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_RESIDENT_MIPS=1")
        add_test(NAME sampler_mip_disabled_gpu COMMAND test_resident_mips)
        set_tests_properties(sampler_mip_disabled_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU
            ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_RESIDENT_MIPS=1;PS5C_TEST_SAMPLER_MIP_MODE=1")
        add_test(NAME image_views_gpu COMMAND test_image_views)
        set_tests_properties(image_views_gpu PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS GPU ENVIRONMENT "PS5C_TEST_GPU=1")
        foreach(mode serial retired legacy no_timeline)
            add_test(NAME scalar_writeback_${mode} COMMAND test_image_views --scalar-writeback)
            set_tests_properties(scalar_writeback_${mode} PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE LABELS GPU)
        endforeach()
        set_tests_properties(scalar_writeback_serial PROPERTIES ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_WORK_OVERLAP=0")
        set_tests_properties(scalar_writeback_retired PROPERTIES ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_WORK_OVERLAP=1;PS5C_VK_RETIRE=1")
        set_tests_properties(scalar_writeback_legacy PROPERTIES ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_WORK_OVERLAP=1;PS5C_VK_RETIRE=0")
        set_tests_properties(scalar_writeback_no_timeline PROPERTIES ENVIRONMENT "PS5C_TEST_GPU=1;PS5C_WORK_OVERLAP=1;PS5C_VK_TIMELINE=0")
    endif()
    if(WIN32)
        add_executable(test_shader_opcode_threads tests/unit/test_shader_opcode_threads.c)
        target_link_libraries(test_shader_opcode_threads PRIVATE ps5c_test_support)
        ps5c_target_defaults(test_shader_opcode_threads)
        add_test(NAME shader_opcode_threads COMMAND test_shader_opcode_threads)
        set_tests_properties(shader_opcode_threads PROPERTIES TIMEOUT 20)
    endif()
    add_executable(test_np_offline tests/unit/test_np_offline.c)
    target_link_libraries(test_np_offline PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_np_offline)
    ps5c_guest_host(test_np_offline)
    add_test(NAME np_offline COMMAND test_np_offline)
    add_executable(test_save_convert tests/unit/test_save_convert.c)
    target_link_libraries(test_save_convert PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_save_convert)
    ps5c_guest_host(test_save_convert)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/testing/save-convert")
    add_test(NAME save_convert COMMAND test_save_convert)
    set_tests_properties(save_convert PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/testing/save-convert")
    add_executable(test_avplayer_policy tests/unit/test_avplayer_policy.c)
    target_link_libraries(test_avplayer_policy PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_avplayer_policy)
    ps5c_guest_host(test_avplayer_policy)
    add_test(NAME avplayer_policy COMMAND test_avplayer_policy)
    add_test(NAME avplayer_async_policy COMMAND test_avplayer_policy --async)
    set_tests_properties(avplayer_async_policy PROPERTIES ENVIRONMENT "PS5C_AVP_ASYNC_EVENTS=1")
    add_test(NAME avplayer_explicit_sync COMMAND test_avplayer_policy --sync-override)
    set_tests_properties(avplayer_explicit_sync PROPERTIES ENVIRONMENT "PS5C_AVP_ASYNC_EVENTS=1;PS5C_AVP_SYNC_EVENTS=1")
    add_executable(test_gpu_watch tests/unit/test_gpu_watch.c)
    target_link_libraries(test_gpu_watch PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_gpu_watch)
    add_test(NAME gpu_watch COMMAND test_gpu_watch)
    add_executable(test_gpu_watch_cow tests/unit/test_gpu_watch_cow.c)
    target_link_libraries(test_gpu_watch_cow PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_gpu_watch_cow)
    add_test(NAME gpu_watch_cow COMMAND test_gpu_watch_cow)
    add_executable(test_mem_pins tests/unit/test_mem_pins.c)
    target_link_libraries(test_mem_pins PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_mem_pins)
    ps5c_guest_host(test_mem_pins)
    add_test(NAME mem_pins COMMAND test_mem_pins)
    set_tests_properties(mem_pins mem_access PROPERTIES TIMEOUT 15)
    if(WIN32)
        add_executable(test_shader_cache tests/unit/test_shader_cache.c)
        target_link_libraries(test_shader_cache PRIVATE ps5c_test_support)
        ps5c_target_defaults(test_shader_cache)
        add_test(NAME shader_cache COMMAND test_shader_cache)
        set_tests_properties(shader_cache PROPERTIES TIMEOUT 30)
        add_executable(test_vk_work_failure tests/unit/test_vk_work_failure.c)
        target_link_libraries(test_vk_work_failure PRIVATE ps5c_test_support)
        ps5c_target_defaults(test_vk_work_failure)
        ps5c_guest_host(test_vk_work_failure)
        add_test(NAME vk_work_failure COMMAND test_vk_work_failure)
        set_tests_properties(vk_work_failure PROPERTIES TIMEOUT 120)
        add_executable(test_mem_demand tests/unit/test_mem_demand.c)
        target_link_libraries(test_mem_demand PRIVATE ps5c_test_support)
        ps5c_target_defaults(test_mem_demand)
        ps5c_guest_host(test_mem_demand)
        add_test(NAME mem_demand COMMAND test_mem_demand)
        set_tests_properties(mem_demand PROPERTIES TIMEOUT 20)
    endif()
    add_executable(test_dynamic_buffers tests/unit/test_dynamic_buffers.c)
    target_link_libraries(test_dynamic_buffers PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_dynamic_buffers)
    ps5c_guest_host(test_dynamic_buffers)
    add_test(NAME dynamic_buffers COMMAND test_dynamic_buffers)
    add_executable(test_dynamic_plan tests/unit/test_dynamic_plan.c)
    target_link_libraries(test_dynamic_plan PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_dynamic_plan)
    ps5c_guest_host(test_dynamic_plan)
    add_test(NAME dynamic_plan COMMAND test_dynamic_plan)
    add_executable(test_shader_bvh tests/unit/test_shader_bvh.c)
    target_link_libraries(test_shader_bvh PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_shader_bvh)
    ps5c_guest_host(test_shader_bvh)
    add_test(NAME shader_bvh COMMAND test_shader_bvh)
    add_executable(test_pipeline_async_wait tests/unit/test_pipeline_async_wait.c src/gpu/vk/vk_pipeline_async.c)
    add_dependencies(test_pipeline_async_wait ps5c_build_info)
    target_link_libraries(test_pipeline_async_wait PRIVATE ps5c_pal)
    ps5c_target_defaults(test_pipeline_async_wait)
    add_test(NAME pipeline_async_wait COMMAND test_pipeline_async_wait "${CMAKE_BINARY_DIR}")
    set_tests_properties(pipeline_async_wait PROPERTIES TIMEOUT 10)
    add_executable(test_pipeline_budget tests/unit/test_pipeline_budget.c src/gpu/vk/vk_pipeline_async.c)
    add_dependencies(test_pipeline_budget ps5c_build_info)
    target_link_libraries(test_pipeline_budget PRIVATE ps5c_pal)
    ps5c_target_defaults(test_pipeline_budget)
    add_test(NAME pipeline_budget COMMAND test_pipeline_budget "${CMAKE_BINARY_DIR}")
    set_tests_properties(pipeline_budget PROPERTIES TIMEOUT 10)
    add_executable(test_ghost_gpu tests/unit/test_ghost_gpu.c)
    target_link_libraries(test_ghost_gpu PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_ghost_gpu)
    ps5c_guest_host(test_ghost_gpu)
    add_test(NAME ghost_gpu COMMAND test_ghost_gpu)
    add_executable(test_shader_precision tests/unit/test_shader_precision.c)
    target_link_libraries(test_shader_precision PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_shader_precision)
    ps5c_guest_host(test_shader_precision)
    add_test(NAME shader_precision COMMAND test_shader_precision)
    add_executable(test_shader_link tests/unit/test_shader_link.c)
    target_link_libraries(test_shader_link PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_shader_link)
    ps5c_guest_host(test_shader_link)
    add_test(NAME shader_link COMMAND test_shader_link)
    add_executable(test_lds64 tests/unit/test_lds64.c)
    target_link_libraries(test_lds64 PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_lds64)
    ps5c_guest_host(test_lds64)
    add_test(NAME lds64 COMMAND test_lds64)
    add_executable(test_lod_stats tests/unit/test_lod_stats.c)
    target_link_libraries(test_lod_stats PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_lod_stats)
    ps5c_guest_host(test_lod_stats)
    add_test(NAME lod_stats COMMAND test_lod_stats)
    add_executable(test_mesh_native tests/unit/test_mesh_native.c)
    target_link_libraries(test_mesh_native PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_mesh_native)
    ps5c_guest_host(test_mesh_native)
    add_test(NAME mesh_native COMMAND test_mesh_native)
    add_test(NAME mesh_ballot32 COMMAND ${CMAKE_COMMAND} -E env
        PS5C_TEST_NGG_BALLOT32=1 PS5C_TEST_BALLOT_LOAD=1 $<TARGET_FILE:test_mesh_native>)
    add_executable(test_shader_waterfall tests/unit/test_shader_waterfall.c)
    target_link_libraries(test_shader_waterfall PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_shader_waterfall)
    ps5c_guest_host(test_shader_waterfall)
    add_test(NAME shader_waterfall COMMAND test_shader_waterfall)
    if(PS5C_RUN_GPU_TESTS)
        add_test(NAME shader_waterfall_gpu COMMAND test_shader_waterfall)
        set_tests_properties(shader_waterfall_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS GPU ENVIRONMENT "PS5C_TEST_GPU=1")
    endif()
    add_executable(test_shader_movrel tests/unit/test_shader_movrel.c)
    target_link_libraries(test_shader_movrel PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_shader_movrel)
    ps5c_guest_host(test_shader_movrel)
    add_test(NAME shader_movrel COMMAND test_shader_movrel)
    add_executable(test_pixel_interlock tests/unit/test_pixel_interlock.c)
    target_link_libraries(test_pixel_interlock PRIVATE ps5c_test_support)
    ps5c_target_defaults(test_pixel_interlock)
    ps5c_guest_host(test_pixel_interlock)
    add_test(NAME pixel_interlock COMMAND test_pixel_interlock)
    set_tests_properties(agc_suspend PROPERTIES TIMEOUT 15 ENVIRONMENT "PS5C_WAIT_SUSPEND_POINT=1")
    add_test(NAME agc_suspend_resumable COMMAND test_agc_suspend)
    set_tests_properties(agc_suspend_resumable agc_queues PROPERTIES TIMEOUT 15 ENVIRONMENT "PS5C_WAIT_SUSPEND_POINT=1;PS5C_CP_WAITS=1")
    # Exercise the real cross-queue dependency with automatic GTA selection,
    # independently of the explicit diagnostic override tested above.
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/agc-gta/sce_sys")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fixtures/agc-gta/sce_sys/param.json" "{\"titleId\":\"PPSA04263\"}\n")
    add_test(NAME agc_queues_gta_default COMMAND test_agc_queues "${CMAKE_CURRENT_BINARY_DIR}/fixtures/agc-gta")
    set_tests_properties(agc_queues_gta_default PROPERTIES TIMEOUT 15 ENVIRONMENT "PS5C_WAIT_SUSPEND_POINT=1;PS5C_CP_WAITS=")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC AND NOT APPLE)
        add_executable(test_mem_remap tests/unit/test_mem_remap.c)
        target_link_libraries(test_mem_remap PRIVATE ps5c_test_support)
        target_link_options(test_mem_remap PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc)
        ps5c_target_defaults(test_mem_remap)
        ps5c_guest_host(test_mem_remap)
        add_test(NAME mem_remap COMMAND test_mem_remap)
        add_test(NAME mem_remap_low_shared COMMAND test_mem_remap)
        set_tests_properties(mem_remap_low_shared PROPERTIES ENVIRONMENT "PS5C_LOW_SHARED_AREA=1")
        add_executable(test_mem_psp_mirrors tests/unit/test_mem_psp_mirrors.c)
        target_link_libraries(test_mem_psp_mirrors PRIVATE ps5c_test_support)
        ps5c_target_defaults(test_mem_psp_mirrors)
        ps5c_guest_host(test_mem_psp_mirrors)
        add_test(NAME mem_psp_mirrors COMMAND test_mem_psp_mirrors)
    endif()
    add_test(NAME cli_host_info COMMAND ps5c --host-info)

    # End to end: synthetic game -> `ps5c analyze` (finds sce_module/, loads the NID database)
    add_executable(mksample tools/mksample.c)
    target_link_libraries(mksample PRIVATE ps5c_test_support)
    ps5c_target_defaults(mksample)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/sample/sce_module)
    add_test(NAME sample_create COMMAND mksample ${CMAKE_BINARY_DIR}/sample)
    add_test(NAME cli_analyze
             COMMAND ps5c analyze ${CMAKE_BINARY_DIR}/sample/eboot.bin -o ${CMAKE_BINARY_DIR}/sample/game_profile.json)
    set_tests_properties(sample_create PROPERTIES FIXTURES_SETUP sample)
    set_tests_properties(cli_analyze PROPERTIES FIXTURES_REQUIRED sample
                         WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                         PASS_REGULAR_EXPRESSION "unique imports : 5 \\(5 with known names\\).*0 HLE, 1 LLE, 4 missing")
    add_test(NAME cli_load COMMAND ps5c load ${CMAKE_BINARY_DIR}/sample/eboot.bin)
    set_tests_properties(cli_load PROPERTIES FIXTURES_REQUIRED sample
                         PASS_REGULAR_EXPRESSION "modules mapped      : 2.*relocations skipped : 0")
endif()
