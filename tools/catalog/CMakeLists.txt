cmake_minimum_required(VERSION 3.20)
project(catalog_tools C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# CSV 是 YAML 的编译产物，沿用现有二进制编码器。
add_executable(catalog_gen catalog_gen.c csv.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../App/catalog/bvpv_catalog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../App/service/bvpv_errcode.c)
target_include_directories(catalog_gen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../App/catalog)
include(CatalogBuild.cmake)
bvpv_catalog_build(catalog_gen)
include(CTest)
if(BUILD_TESTING)
    foreach(case source_integrity source_boundaries deterministic group_mapping metadata_preservation translation_guards tree_guards description_runtime failure_protection check_readonly maintenance)
        add_test(NAME catalog_${case}
            COMMAND ${BVPV_CATALOG_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/Test-Catalog.ps1
                -CatalogGen $<TARGET_FILE:catalog_gen> -CatalogPack ${BVPV_CATALOG_PACK} -Case ${case}
                -WorkRoot ${CMAKE_CURRENT_BINARY_DIR}/catalog-validation)
    endforeach()
endif()
