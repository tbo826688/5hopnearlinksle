#===============================================================================
# @brief    cmake file
#===============================================================================

if(DEFINED CONFIG_SAMPLE_SUPPORT_SLE_UART_SEVER_SERVER)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server_adv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart.c
    )
    set(HEADER_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server
    )
elseif(DEFINED CONFIG_SAMPLE_SUPPORT_SLE_UART_SEVER_CLIENT)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client/sle_uart_client.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart.c
    )
    set(HEADER_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client
    )
elseif(DEFINED CONFIG_SAMPLE_SUPPORT_SLE_UART_SEVER_ALL)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server_adv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client/sle_uart_client.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart.c
    )
    set(HEADER_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client
    )
endif()

set(SOURCES "${SOURCES}" ${SOURCES_LIST} PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" ${HEADER_LIST} PARENT_SCOPE)
