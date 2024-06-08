#
# Copyright(c) 2019 Intel Corporation
#
# This source code is subject to the terms of the BSD 2 Clause License and
# the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
# was not distributed with this source code in the LICENSE file, you can
# obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
# Media Patent License 1.0 was not distributed with this source code in the
# PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
#

cmake_minimum_required(VERSION 3.5...3.28)

# APP Directory CMakeLists.txt

# Include Subdirectories
include_directories(${PROJECT_SOURCE_DIR}/Source/API/)

# libdovi detection & preprocessor macro
option(LIBDOVI_FOUND "Use the libdovi library" OFF)
if(LIBDOVI_FOUND)
  if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_LIBDOVI libdovi QUIET)
  endif()

  find_library(LIBDOVI_LIBRARY NAMES dovi libdovi
                               PATHS ${PC_LIBDOVI_LIBDIR})
  if(NOT LIBDOVI_LIBRARY)
  message(FATAL_ERROR "libdovi library not found!")
  endif()

  find_path(LIBDOVI_INCLUDE_DIR NAMES libdovi/rpu_parser.h
                                PATHS ${PC_LIBDOVI_INCLUDEDIR})

  set(LIBDOVI_INCLUDE_DIRS ${LIBDOVI_INCLUDE_DIR})
  set(LIBDOVI_LIBRARIES ${LIBDOVI_LIBRARY})
  add_definitions(-DLIBDOVI_FOUND=1)
  execute_process(COMMAND ${CMAKE_COMMAND} -E echo_append "-- Building with libdovi support - Yes\n")
else()
  execute_process(COMMAND ${CMAKE_COMMAND} -E echo_append "-- Building with libdovi support - No\n")
endif()

# libhdr10plus-rs detection & preprocessor macro
option(LIBHDR10PLUS_RS_FOUND "Use the libhdr10plus-rs library" OFF)
if(LIBHDR10PLUS_RS_FOUND)
  if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_LIBHDR10PLUS_RS libhdr10plus-rs QUIET)
  endif()

  find_library(LIBHDR10PLUS_RS_LIBRARY NAMES hdr10plus-rs libhdr10plus-rs
                               PATHS ${PC_LIBHDR10PLUS_RS_LIBDIR})
  if(NOT LIBHDR10PLUS_RS_LIBRARY)
  message(FATAL_ERROR "libhdr10plus-rs library not found!")
  endif()

  find_path(LIBHDR10PLUS_RS_INCLUDE_DIR NAMES libhdr10plus-rs/hdr10plus.h
                                PATHS ${PC_LIBHDR10PLUS_RS_INCLUDEDIR})

  set(LIBHDR10PLUS_RS_INCLUDE_DIRS ${LIBHDR10PLUS_RS_INCLUDE_DIR})
  set(LIBHDR10PLUS_RS_LIBRARIES ${LIBHDR10PLUS_RS_LIBRARY})
  add_definitions(-DLIBHDR10PLUS_RS_FOUND=1)
  execute_process(COMMAND ${CMAKE_COMMAND} -E echo_append "-- Building with libhdr10plus support - Yes\n")
else()
  execute_process(COMMAND ${CMAKE_COMMAND} -E echo_append "-- Building with libhdr10plus support - No\n")
endif()

set(all_files
    ../API/EbDebugMacros.h
    ../API/EbSvtAv1.h
    ../API/EbSvtAv1Enc.h
    ../API/EbSvtAv1ErrorCodes.h
    ../API/EbSvtAv1Formats.h
    ../API/EbSvtAv1Metadata.h
    app_config.c
    app_config.h
    app_context.c
    app_context.h
    app_input_y4m.c
    app_input_y4m.h
    app_main.c
    app_output_ivf.c
    app_output_ivf.h
    app_process_cmd.c
    svt_time.c
    svt_time.h
    )

# App Source Files
add_executable(SvtAv1EncApp
    ${all_files})

if(TARGET safestringlib)
    target_sources(SvtAv1EncApp PRIVATE $<TARGET_OBJECTS:safestringlib>)
endif()

#********** SET COMPILE FLAGS************
# Link the Encoder App
target_link_libraries(SvtAv1EncApp
        SvtAv1Enc
        ${LIBDOVI_LIBRARY}
        ${LIBHDR10PLUS_RS_LIBRARY})
if(${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    target_link_libraries(SvtAv1EncApp ${PLATFORM_LIBS})
elseif(UNIX)
    target_link_libraries(SvtAv1EncApp
        pthread
        m)
endif()

install(TARGETS SvtAv1EncApp RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR})
