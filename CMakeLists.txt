cmake_minimum_required(VERSION 3.21)
project(ByteMe VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INSTALL_BINDIR ".")
set(CMAKE_INSTALL_LIBDIR ".")
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network Widgets)

add_library(byteme_core STATIC
    src/protocol.cpp src/protocol.h
    src/discovery.cpp src/discovery.h
    src/transferworker.cpp src/transferworker.h
    src/transfermanager.cpp src/transfermanager.h
)
target_include_directories(byteme_core PUBLIC src)
target_link_libraries(byteme_core PUBLIC Qt6::Core Qt6::Network)

qt_add_executable(ByteMe WIN32 src/main.cpp src/mainwindow.cpp src/mainwindow.h)
target_link_libraries(ByteMe PRIVATE byteme_core Qt6::Widgets)

if(MSVC)
    target_compile_options(byteme_core PRIVATE /W4 /utf-8 /permissive-)
    target_compile_options(ByteMe PRIVATE /W4 /utf-8 /permissive-)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(byteme_tests tests/integration.cpp)
    set_target_properties(byteme_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(byteme_tests PRIVATE byteme_core Qt6::Test)
    if(MSVC)
        target_compile_options(byteme_tests PRIVATE /W4 /utf-8 /permissive-)
    endif()
    add_test(NAME byteme_integration COMMAND byteme_tests -o -,txt -o ${CMAKE_CURRENT_BINARY_DIR}/test-results.txt,txt)
    set_tests_properties(byteme_integration PROPERTIES TIMEOUT 120)
endif()

install(TARGETS ByteMe RUNTIME DESTINATION .)
install(FILES docs/quickstart.txt DESTINATION . RENAME "使用说明.txt")
qt_generate_deploy_app_script(TARGET ByteMe OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})
