# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform detection code.
#
# This sets various OS_* and CPU_* variables based on the OS.
#
# This sorts out what OS and CPU we’re building for, which is
# information used by numerous other parts of the build system.

include_guard()

macro(_nd_windows_config)
  set(OS_WINDOWS True)
  set(OS_WINDOWS_MSYS2 False)
  set(OS_WINDOWS_NATIVE False)

  if("${CMAKE_SYSTEM_NAME}" STREQUAL "MSYS" OR "${CMAKE_SYSTEM_NAME}" STREQUAL "CYGWIN")
    set(OS_WINDOWS_MSYS2 True)
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
    set(OS_WINDOWS_NATIVE True)
  endif()

  if(OS_WINDOWS_MSYS2)
    if(NOT "${CMAKE_INSTALL_PREFIX}" STREQUAL "/opt/netdata")
      message(FATAL_ERROR "CMAKE_INSTALL_PREFIX must be set to /opt/netdata, but it is set to ${CMAKE_INSTALL_PREFIX}")
    endif()

    if(BUILD_FOR_PACKAGING)
      set(NETDATA_RUNTIME_PREFIX "/")
    endif()

    set(BINDIR usr/bin)
  else()
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
      set(CMAKE_INSTALL_PREFIX "C:/Program Files/Netdata" CACHE PATH "Install prefix" FORCE)
    endif()
    set(NETDATA_RUNTIME_PREFIX "${CMAKE_INSTALL_PREFIX}")
    # Keep Netdata's existing Windows directory layout under the install root.
    set(BINDIR usr/bin)
  endif()

  if(OS_WINDOWS_MSYS2)
    set(CMAKE_RC_COMPILER_INIT windres)
  endif()
  ENABLE_LANGUAGE(RC)

  if(OS_WINDOWS_MSYS2)
    SET(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> <FLAGS> -O coff <DEFINES> -i <SOURCE> -o <OBJECT>")
  endif()
  add_definitions(-D_GNU_SOURCE)

  if($ENV{CLION_IDE})
    set(RUN_UNDER_CLION True)

    # clion needs these to find the includes
    if("${CMAKE_SYSTEM_NAME}" STREQUAL "MSYS" OR "${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
      if("$ENV{MSYSTEM}" STREQUAL "MSYS")
        include_directories(c:/msys64/usr/include)
        include_directories(c:/msys64/usr/include/w32api)
      elseif("$ENV{MSYSTEM}" STREQUAL "MINGW64")
        include_directories(c:/msys64/mingw64/include)
      elseif("$ENV{MSYSTEM}" STREQUAL "UCRT64")
        include_directories(c:/msys64/ucrt64/include)
      endif()
    endif()
  endif()

  message(STATUS " Compiling for Windows (${CMAKE_SYSTEM_NAME}, MSYSTEM=$ENV{MSYSTEM})... ")
endmacro()

set(OS_FREEBSD     False)
set(OS_LINUX       False)
set(OS_MACOS       False)
set(OS_WINDOWS     False)
set(OS_WINDOWS_MSYS2 False)
set(OS_WINDOWS_NATIVE False)

set(CPU_X86     False)
set(CPU_X86_64  False)
set(CPU_ARM     False)
set(CPU_ARM64   False)
set(CPU_OTHER   False)

if("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin")
  set(OS_MACOS True)
  find_library(IOKIT IOKit)
  find_library(FOUNDATION Foundation)
  message(STATUS " Compiling for MacOS... ")
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "FreeBSD")
  set(OS_FREEBSD True)
  message(STATUS " Compiling for FreeBSD... ")
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
  set(OS_LINUX True)
  add_definitions(-D_GNU_SOURCE)
  message(STATUS " Compiling for Linux... ")
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "CYGWIN")
  _nd_windows_config()
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "MSYS")
  _nd_windows_config()
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
  _nd_windows_config()
else()
  message(FATAL_ERROR "Unknown/unsupported platform: ${CMAKE_SYSTEM_NAME} (Supported platforms: FreeBSD, Linux, macOS, Windows)")
endif()

string(STRIP "${CMAKE_SYSTEM_PROCESSOR}" _nd_cpu_raw)
string(TOLOWER "${_nd_cpu_raw}" _nd_cpu)

message(STATUS "Detected CPU: ${CMAKE_SYSTEM_PROCESSOR}")

if("${_nd_cpu}" MATCHES "^(x86_64|x64|amd64)")
  set(CPU_X86_64 True)
elseif("${_nd_cpu}" MATCHES "^(x86|i[3456]86|ix86)")
  set(CPU_X86 True)
elseif("${_nd_cpu}" MATCHES "^(aarch|arm)64")
  set(CPU_ARM64 True)
elseif("${_nd_cpu}" MATCHES "^(arm|armv[567].*)")
  set(CPU_ARM True)
else()
  set(CPU_OTHER True)
endif()
