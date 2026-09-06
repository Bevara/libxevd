cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBXEVD_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_evc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/evc_stubs.c
)

SET(LIBXEVD_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libxevd.a
)

SET(LIBXEVD_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libxevd
        "${LIBXEVD_SRC}"
        "${LIBXEVD_LIBS}"
        []
        ""
        "${LIBXEVD_INC}"
        ""
        "1")
