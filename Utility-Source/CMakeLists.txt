cmake_minimum_required(VERSION 3.21)
project(GlassbootInstaller VERSION 0.3.0 LANGUAGES CXX)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../boot/grub/grub.cfg" GLASSBOOT_VERSION_LINE REGEX "^set glassboot_version=")
string(REGEX REPLACE "^set glassboot_version=\"([^\"]+)\".*" "\\1" GLASSBOOT_DISPLAY_VERSION "${GLASSBOOT_VERSION_LINE}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GLASSBOOT_BUILD_GUI "Build the Qt 6 graphical installer" ON)

if(MSVC)
  # Our .rc files embed the manifest; stop the linker from adding a second one.
  add_link_options(/MANIFEST:NO)
  add_compile_options(/W4 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  endif()
endif()

# ---------------------------------------------------------------- core (no Qt)
add_library(glassboot_core STATIC
  core/Sha256.cpp
  core/Pbkdf2.cpp
  core/Config.cpp
  core/Payload.cpp
  core/Mbr.cpp
  core/Gpt.cpp
  core/ExFat.cpp
  core/DataFilesystem.cpp
  core/BlockDevice.cpp
  core/Installer.cpp
)
if(WIN32)
  target_sources(glassboot_core PRIVATE core/Disks_windows.cpp)
else()
  target_sources(glassboot_core PRIVATE core/Disks_linux.cpp)
endif()
target_include_directories(glassboot_core PUBLIC core)

# ---------------------------------------------------------------- command line
add_executable(glassboot-cli cli/main.cpp)
target_compile_definitions(glassboot-cli PRIVATE GLASSBOOT_INSTALLER_VERSION="${GLASSBOOT_DISPLAY_VERSION}")
target_link_libraries(glassboot-cli PRIVATE glassboot_core)
if(WIN32)
  target_sources(glassboot-cli PRIVATE platform/windows/cli.rc)
endif()

# ---------------------------------------------------------------- GUI
if(GLASSBOOT_BUILD_GUI)
  find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)
  qt_standard_project_setup()

  qt_add_executable(glassboot-installer WIN32
    gui/main.cpp
    gui/MainWindow.cpp gui/MainWindow.h gui/TitleBar.h gui/Theme.h gui/GlassWidgets.h
    gui/InstallWorker.cpp gui/InstallWorker.h
    gui/Elevation.cpp gui/Elevation.h
    gui/ConfigDialog.cpp gui/ConfigDialog.h
  )
  qt_add_resources(glassboot-installer gui_assets
    PREFIX "/" FILES gui/chevron.xpm)
  if(WIN32)
    # The .rc embeds a manifest that requests administrator rights (raw disk access).
    target_sources(glassboot-installer PRIVATE platform/windows/gui.rc)
  endif()
  target_link_libraries(glassboot-installer PRIVATE glassboot_core Qt6::Widgets)
  target_compile_definitions(glassboot-installer PRIVATE
    GLASSBOOT_INSTALLER_VERSION="${GLASSBOOT_DISPLAY_VERSION}")

  # Translations: translations/glassboot_<lang>.ts, compiled into the program as :/i18n/*.qm.
  # "cmake --build . --target update_translations" refreshes the .ts files from the sources.
  find_package(Qt6 QUIET COMPONENTS LinguistTools)
  file(GLOB GLASSBOOT_TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/translations/glassboot_*.ts)
  if(Qt6LinguistTools_FOUND AND GLASSBOOT_TS_FILES)
    qt_add_translations(glassboot-installer TS_FILES ${GLASSBOOT_TS_FILES} RESOURCE_PREFIX /i18n)
  elseif(GLASSBOOT_TS_FILES)
    message(STATUS "Qt Linguist tools not found: building without translations")
  endif()

  install(TARGETS glassboot-installer RUNTIME DESTINATION bin)
endif()

install(TARGETS glassboot-cli RUNTIME DESTINATION bin)

# ---------------------------------------------------------------- tests
enable_testing()
if(GLASSBOOT_BUILD_GUI)
  qt_add_executable(glassboot-gui-test
    tests/gui-test.cpp
    gui/MainWindow.cpp gui/MainWindow.h gui/TitleBar.h gui/Theme.h gui/GlassWidgets.h
    gui/InstallWorker.cpp gui/InstallWorker.h
    gui/Elevation.cpp gui/Elevation.h)
  target_include_directories(glassboot-gui-test PRIVATE gui)
  target_link_libraries(glassboot-gui-test PRIVATE glassboot_core Qt6::Widgets)
  qt_add_resources(glassboot-gui-test gui_test_assets PREFIX "/" FILES gui/chevron.xpm)
  foreach(scale IN ITEMS 1 1.5 2)
    add_test(NAME gui-scale-${scale} COMMAND glassboot-gui-test)
    set_tests_properties(gui-scale-${scale} PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale}")
    if(WIN32)
      string(REPLACE ";" "\\;" GLASSBOOT_TEST_PATH "$ENV{PATH}")
      set_property(TEST gui-scale-${scale} APPEND PROPERTY ENVIRONMENT
        "PATH=$<TARGET_FILE_DIR:Qt6::Core>\\;${GLASSBOOT_TEST_PATH}")
    endif()
  endforeach()
endif()
if(UNIX)
  enable_language(C)
  add_executable(glassboot-hook-test tests/hook-test.c)
  target_compile_options(glassboot-hook-test PRIVATE -Wno-format-truncation)
  add_test(NAME hook COMMAND glassboot-hook-test)
endif()
add_executable(glassboot-core-test tests/core-test.cpp)
target_link_libraries(glassboot-core-test PRIVATE glassboot_core)
add_test(NAME core COMMAND glassboot-core-test)

add_executable(glassboot-layout-test tests/layout-test.cpp)
target_link_libraries(glassboot-layout-test PRIVATE glassboot_core)
add_test(NAME layout COMMAND glassboot-layout-test)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME release-tools COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/release-tools-test.py")
  add_test(NAME translations COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check-translations.py")
endif()
