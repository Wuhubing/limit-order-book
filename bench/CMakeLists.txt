# Stage C performance-measurement harness (engine service time only).
# The lob_bench binary links the fixed engine library and the read-only
# reference corpus machinery (Command + WorkloadGenerator + ReferenceBook).
# AllocStats.cpp overrides global new/delete and is linked ONLY here.

execute_process(
    COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse HEAD
    OUTPUT_VARIABLE LOB_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT LOB_GIT_COMMIT)
    set(LOB_GIT_COMMIT "unknown")
endif()

add_executable(lob_bench
    lob_bench.cpp
    AllocStats.cpp
    ../reference/Command.cpp
    ../reference/WorkloadGenerator.cpp
    ../reference/ReferenceBook.cpp
)

target_include_directories(lob_bench PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/reference
    ${CMAKE_SOURCE_DIR}/Limit_Order_Book
)

target_link_libraries(lob_bench PRIVATE LimitOrderBook_lib)

target_compile_definitions(lob_bench PRIVATE
    LOB_GIT_COMMIT="${LOB_GIT_COMMIT}"
    LOB_CXX_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    LOB_CXX_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}"
    LOB_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
