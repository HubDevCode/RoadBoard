cmake_minimum_required(VERSION 3.20)
project(roadboard VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Xml)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

set(SOURCES
  src/core/Params.cpp
  src/core/RemoteScript.cpp
  src/core/Settings.cpp
  src/engine/Mixer.cpp
  src/engine/Engine.cpp
  src/engine/TapeDeck.cpp
  src/engine/RoadLink.cpp
  src/engine/Midi.cpp
  src/engine/RemoteServer.cpp
  src/gui/Widgets.cpp
  src/gui/StripWidgets.cpp
  src/gui/MainWindow.cpp
  src/gui/Dialogs.cpp
  src/gui/EqDialog.cpp
  src/gui/MacroButtons.cpp
  src/gui/Detail.cpp
  src/gui/FxWindow.cpp
  src/gui/AppMixer.cpp
  src/gui/StreamerView.cpp
  src/gui/RecorderBar.cpp
  src/app/main.cpp
  resources/roadboard.qrc
)

add_executable(roadboard ${SOURCES})
target_include_directories(roadboard PRIVATE src)
target_compile_definitions(roadboard PRIVATE ROADBOARD_VERSION="${PROJECT_VERSION}")
target_compile_options(roadboard PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_link_libraries(roadboard PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Xml
  PkgConfig::PIPEWIRE PkgConfig::SNDFILE PkgConfig::SAMPLERATE PkgConfig::ALSA
  pthread)

include(GNUInstallDirs)
install(TARGETS roadboard RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/roadboard.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications RENAME io.github.roadboard.RoadBoard.desktop)
install(FILES resources/icons/roadboard.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME io.github.roadboard.RoadBoard.svg)
install(FILES packaging/io.github.roadboard.RoadBoard.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/roadboard)

# ------------------------------------------------------------------ packages (.deb / .rpm)
set(CPACK_PACKAGE_NAME "roadboard")
set(CPACK_PACKAGE_VENDOR "RoadBoard")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Virtual audio mixer for PipeWire (8 inputs, 8 buses)")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CONTACT "HubDevCode <128942387+HubDevCode@users.noreply.github.com>")
set(CPACK_GENERATOR "TGZ")
find_program(DPKG_CMD dpkg)
find_program(RPMBUILD_CMD rpmbuild)
if(DPKG_CMD)
  list(APPEND CPACK_GENERATOR "DEB")
endif()
if(RPMBUILD_CMD)
  list(APPEND CPACK_GENERATOR "RPM")
endif()
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "pipewire, wireplumber")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "ffmpeg, qpwgraph, pavucontrol")
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_RPM_PACKAGE_REQUIRES "pipewire, wireplumber")
set(CPACK_RPM_PACKAGE_SUGGESTS "ffmpeg")
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
include(CPack)
