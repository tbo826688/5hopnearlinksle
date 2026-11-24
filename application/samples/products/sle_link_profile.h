#ifndef SLE_LINK_PROFILE_H
#define SLE_LINK_PROFILE_H

#include "sle_connection_manager.h"

#define SLE_DATA_PHY                  SLE_PHY_4M
#define SLE_DATA_PILOT_DENSITY        SLE_PHY_PILOT_DENSITY_4_TO_1
#define SLE_DATA_MCS                  8u
#define SLE_DATA_PHY_SETTLE_US        200000ULL
#define SLE_DATA_PHY_RETRY_US         2000000ULL

static inline errcode_t sle_link_request_data_phy(uint16_t conn_id)
{
    sle_set_phy_t phy_param = {
        .tx_format = SLE_RADIO_FRAME_2,
        .rx_format = SLE_RADIO_FRAME_2,
        .tx_phy = SLE_DATA_PHY,
        .rx_phy = SLE_DATA_PHY,
        .tx_pilot_density = SLE_DATA_PILOT_DENSITY,
        .rx_pilot_density = SLE_DATA_PILOT_DENSITY,
        .g_feedback = 0,
        .t_feedback = 0
    };

    return sle_set_phy_param(conn_id, &phy_param);
}

#endif
