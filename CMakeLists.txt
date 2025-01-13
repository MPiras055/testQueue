cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 20)
project(QueueBenchmarks)

set(HEADER_DIRS
    ${CMAKE_SOURCE_DIR}/include/queues/
    ${CMAKE_SOURCE_DIR}/include/utils    
)

# -------------------  EnqueueDequeue  ------------------- #
add_executable(EnqueueDequeue
    ${CMAKE_SOURCE_DIR}/src/benchmark/EnqueueDequeue.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/AdditionalWork.cpp
)

target_precompile_headers(EnqueueDequeue PRIVATE ${CMAKE_SOURCE_DIR}/include/queues/QueueTypeSet.hpp)
target_include_directories(EnqueueDequeue PRIVATE ${HEADER_DIRS})


if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_target_properties(EnqueueDequeue PROPERTIES COMPILE_FLAGS "-O0 -g -DDEBUG -fsanitize=address")
    set_target_properties(EnqueueDequeue PROPERTIES LINK_FLAGS "-fsanitize=address")
else()
    set_target_properties(EnqueueDequeue PROPERTIES COMPILE_FLAGS "-O3")
    set_target_properties(EnqueueDequeue PROPERTIES LINK_FLAGS "")
endif()
    

# -------------------  Testing  ------------------- #

set(HEADER_DIRS
    ${CMAKE_SOURCE_DIR}/include/queues/
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
add_test(NAME Sequential COMMAND SequentialTests)
add_test(NAME Concurrent COMMAND ConcurrentTests)


