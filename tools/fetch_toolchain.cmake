# What preparing the game needs on the player's machine, so that it needs nothing there:
# zig (its `zig cc`, clang with every libc it targets) and a standalone Python for
# recomp/recomp.py. Pinned versions, checked against their SHA-256, unpacked into DEST:
#
#   DEST/zig/zig[.exe]                  and zig's lib/
#   DEST/python/bin/python3             (python/python.exe on Windows)
#   DEST/python-arm64, python-x86_64    instead, for a universal macOS app (-DUNIVERSAL=ON):
#                                       one zig binary for both, one Python each
#
#   cmake -DDEST=<dir> [-DARCH=x86_64|aarch64] [-DUNIVERSAL=ON] -P tools/fetch_toolchain.cmake
#
# A development build fetches it into its build directory (CMakeLists.txt), an app's
# packaging into the app; runtime/game_module.cpp finds it (toolchain_dir). ARCH defaults to
# the machine's. Downloads are kept in GCN_TOOLCHAIN_CACHE (by default
# ~/.cache/gcn-recomp-toolchain, %LOCALAPPDATA%\gcn-recomp-toolchain on Windows), so a second
# run is offline.
cmake_minimum_required(VERSION 3.20)

set(ZIG 0.16.0)
set(PY 3.12.14)
set(PBS 20260924)
set(zig_x86_64-windows 68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e)
set(zig_aarch64-windows aee38316ee4111717900f45dd3130145c39289e105541d737eb8c5ed653c78ef)
set(zig_x86_64-linux 70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00)
set(zig_aarch64-linux ea4b09bfb22ec6f6c6ceac57ab63efb6b46e17ab08d21f69f3a48b38e1534f17)
set(zig_x86_64-macos 0387557ed1877bc6a2e1802c8391953baddba76081876301c522f52977b52ba7)
set(zig_aarch64-macos b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489)
set(py_x86_64-windows x86_64-pc-windows-msvc c5bf8edfe858c1df9891be498b5bbc8761d383df5b9790658b088fea4870433a)
set(py_aarch64-windows aarch64-pc-windows-msvc 1d07bd9c97e6e1942b290bd2daa22d5a41e1e5acd2fd3d4c696892ceffe47144)
set(py_x86_64-linux x86_64-unknown-linux-gnu 269b2c99e4db15b242bf01832f4fea1e8f1a664f273cff519393f296e9820b41)
set(py_aarch64-linux aarch64-unknown-linux-gnu c8499b61252c433280f134df954464d19811527b31cb920c35fc6967c1222e35)
set(py_x86_64-macos x86_64-apple-darwin 7ea9761b9069c10b9a20531d568645849d604c59e9c7f11f6659f1e1790c968e)
set(py_aarch64-macos aarch64-apple-darwin c2edb321cd32ec2b170df208db0446dccc4398db602ca27cf2079098fb1f7d9d)

if(NOT DEST)
  message(FATAL_ERROR "usage: cmake -DDEST=<dir> [-DARCH=x86_64|aarch64] [-DUNIVERSAL=ON] -P ${CMAKE_CURRENT_LIST_FILE}")
endif()
if(CMAKE_HOST_WIN32)
  set(os windows)
  set(cache $ENV{LOCALAPPDATA}/gcn-recomp-toolchain)
elseif(CMAKE_HOST_APPLE)
  set(os macos)
  set(cache $ENV{HOME}/.cache/gcn-recomp-toolchain)
else()
  set(os linux)
  set(cache $ENV{HOME}/.cache/gcn-recomp-toolchain)
endif()
if(DEFINED ENV{GCN_TOOLCHAIN_CACHE})
  set(cache $ENV{GCN_TOOLCHAIN_CACHE})
endif()
if(NOT ARCH)
  cmake_host_system_information(RESULT ARCH QUERY OS_PLATFORM)
endif()
if(ARCH MATCHES "^(arm64|aarch64|ARM64)$")
  set(ARCH aarch64)
else()
  set(ARCH x86_64)
endif()

