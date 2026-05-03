#===============================================================================
# @brief    cmake file
# Copyright (c) CompanyNameMagicTag 2023-2023. All rights reserved.
#===============================================================================

# 1. 检查 Hybrid 模式 (使用新宏名 CONFIG_..._HYBRID_MODE)
if(DEFINED CONFIG_SAMPLE_SUPPORT_SLE_UART_HYBRID_MODE)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server_adv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client/sle_uart_client.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_hybrid.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart.c
    )
    set(HEADER_LIST 
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client
    )

# 2. 检查 Server 模式 (使用新宏名)
elseif(DEFINED CONFIG_SAMPLE_SUPPORT_SLE_UART_HYBRID_SERVER_MODE)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server_adv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart.c
    )
    set(HEADER_LIST ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server)

# 3. 检查 Client 模式 (使用新宏名)
elseif(DEFINED CONFIG_SAMPLE_SUPPORT_SLE_UART_HYBRID_CLIENT_MODE)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client/sle_uart_client.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart.c
    )
    set(HEADER_LIST ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client)
endif()

set(SOURCES "${SOURCES}" ${SOURCES_LIST} PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" ${HEADER_LIST} PARENT_SCOPE)