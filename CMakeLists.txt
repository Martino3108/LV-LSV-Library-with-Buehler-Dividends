cmake_minimum_required(VERSION 3.16)
project(PricingEngine LANGUAGES CXX)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

option(BUEHLER_MC_OPENMP "Parallelize fast Buehler path simulator with OpenMP" ON)
option(BUILD_PYTHON_MODULE "Build pybind11 Python extension module" ON)

if(DEFINED ENV{QUANTLIB_ROOT})
    set(_quantlib_root_default "$ENV{QUANTLIB_ROOT}")
else()
    set(_quantlib_root_default "")
endif()
set(QUANTLIB_ROOT "${_quantlib_root_default}"
    CACHE PATH "Directory that contains a pre-built QuantLib/ tree")
if(QUANTLIB_ROOT STREQUAL "")
    message(FATAL_ERROR
        "Set QUANTLIB_ROOT to the directory that contains QuantLib/ "
        "(CMake -D flag or environment variable).")
endif()

set(DEFAULT_QUANTLIB_BUILD_PRESET "apple-arm64-noxad-ninja-release")
set(QUANTLIB_BUILD_PRESET "${DEFAULT_QUANTLIB_BUILD_PRESET}"
    CACHE STRING "QuantLib build preset folder under QuantLib/build")

set(QL_SOURCE_DIR "${QUANTLIB_ROOT}/QuantLib")
set(QL_BUILD_DIR  "${QL_SOURCE_DIR}/build/${QUANTLIB_BUILD_PRESET}")
set(QUANTLIB_LIBRARY "${QL_BUILD_DIR}/ql/libQuantLib.dylib")

if(NOT EXISTS "${QUANTLIB_LIBRARY}")
    message(FATAL_ERROR "QuantLib library not found at ${QUANTLIB_LIBRARY}")
endif()

find_package(Boost REQUIRED)

set(PRICING_CORE_SOURCES
    src/core/benchmark.cpp
    src/core/benchmark_bs_flat_reference.cpp
    src/core/bs_flat_mc_save_path.cpp
    src/core/benchmark_forward_start.cpp
    src/core/benchmark_log.cpp
    src/core/benchmark_pipeline_sanity.cpp
    src/core/buehler_iv_x_arbitrage.cpp
    src/core/market_data_inspect.cpp
    src/core/verify_fit.cpp
    src/core/market_data.cpp
    src/core/buehler_model.cpp
    src/core/buehler_fixing_path_simulate.cpp
    src/core/buehler_fast_path_simulate.cpp
    src/core/buehler_lsv_path_simulate.cpp
    src/options/buehler_pure_x_strike.cpp
    src/options/mc_observation_schedule.cpp
    src/options/buehler_fixing_save_path.cpp
    src/options/fd_buehler_x_fdm.cpp
    src/options/asian_mc_buehler_option.cpp
    src/options/barrier_mc_buehler_option.cpp
    src/options/lookback_mc_buehler_option.cpp
    src/options/autocall_mc_buehler_option.cpp
    src/options/buehler_mc_path_pricing.cpp
    src/options/european_mc_buehler_option.cpp
    src/options/digital_mc_buehler_option.cpp
    src/options/digital_accrual_mc_buehler_option.cpp
    src/options/lv_digital_fd_buehler_option.cpp
    src/options/lv_digital_accrual_fd_buehler_option.cpp
    src/options/lv_european_fd_buehler_option.cpp
)

add_library(pricing_core STATIC ${PRICING_CORE_SOURCES})

add_executable(pricing_engine src/main.cpp)

set(PRICING_TARGETS pricing_core pricing_engine)

foreach(tgt IN LISTS PRICING_TARGETS)
    target_include_directories(${tgt} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/options"
        "${QL_BUILD_DIR}"
        "${QL_SOURCE_DIR}"
        ${Boost_INCLUDE_DIRS}
        /opt/homebrew/include
        /usr/local/include
    )

    target_link_libraries(${tgt} PRIVATE
        "${QUANTLIB_LIBRARY}"
        ${Boost_LIBRARIES}
    )
endforeach()

target_link_libraries(pricing_engine PRIVATE pricing_core)

# vForce vvexp in buehler_lsv_path_simulate.cpp (Apple). Static lib does not
# pull the framework into the final link unless dependents inherit it.
if(APPLE)
    find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
    target_link_libraries(pricing_core PUBLIC "${ACCELERATE_FRAMEWORK}")
endif()

