cmake_minimum_required(VERSION 2.8 FATAL_ERROR)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project (rdmaperf)

set(PSL_DIR ${CMAKE_SOURCE_DIR}/../psl)
include_directories(${CMAKE_SOURCE_DIR} ${PSL_DIR}/include)
link_directories(${PSL_DIR}/build/lib)

add_compile_options(-std=c++11)
add_compile_options(-Wall -Werror)

find_package(Boost COMPONENTS program_options REQUIRED)
include_directories(${Boost_INCLUDE_DIR})

include_directories(.)

set(RDMA_LIBS rdmacm ibverbs)

add_executable(rdmaperf_server server.cpp)
target_link_libraries(rdmaperf_server ${RDMA_LIBS})
target_link_libraries(rdmaperf_server psl)
target_link_libraries(rdmaperf_server ${Boost_LIBRARIES})

# project(rdmaperf_client)
# add_executable(rdmaperf_client client.cpp)
# target_link_libraries(rdmaperf_client ${RDMA_LIBS})
# target_link_libraries(rdmaperf_client psl)
