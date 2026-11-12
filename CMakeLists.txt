cmake_minimum_required(VERSION 3.20)
project(agora VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(AGORA_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Sql Test)

set(AGORA_WARN_FLAGS -Wall -Wextra -Wpedantic)
set(AGORA_SANITIZER_FLAGS "")
if(AGORA_ENABLE_SANITIZERS)
    list(APPEND AGORA_SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

add_library(agora_core STATIC
    src/core/types.h
    src/core/storage.h
    src/core/storage.cpp
    src/core/sandbox.h
    src/core/sandbox.cpp
    src/core/tools.h
    src/core/tools.cpp
    src/core/router_client.h
    src/core/router_client.cpp
    src/core/orchestrator.h
    src/core/orchestrator.cpp
)

target_include_directories(agora_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(agora_core PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    ${AGORA_SANITIZER_FLAGS}
)

target_compile_options(agora_core PRIVATE ${AGORA_WARN_FLAGS} ${AGORA_SANITIZER_FLAGS})

add_executable(agora
    src/main.cpp
    src/ui/main_window.h
    src/ui/main_window.cpp
    src/ui/transcript_view.h
    src/ui/transcript_view.cpp
    src/ui/workspace_pane.h
    src/ui/workspace_pane.cpp
)

target_link_libraries(agora PRIVATE
    agora_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    ${AGORA_SANITIZER_FLAGS}
)

target_compile_options(agora PRIVATE ${AGORA_WARN_FLAGS} ${AGORA_SANITIZER_FLAGS})

enable_testing()
add_executable(agora_tests
    tests/test_core.cpp
)
target_link_libraries(agora_tests PRIVATE
    agora_core
    Qt6::Core
    Qt6::Test
    ${AGORA_SANITIZER_FLAGS}
)
target_compile_options(agora_tests PRIVATE ${AGORA_WARN_FLAGS} ${AGORA_SANITIZER_FLAGS})
add_test(NAME agora_core_tests COMMAND agora_tests)
