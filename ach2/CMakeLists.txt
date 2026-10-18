cmake_minimum_required(VERSION 3.20)
project(linear_3tier_service VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_HTTP_SERVER "Build the Drogon HTTP server and PostgreSQL adapter" ON)
include(CTest)

# Отделяем правила сценария и журналирование от Drogon и драйвера БД. Поэтому
# модульные тесты можно собрать обычным компилятором C++17 без PostgreSQL и HTTP-сервера.
add_library(number-service-core
    src/number_service.cpp
    src/stdout_logger.cpp
)
target_include_directories(number-service-core PUBLIC src)
target_compile_options(number-service-core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion>
)

if(BUILD_HTTP_SERVER)
    find_package(Drogon CONFIG REQUIRED)

    add_library(number-service-postgres
        src/postgres_number_repository.cpp
    )
    target_include_directories(number-service-postgres PUBLIC src)
    target_link_libraries(number-service-postgres PUBLIC number-service-core Drogon::Drogon)
    target_compile_options(number-service-postgres PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion>
    )

    add_executable(linear-service
        src/main.cpp
        src/increment_controller.cpp
    )
    target_include_directories(linear-service PRIVATE src)
    target_link_libraries(linear-service PRIVATE number-service-postgres Drogon::Drogon)
    target_compile_options(linear-service PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion>
    )
endif()

if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(number-service-unit-tests
        tests/number_service_test.cpp
    )
    target_link_libraries(number-service-unit-tests PRIVATE number-service-core)
    target_compile_options(number-service-unit-tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion>
    )
    add_test(NAME number-service-unit-tests COMMAND number-service-unit-tests)

    # Проверяем сбои транзакционных операций отдельно от базовых правил сценария.
    add_executable(number-service-fault-tests
        tests/number_service_fault_tests.cpp
    )
    target_link_libraries(number-service-fault-tests PRIVATE number-service-core Threads::Threads)
    target_compile_options(number-service-fault-tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion>
    )
    add_test(NAME number-service-fault-tests COMMAND number-service-fault-tests)
endif()
