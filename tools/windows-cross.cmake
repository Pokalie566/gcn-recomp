# Building for Windows (x86_64) from Linux: clang-cl and lld-link, with Microsoft's C runtime and
# Windows SDK as xwin (github.com/Jake-Shadle/xwin) fetches them -- downloading them accepts
# Microsoft's licence for them, as installing Visual Studio's build tools does -- and the Visual
# C++ runtime's DLLs that an app ships beside its executable (InstallRequiredSystemLibraries,
# through MSVC_REDIST_DIR). Pinned versions, checked against their SHA-256.
#
#   cmake -B build-windows --toolchain gcn-recomp/tools/windows-cross.cmake ...
#
# Needs LLVM 19 or newer (clang, lld, llvm: the C++ library of Visual C++ 14.44 wants clang 19)
# and CA certificates. Downloads, about 700 MB once unpacked, go to GCN_TOOLCHAIN_CACHE
# (~/.cache/gcn-recomp-toolchain), on a case-sensitive file system: xwin adds links for the
# names the SDK's headers use in the wrong case.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(_xwin_version 0.10.0)
set(_xwin_sha256_x86_64 d870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88)
set(_xwin_sha256_aarch64 6d56d28537a86f37aa3d041318898f25ee3100c6b6ec332ad873c28faf37be23)
set(_crt 14.44.17.14)
set(_sdk 10.0.26100)
set(_redist https://download.visualstudio.microsoft.com/download/pr/45d3b8dd-bced-4b37-9974-142f748d710c/4aaf54db0bfc9435f7c3660e1a00237a4b556042bfeea64bde44c2e0194e6ee5/Microsoft.VC.14.44.17.14.CRT.Redist.X64.base.vsix)
set(_redist_sha256 4aaf54db0bfc9435f7c3660e1a00237a4b556042bfeea64bde44c2e0194e6ee5)

set(_cache $ENV{HOME}/.cache/gcn-recomp-toolchain)
if(DEFINED ENV{GCN_TOOLCHAIN_CACHE})
  set(_cache $ENV{GCN_TOOLCHAIN_CACHE})
endif()
set(_win ${_cache}/windows-${_crt}-${_sdk})
# This file is read again for each of CMake's test builds: the downloads happen once.
if(NOT EXISTS ${_win}/done)
  file(REMOVE_RECURSE ${_win})
  string(REGEX REPLACE "arm64" "aarch64" _host ${CMAKE_HOST_SYSTEM_PROCESSOR})
  set(_xwin xwin-${_xwin_version}-${_host}-unknown-linux-musl)
  message(STATUS "windows-cross: fetching the Windows SDK and Visual C++'s runtime into ${_win}")
  file(DOWNLOAD https://github.com/Jake-Shadle/xwin/releases/download/${_xwin_version}/${_xwin}.tar.gz
       ${_win}/xwin.tar.gz EXPECTED_HASH SHA256=${_xwin_sha256_${_host}})
  file(ARCHIVE_EXTRACT INPUT ${_win}/xwin.tar.gz DESTINATION ${_win})
  execute_process(COMMAND ${_win}/${_xwin}/xwin --accept-license --http-retry 3 --cache-dir ${_win}/dl
                          --crt-version ${_crt} --sdk-version ${_sdk} splat --output ${_win}/sdk
                  COMMAND_ERROR_IS_FATAL ANY)
  file(DOWNLOAD ${_redist} ${_win}/redist.vsix EXPECTED_HASH SHA256=${_redist_sha256})
  file(ARCHIVE_EXTRACT INPUT ${_win}/redist.vsix DESTINATION ${_win}/redist PATTERNS "*/x64/Microsoft.VC143.CRT/*")
  file(REMOVE_RECURSE ${_win}/dl ${_win}/${_xwin} ${_win}/xwin.tar.gz ${_win}/redist.vsix)
  # Windows does not mind the case of a file's name, nor does code written for it
  # (<Wbemidl.h>, for WbemIdl.h): a clang overlay finds the SDK's headers in any case.
  file(GLOB_RECURSE _headers LIST_DIRECTORIES false ${_win}/sdk/sdk/include/*)
  set(_dirs)
  foreach(_h ${_headers})
    if(NOT IS_SYMLINK ${_h})   # xwin's links for other cases: the overlay covers them
      get_filename_component(_d ${_h} DIRECTORY)
      get_filename_component(_n ${_h} NAME)
      string(MD5 _k ${_d})
      list(APPEND _dirs ${_d})
      string(APPEND _files_${_k} "{\"type\":\"file\",\"name\":\"${_n}\",\"external-contents\":\"${_h}\"},")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _dirs)
  set(_roots)
  foreach(_d ${_dirs})
    string(MD5 _k ${_d})
    string(REGEX REPLACE ",$" "" _files "${_files_${_k}}")
    string(APPEND _roots "{\"type\":\"directory\",\"name\":\"${_d}\",\"contents\":[${_files}]},")
  endforeach()
  string(REGEX REPLACE ",$" "" _roots "${_roots}")
  file(WRITE ${_win}/case-insensitive.json "{\"version\":0,\"case-sensitive\":\"false\",\"roots\":[${_roots}]}\n")
  file(TOUCH ${_win}/done)
endif()
file(GLOB _redist_dir ${_win}/redist/Contents/VC/Redist/MSVC/*)
set(MSVC_REDIST_DIR ${_redist_dir} CACHE PATH "")

file(GLOB _llvm /usr/lib/llvm-*/bin)
list(SORT _llvm COMPARE NATURAL ORDER DESCENDING)
find_program(CMAKE_C_COMPILER clang-cl HINTS ${_llvm} REQUIRED)
find_program(CMAKE_CXX_COMPILER clang-cl HINTS ${_llvm} REQUIRED)
find_program(CMAKE_LINKER lld-link HINTS ${_llvm} REQUIRED)
find_program(CMAKE_AR llvm-lib HINTS ${_llvm} REQUIRED)
find_program(CMAKE_RC_COMPILER llvm-rc HINTS ${_llvm} REQUIRED)
find_program(CMAKE_MT llvm-mt HINTS ${_llvm} REQUIRED)

set(_s ${_win}/sdk)
set(_flags "--target=x86_64-pc-windows-msvc -fms-compatibility-version=19.44 -Xclang -ivfsoverlay -Xclang ${_win}/case-insensitive.json /imsvc ${_s}/crt/include /imsvc ${_s}/sdk/include/ucrt /imsvc ${_s}/sdk/include/um /imsvc ${_s}/sdk/include/shared /imsvc ${_s}/sdk/include/winrt")
set(CMAKE_C_FLAGS_INIT "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags}")
set(CMAKE_RC_FLAGS_INIT "-I ${_s}/sdk/include/um -I ${_s}/sdk/include/shared")
set(_libs "/libpath:${_s}/crt/lib/x86_64 /libpath:${_s}/sdk/lib/um/x86_64 /libpath:${_s}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_libs}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_libs}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_libs}")

# Only the release runtime: Visual C++'s debug one is not redistributable, xwin leaves it out.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