# The archive at <url>, from the cache or downloaded and checked, its single top directory's
# contents unpacked into <dir>.
function(fetch url sha dir)
  get_filename_component(file ${url} NAME)
  set(file ${cache}/${file})
  if(EXISTS ${file})
    file(SHA256 ${file} have)
  endif()
  if(NOT have STREQUAL sha)
    message(STATUS "toolchain: downloading ${url}")
    file(DOWNLOAD ${url} ${file} EXPECTED_HASH SHA256=${sha} STATUS status)
    list(GET status 0 code)
    if(code)
      file(REMOVE ${file})
      message(FATAL_ERROR "toolchain: ${url}: ${status}")
    endif()
  endif()
  file(REMOVE_RECURSE ${dir} ${dir}.tmp)
  file(ARCHIVE_EXTRACT INPUT ${file} DESTINATION ${dir}.tmp)
  file(GLOB top ${dir}.tmp/*)
  file(RENAME ${top} ${dir})
  file(REMOVE_RECURSE ${dir}.tmp)
endfunction()

function(fetch_zig arch dir)
  set(ext tar.xz)
  if(os STREQUAL windows)
    set(ext zip)
  endif()
  fetch(https://ziglang.org/download/${ZIG}/zig-${arch}-${os}-${ZIG}.${ext} ${zig_${arch}-${os}} ${dir})
endfunction()

function(fetch_python arch dir)
  list(GET py_${arch}-${os} 0 triple)
  list(GET py_${arch}-${os} 1 sha)
  fetch(https://github.com/astral-sh/python-build-standalone/releases/download/${PBS}/cpython-${PY}+${PBS}-${triple}-install_only_stripped.tar.gz
        ${sha} ${dir})
endfunction()

if(UNIVERSAL AND os STREQUAL macos)
  fetch_zig(aarch64 ${DEST}/zig)
  fetch_zig(x86_64 ${DEST}/zig-x86_64)
  execute_process(COMMAND lipo -create ${DEST}/zig/zig ${DEST}/zig-x86_64/zig -output ${DEST}/zig/zig.universal
                  COMMAND_ERROR_IS_FATAL ANY)
  file(RENAME ${DEST}/zig/zig.universal ${DEST}/zig/zig)
  file(REMOVE_RECURSE ${DEST}/zig-x86_64)
  fetch_python(aarch64 ${DEST}/python-arm64)
  fetch_python(x86_64 ${DEST}/python-x86_64)
else()
  fetch_zig(${ARCH} ${DEST}/zig)
  fetch_python(${ARCH} ${DEST}/python)
endif()

# Only what building C for this system needs: zig carries every libc it can target (170 MB
# of headers), its C++ libraries and its documentation; Python, its test suite.
if(os STREQUAL macos)
  set(keep "^(any-darwin-any|darwin)$")
elseif(os STREQUAL windows)
  set(keep "^(any-windows-any|mingw)$")
elseif(ARCH STREQUAL x86_64)
  set(keep "^(x86-linux-gnu|x86-linux-any|any-linux-any|generic-glibc|glibc)$")   # x86_64's glibc headers are x86's
else()
  set(keep "^(aarch64-linux-gnu|aarch64-linux-any|any-linux-any|generic-glibc|glibc)$")
endif()
set(libc ${DEST}/zig/lib/libc)
file(GLOB dirs LIST_DIRECTORIES true ${libc}/* ${libc}/include/*)
foreach(d ${dirs})
  get_filename_component(name ${d} NAME)
  if(IS_DIRECTORY ${d} AND NOT name STREQUAL include AND NOT name MATCHES "${keep}")
    file(REMOVE_RECURSE ${d})
  endif()
endforeach()
file(REMOVE_RECURSE ${DEST}/zig/doc ${DEST}/zig/lib/libcxx ${DEST}/zig/lib/libcxxabi ${DEST}/zig/lib/libtsan
     ${DEST}/zig/lib/docs)
file(GLOB pythons LIST_DIRECTORIES true ${DEST}/python*)
foreach(py ${pythons})
  file(GLOB tk LIST_DIRECTORIES true ${py}/lib/*tcl* ${py}/lib/*tk*)
  file(REMOVE_RECURSE ${py}/include ${py}/share ${py}/tcl ${tk})
  file(GLOB libs LIST_DIRECTORIES true ${py}/lib/python3.* ${py}/Lib)
  foreach(lib ${libs})
    file(GLOB pip LIST_DIRECTORIES true ${lib}/site-packages/pip*)
    file(REMOVE_RECURSE ${lib}/test ${lib}/idlelib ${lib}/tkinter ${lib}/turtledemo ${lib}/ensurepip ${lib}/lib2to3
         ${lib}/pydoc_data ${pip})
  endforeach()
endforeach()

# Their licences, beside them: zig's (MIT), LLVM's, which zig is built on (Apache-2.0 with
# LLVM exceptions, the same text as its libunwind's), and Python's (PSF).
file(GLOB python_license ${DEST}/python*/LICENSE.txt ${DEST}/python*/lib/python3.*/LICENSE.txt)
list(GET python_license 0 python_license)
configure_file(${DEST}/zig/LICENSE ${DEST}/licenses/zig.txt COPYONLY)
configure_file(${DEST}/zig/lib/libunwind/LICENSE.TXT ${DEST}/licenses/LLVM.txt COPYONLY)
configure_file(${python_license} ${DEST}/licenses/Python.txt COPYONLY)
message(STATUS "toolchain: zig ${ZIG} and Python ${PY} in ${DEST}")
