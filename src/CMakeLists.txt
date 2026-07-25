# Core backend libraries. Each is a static library named ch_<module>.
# They are deliberately thin at bootstrap; see docs/PLAN.md for the workstream
# that fills each one in.
add_subdirectory(models)
add_subdirectory(persistence)
add_subdirectory(ssh)
add_subdirectory(terminal)
add_subdirectory(viewers)
add_subdirectory(remote)
add_subdirectory(editor)
add_subdirectory(agent)

# QML UI module (src/qml) and the application executable that links everything.
add_subdirectory(qml)
add_subdirectory(app)
