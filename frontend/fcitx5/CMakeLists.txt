# ── Fcitx5 addon: vibetype.so ───────────────────────────────────────

find_package(Fcitx5Core REQUIRED)
include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

add_library(vibetype SHARED vibetype.cpp)
target_link_libraries(vibetype PRIVATE Fcitx5::Core)
set_target_properties(vibetype PROPERTIES
  PREFIX ""
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)

install(TARGETS vibetype
        DESTINATION "${FCITX_INSTALL_LIBDIR}/fcitx5"
        COMPONENT fcitx5)

# Addon registration (processed for @PROJECT_VERSION@)
configure_file(vibetype.conf.in vibetype.conf)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vibetype.conf"
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
        COMPONENT fcitx5)

# Input method registration
install(FILES vibetype-inputmethod.conf
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/inputmethod"
        COMPONENT fcitx5)

# Python helper script
install(PROGRAMS vibetype_fcitx5_helper.py
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        RENAME vibetype-fcitx5-helper
        COMPONENT fcitx5)
