cmake_minimum_required(VERSION 3.20)
project(bvpv_usb_host_tests C)

# 独立 host 测试工程：直接编译本工程 App、Drivers 的实际源码。

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

set(PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../App/protocol)
set(CATALOG_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../App/catalog)
set(SERVICE_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../App/service)
set(CONTENT_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../App/content)

set(PROTOCOL_SRCS
    ${PROTOCOL_DIR}/bvpv_usb_crc16.c
    ${PROTOCOL_DIR}/bvpv_usb_frame.c
    ${PROTOCOL_DIR}/bvpv_usb_frag.c
)
set(CATALOG_SRCS
    ${CATALOG_DIR}/bvpv_catalog.c
)
set(SERVICE_SRCS
    ${SERVICE_DIR}/bvpv_errcode.c
    ${SERVICE_DIR}/bvpv_service.c
    ${SERVICE_DIR}/bvpv_svc_desc.c
    ${SERVICE_DIR}/bvpv_svc_value.c
)

enable_testing()

# 协议核心（L1/L2/CRC16）：仅协议源码
foreach(t crc16 frame frag)
    add_executable(test_${t} test_${t}.c ${PROTOCOL_SRCS})
    target_include_directories(test_${t} PRIVATE ${PROTOCOL_DIR})
    add_test(NAME ${t} COMMAND test_${t})
endforeach()

# 目录模型：协议 + 目录 + 合成目录数据
add_executable(test_catalog
    test_catalog.c test_catalog_data.c
    ${PROTOCOL_SRCS} ${CATALOG_SRCS}
)
target_include_directories(test_catalog PRIVATE ${PROTOCOL_DIR} ${CATALOG_DIR})
add_test(NAME catalog COMMAND test_catalog)

# 服务层：协议 + 目录 + 服务 + 合成目录数据
add_executable(test_service
    test_service.c test_catalog_data.c
    ${PROTOCOL_SRCS} ${CATALOG_SRCS} ${SERVICE_SRCS}
)
target_include_directories(test_service PRIVATE ${PROTOCOL_DIR} ${CATALOG_DIR} ${SERVICE_DIR})
add_test(NAME service COMMAND test_service)

# MSC 光盘镜像元数据：只依赖内容层，无协议/目录依赖
add_executable(test_cd_meta test_cd_meta.c ${CONTENT_DIR}/bvpv_cd_meta.c)
target_include_directories(test_cd_meta PRIVATE ${CONTENT_DIR})
add_test(NAME cd_meta COMMAND test_cd_meta)

