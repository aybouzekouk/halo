cmake_minimum_required(VERSION 3.21)

project(halo VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Qt needs an OpenGL::GL target. Systems that ship the OpenGL development
# package get the stock module; systems with only the runtime driver (no
# GL/gl.h, no libGL.so symlink) get the small shim in cmake/.
find_path(_halo_gl_header GL/gl.h)
find_library(_halo_gl_library GL)
if(NOT _halo_gl_header OR NOT _halo_gl_library)
    message(STATUS "halo: no OpenGL development package, using the local shim")
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
endif()

find_package(Qt6 6.6 REQUIRED COMPONENTS
    Core Gui Qml Quick QuickControls2 Network Multimedia Svg
)

qt_standard_project_setup(REQUIRES 6.6)

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(halo
    src/main.cpp
    src/AppController.cpp      src/AppController.h
    src/MistralClient.cpp      src/MistralClient.h
    src/LocalAsrClient.cpp     src/LocalAsrClient.h
    src/OllamaClient.cpp       src/OllamaClient.h
    src/PiperTts.cpp           src/PiperTts.h
    src/WsClient.cpp           src/WsClient.h
    src/AudioCapture.cpp       src/AudioCapture.h
    src/AudioPlayer.cpp        src/AudioPlayer.h
    src/SpectrumModel.cpp      src/SpectrumModel.h
    src/GlobalHotkey.cpp       src/GlobalHotkey.h
    src/WavCodec.cpp           src/WavCodec.h
)

qt_add_qml_module(halo
    URI Halo
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/SpeakingBar.qml
)

target_link_libraries(halo PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Multimedia
)

# Global Ctrl+Space on X11 is grabbed through libxcb. Optional: the app
# degrades to an in-window shortcut when the library is unavailable.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(XCB IMPORTED_TARGET xcb)
endif()
if(XCB_FOUND)
    target_link_libraries(halo PRIVATE PkgConfig::XCB)
    target_compile_definitions(halo PRIVATE HALO_HAVE_XCB=1)
    message(STATUS "halo: global hotkey enabled (xcb ${XCB_VERSION})")
else()
    message(STATUS "halo: xcb not found, global hotkey disabled")
endif()

install(TARGETS halo BUNDLE DESTINATION . RUNTIME DESTINATION bin)
