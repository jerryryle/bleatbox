cmake_minimum_required(VERSION 3.20.0)
project(bleatbox_tests C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 14)
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../app/src)
set(COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)

enable_testing()

file(GLOB TEST_SOURCES *_test.cpp)
set(MODULE_SOURCES "")
foreach(TEST_FILE ${TEST_SOURCES})
    get_filename_component(NAME ${TEST_FILE} NAME_WE)
    string(REPLACE "_test" "" MODULE ${NAME})
    if(EXISTS ${SRC_DIR}/${MODULE}.c)
        list(APPEND MODULE_SOURCES ${SRC_DIR}/${MODULE}.c)
    elseif(EXISTS ${COMMON_DIR}/${MODULE}.c)
        list(APPEND MODULE_SOURCES ${COMMON_DIR}/${MODULE}.c)
    endif()
endforeach()

add_executable(tests ${TEST_SOURCES} ${MODULE_SOURCES})
target_include_directories(tests PRIVATE ${SRC_DIR} ${COMMON_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/fakes)
target_link_libraries(tests GTest::gtest_main)
add_test(NAME tests COMMAND tests)
