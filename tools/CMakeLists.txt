# Stage E deterministic replay tool (engine only — no reference model at link time).
# Links the fixed engine library plus the read-only corpus parser (Command.cpp);
# it does NOT link ReferenceBook.cpp / WorkloadGenerator.cpp / DifferentialHarness.

add_executable(replay
    replay.cpp
    ../reference/Command.cpp
)

target_include_directories(replay PRIVATE
    ${CMAKE_SOURCE_DIR}/reference
    ${CMAKE_SOURCE_DIR}/Limit_Order_Book
)

target_link_libraries(replay PRIVATE LimitOrderBook_lib)
