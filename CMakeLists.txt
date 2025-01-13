cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 20)
project(QueueBenchmarks)







# -------------------  Testing  ------------------- #

set(HEADER_DIRS
    ${CMAKE_SOURCE_DIR}/include/queues 
    ${CMAKE_SOURCE_DIR}/include/utils    
)

add_executable(ConcurrentTests 
    ${CMAKE_SOURCE_DIR}/src/test/Concurrent.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/AdditionalWork.cpp
)
add_executable(SequentialTests
    ${CMAKE_SOURCE_DIR}/src/test/Sequential.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/AdditionalWork.cpp
)



add_subdirectory(include/googletest)
set(GTEST_DIR ${CMAKE_SOURCE_DIR}/include/googletest/googletest/include)

target_precompile_headers(SequentialTests PRIVATE ${CMAKE_SOURCE_DIR}/include/queues/QueueTypeSet.hpp)
target_precompile_headers(SequentialTests PRIVATE ${CMAKE_SOURCE_DIR}/include/queues/QueueTypeSet.hpp)

target_include_directories(ConcurrentTests PRIVATE ${HEADER_DIRS} ${GTEST_DIR})
target_include_directories(SequentialTests PRIVATE ${HEADER_DIRS} ${GTEST_DIR})

target_compile_options(ConcurrentTests PUBLIC  -O3 -g -fsanitize=address)
target_compile_options(SequentialTests PUBLIC  -O3 -g -fsanitize=address)

target_link_libraries(ConcurrentTests gtest gtest_main pthread -fsanitize=address)
target_link_libraries(SequentialTests gtest gtest_main pthread -fsanitize=address)

enable_testing()
add_test(NAME Concurrent COMMAND ConcurrentTests)
add_test(NAME Sequential COMMAND SequentialTests)


