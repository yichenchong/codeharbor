# Core backend libraries. Each is a static library named ch_<module>. Every one
# of them is portable: Qt Core/Gui/Network/Quick only, no WebEngine anywhere, so
# both the desktop client and the mobile client link the same code. They are
# unconditional for exactly that reason - nothing below them is per-platform.
add_subdirectory(models)
add_subdirectory(persistence)
add_subdirectory(ssh)
add_subdirectory(terminal)
# viewers is added unconditionally, but it is two targets: ch_viewers_core
# (Qt Core only, the URL/MIME classification both clients need) always, and the
# WebEngine-linked ch_viewers only for a desktop build. The split lives in
# src/viewers/CMakeLists.txt so no include path anywhere had to change.
add_subdirectory(viewers)
add_subdirectory(remote)
add_subdirectory(editor)
add_subdirectory(agent)

# Desktop presentation layer: the QML UI module (src/qml, which embeds the
# Monaco/xterm.js/Markdown web bundles) and the `codeharbor` executable.
if(CODEHARBOR_BUILD_DESKTOP)
    add_subdirectory(qml)
endif()

# Mobile presentation layer: the in-process terminal emulator (src/vt, which
# replaces the xterm.js page) and the single-pane Quick shell (src/mobile).
# Added AFTER editor/agent because ch_mobile links ch_app, ch_editor and
# ch_agent; CMake resolves target references at generate time, so the order is
# not strictly required, but keeping the graph in dependency order is what makes
# a link error legible.
if(CODEHARBOR_BUILD_MOBILE)
    add_subdirectory(vt)
    add_subdirectory(mobile)
endif()

# src/app holds ch_app (portable, linked by both clients) and, behind
# CODEHARBOR_BUILD_DESKTOP, the desktop `codeharbor` executable. It is added
# last because the desktop executable links codeharbor_qmlplugin from src/qml.
add_subdirectory(app)
