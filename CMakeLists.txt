cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 20)
project(QueueBenchmarks)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message (STATUS "INFO: Targeting Debug Build")
else()
    message (STATUS "INFO: Targeting Release Build")
endif()


set(HEADER_DIRS
    ${CMAKE_SOURCE_DIR}/include/queues/
    ${CMAKE_SOURCE_DIR}/include/utils    
    ${CMAKE_SOURCE_DIR}/include/All2All
)

# -- Macros Definitions -- #
option (DISABLE_AFFINITY "Disable Thread Pinning to Cores" OFF)
option (DISABLE_HAZARD "Disable Hazard Pointers" OFF)
option (DISABLE_POW2 "Disable rounding up to power of 2 for queue size" OFF)


# -- jemalloc
find_package(PkgConfig REQUIRED)
pkg_search_module(JEMALLOC REQUIRED jemalloc)
# jemalloc --


# -- Debug Compile Options -- #
set(Debug_compile_opt -DDEBUG -O3 -g -fsanitize=address -fsanitize=undefined)
set(Debug_link -fsanitize=address -lnuma -fsanitize=undefined -fsanitize=address -static-libasan)


# -- Release Compile Options -- #
set(Release_compile_opt -O3)
set(Release_link -lnuma)

# ------------------- Automation -------------------- #
# Function to create an executable and apply common settings
function(add_benchmark exec_name)
    # Get the source files from the function arguments
    set(SOURCE_FILES ${ARGN})

    # Create the executable
    add_executable(${exec_name} ${SOURCE_FILES})

    # Apply common compile options, libraries, and include directories
    #target_precompile_headers(${exec_name} PUBLIC ${CMAKE_SOURCE_DIR}/include/queues/QueueTypeSet.hpp)
    target_include_directories(${exec_name} PRIVATE ${HEADER_DIRS} ${JEMALLOC_INCLUDE_DIRS})

    # Apply specific compile options and libraries based on the build type
    if(DISABLE_AFFINITY)
        message (STATUS ${exec_name}": Disabling Thread Pinning to Cores")
        target_compile_definitions(${exec_name} PRIVATE DISABLE_AFFINITY)
    endif()
    if(DISABLE_HAZARD)
        message (STATUS ${exec_name}": Disabling Hazard Pointers")
        target_compile_definitions(${exec_name} PUBLIC DISABLE_HAZARD)
    endif()
    if(DISABLE_POW2)
        message (STATUS ${exec_name}": Disabling rounding up to power of 2 for queue size")
        target_compile_definitions(${exec_name} PUBLIC DISABLE_POW2)
    endif()


    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${exec_name} PUBLIC ${Debug_compile_opt})
        target_link_libraries(${exec_name} ${Debug_link} ${JEMALLOC_LIBRARIES})
    else()  #release version
        target_compile_options(${exec_name} PUBLIC ${Release_compile_opt})
        target_link_libraries(${exec_name} ${Release_link} ${JEMALLOC_LIBRARIES})
    endif()
endfunction()

# -------------------  Benchmarks  ------------------- #

add_benchmark(OneToMany
    ${CMAKE_SOURCE_DIR}/src/benchmark/oneToMany.cpp 
    ${CMAKE_SOURCE_DIR}/src/utils/NumaDispatcher.cpp
)

add_benchmark(ManyToOne
    ${CMAKE_SOURCE_DIR}/src/benchmark/ManyToOne.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/NumaDispatcher.cpp 
)

add_benchmark(ManyToMany
    ${CMAKE_SOURCE_DIR}/src/benchmark/ManyToMany.cpp 
    ${CMAKE_SOURCE_DIR}/src/utils/NumaDispatcher.cpp
)

add_benchmark(SPSC 
    ${CMAKE_SOURCE_DIR}/src/All2All/SPSC_bench.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/NumaDispatcher.cpp
)

add_benchmark(All2All
    ${CMAKE_SOURCE_DIR}/src/All2All/All2All_bench.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/NumaDispatcher.cpp
)

add_executable(time 
    ${CMAKE_SOURCE_DIR}/src/utils/time.cpp
)
target_include_directories(time PRIVATE ${CMAKE_SOURCE_DIR}/include/utils)
target_compile_options(time PUBLIC -O3)

add_executable(timeMean 
    ${CMAKE_SOURCE_DIR}/src/utils/timeMean.cpp
)
target_include_directories(timeMean PUBLIC ${CMAKE_SOURCE_DIR}/include/utils)
target_compile_options(timeMean PUBLIC -O3)

add_executable(timeArray
    ${CMAKE_SOURCE_DIR}/src/utils/timeArray.cpp
)
target_include_directories(timeArray PUBLIC ${CMAKE_SOURCE_DIR}/include/utils)
target_compile_options(timeArray PUBLIC -O3)

    

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

target_compile_definitions(ConcurrentTests PUBLIC DISABLE_POW2)
target_compile_definitions(SequentialTests PUBLIC DISABLE_POW2)

target_link_libraries(ConcurrentTests gtest gtest_main pthread -fsanitize=address)
target_link_libraries(SequentialTests gtest gtest_main pthread -fsanitize=address)

enable_testing()
add_test(NAME Sequential COMMAND SequentialTests)
add_test(NAME Concurrent COMMAND ConcurrentTests)


