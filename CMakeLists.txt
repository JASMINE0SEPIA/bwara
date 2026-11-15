cmake_minimum_required(VERSION 3.22)
project(BwAra VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The first BwARA build targets Windows x64.")
endif()

set(ARA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.deps/ARA_SDK" CACHE PATH "ARA SDK 2.3.0")
set(ARA_VST3_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/vst3sdk" CACHE PATH "VST3 SDK 3.8.1")
if(NOT EXISTS "${ARA_ROOT}/ARA_Examples/CMakeLists.txt" OR NOT EXISTS "${ARA_VST3_SDK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Fetch the dependencies with scripts/setup.ps1.")
endif()

set(ARA_SETUP_DEBUGGING OFF CACHE BOOL "Do not install the SDK examples into the system" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${ARA_ROOT}/ARA_Examples" sdk EXCLUDE_FROM_ALL)

# 借 SDK 的文档模型和回调，VST3 实例由我们管理，避免再养一整套宿主框架。
get_target_property(ara_host_sources ARATestHost SOURCES)
list(FILTER ara_host_sources EXCLUDE REGEX "/(main|TestCases)\\.cpp$|\\.rc$")

# 示例原本只在同步分析时轮询更新；编辑窗口需要持续收消息，但不能等分析把 UI 卡住。
# 只改构建目录里的副本。锚点失踪就报错，别让升级 SDK 变成“编过了但没改到”。
function(ara_replace variable before after)
    string(FIND "${${variable}}" "${before}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "ARA SDK adaptation no longer matches: ${variable}")
    endif()
    string(REPLACE "${before}" "${after}" replaced "${${variable}}")
    set(${variable} "${replaced}" PARENT_SCOPE)
endfunction()

set(ara_host_dir "${CMAKE_CURRENT_BINARY_DIR}/ara-host-source")
file(COPY "${ARA_ROOT}/ARA_Examples/TestHost/" DESTINATION "${ara_host_dir}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ARA_ROOT}/ARA_Examples/TestHost/ARADocumentController.h"
    "${ARA_ROOT}/ARA_Examples/TestHost/ARADocumentController.cpp"
    "${ARA_ROOT}/ARA_Examples/TestHost/ARAHostInterfaces/ARAModelUpdateController.cpp")
file(READ "${ara_host_dir}/ARADocumentController.h" ara_doc_header)
ara_replace(ara_doc_header "    void setMinimalContentUpdateLogging"
    "    void pollModelUpdates();\n    unsigned long long playbackRevision = 0;\n    bool initialAnalysis = true;\n    bool sourceChanged = false;\n    bool playbackChanged = false;\n    void setMinimalContentUpdateLogging"
)
file(WRITE "${ara_host_dir}/ARADocumentController.h" "${ara_doc_header}")
file(APPEND "${ara_host_dir}/ARADocumentController.cpp" [=[
void ARADocumentController::pollModelUpdates()
{
    sourceChanged = false;
    playbackChanged = false;
#if ARA_VALIDATE_API_CALLS
    _isPollingModelUpdates = true;
#endif
    _documentController->notifyModelUpdates();
    // 首次分析也会汇报变化，别把“认出音符了”误记成用户改音。
    if (playbackChanged && !(initialAnalysis && sourceChanged))
        ++playbackRevision;
    if (sourceChanged || playbackChanged)
        initialAnalysis = false;
#if ARA_VALIDATE_API_CALLS
    _isPollingModelUpdates = false;
#endif
}
]=])
set(ara_updates "${ara_host_dir}/ARAHostInterfaces/ARAModelUpdateController.cpp")
file(READ "${ara_updates}" ara_update_code)
ara_replace(ara_update_code "    const auto audioSource = fromHostRef (audioSourceHostRef);\n    ARA_VALIDATE_API_ARGUMENT (audioSource, ARA::contains (getDocument ()->getAudioSources (), audioSource));\n    ARA_VALIDATE_API_ARGUMENT (range"
               "    _araDocumentController->sourceChanged = true;\n    const auto audioSource = fromHostRef (audioSourceHostRef);\n    ARA_VALIDATE_API_ARGUMENT (audioSource, ARA::contains (getDocument ()->getAudioSources (), audioSource));\n    ARA_VALIDATE_API_ARGUMENT (range"
)
ara_replace(ara_update_code "    const auto playbackRegion = fromHostRef (playbackRegionHostRef);"
               "    _araDocumentController->playbackChanged |= scopeFlags.affectSamples();\n    const auto playbackRegion = fromHostRef (playbackRegionHostRef);"
)
file(WRITE "${ara_updates}" "${ara_update_code}")
set(ara_playback "${ara_host_dir}/ARAHostInterfaces/ARAPlaybackController.cpp")
file(READ "${ara_playback}" ara_playback_code)
foreach(command start stop seek)
    if(command STREQUAL "start")
        set(pattern [=[ARA_WARN ("requestStartPlayback () not implemented.");]=])
        set(value 0)
    elseif(command STREQUAL "stop")
        set(pattern [=[ARA_WARN ("requestStopPlayback () not implemented");]=])
        set(value 0)
    else()
        set(pattern [=[ARA_WARN ("requestSetPlaybackPosition () not implemented, requested time is %.2f", timePosition);]=])
        set(value timePosition)
    endif()
    ara_replace(ara_playback_code "${pattern}" "bwaraTransport(\"${command}\", ${value});")
endforeach()
file(WRITE "${ara_playback}" "void bwaraTransport(const char*, double);\n${ara_playback_code}")
list(TRANSFORM ara_host_sources REPLACE "^.*/TestHost/" "${ara_host_dir}/")

add_executable(bwara-host
    native/main.cpp
    native/editor.cpp
    native/audition.cpp
    native/session.cpp
    native/revision_file.cpp
    native/wave_pcm.cpp
    ${ara_host_sources}
    "${ARA_ROOT}/ARA_Examples/ExamplesCommon/Archives/Archives.cpp"
    "${ARA_ROOT}/ARA_Examples/ExamplesCommon/AudioFiles/AudioFiles.cpp"
    "${ARA_ROOT}/ARA_Examples/ExamplesCommon/SignalProcessing/PulsedSineSignal.c"
    "${ARA_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp"
    "${ARA_VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp"
)
target_include_directories(bwara-host PRIVATE "${ARA_ROOT}/ARA_Examples" "${ara_host_dir}")
target_compile_definitions(bwara-host PRIVATE ARA_ENABLE_VST3=0 ARA_ENABLE_CLAP=0)
target_link_libraries(bwara-host PRIVATE ICST_AudioFile cpp-base64 pugixml ARA_Host_Library sdk_hosting ole32 avrt)

set(MINHOOK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.deps/minhook" CACHE PATH "MinHook v1.3.4")
if(NOT EXISTS "${MINHOOK_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "Fetch MinHook with scripts/setup.ps1.")
endif()
add_subdirectory("${MINHOOK_ROOT}" minhook EXCLUDE_FROM_ALL)
add_library(bwara-engine SHARED native/engine.cpp native/wave_pcm.cpp)
target_link_libraries(bwara-engine PRIVATE minhook)
add_executable(bwara-engine-loader native/engine_loader.cpp)
target_link_libraries(bwara-engine-loader PRIVATE bcrypt)
set_target_properties(bwara-host bwara-engine bwara-engine-loader PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
include(CTest)
if(BUILD_TESTING)
    add_executable(bwara-core-test tests/native.cpp native/wave_pcm.cpp native/revision_file.cpp)
    target_include_directories(bwara-core-test PRIVATE native)
    add_test(NAME bwara-core COMMAND bwara-core-test)
endif()

# 注释用中文，编码也要说清楚；不能靠开发机器的系统代码页碰运气。
foreach(target bwara-host bwara-engine bwara-engine-loader bwara-core-test)
    if(TARGET ${target})
        target_compile_features(${target} PRIVATE cxx_std_17)
        target_compile_definitions(${target} PRIVATE NOMINMAX)
        if(MSVC)
            target_compile_options(${target} PRIVATE /utf-8)
        endif()
    endif()
endforeach()
