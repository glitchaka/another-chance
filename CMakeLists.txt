cmake_minimum_required(VERSION 3.21)
project(AnotherChance VERSION 0.1.0 LANGUAGES CXX)
# Bundle MSVC CRT so the Release EXE runs without installing VC++ Redistributable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT WIN32)
    message(FATAL_ERROR "Another Chance 0.1 requiere Windows (Win32, WinHTTP y GDI).")
endif()

add_executable(AnotherChance WIN32
    src/main.cpp
    src/util/Url.cpp
    src/net/HttpClient.cpp
    src/image/ImageStore.cpp
    src/html/HtmlParser.cpp
    src/style/Style.cpp
    src/layout/Layout.cpp
    src/window/BrowserWindow.cpp
)

target_compile_features(AnotherChance PRIVATE cxx_std_20)
target_compile_definitions(AnotherChance PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(AnotherChance PRIVATE src)
target_link_libraries(AnotherChance PRIVATE winhttp shlwapi comctl32 gdi32 user32 windowscodecs ole32 msimg32)
if(MSVC)
    target_compile_options(AnotherChance PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(AnotherChanceTests
        tests/EngineTests.cpp
        src/html/HtmlParser.cpp
        src/style/Style.cpp
        src/util/Url.cpp
    )
    target_compile_features(AnotherChanceTests PRIVATE cxx_std_20)
    target_compile_definitions(AnotherChanceTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(AnotherChanceTests PRIVATE src)
    target_link_libraries(AnotherChanceTests PRIVATE shlwapi gdi32 user32)
    if(MSVC)
        target_compile_options(AnotherChanceTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
    add_test(NAME CoreHtmlCssUrl COMMAND AnotherChanceTests)
endif()

if(BUILD_TESTING)
    add_executable(AnotherChanceSmoke tests/BrowserSmoke.cpp)
    target_compile_features(AnotherChanceSmoke PRIVATE cxx_std_20)
    target_compile_definitions(AnotherChanceSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(AnotherChanceSmoke PRIVATE ws2_32 user32)
    if(MSVC)
        target_compile_options(AnotherChanceSmoke PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
    add_test(NAME BrowserEndToEnd COMMAND AnotherChanceSmoke "$<TARGET_FILE:AnotherChance>")
    set_tests_properties(BrowserEndToEnd PROPERTIES TIMEOUT 110)
endif()
