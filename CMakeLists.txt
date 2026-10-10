# gcn-recomp: a GameCube static-recompilation toolkit. A game project adds this directory
# and calls gcn_add_game() with what is specific to its game; everything else -- the
# PowerPC -> C recompiler, the hardware runtime, the renderer and the frontends -- is here.
cmake_minimum_required(VERSION 3.20)
project(gcn_recomp C CXX)

# Language levels are set per target: gcn_add_game() runs in the game project's scope,
# where directory-wide settings made here would not apply.
function(_gcn_set_standards target)
  set_target_properties(${target} PROPERTIES C_STANDARD 11 CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endfunction()

# GCN_BENCH_ONLY builds just the headless benchmark: no GPU side, no SDL. That is what a
# cross-build for a device uses, since it only needs a C++ toolchain and the recompiled
# code (and Aurora's headers, for the stream it would be sent).
option(GCN_BENCH_ONLY "Build only the headless benchmark (no SDL, no GPU)" OFF)
# See ENTER()/RET() in runtime/recomp.h. ON maintains a guest call stack, which answers
# "which guest function did that?" and gives a crash dump something to print.
option(GCN_TRACE_CALLS "Maintain a guest call stack (diagnostics)" OFF)
option(GCN_APP "Build the game as an app to hand out: no disc, no game code, everything it needs inside (a .app on macOS)" OFF)
option(GCN_GUEST_CHECKS "Call the game project's consistency checks at every poll, DMA and frame (diagnostics)" OFF)

# gcn_add_game(
#   NAME <target>              executable name; the benchmark is <target>_bench
#   TITLE <string>             shown in the window title and messages
#   DISC_ID <6 chars>          e.g. GM8E01; the image and the booted disc are checked against it
#   DOL_SHA1 <hex>             the supported build's main.dol
#   SYMBOLS <file>             decomp-toolkit symbols.txt for that DOL
#   TABLES <dir>               hle.txt, names.txt, patches.txt, special_calls.txt, idle.txt
#   FPS <n>                    the game's frame rate, for the benchmark's percentage
#   [IMAGE_DIR <dir>]          where to look for the user's .iso/.ciso (default rom/)
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
# )
# The user's image may also be given as -DGAME_ISO=<path>.
function(gcn_add_game)
  cmake_parse_arguments(G "" "NAME;TITLE;DISC_ID;DOL_SHA1;SYMBOLS;TABLES;FPS;IMAGE_DIR;UI;APP_NAME;APP_ID;APP_ICON" "SOURCES;LICENSES" ${ARGN})
  foreach(req NAME TITLE DISC_ID DOL_SHA1 SYMBOLS TABLES FPS)
    if(NOT G_${req})
      message(FATAL_ERROR "gcn_add_game: ${req} is required")
    endif()
  endforeach()
  if(NOT G_IMAGE_DIR)
    set(G_IMAGE_DIR ${CMAKE_SOURCE_DIR}/rom)
  endif()
  # This function runs in the game project's scope, so it locates its own files from
  # where it was defined rather than from any variable set alongside it.
  set(GCN_RECOMP_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})

  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  # The game image is supplied by the user: an uncompressed .iso or a .ciso.
  file(GLOB _rom_images ${G_IMAGE_DIR}/*.iso ${G_IMAGE_DIR}/*.ciso)
  list(LENGTH _rom_images _rom_count)
  if(_rom_count GREATER 0)
    list(GET _rom_images 0 _default_iso)
  else()
    set(_default_iso "")
  endif()
  set(GAME_ISO "${_default_iso}" CACHE FILEPATH "Your ${G_TITLE} disc image (.iso or .ciso)")
  if(G_APP_NAME)
    set(_app ${G_APP_NAME})
  else()
    set(_app ${G_NAME})
  endif()
  # An app is handed out: it is built from no disc and knows of none.
  if(GCN_APP)
    set(GAME_ISO "")
  endif()

  # Without an image we can still compile the runtime, which is enough to check a
  # port. Only the recompiled game code and the final link need the disc.
  if(GAME_ISO AND EXISTS "${GAME_ISO}")
    set(HAVE_GAME_ISO TRUE)
  else()
    set(HAVE_GAME_ISO FALSE)
    if(GAME_ISO)
      message(WARNING "GAME_ISO is set to '${GAME_ISO}' but that file does not exist.")
    endif()
    message(STATUS
      "No game image found: the game builds without its module, and prepares one from the "
      "disc on first run (runtime/prepare.h). Put your own ${G_TITLE} .iso or .ciso in "
      "${G_IMAGE_DIR}/ or pass -DGAME_ISO=/path/to/game.iso to build the module here too.")
  endif()

  set(GAME_DOL ${CMAKE_BINARY_DIR}/main.dol)
  set(RECOMP_GEN_DIR ${CMAKE_BINARY_DIR}/gen)
  set(RT ${GCN_RECOMP_DIR}/runtime)

  # ---- Recompiled game code ----
  if(HAVE_GAME_ISO)
    execute_process(COMMAND ${Python3_EXECUTABLE} ${GCN_RECOMP_DIR}/recomp/extract_dol.py
                            "${GAME_ISO}" ${GAME_DOL} ${G_DISC_ID} ${G_DOL_SHA1}
                    RESULT_VARIABLE EXTRACT_RESULT)
    if(NOT EXTRACT_RESULT EQUAL 0)
      message(FATAL_ERROR "could not extract main.dol from ${GAME_ISO}")
    endif()

    file(GLOB RECOMP_TOOL_SOURCES ${GCN_RECOMP_DIR}/recomp/*.py ${G_TABLES}/*.txt)
    execute_process(COMMAND ${Python3_EXECUTABLE} ${GCN_RECOMP_DIR}/recomp/recomp.py
                            ${GAME_DOL} ${G_SYMBOLS} ${RECOMP_GEN_DIR} ${G_TABLES}
                    RESULT_VARIABLE RECOMP_RESULT)
    if(NOT RECOMP_RESULT EQUAL 0)
      message(FATAL_ERROR "recompilation failed")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_TOOL_SOURCES} ${GAME_ISO} ${G_SYMBOLS})
    include(${RECOMP_GEN_DIR}/sources.cmake)

    # Objects, built position-independent: they become the module the runtime loads
    # (runtime/game_module.h).
    add_library(game_recomp OBJECT ${RECOMP_SOURCES})
    set_target_properties(game_recomp PROPERTIES POSITION_INDEPENDENT_CODE ON)
    _gcn_set_standards(game_recomp)
    target_include_directories(game_recomp PUBLIC ${RT} ${RECOMP_GEN_DIR})
    # PUBLIC because the CPU struct grows with the call trace: the runtime and the
    # generated code must agree on the layout.
    if(GCN_TRACE_CALLS)
      target_compile_definitions(game_recomp PUBLIC GCN_TRACE_CALLS)
    endif()
    if(MSVC)
      # Generated code is enormous and machine-written: keep the optimizer honest about
      # aliasing and floating point, and silence the noise it legitimately produces.
      target_compile_options(game_recomp PRIVATE /O2 /fp:precise /fp:except- /wd4101 /wd4102 /wd4723 /bigobj)
    else()
      target_compile_options(game_recomp PRIVATE -O2 -fno-strict-aliasing -Wno-unused-label -Wno-unused-variable
                             -Wno-parentheses-equality -ffp-contract=off)
    endif()

    # The game's code as a module beside the executable, for a development run. An app
    # ships without it and prepares one on the player's machine (runtime/prepare.h); the
    # flags there must stay the ones above.
    add_library(${G_NAME}_game MODULE $<TARGET_OBJECTS:game_recomp>)
    set_target_properties(${G_NAME}_game PROPERTIES PREFIX "" LINKER_LANGUAGE C
                          LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    if(APPLE)
      set_target_properties(${G_NAME}_game PROPERTIES SUFFIX ".dylib")
      target_link_options(${G_NAME}_game PRIVATE -undefined dynamic_lookup)
    elseif(WIN32)
      # A DLL: it takes the runtime's data as imported, links with the executable's import
      # library (below, once the executable exists), and exports what game_module.cpp
      # looks up -- as game_module_build does on a player's machine.
      target_compile_definitions(game_recomp PRIVATE GCN_MODULE_DLL)
      target_link_options(${G_NAME}_game PRIVATE /EXPORT:g_recomp_funcs,DATA /EXPORT:g_recomp_func_count,DATA
                          /EXPORT:g_recomp_code_base,DATA /EXPORT:g_recomp_code_end,DATA /EXPORT:orig_OSSaveContext)
    endif()
  endif()

  # ---- Runtime ----
  file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${RT}/*.cpp)

  # Alternative entry points and the SDL-free audio stub: none of these may land in a
  # target that already has main.cpp or audio.cpp.
  list(REMOVE_ITEM RUNTIME_SOURCES ${RT}/bench_main.cpp ${RT}/audio_stub.cpp ${RT}/hle_card_stub.cpp)

  # Everything below the GPU side and the SDL frontend -- the CPU, OS, hardware and the GX
  # stream translated for Aurora (gx/aurora_bridge.cpp, which never calls it) -- is free of
  # both the GPU and SDL.
  set(CORE_SOURCES ${RUNTIME_SOURCES})
  list(REMOVE_ITEM CORE_SOURCES ${RT}/main.cpp ${RT}/audio.cpp ${RT}/audio_sdl.cpp ${RT}/png.cpp
    ${RT}/gx/aurora_gpu.cpp ${RT}/gx/packs.cpp ${RT}/pad_bindings.cpp ${RT}/hle_card.cpp ${RT}/crash_log.cpp)
  list(FILTER CORE_SOURCES EXCLUDE REGEX "/runtime/ui/")
  list(APPEND CORE_SOURCES ${RT}/audio_stub.cpp ${RT}/hle_card_stub.cpp)
  set(BENCH_SOURCES ${CORE_SOURCES} ${RT}/bench_main.cpp)

  # Aurora (github.com/encounter/aurora, MIT): the window, input and SDL3, the GPU through
  # WebGPU (Dawn, prebuilt: Metal, D3D12, Vulkan) and its GX command processor, and RmlUi for
  # the menus. Only the modules the game uses are built: the game's own SDK runs recompiled,
  # so Aurora's DVD, CARD, THP and the rest are not wanted. A few fixes not yet upstream are
  # patches/aurora-*.patch, applied from the pinned commit each time. The benchmark only
  # takes the headers of its command stream (GCN_BENCH_ONLY: nothing of it is built).
  include(FetchContent)
  if(GCN_BENCH_ONLY)
    set(_aurora_build SOURCE_SUBDIR none)
  else()
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
    ${_aurora_build}
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(aurora)

  # nod (github.com/encounter/nod, MIT): reads every disc image format -- iso, ciso, gcz,
  # rvz, wia, wbfs, nfs, tgc, nkit -- under runtime/disc.cpp. Its prebuilt static libraries,
  # one per platform; a universal macOS build joins its two.
  include(FetchContent)
  set(_nod_sha256_macos-arm64 00c0625f0061b1d5c2211504bc971bd0740092d4018098f541f1941f2ac38b7d)
  set(_nod_sha256_macos-x86_64 50f28fdf7488c5f81e359440dd698abb2e12fa0b3ea377fc1262d76da9ff2aa7)
  set(_nod_sha256_linux-x86_64 920adb1d2386c8728e0521f8d52809057c04bd46913433a0000ee0931d08d69f)
  set(_nod_sha256_linux-aarch64 6872dd3d45d9e688012db30f0a75b10702b8520b5b550ea6b50e9f6d458b05b3)
  set(_nod_sha256_windows-x86_64 c9dfecf8e52066a5222f3cb1cad18d0740c958ce65bf60cb84e780a3856ed881)
  set(_nod_sha256_windows-arm64 24f3a5d2aea299415438917d0078b48731af66f95ff17a6cf6a47ea6993b6c0f)
  if(APPLE)
    set(_nod_archs ${CMAKE_OSX_ARCHITECTURES})
    if(NOT _nod_archs)
      set(_nod_archs ${CMAKE_SYSTEM_PROCESSOR})
    endif()
    list(TRANSFORM _nod_archs PREPEND macos-)
  elseif(WIN32)
    if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
      set(_nod_archs windows-arm64)
    else()
      set(_nod_archs windows-x86_64)
    endif()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_nod_archs linux-aarch64)
  else()
    set(_nod_archs linux-x86_64)
  endif()
  set(_nod_libs "")
  foreach(_p ${_nod_archs})
    FetchContent_Declare(nod_${_p}
      URL https://github.com/encounter/nod/releases/download/v2.0.0-alpha.12/libnod-${_p}.tar.gz
      URL_HASH SHA256=${_nod_sha256_${_p}}
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(nod_${_p})
    set(_nod_dir ${nod_${_p}_SOURCE_DIR})
    if(WIN32)
      list(APPEND _nod_libs ${_nod_dir}/lib/nod_static.lib)
    else()
      list(APPEND _nod_libs ${_nod_dir}/lib/libnod.a)
    endif()
  endforeach()
  list(LENGTH _nod_libs _n)
  if(_n GREATER 1)
    set(_nod_lib ${CMAKE_BINARY_DIR}/libnod-universal.a)
    execute_process(COMMAND lipo -create ${_nod_libs} -output ${_nod_lib} COMMAND_ERROR_IS_FATAL ANY)
  else()
    set(_nod_lib ${_nod_libs})
  endif()
  if(NOT TARGET gcn_nod)
    add_library(gcn_nod STATIC IMPORTED GLOBAL)
    set_target_properties(gcn_nod PROPERTIES IMPORTED_LOCATION ${_nod_lib}
      INTERFACE_INCLUDE_DIRECTORIES ${_nod_dir}/include)
    if(WIN32)
      set_property(TARGET gcn_nod PROPERTY INTERFACE_LINK_LIBRARIES ntdll userenv ws2_32 dbghelp bcrypt)
    elseif(NOT APPLE)
      find_package(Threads REQUIRED)
      set_property(TARGET gcn_nod PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads ${CMAKE_DL_LIBS} m)
    endif()
  endif()

  if(MSVC)
    set(GCN_RUNTIME_WARNINGS /W3 /wd4244 /wd4267)
  else()
    set(GCN_RUNTIME_WARNINGS -Wall -Wno-unused-function)
  endif()

  # Platform libraries the game needs beyond Aurora's: dbghelp to symbolize crash backtraces.
  set(GCN_PLATFORM_LIBS "")
  if(WIN32 AND NOT GCN_BENCH_ONLY)
    list(APPEND GCN_PLATFORM_LIBS dbghelp)
  endif()

  # What the runtime knows about the game it was built for.
  set(GCN_GAME_DEFS
    GCN_DEFAULT_ISO="${GAME_ISO}"
    GCN_GAME_TITLE="${G_TITLE}"
    GCN_DISC_ID="${G_DISC_ID}"
    GCN_GAME_FPS=${G_FPS}
    GCN_GAME_NAME="${G_NAME}"
    GCN_APP_NAME="${_app}"
    GCN_DOL_SHA1="${G_DOL_SHA1}"
    # What preparing the game on a player's machine uses (runtime/prepare.h), from the
    # source tree; an app points GCN_RESOURCES at its own copy.
    GCN_RES_RECOMP="${GCN_RECOMP_DIR}/recomp"
    GCN_RES_TABLES="${G_TABLES}"
    GCN_RES_SYMBOLS="${G_SYMBOLS}"
    GCN_RES_INCLUDE="${RT}")
  # The CPU struct grows with the call trace: the runtime here and the module -- built by
  # game_recomp, or by prepare.cpp, which passes it on -- must agree on the layout.
  if(GCN_TRACE_CALLS)
    list(APPEND GCN_GAME_DEFS GCN_TRACE_CALLS)
  endif()
  if(GCN_GUEST_CHECKS)
    list(APPEND GCN_GAME_DEFS GCN_GUEST_CHECKS)
  endif()

  # Headless benchmark: reports how fast the guest advances, with no graphics or audio
  # in the way. Used to compare host CPUs.
  # Like the game, it holds no game code: it loads the module (runtime/game_module.h).
  add_executable(${G_NAME}_bench ${BENCH_SOURCES})
  _gcn_set_standards(${G_NAME}_bench)
  target_include_directories(${G_NAME}_bench PRIVATE ${RT} ${RECOMP_GEN_DIR} ${aurora_SOURCE_DIR}/include)
  target_link_libraries(${G_NAME}_bench PRIVATE gcn_nod ${CMAKE_DL_LIBS})
  set_target_properties(${G_NAME}_bench PROPERTIES ENABLE_EXPORTS ON)
  if(TARGET ${G_NAME}_game)
    add_dependencies(${G_NAME}_bench ${G_NAME}_game)
  endif()
  target_compile_options(${G_NAME}_bench PRIVATE ${GCN_RUNTIME_WARNINGS})
  # Aurora's headers as its own targets read them (the game gets this from linking them).
  target_compile_definitions(${G_NAME}_bench PRIVATE ${GCN_GAME_DEFS} TARGET_PC=1)
  if(MSVC)
    target_compile_options(${G_NAME}_bench PRIVATE /Zc:__cplusplus)
  endif()
  if(WIN32)
    target_link_libraries(${G_NAME}_bench PRIVATE dbghelp)
  endif()

  if(NOT GCN_BENCH_ONLY)
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
  endif()

  # The game: the runtime and the game project's code, with no game code of its own -- that
  # is a module it loads (runtime/game_module.h) -- so it builds without a disc image. Its
  # symbols are exported for the module to call.
  if(NOT GCN_BENCH_ONLY)
    add_executable(${G_NAME} ${RUNTIME_SOURCES} ${G_SOURCES})
    _gcn_set_standards(${G_NAME})
    target_include_directories(${G_NAME} PRIVATE ${RT} ${aurora_SOURCE_DIR}/lib)
    target_link_libraries(${G_NAME} PRIVATE aurora::core aurora::gx aurora::vi aurora::pad aurora::card gcn_nod ${GCN_PLATFORM_LIBS} ${CMAKE_DL_LIBS})
    # The system's own file picker (runtime/ui/pick_file.h): AppKit on macOS, comdlg32 on Windows.
    if(APPLE)
      target_sources(${G_NAME} PRIVATE ${RT}/ui/pick_file_mac.mm)
      set_source_files_properties(${RT}/ui/pick_file_mac.mm PROPERTIES LANGUAGE CXX COMPILE_FLAGS "-x objective-c++ -fobjc-arc")
      target_link_libraries(${G_NAME} PRIVATE "-framework AppKit" "-framework UniformTypeIdentifiers")
    elseif(WIN32)
      target_link_libraries(${G_NAME} PRIVATE comdlg32)
      if(G_APP_ICON)
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${G_NAME}_icon.rc "1 ICON \"${G_APP_ICON}.ico\"\n")
        target_sources(${G_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${G_NAME}_icon.rc)
      endif()
    endif()
    set_target_properties(${G_NAME} PROPERTIES ENABLE_EXPORTS ON)
    add_dependencies(${G_NAME} ${G_NAME}_ui)
    if(WIN32)
      # A DLL cannot leave symbols for the executable to fill in as it loads: the module
      # links with the executable's import library instead, of everything it defines.
      set_target_properties(${G_NAME} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
      target_compile_definitions(${G_NAME} PRIVATE GCN_RES_IMPORT_LIB="$<TARGET_LINKER_FILE:${G_NAME}>")
    endif()
    if(TARGET ${G_NAME}_game AND WIN32)
      target_link_libraries(${G_NAME}_game PRIVATE ${G_NAME})   # the import library: built after
    elseif(TARGET ${G_NAME}_game)
      add_dependencies(${G_NAME} ${G_NAME}_game)
    endif()
    target_compile_options(${G_NAME} PRIVATE ${GCN_RUNTIME_WARNINGS})
    target_compile_definitions(${G_NAME} PRIVATE ${GCN_GAME_DEFS})

    # The app: what preparing the game on the player's machine needs (runtime/prepare.h:
    # the recompiler, the tables, the symbols, the headers the generated code includes),
    # the menus and the licences, in <APP_NAME>.app/Contents/Resources on macOS, in
    # resources/ beside the executable elsewhere. main.cpp finds them there, and keeps the
    # player's state in the system's place for it (Application Support, ~/.local/share,
    # %APPDATA%).
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
        set(_res $<TARGET_FILE_DIR:${G_NAME}>/resources)
      endif()
      file(GLOB _recomp_py ${GCN_RECOMP_DIR}/recomp/*.py)
      set(_game_licenses)
      if(G_LICENSES)
        set(_game_licenses COMMAND ${CMAKE_COMMAND} -E copy ${G_LICENSES} ${_res}/licenses/)
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
      add_custom_command(TARGET ${G_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_res}/recomp ${_res}/include ${_res}/licenses
        COMMAND ${CMAKE_COMMAND} -E copy ${_recomp_py} ${_res}/recomp/
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${G_TABLES} ${_res}/tables
        COMMAND ${CMAKE_COMMAND} -E copy ${G_SYMBOLS} ${_res}/symbols.txt
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
  endif()

  # For a game project that adds targets of its own, the pieces such a target is assembled
  # from. GCN_CORE_SOURCES is the guest, the hardware and the GX stream with no GPU, no SDL
  # and the silent audio stub; GCN_AUDIO_SOURCES is the real mix (a frontend adds its own
  # device backend). Every target also needs the include directories GCN_RT and
  # GCN_GEN_DIR and the definitions GCN_GAME_DEFS.
  set(GCN_RT ${RT} PARENT_SCOPE)
  set(GCN_GEN_DIR ${RECOMP_GEN_DIR} PARENT_SCOPE)
  set(GCN_HAVE_GAME_ISO ${HAVE_GAME_ISO} PARENT_SCOPE)
  set(GCN_CORE_SOURCES ${CORE_SOURCES} PARENT_SCOPE)
  set(GCN_AUDIO_SOURCES ${RT}/audio.cpp PARENT_SCOPE)
  set(GCN_RUNTIME_WARNINGS ${GCN_RUNTIME_WARNINGS} PARENT_SCOPE)
  set(GCN_GAME_DEFS ${GCN_GAME_DEFS} PARENT_SCOPE)
endfunction()
