cmake_minimum_required(VERSION 3.21)
project(WinAirPlay VERSION 0.5.0 LANGUAGES C CXX)
if(WIN32)
    enable_language(RC)   # application icon and version information (assets/winairplay.rc)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    add_definitions(-D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX
                    -D_CRT_SECURE_NO_WARNINGS -D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- libplist (required) ------------------------------------------------------
set(LIBPLIST_TARGET "")

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBPLIST IMPORTED_TARGET libplist-2.0 QUIET)
    if(LIBPLIST_FOUND)
        set(LIBPLIST_TARGET PkgConfig::LIBPLIST)
    endif()
endif()

if(NOT LIBPLIST_TARGET)
    find_package(libplist CONFIG QUIET)
    foreach(_t libplist::libplist-2.0 libplist::libplist unofficial::libplist::libplist)
        if(TARGET ${_t})
            set(LIBPLIST_TARGET ${_t})
            break()
        endif()
    endforeach()
endif()

if(NOT LIBPLIST_TARGET)
    find_path(LIBPLIST_INCLUDE_DIR plist/plist.h)
    find_library(LIBPLIST_LIBRARY NAMES plist-2.0 libplist-2.0 plist libplist)
    if(LIBPLIST_INCLUDE_DIR AND LIBPLIST_LIBRARY)
        message(STATUS "libplist: raw find — include=${LIBPLIST_INCLUDE_DIR} "
                       "lib=${LIBPLIST_LIBRARY}")
        add_library(airplay_libplist INTERFACE)
        target_include_directories(airplay_libplist INTERFACE "${LIBPLIST_INCLUDE_DIR}")
        target_link_libraries(airplay_libplist INTERFACE "${LIBPLIST_LIBRARY}")
        set(LIBPLIST_TARGET airplay_libplist)
    endif()
endif()

# ---- OpenSSL (required) -------------------------------------------------------
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

# ---- FFmpeg (required for H.264/HEVC decoding) --------------------------------
set(FFMPEG_TARGETS "")
if(PkgConfig_FOUND)
    pkg_check_modules(AVCODEC  IMPORTED_TARGET libavcodec)
    pkg_check_modules(AVFORMAT IMPORTED_TARGET libavformat)
    pkg_check_modules(AVUTIL   IMPORTED_TARGET libavutil)
    pkg_check_modules(SWSCALE  IMPORTED_TARGET libswscale)
    if(AVCODEC_FOUND AND AVFORMAT_FOUND AND AVUTIL_FOUND AND SWSCALE_FOUND)
        set(FFMPEG_TARGETS PkgConfig::AVCODEC PkgConfig::AVFORMAT
                           PkgConfig::AVUTIL PkgConfig::SWSCALE)
    endif()
endif()
if(NOT FFMPEG_TARGETS)
    find_package(FFMPEG QUIET)
    if(FFMPEG_FOUND)
        set(FFMPEG_TARGETS ${FFMPEG_LIBRARIES})
        set(FFMPEG_INCLUDE_DIRS_OK "${FFMPEG_INCLUDE_DIRS}")
    endif()
endif()
if(NOT FFMPEG_TARGETS)
    message(FATAL_ERROR
        "FFmpeg (libavcodec + libavutil + libswscale) not found.\n"
        "  Windows: vcpkg.json already declares the dependency — rerun cmake "
        "with the vcpkg toolchain.\n"
        "  Linux:   apt install libavcodec-dev libswscale-dev")
endif()

# ---- SDL2 (required for the video renderer window) ----------------------------
set(SDL2_TARGET "")
if(NOT WIN32 AND PkgConfig_FOUND)
    pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
    if(SDL2_FOUND)
        set(SDL2_TARGET PkgConfig::SDL2)
    endif()
