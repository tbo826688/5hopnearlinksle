#===============================================================================
# @brief    cmake file
#===============================================================================

# [修改] 判断新的 SERVER 宏
if(DEFINED CONFIG_SAMPLE_SUPPORT_ROUTE_SERVER)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/route_server/route_server_adv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/route_server/route_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart.c
    )
    set(HEADER_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/route_server
    )

# [修改] 判断新的 CLIENT 宏
elseif(DEFINED CONFIG_SAMPLE_SUPPORT_ROUTE_CLIENT)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/route_client/route_client.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart.c
    )
    set(HEADER_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/route_client
    )

# [修改] 判断新的 ALL 宏
elseif(DEFINED CONFIG_SAMPLE_SUPPORT_ROUTE_ALL)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/route_server/route_server_adv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/route_server/route_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/route_client/route_client.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart.c
    )
    set(HEADER_LIST 
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/route_server
        ${CMAKE_CURRENT_SOURCE_DIR}/route_client
    )
endif()

set(SOURCES "${SOURCES}" ${SOURCES_LIST} PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" ${HEADER_LIST} PARENT_SCOPE)
