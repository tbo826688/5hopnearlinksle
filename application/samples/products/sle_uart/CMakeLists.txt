#===============================================================================
# @brief    cmake file
# Copyright (c) CompanyNameMagicTag 2023-2023. All rights reserved.
#===============================================================================
if(DEFINED CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server_adv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart.c
    )
    set(HEADER_LIST ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server)
elseif(DEFINED CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client/sle_uart_client.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart.c
    )
    set(HEADER_LIST ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client)
# 【新增】主从一体编译配置
elseif(DEFINED CONFIG_SAMPLE_SUPPORT_SLE_UART_ALL)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server_adv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client/sle_uart_client.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart.c
    )
    set(HEADER_LIST 
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client
    )
endif()
# 强制移除内存监控的宏定义
add_compile_definitions(CONFIG_DUMP_MEM_SUPPORT=0)
add_compile_definitions(DUMP_MEM_SUPPORT=0)
set(SOURCES "${SOURCES}" ${SOURCES_LIST} PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" ${HEADER_LIST} PARENT_SCOPE)