endif()
if(NOT SDL2_TARGET)
    find_package(SDL2 CONFIG QUIET)
    if(TARGET SDL2::SDL2)
        set(SDL2_TARGET SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        set(SDL2_TARGET SDL2::SDL2-static)
    endif()
endif()
if(NOT SDL2_TARGET AND PkgConfig_FOUND)
    pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
    if(SDL2_FOUND)
        set(SDL2_TARGET PkgConfig::SDL2)
    endif()
endif()
if(NOT SDL2_TARGET)
    message(FATAL_ERROR
        "SDL2 not found.\n"
        "  Windows: rerun cmake with the vcpkg toolchain.\n"
        "  Linux:   apt install libsdl2-dev")
endif()

# ---- SDL2_ttf (required for the metadata text overlay) ------------------------
set(SDL2_TTF_TARGET "")
if(NOT WIN32 AND PkgConfig_FOUND)
    pkg_check_modules(SDL2_TTF IMPORTED_TARGET SDL2_ttf)
    if(SDL2_TTF_FOUND)
        set(SDL2_TTF_TARGET PkgConfig::SDL2_TTF)
    endif()
endif()
if(NOT SDL2_TTF_TARGET)
    find_package(SDL2_ttf CONFIG QUIET)
    if(TARGET SDL2_ttf::SDL2_ttf)
        set(SDL2_TTF_TARGET SDL2_ttf::SDL2_ttf)
    elseif(TARGET SDL2_ttf::SDL2_ttf-static)
        set(SDL2_TTF_TARGET SDL2_ttf::SDL2_ttf-static)
    endif()
endif()
if(NOT SDL2_TTF_TARGET AND PkgConfig_FOUND)
    pkg_check_modules(SDL2_TTF IMPORTED_TARGET SDL2_ttf)
    if(SDL2_TTF_FOUND)
        set(SDL2_TTF_TARGET PkgConfig::SDL2_TTF)
    endif()
endif()
if(NOT SDL2_TTF_TARGET)
    message(FATAL_ERROR
        "SDL2_ttf not found.\n"
        "  Windows: rerun cmake with the vcpkg toolchain (already in vcpkg.json).\n"
        "  Linux:   apt install libsdl2-ttf-dev")
endif()

# ---- GStreamer (preferred HLS backend if available) ---------------------------
set(USE_GSTREAMER_HLS "AUTO" CACHE STRING
    "HLS backend: AUTO (prefer GStreamer if found), ON (require it), OFF (libavformat)")
set_property(CACHE USE_GSTREAMER_HLS PROPERTY STRINGS AUTO ON OFF)
set(HLS_PLAYER_SRC src/video/hls_player.cpp)
set(_use_gst FALSE)
if(USE_GSTREAMER_HLS STREQUAL "ON")
    set(_use_gst TRUE)
elseif(USE_GSTREAMER_HLS STREQUAL "AUTO")
    set(_use_gst TRUE)
endif()
if(_use_gst)
    if(NOT PkgConfig_FOUND)
        if(USE_GSTREAMER_HLS STREQUAL "ON")
            message(FATAL_ERROR "USE_GSTREAMER_HLS=ON requires pkg-config")
        endif()
        message(STATUS "HLS backend: pkg-config missing -> falling back to libavformat")
        set(_use_gst FALSE)
    endif()
endif()
if(_use_gst)
    if(GSTREAMER_ROOT)
        string(REPLACE "\\" "/" _gst_root "${GSTREAMER_ROOT}")
        set(_gst_pkg_dir "")
        foreach(_cand
                "${_gst_root}"
                "${_gst_root}/pkgconfig"
                "${_gst_root}/lib/pkgconfig"
                "${_gst_root}/lib")
            if(EXISTS "${_cand}/gstreamer-1.0.pc")
                set(_gst_pkg_dir "${_cand}")
                break()
            endif()
        endforeach()
        if(_gst_pkg_dir)
            message(STATUS "GSTREAMER_ROOT=${GSTREAMER_ROOT}")
            message(STATUS "  -> pkgconfig dir: ${_gst_pkg_dir}")
            if(WIN32)
                set(ENV{PKG_CONFIG_PATH}
                    "${_gst_pkg_dir};$ENV{PKG_CONFIG_PATH}")
            else()
                set(ENV{PKG_CONFIG_PATH}
                    "${_gst_pkg_dir}:$ENV{PKG_CONFIG_PATH}")
            endif()
        else()
            message(WARNING
                "GSTREAMER_ROOT=${GSTREAMER_ROOT}\n"
                "  gstreamer-1.0.pc not found under that path.")
        endif()
    endif()
    pkg_check_modules(GSTREAMER IMPORTED_TARGET
        gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
    if(NOT GSTREAMER_FOUND)
        if(USE_GSTREAMER_HLS STREQUAL "ON")
            message(FATAL_ERROR
                "USE_GSTREAMER_HLS=ON but gstreamer-1.0/gstreamer-app-1.0/gstreamer-video-1.0 not found.")
        endif()
        message(STATUS "HLS backend: GStreamer not found -> using libavformat")
        set(_use_gst FALSE)
    else()
        message(STATUS "HLS backend: GStreamer (${GSTREAMER_VERSION})")
        set(HLS_PLAYER_SRC src/video/hls_player_gstreamer.cpp)
    endif()
endif()
if(NOT _use_gst)
    message(STATUS "HLS backend: libavformat "
                   "(force GStreamer with -DUSE_GSTREAMER_HLS=ON)")
endif()

# ---- Dear ImGui (in-app overlay UI) ------------------------------------------
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC ${SDL2_TARGET})
if(MSVC)
    target_compile_options(imgui PRIVATE /W0)
endif()

# ---- Sources ------------------------------------------------------------------
add_executable(WinAirPlay
    src/main.cpp
    src/log.cpp
    src/settings.cpp
    src/i18n/localization.cpp
    src/net/socket.cpp
    src/net/tcp_server.cpp
    src/mdns/mdns_service.cpp
    src/crypto/identity.cpp
    src/crypto/pair_verify.cpp
    src/crypto/fairplay.cpp
    src/crypto/mirror_decrypt.cpp
    src/airplay/airplay2_setup.cpp
    src/airplay/daap.cpp
    src/airplay/h264_nal.cpp
    src/airplay/h264_sps.cpp
    src/airplay/info_plist.cpp
    src/airplay/mirror_listener.cpp
    src/airplay/ntp_client.cpp
    src/airplay/rtsp_parser.cpp
    src/airplay/sdp.cpp
    src/airplay/dacp_remote.cpp
    src/airplay/server.cpp
    src/airplay/streams.cpp
    src/airplay/fcup.cpp
    src/airplay/hls_local_server.cpp
    src/airplay/hls_session.cpp
    src/airplay/reverse_channel.cpp
    src/airplay/routes.cpp
    src/android/android_mirror.cpp
    src/video/h264_decoder.cpp
    src/video/gpu_video.cpp
    src/video/gpu_recording.cpp
    src/video/video_recorder.cpp
    ${HLS_PLAYER_SRC}
    src/video/video_renderer.cpp
    src/platform/firewall.cpp
    src/platform/desktop_controls.cpp
    src/video/cover_art.cpp
    src/video/image_loader.cpp
    src/audio/audio_receiver.cpp
    src/audio/aac_decoder.cpp
    src/audio/audio_output.cpp
    src/usb/pair_record.cpp
    src/usb/usb_supervisor.cpp
)

target_include_directories(WinAirPlay PRIVATE src third_party)
target_link_libraries(WinAirPlay PRIVATE OpenSSL::Crypto ${FFMPEG_TARGETS} ${SDL2_TARGET} ${SDL2_TTF_TARGET} imgui)
if(_use_gst AND GSTREAMER_FOUND)
    target_link_libraries(WinAirPlay PRIVATE PkgConfig::GSTREAMER)
    target_compile_definitions(WinAirPlay PRIVATE HAVE_GSTREAMER_HLS=1)
endif()
if(DEFINED FFMPEG_INCLUDE_DIRS_OK)
    target_include_directories(WinAirPlay PRIVATE ${FFMPEG_INCLUDE_DIRS_OK})
endif()

# ---- FairPlay SAP blobs (swap-ready) ------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/fairplay_blobs_real.cpp")
    message(STATUS "FairPlay: using provisioned blob (fairplay_blobs_real.cpp)")
    target_sources(WinAirPlay PRIVATE
        third_party/fairplay_blobs_real.cpp)
else()
    message(STATUS "FairPlay: using zero-filled STUB — iOS handshake will "
                   "fail at /fp-setup until a real blob is provided")
    target_sources(WinAirPlay PRIVATE
        third_party/fairplay_blobs_stub.cpp)
endif()

# ---- playfair (level-B FairPlay decrypt, swap-ready) --------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/playfair/playfair.c")
    message(STATUS "playfair: using provisioned decryption library")
    set(_playfair_srcs
        third_party/playfair/playfair.c
        third_party/playfair/hand_garble.c
        third_party/playfair/omg_hax.c
        third_party/playfair/modified_md5.c
        third_party/playfair/sap_hash.c)
    target_sources(WinAirPlay PRIVATE ${_playfair_srcs})
    target_include_directories(WinAirPlay PRIVATE
        third_party
        third_party/playfair)
    target_compile_definitions(WinAirPlay PRIVATE HAVE_PLAYFAIR=1)

    if(MSVC)
        set_source_files_properties(${_playfair_srcs} PROPERTIES
            COMPILE_FLAGS "/wd4100 /wd4101 /wd4189 /wd4244 /wd4305 /wd4701")
    else()
        set_source_files_properties(${_playfair_srcs} PROPERTIES
            COMPILE_FLAGS "-w")
    endif()
else()
    message(STATUS "playfair: not provisioned — fairplay_decrypt() will be a no-op")
endif()

if(LIBPLIST_TARGET)
    message(STATUS "libplist: linking via ${LIBPLIST_TARGET}")
    target_link_libraries(WinAirPlay PRIVATE ${LIBPLIST_TARGET})
    target_compile_definitions(WinAirPlay PRIVATE HAVE_LIBPLIST=1)
else()
    message(FATAL_ERROR
        "libplist not found. Required for the /info binary plist — iOS will "
        "reject the handshake without it.")
endif()

if(WIN32)
    target_link_libraries(WinAirPlay PRIVATE
        ws2_32 iphlpapi dnsapi dwmapi d3d11
        setupapi shell32 ole32 oleaut32 windowsapp)

    set_target_properties(WinAirPlay PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(WinAirPlay PRIVATE assets/winairplay.rc)
    set_source_files_properties(assets/winairplay.rc PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    if(MSVC)
        target_link_options(WinAirPlay PRIVATE "/ENTRY:mainCRTStartup")
    endif()
endif()

# ---- Runtime DLL deployment (Windows) ----------------------------------------
option(BUILD_GPU_TESTS "Build the D3D11 hardware integration test" OFF)
if(WIN32 AND BUILD_GPU_TESTS)
    add_executable(test-gpu-video tools/test_gpu_video.cpp src/video/gpu_video.cpp
        src/video/h264_decoder.cpp src/video/gpu_recording.cpp
        src/video/video_recorder.cpp src/log.cpp)
    target_include_directories(test-gpu-video PRIVATE src ${FFMPEG_INCLUDE_DIRS_OK})
    target_compile_definitions(test-gpu-video PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(test-gpu-video PRIVATE ${FFMPEG_TARGETS} ${SDL2_TARGET} d3d11)
    add_custom_command(TARGET test-gpu-video POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:test-gpu-video> $<TARGET_FILE_DIR:test-gpu-video>
        COMMAND_EXPAND_LISTS)
endif()

if(WIN32)
    add_custom_command(TARGET WinAirPlay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:WinAirPlay>
            $<TARGET_FILE_DIR:WinAirPlay>
        COMMAND_EXPAND_LISTS
        COMMENT "Deploying linked runtime DLLs next to WinAirPlay.exe"
        VERBATIM)
endif()

# ---- Avahi (optional, Linux only — dev-mode mDNS) ----------------------------
if(NOT WIN32 AND PkgConfig_FOUND)
    pkg_check_modules(AVAHI_CLIENT IMPORTED_TARGET avahi-client)
    if(AVAHI_CLIENT_FOUND)
        target_link_libraries(WinAirPlay PRIVATE PkgConfig::AVAHI_CLIENT)
        target_compile_definitions(WinAirPlay PRIVATE HAVE_AVAHI=1)
        message(STATUS "Avahi: linking via PkgConfig::AVAHI_CLIENT")
    else()
        message(STATUS "Avahi: not found — mDNS disabled on this Linux build")
    endif()
endif()

# Focused regression tests for receiver disconnect and shutdown.
option(BUILD_NETWORK_TESTS "Build socket shutdown regression tests" OFF)
if(BUILD_NETWORK_TESTS)
    add_executable(test-firewall tools/test_firewall.cpp src/platform/firewall.cpp)
    target_include_directories(test-firewall PRIVATE src)
    if(WIN32)
        target_link_libraries(test-firewall PRIVATE shell32 ole32 oleaut32)
    endif()
    add_executable(test-settings tools/test_settings.cpp src/settings.cpp
        src/i18n/localization.cpp src/log.cpp)
    target_include_directories(test-settings PRIVATE src)
    if(WIN32)
        target_link_libraries(test-settings PRIVATE shell32 ole32)
    endif()
    add_executable(test-cover-art tools/test_cover_art.cpp src/video/cover_art.cpp)
    target_include_directories(test-cover-art PRIVATE src ${FFMPEG_INCLUDE_DIRS_OK})
    target_link_libraries(test-cover-art PRIVATE ${FFMPEG_TARGETS})
    add_executable(test-audio-timing tools/test_audio_timing.cpp src/airplay/ntp_client.cpp
        src/audio/audio_receiver.cpp src/audio/aac_decoder.cpp src/audio/audio_output.cpp src/net/socket.cpp)
    target_include_directories(test-audio-timing PRIVATE src ${FFMPEG_INCLUDE_DIRS_OK})
    target_link_libraries(test-audio-timing PRIVATE ${FFMPEG_TARGETS} ${SDL2_TARGET} OpenSSL::Crypto)
    if(WIN32)
        target_link_libraries(test-audio-timing PRIVATE ws2_32 iphlpapi)
    endif()
    add_executable(test-shutdown tools/test_shutdown.cpp src/airplay/dacp_remote.cpp src/airplay/rtsp_parser.cpp src/net/tcp_server.cpp
        src/net/socket.cpp src/mdns/mdns_service.cpp src/log.cpp)
    target_include_directories(test-shutdown PRIVATE src)
    if(WIN32)
        target_link_libraries(test-shutdown PRIVATE ws2_32 iphlpapi dnsapi)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(test-shutdown PRIVATE Threads::Threads)
    endif()
    enable_testing()
    add_test(NAME firewall COMMAND test-firewall)
    add_test(NAME settings COMMAND test-settings)
    add_test(NAME socket-shutdown COMMAND test-shutdown)
    add_test(NAME audio-timing COMMAND test-audio-timing)
    add_test(NAME cover-art COMMAND test-cover-art)
    set_tests_properties(audio-timing PROPERTIES TIMEOUT 20)
    set_tests_properties(socket-shutdown PROPERTIES TIMEOUT 25)
endif()

option(BUILD_STABILITY_TESTS "Build the long-session stability tests" OFF)
set(_stability_files
    tools/test_seq_dedup.cpp tools/test_audio_output.cpp
    tools/test_decoder_recovery.cpp tools/test_mirror_pipeline.cpp
    tools/test_volume_memory.cpp tools/test_link_watchdog.cpp
    tools/test_image_loader.cpp tools/image_fixtures.h tools/test_master_volume.cpp
    tools/h264_test_stream.h)
set(_stability_present TRUE)
foreach(_f IN LISTS _stability_files)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
        set(_stability_present FALSE)
    endif()
endforeach()
if(BUILD_STABILITY_TESTS AND NOT _stability_present)
    message(WARNING "BUILD_STABILITY_TESTS is ON but files in tools/ are missing; "
                    "skipping the stability tests.")
endif()
if(BUILD_STABILITY_TESTS AND _stability_present)
    add_executable(test-seq-dedup tools/test_seq_dedup.cpp)
    target_include_directories(test-seq-dedup PRIVATE src)
    add_executable(test-volume-memory tools/test_volume_memory.cpp)
    target_include_directories(test-volume-memory PRIVATE src)
    add_executable(test-link-watchdog tools/test_link_watchdog.cpp)
    target_include_directories(test-link-watchdog PRIVATE src)
    add_executable(test-master-volume tools/test_master_volume.cpp)
    target_include_directories(test-master-volume PRIVATE src)
    add_executable(test-image-loader tools/test_image_loader.cpp src/video/image_loader.cpp)
    target_include_directories(test-image-loader PRIVATE src third_party tools)
    add_executable(test-audio-output tools/test_audio_output.cpp src/audio/audio_output.cpp src/log.cpp)
    target_include_directories(test-audio-output PRIVATE src)
    target_link_libraries(test-audio-output PRIVATE ${SDL2_TARGET})
    add_executable(test-decoder-recovery tools/test_decoder_recovery.cpp
        src/video/h264_decoder.cpp src/log.cpp)
    target_include_directories(test-decoder-recovery PRIVATE src tools ${FFMPEG_INCLUDE_DIRS_OK})
    target_link_libraries(test-decoder-recovery PRIVATE ${FFMPEG_TARGETS})
    add_executable(test-mirror-pipeline tools/test_mirror_pipeline.cpp
        src/airplay/mirror_listener.cpp src/airplay/h264_nal.cpp src/airplay/h264_sps.cpp
        src/crypto/mirror_decrypt.cpp src/video/h264_decoder.cpp src/net/socket.cpp src/log.cpp)
    target_include_directories(test-mirror-pipeline PRIVATE src tools ${FFMPEG_INCLUDE_DIRS_OK})
    target_link_libraries(test-mirror-pipeline PRIVATE ${FFMPEG_TARGETS} ${SDL2_TARGET}
        ${SDL2_TTF_TARGET} OpenSSL::Crypto)
    if(WIN32)
        target_link_libraries(test-mirror-pipeline PRIVATE ws2_32 iphlpapi)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(test-decoder-recovery PRIVATE Threads::Threads)
        target_link_libraries(test-audio-output PRIVATE Threads::Threads)
        target_link_libraries(test-mirror-pipeline PRIVATE Threads::Threads)
    endif()
    enable_testing()
    add_test(NAME seq-dedup COMMAND test-seq-dedup)
    add_test(NAME volume-memory COMMAND test-volume-memory)
    add_test(NAME link-watchdog COMMAND test-link-watchdog)
    add_test(NAME master-volume COMMAND test-master-volume)
    add_test(NAME image-loader COMMAND test-image-loader)
    add_test(NAME audio-output COMMAND test-audio-output)
    add_test(NAME decoder-recovery COMMAND test-decoder-recovery)
    add_test(NAME mirror-pipeline COMMAND test-mirror-pipeline)
    set_tests_properties(decoder-recovery PROPERTIES TIMEOUT 30)
    set_tests_properties(mirror-pipeline PROPERTIES TIMEOUT 60)
endif()
