add_library(xenia-hid-xbkbm STATIC)
xe_platform_sources(xenia-hid-xbkbm ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-hid-xbkbm PUBLIC xenia-base xenia-hid xenia-ui)
xe_target_defaults(xenia-hid-xbkbm)
