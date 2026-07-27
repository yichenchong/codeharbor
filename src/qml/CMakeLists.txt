# QML module for the CodeHarbor UI, kept as a sibling of app/ per the proposed
# architecture (SPEC 14). Built as a static QML module library that the app
# executable links against.
qt_add_library(codeharbor_qml STATIC)

qt_add_qml_module(codeharbor_qml
    URI CodeHarbor
    VERSION 1.0
    # Output dir must end in the module target path (URI) so qmllint/qmlls
    # tooling resolves the module correctly.
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/CodeHarbor
    QML_FILES
        Main.qml
        SessionsSidebar.qml
        GroupHeader.qml
        SessionRow.qml
        TerminalRegion.qml
        TerminalPaneView.qml
        ViewerRegion.qml
        ViewerPane.qml
        ViewerWebView.qml
        ViewerTextView.qml
        ViewerImageView.qml
        ViewerPdfView.qml
        ViewerDirectoryView.qml
        ViewerBinaryView.qml
        EditorPaneView.qml
        CommandPalette.qml
        ConnectSheet.qml
)

target_link_libraries(codeharbor_qml PUBLIC
    Qt6::Quick
    Qt6::QuickControls2
)

# Trusted Monaco editor page (src/web/editor), embedded as Qt resources so
# EditorPaneView.qml can navigate to ONE constant URL —
# qrc:/codeharbor/web/editor/index.html — that resolves identically in the build
# tree, in a relocated install, and inside a macOS bundle. No filesystem path
# ever has to be plumbed into QML.
#
# The bundle is an esbuild BUILD ARTIFACT (~4 MB) and is deliberately gitignored,
# so a fresh clone has none. Qt reads resource files at CONFIGURE time, so a
# missing bundle used to configure "successfully" and silently ship an editor pane
# with nothing to load. Instead we build it on demand here, and refuse to
# configure if we cannot: a broken editor must never be the quiet default.
#
# Escape hatch for C++-only/offline work: -DCODEHARBOR_SKIP_WEB_BUNDLE=ON, which
# downgrades this to a warning and yields a client whose editor pane is inert.
set(_ch_editor_src ${CMAKE_CURRENT_SOURCE_DIR}/../web/editor)
set(_ch_editor_dist ${_ch_editor_src}/dist)

