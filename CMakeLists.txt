# gcn-recomp: a GameCube static-recompilation toolkit. A game project adds this directory
# and calls gcn_add_game() with what is specific to its game; everything else -- the
# PowerPC -> C recompiler, the hardware runtime, the renderer and the frontend -- is here.
cmake_minimum_required(VERSION 3.21)
project(gcn_recomp C CXX)

# Language levels are set per target: gcn_add_game() runs in the game project's scope,
# where directory-wide settings made here would not apply.
function(_gcn_set_standards target)
  set_target_properties(${target} PROPERTIES C_STANDARD 11 CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endfunction()

# See ENTER()/RET() in runtime/recomp.h. ON maintains a guest call stack, which answers
# "which guest function did that?" and gives a crash dump something to print.
option(GCN_TRACE_CALLS "Maintain a guest call stack (diagnostics)" OFF)
option(GCN_APP "Build the game as an app to hand out: no disc, no game code, everything it needs inside (a .app on macOS)" OFF)

# gcn_add_game(
#   NAME <target>              executable name
#   TITLE <string>             shown in the window title and messages
#   DISC_ID <6 chars>          e.g. GM8E01; the booted disc is checked against it
#   DOL_SHA1 <hex>             the main.dol SYMBOLS describes
#   SYMBOLS <file>             decomp-toolkit symbols.txt for that DOL
#   TABLES <dir>               hle.txt, names.txt, patches.txt, special_calls.txt, idle.txt,
#                              versions.txt
#   [SOURCES <files>...]       the game project's own runtime sources, compiled into the
#                              game executable: hooks the runtime offers a game (an HLE,
#                              g_widescreen in runtime/runtime.h), installed from a static
#                              initializer
#   [UI <dir>]                 the game's own files for the menus, copied over the
#                              defaults (runtime/ui/assets): its theme.rcss, typically
#   [APP_NAME <name>]          with GCN_APP: the app's name (default NAME), which is also
#                              where it keeps the player's state
#   [APP_ID <id>]              with GCN_APP: its bundle identifier, e.g. io.github.me.game
#   [LICENSES <files>...]      with GCN_APP: the game project's licences, shipped with the
#                              libraries' in the app
#   [APP_ICON <path>]          its icon, without extension: <path>.icns (the macOS app),
#                              .ico (the Windows executable), .png (the window, elsewhere)
#   [UPDATE_URL <url>]         with GCN_APP: the GitHub repository whose releases update the
#                              app (runtime/update.h), e.g. https://github.com/me/game
# )
# The executable holds no game code: it prepares the game from the player's disc on first run
# (runtime/prepare.h), with the toolchain (tools/fetch_toolchain.cmake) -- an app's own, or
# for a development build the one fetched here into the build directory, GCN_TOOLCHAIN_DIR
# then, with GCN_PYTHON its Python, for the game project's tests.
function(gcn_add_game)
  cmake_parse_arguments(G "" "NAME;TITLE;DISC_ID;DOL_SHA1;SYMBOLS;TABLES;UI;APP_NAME;APP_ID;APP_ICON;UPDATE_URL" "SOURCES;LICENSES" ${ARGN})
  foreach(req NAME TITLE DISC_ID DOL_SHA1 SYMBOLS TABLES)
    if(NOT G_${req})
      message(FATAL_ERROR "gcn_add_game: ${req} is required")
    endif()
  endforeach()
  if(G_APP_NAME)
    set(_app ${G_APP_NAME})
  else()
    set(_app ${G_NAME})
  endif()
  # This function runs in the game project's scope, so it locates its own files from
  # where it was defined rather than from any variable set alongside it.
  set(GCN_RECOMP_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  set(RT ${GCN_RECOMP_DIR}/runtime)

  # The system and architecture this builds for: one of each per build.
  if(APPLE)
    set(_os macos)
  elseif(WIN32)
    set(_os windows)
  else()
    set(_os linux)
  endif()
  if(CMAKE_OSX_ARCHITECTURES)
    set(_arch ${CMAKE_OSX_ARCHITECTURES})
  elseif(MSVC)
    set(_arch ${CMAKE_CXX_COMPILER_ARCHITECTURE_ID})
  else()
    set(_arch ${CMAKE_SYSTEM_PROCESSOR})
  endif()
  if(_arch MATCHES "^(arm64|aarch64|ARM64)$")
    set(_arch aarch64)
  else()
    set(_arch x86_64)
  endif()

  # The toolchain that prepares the game, for a development build: as an app's, so that this
  # machine needs neither a C compiler nor Python for it. Fetched again when the script
  # changes (a new version).
  set(_toolchain "")
  if(NOT GCN_APP)
    set(_toolchain ${CMAKE_BINARY_DIR}/toolchain)
    file(SHA1 ${GCN_RECOMP_DIR}/tools/fetch_toolchain.cmake _stamp)
    if(NOT EXISTS ${_toolchain}/fetched-${_stamp})
      execute_process(COMMAND ${CMAKE_COMMAND} -DDEST=${_toolchain} -DOS=${_os} -DARCH=${_arch}
                              -P ${GCN_RECOMP_DIR}/tools/fetch_toolchain.cmake
                      COMMAND_ERROR_IS_FATAL ANY)
      file(TOUCH ${_toolchain}/fetched-${_stamp})
    endif()
    set(GCN_TOOLCHAIN_DIR ${_toolchain} PARENT_SCOPE)
    if(WIN32)
      set(GCN_PYTHON ${_toolchain}/python/python.exe PARENT_SCOPE)
    else()
      set(GCN_PYTHON ${_toolchain}/python/bin/python3 PARENT_SCOPE)
    endif()
  endif()

  file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${RT}/*.cpp)

  # Aurora (github.com/encounter/aurora, MIT): the window, input and SDL3, the GPU through
  # WebGPU (Dawn, prebuilt: Metal, D3D12, Vulkan) and its GX command processor, and RmlUi for
  # the menus. Only the modules the game uses are built: the game's own SDK runs recompiled,
  # so Aurora's DVD, CARD, THP and the rest are not wanted. A few fixes not yet upstream are
  # patches/aurora-*.patch, applied from the pinned commit each time.
  include(FetchContent)
  set(AURORA_ENABLE_RMLUI ON CACHE BOOL "" FORCE)
  set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
  set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
  set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
  set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  # SDL3 built here and linked in, everywhere: the same one on every system, and an app
  # carries it rather than depending on one installed.
  set(AURORA_SDL3_PROVIDER vendor CACHE STRING "" FORCE)
  set(AURORA_SDL3_LINKAGE static CACHE STRING "" FORCE)
  if(GCN_APP)
    # Nor anything else from the system it is built on (Homebrew, pkg-config): Aurora then
    # builds each of these from source. zlib is the system's, which every system has.
    foreach(pkg absl xxHash fmt PNG Freetype SQLite3 zstd Tracy PkgConfig)
      set(CMAKE_DISABLE_FIND_PACKAGE_${pkg} TRUE)
    endforeach()
    if(APPLE)
      set(CMAKE_IGNORE_PREFIX_PATH /opt/homebrew /usr/local)
    endif()
  endif()
  FetchContent_Declare(aurora
    GIT_REPOSITORY https://github.com/encounter/aurora.git
    GIT_TAG 40cf48fce6ce85366608596bae018c6188f2b2e7   # 2026-10-09
    PATCH_COMMAND git checkout -q -- . && git apply --ignore-whitespace
                  ${GCN_RECOMP_DIR}/patches/aurora-srtg-lit-color.patch
                  ${GCN_RECOMP_DIR}/patches/aurora-blocking-pipelines.patch
                  ${GCN_RECOMP_DIR}/patches/aurora-rmlui-hooks.patch
                  ${GCN_RECOMP_DIR}/patches/aurora-card-game-name.patch
                  ${GCN_RECOMP_DIR}/patches/aurora-msaa-depth-copy.patch
                  ${GCN_RECOMP_DIR}/patches/aurora-missing-vertex-color.patch
                  ${GCN_RECOMP_DIR}/patches/aurora-scissor-offset.patch
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(aurora)

  # nod (github.com/encounter/nod, MIT): reads every disc image format -- iso, ciso, gcz,
  # rvz, wia, wbfs, nfs, tgc, nkit -- under runtime/disc.cpp. Its prebuilt static library.
  set(_nod_sha256_macos-arm64 00c0625f0061b1d5c2211504bc971bd0740092d4018098f541f1941f2ac38b7d)
  set(_nod_sha256_macos-x86_64 50f28fdf7488c5f81e359440dd698abb2e12fa0b3ea377fc1262d76da9ff2aa7)
  set(_nod_sha256_linux-x86_64 920adb1d2386c8728e0521f8d52809057c04bd46913433a0000ee0931d08d69f)
  set(_nod_sha256_linux-aarch64 6872dd3d45d9e688012db30f0a75b10702b8520b5b550ea6b50e9f6d458b05b3)
  set(_nod_sha256_windows-x86_64 c9dfecf8e52066a5222f3cb1cad18d0740c958ce65bf60cb84e780a3856ed881)
  set(_nod_sha256_windows-arm64 24f3a5d2aea299415438917d0078b48731af66f95ff17a6cf6a47ea6993b6c0f)
  set(_nod ${_os}-${_arch})
  if(NOT _os STREQUAL linux)
    string(REPLACE aarch64 arm64 _nod ${_nod})
  endif()
  FetchContent_Declare(gcn_nod
    URL https://github.com/encounter/nod/releases/download/v2.0.0-alpha.12/libnod-${_nod}.tar.gz
    URL_HASH SHA256=${_nod_sha256_${_nod}}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(gcn_nod)
  if(NOT TARGET gcn_nod)
    add_library(gcn_nod STATIC IMPORTED GLOBAL)
    if(WIN32)
      set_target_properties(gcn_nod PROPERTIES IMPORTED_LOCATION ${gcn_nod_SOURCE_DIR}/lib/nod_static.lib)
      set_property(TARGET gcn_nod PROPERTY INTERFACE_LINK_LIBRARIES ntdll userenv ws2_32 dbghelp bcrypt)
    else()
      set_target_properties(gcn_nod PROPERTIES IMPORTED_LOCATION ${gcn_nod_SOURCE_DIR}/lib/libnod.a)
    endif()
    set_target_properties(gcn_nod PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${gcn_nod_SOURCE_DIR}/include)
    if(_os STREQUAL linux)
      find_package(Threads REQUIRED)
      set_property(TARGET gcn_nod PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads ${CMAKE_DL_LIBS} m)
    endif()
  endif()

  # The menus' documents beside the executable, as ui/ (in an app, its Resources/ui):
  # ours, then the game's over them.
  if(GCN_APP AND APPLE)
    set(_ui_dest $<TARGET_BUNDLE_CONTENT_DIR:${G_NAME}>/Resources/ui)
  elseif(GCN_APP)
    set(_ui_dest $<TARGET_FILE_DIR:${G_NAME}>/resources/ui)
  else()
    set(_ui_dest $<TARGET_FILE_DIR:${G_NAME}>/ui)
  endif()
  set(_ui_copy COMMAND ${CMAKE_COMMAND} -E copy_directory ${RT}/ui/assets ${_ui_dest})
  if(G_UI)
    list(APPEND _ui_copy COMMAND ${CMAKE_COMMAND} -E copy_directory ${G_UI} ${_ui_dest})
  endif()
  if(G_APP_ICON)
    list(APPEND _ui_copy COMMAND ${CMAKE_COMMAND} -E copy ${G_APP_ICON}.png ${_ui_dest}/icon.png)
  endif()
  add_custom_target(${G_NAME}_ui ${_ui_copy} VERBATIM)

  # The game: the runtime and the game project's code, with no game code of its own -- that
  # is a module it loads (runtime/game_module.h) -- so it builds without a disc image. Its
  # symbols are exported for the module to call.
  add_executable(${G_NAME} ${RUNTIME_SOURCES} ${G_SOURCES})
  _gcn_set_standards(${G_NAME})
  target_include_directories(${G_NAME} PRIVATE ${RT} ${aurora_SOURCE_DIR}/lib)
  target_link_libraries(${G_NAME} PRIVATE aurora::core aurora::gx aurora::vi aurora::pad aurora::card gcn_nod ${CMAKE_DL_LIBS})
  # The system's own file picker (runtime/ui/pick_file.h): AppKit on macOS, comdlg32 on
  # Windows; dbghelp symbolizes a crash's backtrace there.
  if(APPLE)
    target_sources(${G_NAME} PRIVATE ${RT}/ui/pick_file_mac.mm)
    set_source_files_properties(${RT}/ui/pick_file_mac.mm PROPERTIES LANGUAGE CXX COMPILE_FLAGS "-x objective-c++ -fobjc-arc")
    target_link_libraries(${G_NAME} PRIVATE "-framework AppKit" "-framework UniformTypeIdentifiers")
  elseif(WIN32)
    target_link_libraries(${G_NAME} PRIVATE comdlg32 dbghelp)
    if(G_APP_ICON)
      file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${G_NAME}_icon.rc "1 ICON \"${G_APP_ICON}.ico\"\n")
      target_sources(${G_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${G_NAME}_icon.rc)
    endif()
    # A DLL cannot leave symbols for the executable to fill in as it loads: the module
    # links with the executable's import library instead, of everything it defines.
    set_target_properties(${G_NAME} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    if(NOT CMAKE_HOST_WIN32)   # built from Linux: its variables at zero, tools/export_bss.cmake
      set(_bss_def ${CMAKE_CURRENT_BINARY_DIR}/${G_NAME}_bss.def)
      if(NOT EXISTS ${_bss_def})
        file(WRITE ${_bss_def} "EXPORTS\n")
      endif()
      target_sources(${G_NAME} PRIVATE ${_bss_def})
      add_custom_command(TARGET ${G_NAME} PRE_LINK
        COMMAND ${CMAKE_COMMAND} -DOBJS=${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${G_NAME}.dir/exports.def.objs
                -DNM=${CMAKE_NM} -DOUT=${_bss_def} -P ${GCN_RECOMP_DIR}/tools/export_bss.cmake VERBATIM)
    endif()
    target_compile_definitions(${G_NAME} PRIVATE GCN_RES_IMPORT_LIB="$<TARGET_LINKER_FILE:${G_NAME}>")
  endif()
  set_target_properties(${G_NAME} PROPERTIES ENABLE_EXPORTS ON)
  add_dependencies(${G_NAME} ${G_NAME}_ui)
  if(MSVC)
    target_compile_options(${G_NAME} PRIVATE /W3 /wd4244 /wd4267)
  else()
    target_compile_options(${G_NAME} PRIVATE -Wall -Wno-unused-function)
  endif()
  # What the runtime knows about the game it was built for, and where a development build
  # finds what preparing the game uses (runtime/prepare.h): the source tree and the
  # toolchain above. An app points GCN_RESOURCES at its own copy.
  target_compile_definitions(${G_NAME} PRIVATE
    GCN_GAME_TITLE="${G_TITLE}"
    GCN_DISC_ID="${G_DISC_ID}"
    GCN_GAME_NAME="${G_NAME}"
    GCN_APP_NAME="${_app}"
    GCN_DOL_SHA1="${G_DOL_SHA1}"
    GCN_RES_RECOMP="${GCN_RECOMP_DIR}/recomp"
    GCN_RES_TABLES="${G_TABLES}"
    GCN_RES_SYMBOLS="${G_SYMBOLS}"
    GCN_RES_INCLUDE="${RT}"
    GCN_RES_TOOLCHAIN="${_toolchain}")
  # The CPU struct grows with the call trace: the runtime and the module, which prepare.cpp
  # builds with the same flag, must agree on the layout.
  if(GCN_TRACE_CALLS)
    target_compile_definitions(${G_NAME} PRIVATE GCN_TRACE_CALLS)
  endif()

  # Updates (runtime/update.h): Velopack's C/C++ library, in an app that says where its
  # releases are. Its shared library, beside the executable (in a macOS app, in Frameworks):
  # the static one would bring a second copy of Rust's standard library, nod having one.
  if(GCN_APP AND G_UPDATE_URL)
    FetchContent_Declare(gcn_velopack
      URL https://github.com/velopack/velopack/releases/download/1.2.161/velopack_libc_1.2.161.zip
      URL_HASH SHA256=31de8f2cf05a47bd5328a9f98b0c41aec1582156f6075f1058b26941f7f17c20
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(gcn_velopack)
    set(_vpk ${gcn_velopack_SOURCE_DIR})
    set(_vpk_arch x64)
    if(_arch STREQUAL aarch64)
      set(_vpk_arch arm64)
    endif()
    add_library(gcn_velopack SHARED IMPORTED)
    set_target_properties(gcn_velopack PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${_vpk}/include)
    if(WIN32)
      # Under the name its import library gives it; the app's copy of its DLLs (below) puts it
      # beside the executable.
      configure_file(${_vpk}/lib/velopack_libc_win_${_vpk_arch}_msvc.dll ${CMAKE_BINARY_DIR}/velopack/velopack_libc.dll COPYONLY)
      set_target_properties(gcn_velopack PROPERTIES IMPORTED_LOCATION ${CMAKE_BINARY_DIR}/velopack/velopack_libc.dll
        IMPORTED_IMPLIB ${_vpk}/lib/velopack_libc_win_${_vpk_arch}_msvc.dll.lib)
    elseif(APPLE)
      # Named for where the app finds it, its Frameworks (the one shipped names the build's
      # machine).
      set(_lib ${CMAKE_BINARY_DIR}/velopack/libvelopack_libc.dylib)
      configure_file(${_vpk}/lib/velopack_libc_osx.dylib ${_lib} COPYONLY)
      execute_process(COMMAND install_name_tool -id @rpath/libvelopack_libc.dylib ${_lib} COMMAND_ERROR_IS_FATAL ANY)
      set_target_properties(gcn_velopack PROPERTIES IMPORTED_LOCATION ${_lib})
      set_target_properties(${G_NAME} PROPERTIES SKIP_BUILD_RPATH ON)
      target_link_options(${G_NAME} PRIVATE -Wl,-rpath,@executable_path/../Frameworks)
      add_custom_command(TARGET ${G_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_lib} $<TARGET_BUNDLE_CONTENT_DIR:${G_NAME}>/Frameworks/libvelopack_libc.dylib)
    else()
      # Beside the executable, found there ($ORIGIN). It has no soname: linked as -l.
      configure_file(${_vpk}/lib/velopack_libc_linux_${_vpk_arch}_gnu.so ${CMAKE_BINARY_DIR}/libvelopack_libc.so COPYONLY)
      set_target_properties(gcn_velopack PROPERTIES IMPORTED_LOCATION ${CMAKE_BINARY_DIR}/libvelopack_libc.so IMPORTED_NO_SONAME ON)
      set_target_properties(${G_NAME} PROPERTIES BUILD_RPATH_USE_ORIGIN ON)
    endif()
    target_link_libraries(${G_NAME} PRIVATE gcn_velopack)
    target_compile_definitions(${G_NAME} PRIVATE GCN_UPDATE_URL="${G_UPDATE_URL}")
  endif()

  # The app: what preparing the game on the player's machine needs (runtime/prepare.h: the
  # recompiler, the tables, the symbols, the headers the generated code includes), the menus
  # and the licences, in <APP_NAME>.app/Contents/Resources on macOS, in resources/ beside the
  # executable elsewhere -- the toolchain is the packaging's to add. main.cpp finds them
  # there, and keeps the player's state in the system's place for it (Application Support,
  # ~/.local/share, %APPDATA%).
  if(GCN_APP)
    if(APPLE)
      set_target_properties(${G_NAME} PROPERTIES MACOSX_BUNDLE ON OUTPUT_NAME "${_app}"
        MACOSX_BUNDLE_INFO_PLIST ${RT}/app/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "${_app}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${G_APP_ID}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
      set(_res $<TARGET_BUNDLE_CONTENT_DIR:${G_NAME}>/Resources)
      if(G_APP_ICON)
        get_filename_component(_icon_name ${G_APP_ICON} NAME)
        set_target_properties(${G_NAME} PROPERTIES MACOSX_BUNDLE_ICON_FILE ${_icon_name}.icns)
        add_custom_command(TARGET ${G_NAME} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy ${G_APP_ICON}.icns ${_res}/${_icon_name}.icns VERBATIM)
      endif()
    else()
      set_target_properties(${G_NAME} PROPERTIES OUTPUT_NAME "${_app}")
      set(_res $<TARGET_FILE_DIR:${G_NAME}>/resources)
    endif()
    if(WIN32)
      # A window and no console; and beside the executable the DLLs it needs that Windows does
      # not have: those of the libraries built as DLLs (Dawn, libpng...), the shader compiler
      # Dawn loads (DXC, which Aurora asks for), and Visual C++'s runtime, so that it starts
      # on any PC.
      set_target_properties(${G_NAME} PROPERTIES WIN32_EXECUTABLE ON)
      target_link_options(${G_NAME} PRIVATE /ENTRY:mainCRTStartup)
      if(CMAKE_HOST_WIN32)
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
        include(InstallRequiredSystemLibraries)
      else()   # built from Linux, tools/windows-cross.cmake: its copy of the runtime
        file(GLOB CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${MSVC_REDIST_DIR}/x64/Microsoft.VC143.CRT/msvcp*.dll
             ${MSVC_REDIST_DIR}/x64/Microsoft.VC143.CRT/vcruntime*.dll ${MSVC_REDIST_DIR}/x64/Microsoft.VC143.CRT/concrt*.dll)
      endif()
      include(${aurora_SOURCE_DIR}/cmake/AuroraCopyRuntimeDLLs.cmake)
      aurora_copy_runtime_dlls(${G_NAME})
      add_custom_command(TARGET ${G_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} $<TARGET_FILE_DIR:${G_NAME}>)
    endif()
    file(GLOB _recomp_py ${GCN_RECOMP_DIR}/recomp/*.py)
    set(_game_licenses)
    if(G_UPDATE_URL)
      list(APPEND _game_licenses COMMAND ${CMAKE_COMMAND} -E copy ${RT}/third_party/velopack-LICENSE.txt ${_res}/licenses/Velopack.txt)
    endif()
    if(G_LICENSES)
      list(APPEND _game_licenses COMMAND ${CMAKE_COMMAND} -E copy ${G_LICENSES} ${_res}/licenses/)
    endif()
    # What the app links in from Aurora's dependencies, with their licences.
    # (Those not built here, found on the system, are skipped.)
    foreach(dep rmlui freetype sdl imgui fmt abseil-cpp xxhash zstd png)
      FetchContent_GetProperties(${dep})
    endforeach()
    set(_dep_licenses)
    foreach(lic "Aurora.txt|${aurora_SOURCE_DIR}/LICENSE" "RmlUi.txt|${rmlui_SOURCE_DIR}/LICENSE.txt"
                "FreeType.txt|${freetype_SOURCE_DIR}/docs/FTL.TXT" "SDL3.txt|${sdl_SOURCE_DIR}/LICENSE.txt"
                "Dawn.txt|${RT}/third_party/dawn-LICENSE.txt" "imgui.txt|${imgui_SOURCE_DIR}/LICENSE.txt"
                "fmt.txt|${fmt_SOURCE_DIR}/LICENSE" "abseil.txt|${abseil-cpp_SOURCE_DIR}/LICENSE"
                "xxHash.txt|${xxhash_SOURCE_DIR}/LICENSE" "zstd.txt|${zstd_SOURCE_DIR}/LICENSE"
                "libpng.txt|${png_SOURCE_DIR}/LICENSE")
      string(REPLACE "|" ";" lic ${lic})
      list(GET lic 0 _to)
      list(GET lic 1 _from)
      if(EXISTS ${_from})
        list(APPEND _dep_licenses COMMAND ${CMAKE_COMMAND} -E copy ${_from} ${_res}/licenses/${_to})
      endif()
    endforeach()
    # The other versions' symbols beside the build's, symbols-<DOL SHA-1, 8 digits>.txt
    # (recomp.py takes the one for the player's main.dol).
    get_filename_component(_sym_dir ${G_SYMBOLS} DIRECTORY)
    get_filename_component(_sym_name ${G_SYMBOLS} NAME_WE)
    file(GLOB _ver_syms ${_sym_dir}/${_sym_name}-*.txt)
    set(_ver_sym_copies)
    foreach(_vs ${_ver_syms})
      get_filename_component(_vs_name ${_vs} NAME)
      string(REPLACE "${_sym_name}-" "symbols-" _vs_name ${_vs_name})
      list(APPEND _ver_sym_copies COMMAND ${CMAKE_COMMAND} -E copy ${_vs} ${_res}/${_vs_name})
    endforeach()
    add_custom_command(TARGET ${G_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory ${_res}/recomp ${_res}/include ${_res}/licenses
      COMMAND ${CMAKE_COMMAND} -E copy ${_recomp_py} ${_res}/recomp/
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${G_TABLES} ${_res}/tables
      COMMAND ${CMAKE_COMMAND} -E copy ${G_SYMBOLS} ${_res}/symbols.txt
      ${_ver_sym_copies}
      COMMAND ${CMAKE_COMMAND} -E copy ${RT}/recomp.h ${RT}/compat.h ${_res}/include/
      ${_dep_licenses}
      COMMAND ${CMAKE_COMMAND} -E copy ${RT}/third_party/nod-LICENSE-MIT.txt ${_res}/licenses/nod.txt
      COMMAND ${CMAKE_COMMAND} -E copy ${RT}/ui/assets/fonts/OFL.txt ${_res}/licenses/FiraSans-OFL.txt
      ${_game_licenses}
      VERBATIM)
    if(WIN32)
      add_custom_command(TARGET ${G_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_res}/lib
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_LINKER_FILE:${G_NAME}> ${_res}/lib/${G_NAME}.lib
        VERBATIM)
    endif()
  endif()
endfunction()
