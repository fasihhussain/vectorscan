find_package(Threads)

# remove some warnings
if(CMAKE_CXX_FLAGS MATCHES "-Wmissing-declarations" )
    string(REPLACE "-Wmissing-declarations" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${PROJECT_SOURCE_DIR}/util)

# add any subdir with a cmake file
file(GLOB dirents RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *)
foreach(e ${dirents})
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${e} AND
       EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${e}/CMakeLists.txt)
        add_subdirectory(${e})
    endif ()
endforeach ()

# vs_grammar_bench — grammar benchmark executable (compile+scan measurement -> JSON on stdout).
# Drives the public hs_compile_multi() + HS_FLAG_GRAMMAR_REF path; no engine changes. Consumed by
# tools/vs_grammar_bench.py. Build: cmake --build build --target vs_grammar_bench -j
add_executable(vs_grammar_bench vs_grammar_bench_exec.cpp)
target_link_libraries(vs_grammar_bench hs)