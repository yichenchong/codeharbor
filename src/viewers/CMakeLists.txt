# ViewerHandlerRegistry is the one part of this module that is not a WebEngine
# part: it answers "what kind of thing is this URL" using nothing but QUrl,
# QString and the view-kind records in ch_models - no QMimeDatabase (that lives
# in the Chromium-bound InternalUrlSchemeHandler below) and no Qt GUI type. The
# mobile client needs exactly that answer - to decide whether a leaf opens as
# text, Markdown, an image, a PDF, a directory or an editor - while having no
# Chromium at all.
#
# So it is its own target rather than a copy. The files DO NOT MOVE on disk and
# both targets export this directory as a PUBLIC include dir, so every existing
# `#include "ViewerHandlerRegistry.h"` and every existing test resolves exactly
# as before; ch_viewers links ch_viewers_core PUBLIC, which means anything that
# linked ch_viewers still gets both the header path and the symbols with no
# change to its link line.
qt_add_library(ch_viewers_core STATIC
    ViewerHandlerRegistry.h
    ViewerHandlerRegistry.cpp
)

target_include_directories(ch_viewers_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_viewers_core PUBLIC
    Qt6::Core
    ch_models
)

# The WebEngine half. Everything left here - the codeharbor-internal:// scheme
# handler, the two QWebEngineProfiles, and the ViewerModel that drives them -
# is Chromium-bound, and Qt ships no WebEngine for Android or iOS, so the whole
# target (and its tests, which instantiate a WebEngine profile) is desktop-only.
if(CODEHARBOR_BUILD_DESKTOP)
    qt_add_library(ch_viewers STATIC
        InternalUrlSchemeHandler.h
        InternalUrlSchemeHandler.cpp
        ViewerProfiles.h
        ViewerProfiles.cpp
        ViewerModel.h
        ViewerModel.cpp
    )

    target_include_directories(ch_viewers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ch_viewers PUBLIC
        Qt6::Core
        Qt6::WebEngineCore
        Qt6::WebEngineQuick
        Qt6::WebChannel
        ch_remote
        ch_models
        # PUBLIC on purpose: ViewerModel's callers use ch::ViewerHandlerRegistry
        # through this target, exactly as they did when the two files were
        # compiled into it.
        ch_viewers_core
    )

    if(CODEHARBOR_BUILD_TESTS)
        add_subdirectory(tests)
    endif()
endif()