# 编译固件实际回调，验证主机可见的 USB 契约。
set(FIRMWARE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../App/usb)
set(USB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../App/usb)
set(GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../App/generated)
add_executable(test_usb_device
    test_usb_device.c
    ${USB_DIR}/usb_descriptors.c
    ${USB_DIR}/msc_disk.c
    ${CONTENT_DIR}/bvpv_cd_meta.c
)
target_include_directories(test_usb_device PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_host_config ${FIRMWARE_DIR} ${CONTENT_DIR} ${USB_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/TinyUSB/src
)
add_test(NAME usb_device COMMAND test_usb_device)
# 单槽更新与启动恢复：真实内容模块 + 可注入断电的 NOR adapter。
add_executable(test_cd_store test_cd_store.c
    ${CONTENT_DIR}/bvpv_cd_store.c ${CONTENT_DIR}/bvpv_cd_meta.c)
target_include_directories(test_cd_store PRIVATE ${CONTENT_DIR})
target_compile_definitions(test_cd_store PRIVATE
    CATALOG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/catalog")
add_test(NAME cd_store COMMAND test_cd_store)

# 旧 wire 回归的完整目录只生成到 host 构建目录，固件链接 compact 运行契约。
add_executable(catalog_fixture_gen
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/catalog/catalog_gen.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/catalog/csv.c
    ${CATALOG_SRCS} ${SERVICE_DIR}/bvpv_errcode.c)
target_include_directories(catalog_fixture_gen PRIVATE ${CATALOG_DIR} ${CONTENT_DIR})
include(${CMAKE_CURRENT_SOURCE_DIR}/../../tools/catalog/CatalogBuild.cmake)
bvpv_catalog_build(catalog_fixture_gen)
set(CURRENT_GENERATED_DIR ${BVPV_CATALOG_CURRENT}/App/generated)

# 双任务真实源码的接收与完成隔离在 test_usb_handoff 中验证。

# 编译真实 Flash 驱动及 SPI2 仲裁，只替换 HAL 的外设边界。
add_executable(test_flash_driver test_flash_driver.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/W25Q64/w25q64.c ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/W25Q64/spi2_bus.c)
target_include_directories(test_flash_driver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/flash_stubs ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/W25Q64)
foreach(mode normal erase_timeout bus_timeout io_fault)
    add_test(NAME flash_${mode} COMMAND test_flash_driver ${mode})
endforeach()

# 密码实现与只读镜像验证共用生产密码库。

set(CRYPTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/Crypto)
set(MBEDTLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/MbedTLS)
set(FW_CRYPTO_SOURCES
    ${MBEDTLS_DIR}/library/asn1parse.c ${MBEDTLS_DIR}/library/asn1write.c
    ${MBEDTLS_DIR}/library/bignum.c ${MBEDTLS_DIR}/library/bignum_core.c
    ${MBEDTLS_DIR}/library/bignum_mod.c ${MBEDTLS_DIR}/library/bignum_mod_raw.c
    ${MBEDTLS_DIR}/library/constant_time.c ${MBEDTLS_DIR}/library/ecp.c
    ${MBEDTLS_DIR}/library/ecp_curves.c ${MBEDTLS_DIR}/library/ecdsa.c
    ${MBEDTLS_DIR}/library/sha256.c ${MBEDTLS_DIR}/library/platform.c
    ${MBEDTLS_DIR}/library/platform_util.c ${MBEDTLS_DIR}/library/memory_buffer_alloc.c)
add_library(fw_crypto STATIC ${FW_CRYPTO_SOURCES})
target_include_directories(fw_crypto PUBLIC ${MBEDTLS_DIR}/include ${CRYPTO_DIR}
    PRIVATE ${MBEDTLS_DIR}/library)
target_compile_definitions(fw_crypto PUBLIC MBEDTLS_CONFIG_FILE="fw_mbedtls_config.h")
if(NOT MSVC)
    target_compile_options(fw_crypto PRIVATE -ffunction-sections -fdata-sections)
endif()

# 实际固件只链接紧凑运行契约，冻结初值和 USB 策略在此验证。
add_executable(test_compact_runtime test_compact_runtime.c
    ${GENERATED_DIR}/bvpv_catalog_data.c ${CATALOG_SRCS}
    ${SERVICE_DIR}/bvpv_svc_value.c)
target_include_directories(test_compact_runtime PRIVATE
    ${GENERATED_DIR} ${CATALOG_DIR} ${SERVICE_DIR} ${PROTOCOL_DIR})
add_test(NAME compact_runtime COMMAND test_compact_runtime)

# 外部单槽目录、签名预检、NOR 提交与恢复使用真实模块。
add_executable(test_catalog_store test_catalog_store.c
    ${CONTENT_DIR}/bvpv_catalog_store.c ${CONTENT_DIR}/bvpv_cd_meta.c
    ${CONTENT_DIR}/bvpv_cd_store.c ${CATALOG_SRCS} ${CRYPTO_DIR}/fw_crypto.c)
target_include_directories(test_catalog_store PRIVATE
    ${CURRENT_GENERATED_DIR} ${CONTENT_DIR} ${CATALOG_DIR} ${SERVICE_DIR} ${CRYPTO_DIR})
add_dependencies(test_catalog_store current_catalog)
target_link_libraries(test_catalog_store PRIVATE fw_crypto)
target_compile_definitions(test_catalog_store PRIVATE
    CATALOG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/catalog")
if(NOT MSVC)
    target_link_options(test_catalog_store PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME catalog_store COMMAND test_catalog_store)

# 生成器的完整签名包穿过生产目录解析 interface，初值仍取实际固件契约。
add_executable(test_catalog_real test_catalog_real.c
    ${GENERATED_DIR}/bvpv_catalog_data.c ${CONTENT_DIR}/bvpv_catalog_store.c
    ${CONTENT_DIR}/bvpv_cd_meta.c ${CATALOG_SRCS} ${CRYPTO_DIR}/fw_crypto.c)
target_include_directories(test_catalog_real PRIVATE
    ${CURRENT_GENERATED_DIR} ${GENERATED_DIR} ${CONTENT_DIR} ${CATALOG_DIR} ${SERVICE_DIR} ${CRYPTO_DIR})
target_link_libraries(test_catalog_real PRIVATE fw_crypto)
if(NOT MSVC)
    target_link_options(test_catalog_real PRIVATE -Wl,--gc-sections)
endif()
set(CATALOG_TEST_PACKAGE ${BVPV_CATALOG_CURRENT}/generated.acat)
add_custom_command(OUTPUT ${CATALOG_TEST_PACKAGE} ${CATALOG_TEST_PACKAGE}.public.bin
    COMMAND ${BVPV_CATALOG_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
        -File ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/catalog/Build-TestCatalog.ps1
        -CatalogPack ${BVPV_CATALOG_PACK}
        -InputBody ${BVPV_CATALOG_CURRENT}/build-catalog/SW110048.catalog.body
        -Output ${CATALOG_TEST_PACKAGE}
    DEPENDS current_catalog ${BVPV_CATALOG_CURRENT}/build-catalog/SW110048.catalog.body
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/catalog/Build-TestCatalog.ps1
    VERBATIM)
add_custom_target(catalog_test_package DEPENDS ${CATALOG_TEST_PACKAGE} ${CATALOG_TEST_PACKAGE}.public.bin)
add_dependencies(test_catalog_real catalog_test_package)
add_test(NAME catalog_real COMMAND test_catalog_real
    ${CATALOG_TEST_PACKAGE} ${CATALOG_TEST_PACKAGE}.public.bin)

add_executable(test_usb_handoff test_usb_handoff.c ${USB_DIR}/usb_proto.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/USB/usb_io.c ${PROTOCOL_SRCS})
target_include_directories(test_usb_handoff PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_stubs ${USB_DIR} ${PROTOCOL_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/USB)
add_test(NAME usb_handoff COMMAND test_usb_handoff)
add_executable(test_usb_media test_usb_media.c ${USB_DIR}/usb_media.c)
target_include_directories(test_usb_media PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/flash_stubs ${USB_DIR} ${CONTENT_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/USB)
add_test(NAME usb_media COMMAND test_usb_media)

add_executable(test_fw_image test_fw_image.c ${CMAKE_CURRENT_SOURCE_DIR}/../../App/firmware/fw_image.c ${CRYPTO_DIR}/fw_crypto.c)
target_link_libraries(test_fw_image PRIVATE fw_crypto)
target_include_directories(test_fw_image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../App/firmware)
target_compile_definitions(test_fw_image PRIVATE FW_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/firmware")
add_test(NAME fw_image COMMAND test_fw_image)
