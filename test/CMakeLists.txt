cmake_minimum_required(VERSION 3.29.0)

set(This LimitOrderBookTests)

set(Sources
    LimitOrderBookTests.cpp
    ExampleOrdersTests.cpp
)

add_executable(${This} ${Sources})
target_link_libraries(${This} PUBLIC
    gtest_main
    LimitOrderBook_lib
)

add_test(
    NAME ${This}
    COMMAND ${This}
)

add_executable(EngineCorrectnessTests EngineCorrectnessTests.cpp)
target_link_libraries(EngineCorrectnessTests PUBLIC
    gtest_main
    LimitOrderBook_lib
)

add_test(
    NAME EngineCorrectnessTests
    COMMAND EngineCorrectnessTests
)

# Independent reference model + workload generator (engine-free oracle core).
set(ReferenceSources
    ../reference/ReferenceBook.cpp
    ../reference/Command.cpp
    ../reference/WorkloadGenerator.cpp
)
add_library(ReferenceCore STATIC ${ReferenceSources})
target_include_directories(ReferenceCore PUBLIC
    ${CMAKE_SOURCE_DIR}/reference
    ${CMAKE_SOURCE_DIR}/Limit_Order_Book
)

add_executable(DifferentialTests DifferentialTests.cpp)
target_link_libraries(DifferentialTests PUBLIC
    gtest_main
    LimitOrderBook_lib
    ReferenceCore
)

add_test(
    NAME DifferentialTests
    COMMAND DifferentialTests
)

add_executable(diff_fuzz ../tools/diff_fuzz.cpp)
target_link_libraries(diff_fuzz PUBLIC
    LimitOrderBook_lib
    ReferenceCore
)
