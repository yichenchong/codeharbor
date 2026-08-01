# QML module for the CodeHarbor UI, kept as a sibling of app/ per the proposed
# architecture (SPEC 14). Built as a static QML module library that the app
# executable links against.
qt_add_library(codeharbor_qml STATIC)

# Theme.qml is the module's single colour/metric vocabulary. It must be marked a
# QML singleton BEFORE qt_add_qml_module() reads the file list, and the property
# here must agree with the `pragma Singleton` at the top of the file — if either
# side is missing, `import CodeHarbor` still succeeds and `Theme.x` silently
# resolves to undefined, which in QML is a blank colour rather than an error.
set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(codeharbor_qml
    URI CodeHarbor
    VERSION 1.0
    # Output dir must end in the module target path (URI) so qmllint/qmlls
    # tooling resolves the module correctly.
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/CodeHarbor
    QML_FILES
        Theme.qml
        RemotePath.js
        Main.qml
        AppTitleBar.qml
        AppSplitHandle.qml
        AppScrollBar.qml
        AppPaneHeader.qml
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
        AppDialog.qml
)

target_link_libraries(codeharbor_qml PUBLIC
    Qt6::Quick
    Qt6::QuickControls2
)

# Trusted web bundles (src/web/editor, src/web/terminal), embedded as Qt
# resources so EditorPaneView.qml and TerminalPaneView.qml can each navigate to
# ONE constant URL — qrc:/codeharbor/web/<name>/index.html — that resolves
# identically in the build tree, in a relocated install, and inside a macOS
# bundle. No filesystem path ever has to be plumbed into QML.
#
# A bundle is an esbuild BUILD ARTIFACT (~4 MB for Monaco) and is deliberately
# gitignored, so a fresh clone has none. Qt reads resource files at CONFIGURE
# time, so a missing bundle used to configure "successfully" and silently ship a
# pane with nothing to load. Instead we build it on demand here, and refuse to
# configure if we cannot: a broken pane must never be the quiet default.
#
# Escape hatch for C++-only/offline work: -DCODEHARBOR_SKIP_WEB_BUNDLE=ON, which
# downgrades this to a warning and yields a client whose pane is inert.
#
# Source maps are a build-type-driven option. Without one, a JavaScript fault in
# either page shows up in the embedded browser's devtools as a stack trace into
# a single minified line, which is unusable; with one, devtools shows the
# original TypeScript. They are not free — Monaco's three maps alone are ~15 MB
# — so they are ON for a Debug configure and OFF otherwise, and the shipped
# Release binary does not grow by a byte. Override with
# -DCODEHARBOR_WEB_SOURCEMAP=ON/OFF; a multi-config generator (which has no
# CMAKE_BUILD_TYPE at configure time, and the bundle IS built at configure time)
# defaults to OFF and must use the override to get maps.
#
# Derived fresh on every configure rather than cached with option(). A cached
# default would stick to whatever the build type was on the FIRST configure of a
# directory, so re-configuring an existing Debug directory as Release would go on
# embedding ~15 MB of maps in a binary labelled Release. An explicit
# -DCODEHARBOR_WEB_SOURCEMAP=ON/OFF does persist — CMake puts it in the cache
# before this file runs, so it is already DEFINED here and wins.
#
# Both bundles are handled by this ONE function, called twice below: the two
# blocks it replaced were near-identical, so a fix applied to one of them could
# silently miss the other.
#
#   DIR       directory under src/web (also the npm workspace name)
#   RESOURCE  qt_add_resources resource-set name (must be unique per target)
#   LABEL     human name of the bundle, used in configure messages
#   PANE      human name of the QML pane that loads it, used in messages
#   FILES     dist file names to embed, relative to src/web/<DIR>/dist

if(NOT DEFINED CODEHARBOR_WEB_SOURCEMAP)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CODEHARBOR_WEB_SOURCEMAP ON)
    else()
        set(CODEHARBOR_WEB_SOURCEMAP OFF)
    endif()
endif()

