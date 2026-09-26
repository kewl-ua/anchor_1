# Fails if engine sources use anything that can break lockstep determinism
# or couple the simulation to the renderer.
#
# Usage: cmake -DENGINE_DIR=<path> -P check_engine_purity.cmake

file(GLOB_RECURSE sources "${ENGINE_DIR}/*.h" "${ENGINE_DIR}/*.cpp")

set(forbidden
    "(^|[^A-Za-z0-9_])(float|double)([^A-Za-z0-9_]|$)"   # floating-point types
    "raylib"                                              # graphics layer
    "<random>"                                            # implementation-defined distributions
    "<chrono>|<ctime>"                                    # wall-clock time
    "(^|[^A-Za-z0-9_])s?rand[ ]*\\("                      # C rand()
    "unordered_(map|set)"                                 # iteration order differs across STLs
)

set(violations "")
foreach(file ${sources})
    foreach(pattern ${forbidden})
        file(STRINGS "${file}" hits REGEX "${pattern}")
        foreach(hit ${hits})
            # A ';' would split the report line into several list items.
            string(REPLACE ";" "" hit "${hit}")
            string(STRIP "${hit}" hit)
            file(RELATIVE_PATH rel "${ENGINE_DIR}" "${file}")
            list(APPEND violations "  ${rel}: ${hit}")
        endforeach()
    endforeach()
endforeach()

if(violations)
    list(REMOVE_DUPLICATES violations)
    list(JOIN violations "\n" report)
    message(FATAL_ERROR "Engine purity check failed:\n${report}")
endif()

list(LENGTH sources count)
message(STATUS "Engine purity check passed (${count} files)")
