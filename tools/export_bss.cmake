# The executable's C variables that start at zero (.bss, g_mem among them), for the export list
# CMake makes for it on Windows (WINDOWS_EXPORT_ALL_SYMBOLS), which the game module links with:
# when CMake reads the objects with llvm-nm -- building from Linux, tools/windows-cross.cmake --
# it leaves those out. A .def file of them, which CMake merges into its list: CMakeLists.txt
# gives it to the target and runs this before the link.
#
#   cmake -DOBJS=<the target's exports.def.objs> -DNM=<llvm-nm> -DOUT=<.def> -P tools/export_bss.cmake
cmake_minimum_required(VERSION 3.20)
file(STRINGS ${OBJS} objs REGEX "\\.obj$")
set(names)
foreach(o ${objs})
  execute_process(COMMAND ${NM} --extern-only --defined-only ${o} OUTPUT_VARIABLE out COMMAND_ERROR_IS_FATAL ANY)
  string(REGEX MATCHALL "[0-9a-f]+ B [A-Za-z_][A-Za-z0-9_]*\n" bss "${out}")
  foreach(line ${bss})
    string(REGEX REPLACE "^[0-9a-f]+ B ([A-Za-z0-9_]+)\n$" "\\1" name "${line}")
    list(APPEND names ${name})
  endforeach()
endforeach()
list(REMOVE_DUPLICATES names)
set(def "EXPORTS\n")
foreach(name ${names})
  string(APPEND def "\t${name} DATA\n")
endforeach()
file(WRITE ${OUT} "${def}")