function(codeharbor_embed_web_bundle)
    cmake_parse_arguments(CHB "" "DIR;RESOURCE;LABEL;PANE" "FILES" ${ARGN})

    set(_chb_src ${CMAKE_CURRENT_SOURCE_DIR}/../web/${CHB_DIR})
    set(_chb_dist ${_chb_src}/dist)

    # Inputs the bundle is built FROM. Tracking these (not just the output) is
    # what makes the embedded copy honest: editing the sources re-triggers
    # configure, which rebuilds a stale bundle before it is embedded.
    #
    # CONFIGURE_DEPENDS is what makes that true for a source file that did not
    # exist yet. A plain glob is evaluated ONLY during this configure run, and
    # nothing asks CMake to look again: the CMAKE_CONFIGURE_DEPENDS property set
    # at the bottom of this function can only list the files the glob already
    # found, so ADDING a new .ts triggered no reconfigure, the new file was in
    # neither list, and the embedded bundle silently stayed on the old sources
    # until something unrelated happened to re-run configure. (That is not
    # hypothetical: src/web/terminal/src/writer.ts was added and had to be
    # rebuilt by hand.) With CONFIGURE_DEPENDS the generator re-runs the glob on
    # every build and re-runs configure when the matched SET changes, so a new
    # file is picked up by the next build like an edit to an existing one.
    #
    # The two mechanisms are complementary and both are needed: this catches
    # files appearing and disappearing, CMAKE_CONFIGURE_DEPENDS below catches
    # edits to the files themselves. The staleness comparison underneath is
    # untouched — for a tree whose file set has not changed the glob yields
    # exactly the same list as before.
    file(GLOB_RECURSE _chb_inputs CONFIGURE_DEPENDS ${_chb_src}/src/*.ts)
    list(APPEND _chb_inputs
         ${_chb_src}/build.mjs
         ${_chb_src}/index.html
         ${_chb_src}/package.json)

    set(_chb_stale FALSE)
    if(NOT EXISTS ${_chb_dist}/index.html)
        set(_chb_stale TRUE)
    else()
        foreach(_chb_in IN LISTS _chb_inputs)
            if(${_chb_in} IS_NEWER_THAN ${_chb_dist}/index.html)
                set(_chb_stale TRUE)
            endif()
        endforeach()
    endif()

    # Flipping CODEHARBOR_WEB_SOURCEMAP changes what dist/ must CONTAIN without
    # touching the mtime of a single input, so the timestamp comparison above
    # would happily keep a mapless bundle for a Debug configure — and then fail
    # the resource step below on a .map that was never built. Compare what is on
    # disk against what was asked for as well.
    #
    # _chb_maps is derived from the caller's EXPLICIT file list, not from a glob
    # of dist/: esbuild writes exactly one <output>.map beside each .js and .css
    # it emits, so the names are known here without looking at the tree, and a
    # stale leftover in dist/ can never sneak into the resource.
    unset(_chb_maps)
    foreach(_chb_file IN LISTS CHB_FILES)
        if(_chb_file MATCHES "\\.(js|css)$")
            list(APPEND _chb_maps ${_chb_file}.map)
        endif()
    endforeach()
    foreach(_chb_map IN LISTS _chb_maps)
        if(CODEHARBOR_WEB_SOURCEMAP AND NOT EXISTS ${_chb_dist}/${_chb_map})
            set(_chb_stale TRUE)
        elseif(NOT CODEHARBOR_WEB_SOURCEMAP AND EXISTS ${_chb_dist}/${_chb_map})
            set(_chb_stale TRUE)
        endif()
    endforeach()

    # A named npm script, not `npm run build -- --sourcemap`: the flag is then
    # declared in package.json, so a developer can reproduce exactly what
    # configure ran by name instead of reverse-engineering it from here.
    set(_chb_npm_script build)
    if(CODEHARBOR_WEB_SOURCEMAP)
        set(_chb_npm_script build:sourcemap)
    endif()

    if(_chb_stale AND NOT CODEHARBOR_SKIP_WEB_BUNDLE)
        # npm.cmd FIRST: on Windows find_program only auto-appends .com/.exe, so
        # a bare `npm` resolves the extensionless shell script, which
        # execute_process cannot launch - and the failure surfaces as an EMPTY
        # error string.
        find_program(CODEHARBOR_NPM NAMES npm.cmd npm)
        if(CODEHARBOR_NPM)
            if(NOT EXISTS ${CMAKE_SOURCE_DIR}/node_modules)
                message(STATUS "CodeHarbor: installing npm workspaces (first configure)")
                execute_process(COMMAND ${CODEHARBOR_NPM} install
                                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                                RESULT_VARIABLE _chb_install_rc
                                OUTPUT_VARIABLE _chb_install_out
                                ERROR_VARIABLE _chb_install_out)
                # Stop HERE on a failed install. Carrying on to the build step
                # only produces a second, more confusing failure (missing
                # esbuild) far away from the real cause.
                if(NOT _chb_install_rc EQUAL 0)
                    message(FATAL_ERROR
                        "CodeHarbor: `npm install` failed (${_chb_install_rc}), so the "
                        "${CHB_LABEL} bundle cannot be built.\n"
                        "${_chb_install_out}\n"
                        "Fix the dependency install, then re-run CMake:\n"
                        "    npm install\n"
                        "Or configure with -DCODEHARBOR_SKIP_WEB_BUNDLE=ON to build a "
                        "client whose ${CHB_PANE} pane is intentionally inert.")
                endif()
            endif()
            message(STATUS "CodeHarbor: building the ${CHB_LABEL} bundle (missing or stale)")
            execute_process(COMMAND ${CODEHARBOR_NPM} run ${_chb_npm_script} --workspace src/web/${CHB_DIR}
                            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                            RESULT_VARIABLE _chb_bundle_rc
                            OUTPUT_VARIABLE _chb_bundle_out
                            ERROR_VARIABLE _chb_bundle_out)
            if(NOT _chb_bundle_rc EQUAL 0)
                # WARNING, not STATUS. When dist/ is absent the FATAL_ERROR below
                # reports the problem anyway, but when a STALE dist/ is present
                # configure carries on and embeds it - so the ONLY notice that the
                # binary now contains an out-of-date bundle is this line, and a
                # STATUS line is indistinguishable from the dozens of ordinary
                # ones CMake prints.
                message(WARNING
                    "CodeHarbor: the ${CHB_LABEL} bundle failed to build (${_chb_bundle_rc}). "
                    "If a previously built ${_chb_dist} is present it will be embedded AS IS, "
                    "which means the ${CHB_PANE} pane runs stale code.\n${_chb_bundle_out}")
            endif()
        endif()
    endif()

    if(EXISTS ${_chb_dist}/index.html)
        # unset (not set to "") so the list starts genuinely empty.
        unset(_chb_files)
        foreach(_chb_file IN LISTS CHB_FILES)
            list(APPEND _chb_files ${_chb_dist}/${_chb_file})
        endforeach()
        # Maps are embedded only when they were actually produced. qt_add_resources
        # reads FILES at CONFIGURE time and hard-errors on one that is not there,
        # so this EXISTS gate is what keeps a Debug configure whose bundle build
        # was skipped (no npm) or failed from piling a baffling missing-.map
        # error on top of the real cause.
        if(CODEHARBOR_WEB_SOURCEMAP)
            set(_chb_have_maps TRUE)
            foreach(_chb_map IN LISTS _chb_maps)
                if(NOT EXISTS ${_chb_dist}/${_chb_map})
                    set(_chb_have_maps FALSE)
                endif()
            endforeach()
            if(_chb_have_maps)
                foreach(_chb_map IN LISTS _chb_maps)
                    list(APPEND _chb_files ${_chb_dist}/${_chb_map})
                endforeach()
            else()
                message(STATUS
                    "CodeHarbor: ${CHB_LABEL} source maps were requested but are not "
                    "in dist/; embedding the bundle without them (its devtools stack "
                    "traces will point into minified code)")
            endif()
        endif()
        qt_add_resources(codeharbor_qml ${CHB_RESOURCE}
            PREFIX /codeharbor/web/${CHB_DIR}
            BASE ${_chb_dist}
            FILES ${_chb_files}
        )
        # Editing any bundle input re-runs configure (which rebuilds it above),
        # so the embedded copy cannot silently lag its sources. Inside a function
        # DIRECTORY still means the calling directory, i.e. src/qml.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     ${_chb_inputs})
    elseif(CODEHARBOR_SKIP_WEB_BUNDLE)
        message(WARNING
            "CODEHARBOR_SKIP_WEB_BUNDLE=ON: no ${CHB_LABEL} bundle is embedded, so "
            "the ${CHB_PANE} pane will load nothing. Do not ship this build.")
    else()
        message(FATAL_ERROR
            "The ${CHB_LABEL} bundle (src/web/${CHB_DIR}/dist) is missing and could "
            "not be built automatically.\n"
            "Build it, then re-run CMake:\n"
            "    npm install\n"
            "    npm run ${_chb_npm_script} --workspace src/web/${CHB_DIR}\n"
            "Or configure with -DCODEHARBOR_SKIP_WEB_BUNDLE=ON to build a client "
            "whose ${CHB_PANE} pane is intentionally inert.")
    endif()
endfunction()

# The Monaco editor page, loaded by EditorPaneView.qml from
# qrc:/codeharbor/web/editor/index.html.
codeharbor_embed_web_bundle(
    DIR editor
    RESOURCE ch_editor_bundle
    LABEL "Monaco editor"
    PANE editor
    FILES index.html editor.js editor.css editor.worker.js
)

# The xterm.js terminal page, loaded by TerminalPaneView.qml from
# qrc:/codeharbor/web/terminal/index.html.
codeharbor_embed_web_bundle(
    DIR terminal
    RESOURCE ch_terminal_bundle
    LABEL "xterm.js terminal"
    PANE terminal
    FILES index.html terminal.js terminal.css
)

if(CODEHARBOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
