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
  # Build xtils with PIC so it can be linked into vibetype.so (shared library)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
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

# ── Version from git tag (strip leading 'v' from e.g. v0.1.2) ──
execute_process(
  COMMAND git describe --tags --abbrev=0
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_TAG
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(GIT_TAG MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+$")
  string(SUBSTRING "${GIT_TAG}" 1 -1 CPACK_PACKAGE_VERSION)
  message(STATUS "Version from git tag: ${CPACK_PACKAGE_VERSION}")
else()
  set(CPACK_PACKAGE_VERSION "0.0.0-dev")
  message(WARNING "No valid git tag found; using ${CPACK_PACKAGE_VERSION}")
endif()

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

# Package variants:
# - cli: standalone backend + CLI package.
# - ibus: standalone backend + CLI + IBus package.
# - fcitx5: standalone backend + CLI + Fcitx5 package.
function(vibetype_install_core component_name)
  install(TARGETS vibetype-backend
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          COMPONENT ${component_name})
  install(PROGRAMS frontend/cli/vibetype_cli.py
          DESTINATION ${CMAKE_INSTALL_BINDIR}
          RENAME vibetype-cli
          COMPONENT ${component_name})
  install(FILES frontend/common/vibetype_client.py
          DESTINATION ${CMAKE_INSTALL_DATADIR}/vibetype/python
          COMPONENT ${component_name})
  install(FILES packaging/systemd/vibetype-backend.service
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user
          COMPONENT ${component_name})
  install(FILES
            README.md
            README.en.md
            docs/cli.md
            docs/install.md
            docs/vibetype-spec.md
          DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/vibetype
          COMPONENT ${component_name})
endfunction()

vibetype_install_core(cli)
vibetype_install_core(ibus)
vibetype_install_core(fcitx5)

install(PROGRAMS frontend/ibus/vibetype_ibus.py
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME vibetype-ibus
        COMPONENT ibus)
configure_file(packaging/ibus/vibetype.xml.in
               ${CMAKE_CURRENT_BINARY_DIR}/packaging/ibus/vibetype.xml)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/packaging/ibus/vibetype.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/ibus/component
        COMPONENT ibus)
install(FILES docs/ibus-frontend.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/vibetype
        COMPONENT ibus)

install(FILES docs/fcitx5-frontend.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/vibetype
        COMPONENT fcitx5)

set(CPACK_PACKAGE_NAME "vibetype")
set(CPACK_PACKAGE_VENDOR "Vibetype")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Linux voice input method")
set(CPACK_PACKAGE_CONTACT "Vibetype Maintainers")
set(CPACK_GENERATOR "TGZ;DEB")
find_program(HAVE_RPMBUILD rpmbuild)
if(HAVE_RPMBUILD)
  list(APPEND CPACK_GENERATOR "RPM")
endif()
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")

set(CPACK_COMPONENTS_ALL cli ibus fcitx5)
set(CPACK_COMPONENT_CLI_DISPLAY_NAME "Vibetype CLI")
set(CPACK_COMPONENT_CLI_DESCRIPTION "Local offline voice input for Linux. ASR model (SenseVoice GGUF ~200MB) auto-downloads on first run.")
set(CPACK_COMPONENT_IBUS_DISPLAY_NAME "Vibetype IBus")
set(CPACK_COMPONENT_IBUS_DESCRIPTION "Local offline voice input for Linux (IBus frontend). ASR model (SenseVoice GGUF ~200MB) auto-downloads on first run.")
set(CPACK_COMPONENT_FCITX5_DISPLAY_NAME "Vibetype Fcitx5")
set(CPACK_COMPONENT_FCITX5_DESCRIPTION "Local offline voice input for Linux (Fcitx5 frontend). ASR model (SenseVoice GGUF ~200MB) auto-downloads on first run.")
set(CPACK_COMPONENTS_GROUPING IGNORE)

set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_ARCHIVE_CLI_FILE_NAME "vibetype-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_ARCHIVE_IBUS_FILE_NAME "vibetype-ibus-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_ARCHIVE_FCITX5_FILE_NAME "vibetype-fcitx5-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")

