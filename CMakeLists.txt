cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 20)
project(QueueBenchmarks)

set(HEADER_DIRS
    ${CMAKE_SOURCE_DIR}/include/queues/
    ${CMAKE_SOURCE_DIR}/include/utils    
)

set(Debug_compile_opt "-O0 -g -DDEBUG -fsanitize=address")
set(Debug_link "-fsanitize=address")
set(Release_compile_opt "-O3")
set(Release_link " ")

# ------------------- Automation -------------------- #
# Function to create an executable and apply common settings
function(add_benchmark exec_name)
    # Get the source files from the function arguments
    set(SOURCE_FILES ${ARGN})

    # Create the executable
    add_executable(${exec_name} ${SOURCE_FILES})

    # Apply common compile options, libraries, and include directories
    target_precompile_headers(${exec_name} PRIVATE ${CMAKE_SOURCE_DIR}/include/queues/QueueTypeSet.hpp)
    target_include_directories(${exec_name} PRIVATE ${HEADER_DIRS})

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set_target_properties(${exec_name} PROPERTIES COMPILE_FLAGS ${Debug_compile_opt})
        set_target_properties(${exec_name} PROPERTIES LINK_FLAGS ${Debug_link})
    else()
        set_target_properties(${exec_name} PROPERTIES COMPILE_FLAGS ${Release_compile_opt})
        set_target_properties(${exec_name} PROPERTIES LINK_FLAGS ${Release_link})
    endif()
endfunction()

# -------------------  Benchmarks  ------------------- #

add_benchmark(EnqueueDequeue
    ${CMAKE_SOURCE_DIR}/src/benchmark/EnqueueDequeue.cpp   
)

add_benchmark(ProducerConsumer
    ${CMAKE_SOURCE_DIR}/src/benchmark/ProducerConsumer.cpp 
)

add_benchmark(oneToMany
    ${CMAKE_SOURCE_DIR}/src/benchmark/oneToMany.cpp 
)

add_benchmark(ManyToOne
    ${CMAKE_SOURCE_DIR}/src/benchmark/ManyToOne.cpp 
)

add_benchmark(ManyToMany
    ${CMAKE_SOURCE_DIR}/src/benchmark/ManyToMany.cpp 
)

    

# -------------------  Testing  ------------------- #

set(HEADER_DIRS
    ${CMAKE_SOURCE_DIR}/include/queues/
    ${CMAKE_SOURCE_DIR}/include/utils    
)

add_executable(ConcurrentTests 
    ${CMAKE_SOURCE_DIR}/src/test/Concurrent.cpp
)
add_executable(SequentialTests
    ${CMAKE_SOURCE_DIR}/src/test/Sequential.cpp
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


