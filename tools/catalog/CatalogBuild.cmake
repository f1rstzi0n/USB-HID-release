# 独立工具、PC 协议和设备离线回归共用当前 YAML 的构建接缝。
function(bvpv_catalog_build generator)
    find_program(CATALOG_POWERSHELL NAMES pwsh powershell REQUIRED)
    set(CATALOG_PACK "" CACHE FILEPATH "已有 Andromeda.CatalogPack.exe；空时在构建目录编译")
    get_filename_component(catalog_repo "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
    if(CATALOG_PACK)
        set(pack "${CATALOG_PACK}")
    else()
        find_program(CATALOG_DOTNET NAMES dotnet REQUIRED)
        set(pack_dir "${CMAKE_CURRENT_BINARY_DIR}/catalog-pack")
        set(pack "${pack_dir}/Andromeda.CatalogPack.exe")
        add_custom_command(OUTPUT "${pack}"
            COMMAND ${CATALOG_DOTNET} build "${catalog_repo}/tools/catalog/Andromeda.CatalogPack.csproj"
                -c Release --output "${pack_dir}"
                "-p:BaseIntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/catalog-pack-obj/"
            DEPENDS "${catalog_repo}/tools/catalog/Andromeda.CatalogPack.csproj"
                "${catalog_repo}/tools/catalog/CatalogPack.cs" "${catalog_repo}/tools/catalog/CatalogSource.cs"
            VERBATIM)
    endif()
    set(current "${CMAKE_CURRENT_BINARY_DIR}/current-catalog")
    set(generated "${current}/App/generated/bvpv_catalog_data.c")
    set(csv "${current}/catalog/SW110048.catalog.csv")
    set(tree "${current}/catalog/SW110048.tree.csv")
    set(body "${current}/build-catalog/SW110048.catalog.body")
    add_custom_command(OUTPUT "${generated}" "${csv}" "${tree}" "${body}" "${body}.json" "${body}.runtime"
            "${current}/App/generated/bvpv_catalog_bindings.h"
        COMMAND ${CATALOG_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
            -File "${catalog_repo}/tools/catalog/Generate-Catalog.ps1" -Mode Generate
            -CatalogGen $<TARGET_FILE:${generator}> -CatalogPack "${pack}" -OutputRoot "${current}"
        DEPENDS ${generator} "${pack}" "${catalog_repo}/catalog/SW110048.catalog.yaml"
            "${catalog_repo}/tools/catalog/Generate-Catalog.ps1"
        VERBATIM)
    add_custom_target(current_catalog ALL DEPENDS "${generated}" "${csv}" "${tree}" "${body}")
    set(BVPV_CATALOG_CURRENT "${current}" PARENT_SCOPE)
    set(BVPV_CATALOG_PACK "${pack}" PARENT_SCOPE)
    set(BVPV_CATALOG_POWERSHELL "${CATALOG_POWERSHELL}" PARENT_SCOPE)
endfunction()