set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS OFF)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Vibetype Maintainers")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
endif()
set(CPACK_DEBIAN_CLI_PACKAGE_NAME "vibetype")
set(CPACK_DEBIAN_CLI_PACKAGE_DESCRIPTION "Local offline voice input for Linux. ASR model (SenseVoice GGUF ~200MB) auto-downloads on first run.")
set(CPACK_DEBIAN_CLI_PACKAGE_DEPENDS "curl, python3, alsa-utils, xclip")
set(CPACK_DEBIAN_CLI_PACKAGE_CONFLICTS "vibetype-ibus, vibetype-fcitx5")
set(CPACK_DEBIAN_IBUS_PACKAGE_NAME "vibetype-ibus")
set(CPACK_DEBIAN_IBUS_PACKAGE_DESCRIPTION "Local offline voice input for Linux (IBus frontend). ASR model (SenseVoice GGUF ~200MB) auto-downloads on first run.")
set(CPACK_DEBIAN_IBUS_PACKAGE_DEPENDS "curl, python3, alsa-utils, xclip, python3-gi, gir1.2-ibus-1.0, ibus")
set(CPACK_DEBIAN_IBUS_PACKAGE_CONFLICTS "vibetype, vibetype-fcitx5")
set(CPACK_DEBIAN_FCITX5_PACKAGE_NAME "vibetype-fcitx5")
set(CPACK_DEBIAN_FCITX5_PACKAGE_DESCRIPTION "Local offline voice input for Linux (Fcitx5 frontend). ASR model (SenseVoice GGUF ~200MB) auto-downloads on first run.")
set(CPACK_DEBIAN_FCITX5_PACKAGE_DEPENDS "curl, alsa-utils, xclip, fcitx5")
set(CPACK_DEBIAN_FCITX5_PACKAGE_CONFLICTS "vibetype, vibetype-ibus")


if(HAVE_RPMBUILD)
  set(CPACK_RPM_COMPONENT_INSTALL ON)
  set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
  set(CPACK_RPM_PACKAGE_LICENSE "MIT")
  set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
  set(CPACK_RPM_PACKAGE_URL "https://github.com/lingzolabs/vibetype")
  set(CPACK_RPM_CLI_PACKAGE_NAME "vibetype")
  set(CPACK_RPM_CLI_PACKAGE_DESCRIPTION "Local offline voice input for Linux. ASR model (SenseVoice GGUF ~200MB) auto-downloads on first run.")
  set(CPACK_RPM_CLI_PACKAGE_REQUIRES "curl, python3, alsa-utils, xclip")
  set(CPACK_RPM_CLI_PACKAGE_CONFLICTS "vibetype-ibus, vibetype-fcitx5")
  set(CPACK_RPM_IBUS_PACKAGE_NAME "vibetype-ibus")
  set(CPACK_RPM_IBUS_PACKAGE_DESCRIPTION "Local offline voice input for Linux (IBus frontend). ASR model (SenseVoice GGUF ~200MB) auto-downloads on first run.")
  set(CPACK_RPM_IBUS_PACKAGE_REQUIRES "curl, python3, alsa-utils, xclip, python3-gobject, ibus")
  set(CPACK_RPM_IBUS_PACKAGE_CONFLICTS "vibetype, vibetype-fcitx5")
  set(CPACK_RPM_FCITX5_PACKAGE_NAME "vibetype-fcitx5")
  set(CPACK_RPM_FCITX5_PACKAGE_DESCRIPTION "Local offline voice input for Linux (Fcitx5 frontend). ASR model (SenseVoice GGUF ~200MB) auto-downloads on first run.")
  set(CPACK_RPM_FCITX5_PACKAGE_REQUIRES "curl, alsa-utils, xclip, fcitx5")
  set(CPACK_RPM_FCITX5_PACKAGE_CONFLICTS "vibetype, vibetype-ibus")
endif()
include(CPack)
