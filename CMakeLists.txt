cmake_minimum_required(VERSION 3.16)
project(vibetype LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(FetchContent)

option(VIBETYPE_USE_SYSTEM_XTILS "Use installed xtils package instead of fetching it" OFF)
set(XTILS_GIT_REPOSITORY "https://github.com/lingzolabs/xtils.git" CACHE STRING "xtils Git repository")
set(XTILS_GIT_TAG "master" CACHE STRING "xtils Git tag, branch, or commit")

if(VIBETYPE_USE_SYSTEM_XTILS)
  find_package(xtils REQUIRED)
  set(VIBETYPE_XTILS_TARGET xtils::xtils)
else()
  FetchContent_Declare(xtils
    GIT_REPOSITORY ${XTILS_GIT_REPOSITORY}
    GIT_TAG ${XTILS_GIT_TAG}
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(xtils)
  set(VIBETYPE_XTILS_TARGET xtils)
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_CPP_VERSION "b9859" CACHE STRING "llama.cpp source release tag")
set(LLAMA_CPP_SOURCE_URL "https://github.com/ggml-org/llama.cpp/archive/refs/tags/${LLAMA_CPP_VERSION}.zip" CACHE STRING "llama.cpp source zip URL")
set(LLAMA_CPP_SOURCE_SHA256 "b4dc3cbd2e38091a71962c80eb4d553524f30cdca145c3f7bbb411b17b078d21" CACHE STRING "llama.cpp source zip SHA256")
FetchContent_Declare(llama
  URL ${LLAMA_CPP_SOURCE_URL}
  URL_HASH SHA256=${LLAMA_CPP_SOURCE_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(llama)
if(NOT llama_POPULATED)
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(llama)
  add_subdirectory("${llama_SOURCE_DIR}/ggml" "${llama_BINARY_DIR}/ggml" EXCLUDE_FROM_ALL)
endif()

add_executable(vibetype-backend
  src/backend/main.cc
  src/backend/sensevoice_engine.cc)
target_include_directories(vibetype-backend PRIVATE
  src/backend)
target_compile_definitions(vibetype-backend PRIVATE LOG_TAG_STRING="vibetype-backend")
target_link_libraries(vibetype-backend PRIVATE ${VIBETYPE_XTILS_TARGET} ggml)

include(GNUInstallDirs)

# ── Optional Fcitx5 addon ──────────────────────────────────────────
option(BUILD_FCITX5_ADDON "Build the Fcitx5 frontend addon" ON)
if(BUILD_FCITX5_ADDON)
  find_package(Fcitx5Core QUIET)
  if(Fcitx5Core_FOUND)
    add_subdirectory(frontend/fcitx5)
  else()
    message(STATUS "Fcitx5Core not found — skipping Fcitx5 addon build")
  endif()
endif()

# Package split:
# - cli: backend + CLI + shared Python client + user systemd service + docs.
# - ibus: IBus frontend add-on; install together with cli.
# - fcitx5: Fcitx5 frontend add-on; install together with cli.
install(TARGETS vibetype-backend
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT cli)
install(PROGRAMS frontend/cli/vibetype_cli.py
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME vibetype-cli
        COMPONENT cli)
install(FILES frontend/common/vibetype_client.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/vibetype/python
        COMPONENT cli)
install(FILES packaging/systemd/vibetype-backend.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user
        COMPONENT cli)
install(FILES
          README.md
          README_zh.md
          docs/cli.md
          docs/install.md
          docs/vibetype-spec.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/vibetype
        COMPONENT cli)

install(PROGRAMS frontend/ibus/vibetype_ibus.py
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME vibetype-ibus
        COMPONENT ibus)
install(FILES packaging/ibus/vibetype.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/ibus/component
        COMPONENT ibus)
install(FILES docs/ibus-frontend.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/vibetype
        COMPONENT ibus)

install(PROGRAMS frontend/fcitx5/vibetype_fcitx5_helper.py
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME vibetype-fcitx5-helper
        COMPONENT fcitx5)
install(FILES docs/fcitx5-frontend.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/vibetype
        COMPONENT fcitx5)

set(CPACK_PACKAGE_NAME "vibetype")
set(CPACK_PACKAGE_VENDOR "Vibetype")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Linux voice input method")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_PACKAGE_CONTACT "Vibetype Maintainers")
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")

set(CPACK_COMPONENTS_ALL cli ibus fcitx5)
set(CPACK_COMPONENT_CLI_DISPLAY_NAME "Vibetype CLI")
set(CPACK_COMPONENT_CLI_DESCRIPTION "Vibetype backend, CLI frontend, shared Python code, systemd user service, and documentation")
set(CPACK_COMPONENT_IBUS_DISPLAY_NAME "Vibetype IBus frontend")
set(CPACK_COMPONENT_IBUS_DESCRIPTION "IBus frontend add-on for Vibetype; install together with vibetype-cli")
set(CPACK_COMPONENT_IBUS_DEPENDS cli)
set(CPACK_COMPONENT_FCITX5_DISPLAY_NAME "Vibetype Fcitx5 frontend")
set(CPACK_COMPONENT_FCITX5_DESCRIPTION "Fcitx5 frontend add-on for Vibetype; install together with vibetype-cli")
set(CPACK_COMPONENT_FCITX5_DEPENDS cli)
set(CPACK_COMPONENTS_GROUPING IGNORE)

set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_ARCHIVE_CLI_FILE_NAME "vibetype-cli-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_ARCHIVE_IBUS_FILE_NAME "vibetype-ibus-addon-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_ARCHIVE_FCITX5_FILE_NAME "vibetype-fcitx5-addon-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")

set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS ON)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Vibetype Maintainers")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
endif()
set(CPACK_DEBIAN_CLI_PACKAGE_NAME "vibetype-cli")
set(CPACK_DEBIAN_CLI_PACKAGE_DESCRIPTION "Vibetype backend and CLI frontend")
set(CPACK_DEBIAN_CLI_PACKAGE_DEPENDS "curl, python3, alsa-utils, wl-clipboard, xclip, xdotool")
set(CPACK_DEBIAN_IBUS_PACKAGE_NAME "vibetype-ibus")
set(CPACK_DEBIAN_IBUS_PACKAGE_DESCRIPTION "Vibetype IBus frontend add-on")
set(CPACK_DEBIAN_IBUS_PACKAGE_DEPENDS "vibetype-cli (= ${CPACK_PACKAGE_VERSION}), python3-gi, gir1.2-ibus-1.0, ibus")
set(CPACK_DEBIAN_FCITX5_PACKAGE_NAME "vibetype-fcitx5")
set(CPACK_DEBIAN_FCITX5_PACKAGE_DESCRIPTION "Vibetype Fcitx5 frontend add-on")
set(CPACK_DEBIAN_FCITX5_PACKAGE_DEPENDS "vibetype-cli (= ${CPACK_PACKAGE_VERSION}), fcitx5, python3")
set(CPACK_DEBIAN_FCITX5_PACKAGE_SUGGESTS "python3-gi")
include(CPack)
