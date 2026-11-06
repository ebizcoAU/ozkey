/* oz_port_esp32c6 — the C6 port's own setup calls (ozkey-55 F5). */
#ifndef OZ_PORT_ESP32C6_H
#define OZ_PORT_ESP32C6_H
#ifdef __cplusplus
extern "C" {
#endif

/* Open NVS (namespace "ozkie"). Idempotent; kv calls open it on first use too.
 * OZ_OK or OZ_E_HW. */
int oz_esp32c6_init(void);

/* Erase every OZKIE key: a factory reset of the lock's identity and bonds. */
int oz_esp32c6_kv_wipe(void);

#ifdef __cplusplus
}
#endif
#endif
