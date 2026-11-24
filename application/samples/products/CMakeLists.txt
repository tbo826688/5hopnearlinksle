#===============================================================================
# @brief    cmake file
# Copyright (c) CompanyNameMagicTag 2023-2023. All rights reserved.
#===============================================================================


if(DEFINED CONFIG_SAMPLE_SUPPORT_SLE_UART)
    add_subdirectory_if_exist(sle_uart)
endif()

if(DEFINED CONFIG_SAMPLE_SUPPORT_SLE_UART_1_VS_8)
    add_subdirectory_if_exist(sle_uart_1_vs_8)
endif()

if(DEFINED CONFIG_SAMPLE_SUPPORT_BLE_UART)
    add_subdirectory_if_exist(ble_uart)
endif()

if(DEFINED CONFIG_SAMPLE_SUPPORT_SLE_GETAWAY)
    add_subdirectory_if_exist(sle_gateway)
endif()
# 在 products/CMakeLists.txt 中添加：

if(DEFINED CONFIG_SAMPLE_SUPPORT_ROUTE AND DEFINED CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE)
    message(FATAL_ERROR "Select either the single-board SLE route or the dual-board route bridge, not both.")
endif()

if(DEFINED CONFIG_SAMPLE_SUPPORT_ROUTE)
    add_subdirectory_if_exist(route)
endif()

if(DEFINED CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE)
    add_subdirectory_if_exist(route_bridge)
endif()

set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)
set(LIBS "${LIBS}" PARENT_SCOPE)