option(BUILD_TESTS "Build CTest executables under tests/" ON)
if(BUILD_TESTS)
    enable_testing()

    add_executable(pricing_tests tests/test_pricing_engine.cpp)
    add_executable(pipeline_sanity_check tests/test_pipeline_sanity.cpp)

    foreach(tgt IN ITEMS pricing_tests pipeline_sanity_check)
        target_include_directories(${tgt} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/options"
            "${QL_BUILD_DIR}"
            "${QL_SOURCE_DIR}"
            ${Boost_INCLUDE_DIRS}
            /opt/homebrew/include
            /usr/local/include
        )
        target_link_libraries(${tgt} PRIVATE
            pricing_core
            "${QUANTLIB_LIBRARY}"
            ${Boost_LIBRARIES}
        )
        list(APPEND PRICING_TARGETS ${tgt})
    endforeach()

    add_test(NAME pricing_tests COMMAND pricing_tests)
    add_test(NAME pipeline_sanity_check COMMAND pipeline_sanity_check)
endif()

if(BUILD_PYTHON_MODULE)
    find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)

    include(FetchContent)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG v2.13.6
    )
    FetchContent_MakeAvailable(pybind11)

    pybind11_add_module(pricing_engine_py MODULE
        "${CMAKE_CURRENT_SOURCE_DIR}/python/pricing_engine_pybind.cpp")
    target_link_libraries(pricing_engine_py PRIVATE pricing_core)
    target_include_directories(pricing_engine_py PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/options"
        "${QL_BUILD_DIR}"
        "${QL_SOURCE_DIR}"
        ${Boost_INCLUDE_DIRS}
        /opt/homebrew/include
        /usr/local/include
    )

    set_target_properties(pricing_engine_py PROPERTIES OUTPUT_NAME pricing_engine)
    list(APPEND PRICING_TARGETS pricing_engine_py)
endif()

if(BUEHLER_MC_OPENMP)
    if(APPLE)
        execute_process(
            COMMAND brew --prefix libomp
            OUTPUT_VARIABLE HOMEBREW_LIBOMP_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(HOMEBREW_LIBOMP_PREFIX AND EXISTS "${HOMEBREW_LIBOMP_PREFIX}/include/omp.h")
            set(OpenMP_ROOT "${HOMEBREW_LIBOMP_PREFIX}" CACHE PATH "Homebrew libomp prefix")
            list(APPEND CMAKE_PREFIX_PATH "${HOMEBREW_LIBOMP_PREFIX}")
        endif()
    endif()

    find_package(OpenMP)

    # Apple Clang: FindOpenMP often fails without a hint; link Homebrew libomp explicitly.
    if(NOT OpenMP_CXX_FOUND AND APPLE AND HOMEBREW_LIBOMP_PREFIX)
        find_library(BUEHLER_LIBOMP_LIBRARY NAMES omp
            PATHS "${HOMEBREW_LIBOMP_PREFIX}/lib" NO_DEFAULT_PATH)
        find_path(BUEHLER_LIBOMP_INCLUDE_DIR NAMES omp.h
            PATHS "${HOMEBREW_LIBOMP_PREFIX}/include" NO_DEFAULT_PATH)
        if(BUEHLER_LIBOMP_LIBRARY AND BUEHLER_LIBOMP_INCLUDE_DIR)
            add_library(OpenMP::OpenMP_CXX IMPORTED INTERFACE)
            set_target_properties(OpenMP::OpenMP_CXX PROPERTIES
                INTERFACE_COMPILE_OPTIONS "-Xpreprocessor;-fopenmp"
                INTERFACE_INCLUDE_DIRECTORIES "${BUEHLER_LIBOMP_INCLUDE_DIR}"
                INTERFACE_LINK_LIBRARIES "${BUEHLER_LIBOMP_LIBRARY}"
            )
            set(OpenMP_CXX_FOUND TRUE)
        endif()
    endif()

    if(OpenMP_CXX_FOUND)
        foreach(tgt IN LISTS PRICING_TARGETS)
            target_link_libraries(${tgt} PRIVATE OpenMP::OpenMP_CXX)
        endforeach()
        message(STATUS "Buehler MC fast path: OpenMP enabled")
    else()
        message(STATUS "Buehler MC fast path: OpenMP not found; std::thread fallback")
    endif()
endif()

if(APPLE)
    foreach(tgt IN LISTS PRICING_TARGETS)
        if(NOT tgt STREQUAL "pricing_core")
            set_target_properties(${tgt} PROPERTIES
                BUILD_RPATH   "${QL_BUILD_DIR}/ql"
                INSTALL_RPATH "${QL_BUILD_DIR}/ql"
            )
        endif()
    endforeach()
endif()
