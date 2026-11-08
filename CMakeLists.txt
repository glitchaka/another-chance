cmake_minimum_required(VERSION 3.21)
project(AnotherChance VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Another Chance 0.1 requiere Windows (Win32, WinHTTP y GDI).")
endif()

add_executable(AnotherChance WIN32
    src/main.cpp
    src/util/Url.cpp
    src/net/HttpClient.cpp
    src/html/HtmlParser.cpp
    src/style/Style.cpp
    src/layout/Layout.cpp
    src/window/BrowserWindow.cpp
)

target_compile_features(AnotherChance PRIVATE cxx_std_20)
target_compile_definitions(AnotherChance PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(AnotherChance PRIVATE src)
target_link_libraries(AnotherChance PRIVATE winhttp shlwapi comctl32 gdi32 user32)
if(MSVC)
    target_compile_options(AnotherChance PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()
