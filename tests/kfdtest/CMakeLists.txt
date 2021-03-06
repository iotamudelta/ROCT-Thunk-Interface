#
# Copyright (C) 2018 Advanced Micro Devices, Inc. All Rights Reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
#

# If environment variable DRM_DIR or LIBHSAKMT_PATH is set, the script
# will pick up the corresponding libraries from those pathes.

cmake_minimum_required(VERSION 2.8 FATAL_ERROR)

project(KFDTest)

#set ( CMAKE_VERBOSE_MAKEFILE on )

find_package(PkgConfig)

if( DEFINED ENV{DRM_DIR} )
#assume header files and libraries are under the same path
    set ( DRM_DIR $ENV{DRM_DIR} )
    set ( DRM_INCLUDE_DIRS ${DRM_DIR}/include )
    link_directories(${DRM_DIR}/lib64)
    set ( DRM_LIBRARIES drm )
    set ( DRM_AMDGPU_LIBRARIES drm_amdgpu )
else()
# The module name passed to pkg_check_modules() is determined by the
# name of file *.pc
    pkg_check_modules(DRM REQUIRED libdrm)
    pkg_check_modules(DRM_AMDGPU REQUIRED libdrm_amdgpu)
    include_directories(${DRM_AMDGPU_INCLUDE_DIRS})
endif()


if( DEFINED ENV{LIBHSAKMT_PATH} )
    set ( LIBHSAKMT_PATH $ENV{LIBHSAKMT_PATH} )
    message ( "LIBHSAKMT_PATH environment variable is set" )
else()
    set ( ENV{PKG_CONFIG_PATH} /opt/rocm/libhsakmt/ )
    pkg_check_modules(HSAKMT libhsakmt)
    if( NOT HSAKMT_FOUND )
        set ( LIBHSAKMT_PATH $ENV{OUT_DIR} )
    endif()
endif()

if( DEFINED LIBHSAKMT_PATH )
    set ( HSAKMT_INCLUDE_DIRS ${LIBHSAKMT_PATH}/include )
    set ( HSAKMT_LIBRARY_DIRS ${LIBHSAKMT_PATH}/lib )
    set ( HSAKMT_LIBRARIES hsakmt )
endif()

message ( "Find libhsakmt at ${HSAKMT_LIBRARY_DIRS}" )

set ( SP3_DIR ${PROJECT_SOURCE_DIR}/sp3 )

include_directories(${PROJECT_SOURCE_DIR}/gtest-1.6.0)
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${SP3_DIR})

include_directories(${HSAKMT_INCLUDE_DIRS})
include_directories(${DRM_INCLUDE_DIRS})

aux_source_directory(${PROJECT_SOURCE_DIR}/gtest-1.6.0 SRC_FILES)
aux_source_directory(${PROJECT_SOURCE_DIR}/src SRC_FILES)

message( STATUS "PROJECT_SOURCE_DIR:" ${PROJECT_SOURCE_DIR} )
#message( STATUS "SRC_FILES: ")
#foreach(file ${SRC_FILES})
#  message(STATUS "${file}")
#endforeach()

#add_definitions(-Wall -std=c++11)

if ( "${CMAKE_C_COMPILER_VERSION}" STRGREATER "4.8.0")
set ( CMAKE_CXX_FLAGS "-std=gnu++11" )
endif()
if ( "${CMAKE_BUILD_TYPE}" STREQUAL Release )
    set ( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2" )
else ()
    set ( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g" )
endif ()

# link_directories() has to be put before add_executable()
# The modules found by pkg_check_modules() in the default pkg config
# path do not need to use link_directories() here.
link_directories(${HSAKMT_LIBRARY_DIRS})
link_directories(${SP3_DIR})

add_executable(kfdtest ${SRC_FILES})

target_link_libraries(kfdtest ${HSAKMT_LIBRARIES} ${DRM_LIBRARIES} ${DRM_AMDGPU_LIBRARIES} pthread m stdc++ rt amdsp3)

configure_file ( scripts/kfdtest.exclude kfdtest.exclude COPYONLY )
configure_file ( scripts/run_kfdtest.sh run_kfdtest.sh COPYONLY )
