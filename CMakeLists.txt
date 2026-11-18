cmake_minimum_required(VERSION 3.16)
project(nevla VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Inside another project (add_subdirectory / FetchContent): tests / examples default to OFF
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(NEVLA_TOP ON)
else()
  set(NEVLA_TOP OFF)
endif()
option(NEVLA_BUILD_TESTS "Build nevla tests" ${NEVLA_TOP})
option(NEVLA_BUILD_EXAMPLES "Build nevla examples" ${NEVLA_TOP})

find_package(mongocxx CONFIG REQUIRED)
if(TARGET mongo::mongocxx_shared)
  set(NEVLA_MONGOCXX mongo::mongocxx_shared)
else()
  set(NEVLA_MONGOCXX mongo::mongocxx_static)
endif()

add_library(nevla src/value.cc src/schema.cc src/model.cc src/formula.cc src/async.cc src/plugins.cc src/crypto.cc src/memory.cc)
add_library(nevla::nevla ALIAS nevla)
target_include_directories(nevla PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
target_link_libraries(nevla PUBLIC ${NEVLA_MONGOCXX} Threads::Threads PRIVATE OpenSSL::Crypto)
target_compile_options(nevla PRIVATE -Wall -Wextra)

if(NEVLA_BUILD_TESTS)
  enable_testing()
  add_executable(nevla_tests tests/test.cc)
  target_link_libraries(nevla_tests PRIVATE nevla)
  add_test(NAME nevla_tests COMMAND nevla_tests)
  # the same suite on the built-in in-memory database (no MongoDB server needed)
  add_test(NAME nevla_tests_memory COMMAND nevla_tests)
  set_tests_properties(nevla_tests_memory PROPERTIES ENVIRONMENT "NEVLA_TEST_URI=memory://nevla_test")
  # Typed models: these files must be rejected by the compiler (typos / wrong types are compile errors)
  foreach(cf typo_member wrong_value_type inc_on_string ok_control)
    add_test(NAME compile_${cf}
             COMMAND ${CMAKE_CXX_COMPILER} -std=c++20 -fsyntax-only -I${CMAKE_CURRENT_SOURCE_DIR}/include
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/${cf}.cc)
    if(NOT cf STREQUAL "ok_control")
      set_tests_properties(compile_${cf} PROPERTIES WILL_FAIL TRUE)
    endif()
  endforeach()
endif()

if(NEVLA_BUILD_EXAMPLES)
  foreach(ex 01_quickstart 02_blog 03_iot 04_typed_struct 05_features 06_iot_dashboard 07_offline_api)
    add_executable(${ex} examples/${ex}.cc)
    target_link_libraries(${ex} PRIVATE nevla)
  endforeach()
  # nevla inside a Drogon app: only when Drogon is installed
  find_package(Drogon CONFIG QUIET)
  if(Drogon_FOUND)
    add_executable(08_drogon examples/08_drogon.cc)
    target_link_libraries(08_drogon PRIVATE nevla Drogon::Drogon)
    if(NEVLA_BUILD_TESTS)
      add_test(NAME drogon_resume_on_loop COMMAND 08_drogon --check)
    endif()
  endif()
endif()

# sudo cmake --install build  ->  find_package(nevla 0.2 CONFIG REQUIRED) ; target_link_libraries(app nevla::nevla)
include(GNUInstallDirs)
install(TARGETS nevla EXPORT nevlaTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/nevla DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT nevlaTargets NAMESPACE nevla:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nevla)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/nevlaConfig.cmake
"include(CMakeFindDependencyMacro)\nfind_dependency(mongocxx)\nfind_dependency(Threads)\nfind_dependency(OpenSSL)\ninclude(\${CMAKE_CURRENT_LIST_DIR}/nevlaTargets.cmake)\n")
include(CMakePackageConfigHelpers)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/nevlaConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nevlaConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/nevlaConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nevla)
