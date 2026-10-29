/* com_ble.h (host mock): the GATT server subset port/ak1037 uses. Enum
 * values that cross the API follow the SDK; the rest only need to differ. */
#ifndef COM_BLE_H_MOCK
#define COM_BLE_H_MOCK
#include "ak_mock_types.h"

#define BLE_MTU_MAX_SIZE 517
#define BLE_MTU_MIN_SIZE 23

/* T_BT_ATTREC type/state flags */
enum { arPrimaryService = 1, arCharacteristic, arChrValue, arChrClientConfiguration };
enum { arLongUUID = 0x01, arRead = 0x02, arWrite = 0x04, arNotify = 0x08 };

typedef struct {
    uint16_t    type;
    uint16_t    state;
    const void *p_data;
} T_BT_ATTREC;

typedef struct {
    void    *p_buf;
    uint16_t buf_size;
    uint16_t buf_used;
    void    *p_att_rec;
    uint32_t type;
    uint32_t opcode;
} T_BT_ATTSEND_CB;
#define T_BT_ATTRECV_CB T_BT_ATTSEND_CB

typedef enum {
    eBT_MSG_LE_MTU_EXCHANGED = 1,
    eBT_MSG_LE_CONNECTION_SUCC,
    eBT_MSG_LE_LINK_DISCONNECTED,
} T_eBT_MSG;

typedef enum { eBLE_ROLE_SERVER = 0, eBLE_ROLE_CLIENT } T_eBLE_ROLE_TYPE;
typedef enum { eBLE_DISABLE = 0, eBLE_ENABLE } T_eBLE_ENABLE_TYPE;
typedef enum {
    eBLE_RESULT_SUCCESS = 0,
    eBLE_RESULT_FAIL,
    eBLE_RESULT_LIB_ERR,
    eBLE_RESULT_INIT_ERR,
    eBLE_RESULT_RXDATA_CB_ERR,
    eBLE_RESULT_TXDATA_CB_ERR,
    eBLE_RESULT_GETINFO_CB_ERR,
    eBLE_RESULT_SEND_ERR,
    eBLE_RESULT_PARAM_ERR,
    eBLE_RESULT_ROLE_ERR,
    eBLE_RESULT_BUFFER_FULL,
} T_eBLE_RESULT;
typedef enum {
    eBLE_ADV_IND = 0x00,
    eBLE_ADV_DIRECT_IND = 0x01,
    eBLE_ADV_SCAN_IND = 0x02,
    eBLE_ADV_NONCONN_IND = 0x03,
} T_eBLE_ADV_TYPE;

typedef struct { uint8_t addr[6]; } T_BLE_ADDR;

typedef struct {
    uint32_t data_len;
    uint8_t *p_data;
} T_BT_ADV_CUSTOM_DATA;
typedef struct {
    uint8_t              flags;
    T_BT_ADV_CUSTOM_DATA custom_data;
} T_BT_ADV_DATA;

typedef void (*T_fBLE_CBFUNC_RX_DATA)(void *handle, T_BT_ATTRECV_CB *info, uint32_t info_len);
typedef uint16_t (*T_fBLE_CBFUNC_EVENT_NOTIFY)(void *handle, T_eBT_MSG msg, int32_t param1, int32_t param2);

typedef struct {
    T_fBLE_CBFUNC_RX_DATA      recv_cb;
    T_fBLE_CBFUNC_EVENT_NOTIFY evt_handle_cb;
    T_eBLE_ROLE_TYPE           role_type;
    T_fBLE_CBFUNC_RX_DATA      read_cb;
    uint8_t                    adv_def_on;
    uint8_t                    lp_recovery_enable;
} T_BLE_INIT_PARAM;

T_eBLE_RESULT com_ble_init(T_BLE_INIT_PARAM *init_param);
T_eBLE_RESULT com_ble_set_gatt_service(T_BT_ATTREC *gatt_serve_table, uint32_t table_len, uint8_t role_type);
T_eBLE_RESULT com_ble_server_notify_ext(T_pVOID p_lelink, uint8_t *p_send_buf, uint16_t len,
                                        uint16_t notify_att_rec_idx);
T_eBLE_RESULT com_ble_set_adv_enable(T_eBLE_ENABLE_TYPE adv_enable);
T_eBLE_RESULT com_ble_set_adv_interval(uint8_t adv_type, uint16_t interval_min, uint16_t interval_max,
                                       uint8_t peer_addr_type, T_BLE_ADDR peer_addr);
T_eBLE_RESULT com_ble_set_adv_data(T_BT_ADV_DATA *p_adv_data, T_BT_ADV_DATA *p_scan_rsp_data);
uint8_t       com_ble_set_mtu_size(T_pVOID handle, uint16_t mtu);
uint8_t      *com_ble_get_dev_addr(void);
#endif