# Inputs the bundle is built FROM. Tracking these (not just the output) is what
# makes the embedded copy honest: editing the editor sources re-triggers configure,
# which rebuilds a stale bundle before it is embedded.
file(GLOB_RECURSE _ch_editor_inputs ${_ch_editor_src}/src/*.ts)
list(APPEND _ch_editor_inputs
     ${_ch_editor_src}/build.mjs
     ${_ch_editor_src}/index.html
     ${_ch_editor_src}/package.json)

set(_ch_bundle_stale FALSE)
if(NOT EXISTS ${_ch_editor_dist}/index.html)
    set(_ch_bundle_stale TRUE)
else()
    foreach(_ch_in IN LISTS _ch_editor_inputs)
        if(${_ch_in} IS_NEWER_THAN ${_ch_editor_dist}/index.html)
            set(_ch_bundle_stale TRUE)
        endif()
    endforeach()
endif()

if(_ch_bundle_stale AND NOT CODEHARBOR_SKIP_WEB_BUNDLE)
    # npm.cmd FIRST: on Windows find_program only auto-appends .com/.exe, so a
    # bare `npm` resolves the extensionless shell script, which execute_process
    # cannot launch - and the failure surfaces as an EMPTY error string.
    find_program(CODEHARBOR_NPM NAMES npm.cmd npm)
    if(CODEHARBOR_NPM)
        if(NOT EXISTS ${CMAKE_SOURCE_DIR}/node_modules)
            message(STATUS "CodeHarbor: installing npm workspaces (first configure)")
            execute_process(COMMAND ${CODEHARBOR_NPM} install
                            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                            RESULT_VARIABLE _ch_npm_install_rc
                            OUTPUT_QUIET ERROR_QUIET)
        endif()
        message(STATUS "CodeHarbor: building the Monaco editor bundle (missing or stale)")
        execute_process(COMMAND ${CODEHARBOR_NPM} run build --workspace src/web/editor
                        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                        RESULT_VARIABLE _ch_bundle_rc
                        OUTPUT_VARIABLE _ch_bundle_out
                        ERROR_VARIABLE _ch_bundle_out)
        if(NOT _ch_bundle_rc EQUAL 0)
            message(STATUS "CodeHarbor: bundle build failed (${_ch_bundle_rc}):\n${_ch_bundle_out}")
        endif()
    endif()
endif()

if(EXISTS ${_ch_editor_dist}/index.html)
    qt_add_resources(codeharbor_qml ch_editor_bundle
        PREFIX /codeharbor/web/editor
        BASE ${_ch_editor_dist}
        FILES
            ${_ch_editor_dist}/index.html
            ${_ch_editor_dist}/editor.js
            ${_ch_editor_dist}/editor.css
            ${_ch_editor_dist}/editor.worker.js
    )
    # Editing any bundle input re-runs configure (which rebuilds it above), so the
    # embedded copy cannot silently lag its sources.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 ${_ch_editor_inputs})
elseif(CODEHARBOR_SKIP_WEB_BUNDLE)
    message(WARNING
        "CODEHARBOR_SKIP_WEB_BUNDLE=ON: no Monaco bundle is embedded, so the "
        "editor pane will load nothing. Do not ship this build.")
else()
    message(FATAL_ERROR
        "The Monaco editor bundle (src/web/editor/dist) is missing and could not "
        "be built automatically.\n"
        "Build it, then re-run CMake:\n"
        "    npm install\n"
        "    npm run build --workspace src/web/editor\n"
        "Or configure with -DCODEHARBOR_SKIP_WEB_BUNDLE=ON to build a client "
        "whose editor pane is intentionally inert.")
endif()

# Trusted xterm.js terminal page (src/web/terminal), embedded exactly like the
# editor bundle above so TerminalPaneView.qml can navigate to ONE constant URL —
# qrc:/codeharbor/web/terminal/index.html.
#
# Same reasoning as the editor block: the bundle is a gitignored esbuild artifact
# and Qt reads resource files at CONFIGURE time, so a missing bundle would
# silently ship a terminal pane with nothing to load. Build it on demand, refuse
# to configure if we cannot, and offer -DCODEHARBOR_SKIP_WEB_BUNDLE=ON for
# C++-only/offline work.
set(_ch_terminal_src ${CMAKE_CURRENT_SOURCE_DIR}/../web/terminal)
set(_ch_terminal_dist ${_ch_terminal_src}/dist)

file(GLOB_RECURSE _ch_terminal_inputs ${_ch_terminal_src}/src/*.ts)
list(APPEND _ch_terminal_inputs
     ${_ch_terminal_src}/build.mjs
     ${_ch_terminal_src}/index.html
     ${_ch_terminal_src}/package.json)

set(_ch_terminal_stale FALSE)
if(NOT EXISTS ${_ch_terminal_dist}/index.html)
    set(_ch_terminal_stale TRUE)
else()
    foreach(_ch_in IN LISTS _ch_terminal_inputs)
        if(${_ch_in} IS_NEWER_THAN ${_ch_terminal_dist}/index.html)
            set(_ch_terminal_stale TRUE)
        endif()
    endforeach()
endif()

if(_ch_terminal_stale AND NOT CODEHARBOR_SKIP_WEB_BUNDLE)
    find_program(CODEHARBOR_NPM NAMES npm.cmd npm)
    if(CODEHARBOR_NPM)
        if(NOT EXISTS ${CMAKE_SOURCE_DIR}/node_modules)
            message(STATUS "CodeHarbor: installing npm workspaces (first configure)")
            execute_process(COMMAND ${CODEHARBOR_NPM} install
                            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                            RESULT_VARIABLE _ch_npm_install_rc
                            OUTPUT_QUIET ERROR_QUIET)
        endif()
        message(STATUS "CodeHarbor: building the xterm.js terminal bundle (missing or stale)")
        execute_process(COMMAND ${CODEHARBOR_NPM} run build --workspace src/web/terminal
                        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                        RESULT_VARIABLE _ch_terminal_bundle_rc
                        OUTPUT_VARIABLE _ch_terminal_bundle_out
                        ERROR_VARIABLE _ch_terminal_bundle_out)
        if(NOT _ch_terminal_bundle_rc EQUAL 0)
            message(STATUS "CodeHarbor: terminal bundle build failed (${_ch_terminal_bundle_rc}):\n${_ch_terminal_bundle_out}")
        endif()
    endif()
endif()

if(EXISTS ${_ch_terminal_dist}/index.html)
    qt_add_resources(codeharbor_qml ch_terminal_bundle
        PREFIX /codeharbor/web/terminal
        BASE ${_ch_terminal_dist}
        FILES
            ${_ch_terminal_dist}/index.html
            ${_ch_terminal_dist}/terminal.js
            ${_ch_terminal_dist}/terminal.css
    )
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 ${_ch_terminal_inputs})
elseif(CODEHARBOR_SKIP_WEB_BUNDLE)
    message(WARNING
        "CODEHARBOR_SKIP_WEB_BUNDLE=ON: no xterm.js bundle is embedded, so the "
        "terminal pane will load nothing. Do not ship this build.")
else()
    message(FATAL_ERROR
        "The xterm.js terminal bundle (src/web/terminal/dist) is missing and could "
        "not be built automatically.\n"
        "Build it, then re-run CMake:\n"
        "    npm install\n"
        "    npm run build --workspace src/web/terminal\n"
        "Or configure with -DCODEHARBOR_SKIP_WEB_BUNDLE=ON to build a client "
        "whose terminal pane is intentionally inert.")
endif()

if(CODEHARBOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
