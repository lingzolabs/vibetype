# ── Fcitx5 addon: vibetype.so ───────────────────────────────────────

find_package(Fcitx5Core REQUIRED)
include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

add_library(vibetype SHARED vibetype.cpp)
target_link_libraries(vibetype PRIVATE Fcitx5::Core ${VIBETYPE_XTILS_TARGET})
set_target_properties(vibetype PROPERTIES
  PREFIX ""
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON)

set(VIBETYPE_FCITX5_LIBDIR "${CMAKE_INSTALL_LIBDIR}/fcitx5")
set(VIBETYPE_FCITX5_ADDONDIR "${CMAKE_INSTALL_DATADIR}/fcitx5/addon")
set(VIBETYPE_FCITX5_INPUTMETHODDIR "${CMAKE_INSTALL_DATADIR}/fcitx5/inputmethod")

install(TARGETS vibetype
        DESTINATION "${VIBETYPE_FCITX5_LIBDIR}"
        COMPONENT fcitx5)

# Addon registration (processed for @PROJECT_VERSION@)
configure_file(vibetype.conf.in vibetype.conf)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vibetype.conf"
        DESTINATION "${VIBETYPE_FCITX5_ADDONDIR}"
        COMPONENT fcitx5)

# Input method registration
install(FILES vibetype-inputmethod.conf
        DESTINATION "${VIBETYPE_FCITX5_INPUTMETHODDIR}"
        COMPONENT fcitx5)
