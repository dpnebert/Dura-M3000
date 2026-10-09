#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
void vTaskDelay(unsigned ticks);
#define CONFIG_BT_ENABLED 1
#define CONFIG_BT_NIMBLE_ENABLED 1
#define CONFIG_BLE_SHELL_EVENT_QUEUE_LEN 8
#define CONFIG_BLE_SHELL_QUEUE_RECEIVE_TIMEOUT_MS 100
#define CONFIG_BLE_SHELL_STRING_FIELD_LEN 64
#define CONFIG_BLE_SHELL_TASK_PRIORITY 3
#define CONFIG_BLE_SHELL_PAIR_BUTTON_GPIO -1
#define CONFIG_BLE_SHELL_MFG_DATA_LEN 8
#define CONFIG_BLE_SHELL_MFG_COMPANY_ID 1
#define CONFIG_BLE_SHELL_ADV_MIN_DURATION_MS 1
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_ERROR_CHECK(x) assert((x)==0)
#define pdMS_TO_TICKS(x) (x)
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_IO_NO_INPUT_OUTPUT 0
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_HS_ADV_F_DISC_GEN 1
#define BLE_HS_ADV_F_BREDR_UNSUP 2
#define BLE_GAP_CONN_MODE_UND 2
#define BLE_GAP_DISC_MODE_GEN 2
#define BLE_GAP_EVENT_CONNECT 1
#define BLE_GAP_EVENT_DISCONNECT 2
#define BLE_GAP_EVENT_SUBSCRIBE 3
#define BLE_GAP_EVENT_ENC_CHANGE 4
#define BLE_GAP_EVENT_ADV_COMPLETE 5
#define BLE_UUID128(...) NULL
#define BLE_UUID128_DECLARE(...) NULL
#include "gama_blefob.h"
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
struct ble_gatt_svc_def {int unused;};
struct ble_gap_event {int type; struct {int status;uint16_t conn_handle;} connect; struct {int status;} enc_change; struct {uint16_t attr_handle;bool cur_notify;} subscribe;};
struct ble_gap_adv_params {int conn_mode,disc_mode;};
struct ble_hs_adv_fields {int flags;uint8_t *name;uint8_t name_len,name_is_complete;void *uuids128;int num_uuids128,uuids128_is_complete;uint8_t *mfg_data;size_t mfg_data_len;};
struct ble_npl_event {void *event;};
typedef void ble_npl_event_fn(struct ble_npl_event *);
struct ble_npl_event_freertos {bool queued;ble_npl_event_fn *fn;void *arg;};
struct ble_hs_cfg_struct {void (*reset_cb)(int);void (*sync_cb)(void);int sm_io_cap,sm_bonding,sm_mitm,sm_sc,sm_our_key_dist,sm_their_key_dist;};
extern struct ble_hs_cfg_struct ble_hs_cfg;
struct os_mbuf;
#define BLE_ATT_ATTR_MAX_LEN 512
#define BLE_ATT_ERR_INSUFFICIENT_RES 0x11
uint16_t ble_att_mtu(uint16_t conn);
int os_mbuf_append(struct os_mbuf *om, const void *data, uint16_t len);
#include "ble_sleep_types.h"
extern const char *ble_test;
extern int stop_calls,deinit_calls,init_calls,adv_calls,host_starts,worker_starts;
extern bool radio_up,host_link,host_done;
extern int (*gap_cb)(struct ble_gap_event*,void*);
void test_connect(void);
void test_disconnect(void);
void test_shutdown_injection(void);
int gama_blefob_sleep_shutdown(void);
int gama_blefob_sleep_resume(void);
bool ble_shell_stack_enter(void);
void ble_shell_stack_exit(void);
QueueHandle_t xQueueCreate(unsigned,size_t);
int xQueueReceive(QueueHandle_t,void*,unsigned);
int xQueuePeek(QueueHandle_t,void*,unsigned);
void test_worker_run(bool binary);
void test_worker_interleave(void);
void ble_shell_handle_command(char*);
void test_event_pool_teardown(void);
int xTaskCreate(void(*)(void*),const char*,unsigned,void*,unsigned,void*);
void nimble_port_run(void);
void nimble_port_freertos_init(void(*)(void*));
void nimble_port_freertos_deinit(void);
int nimble_port_init(void);
int nimble_port_stop(void);
int nimble_port_deinit(void);
int nvs_flash_init(void);
void ble_svc_gap_init(void);
void ble_svc_gatt_init(void);
int ble_svc_gap_device_name_set(const char*);
const char *ble_svc_gap_device_name(void);
int ble_gatts_count_cfg(const struct ble_gatt_svc_def*);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def*);
int ble_gap_adv_active(void);
int ble_gap_adv_stop(void);
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields*);
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields*);
int ble_gap_adv_start(uint8_t,void*,int32_t,const struct ble_gap_adv_params*,int(*)(struct ble_gap_event*,void*),void*);
int ble_hs_id_infer_auto(int,uint8_t*);
int ble_gap_terminate(uint16_t,int);
typedef int ble_gap_conn_foreach_handle_fn(uint16_t,void*);
void ble_gap_conn_foreach_handle(ble_gap_conn_foreach_handle_fn*,void*);
void ble_npl_event_init(struct ble_npl_event*,void(*)(struct ble_npl_event*),void*);
void ble_npl_event_deinit(struct ble_npl_event*);
void ble_npl_eventq_put(void*,struct ble_npl_event*);
void *nimble_port_get_dflt_eventq(void);
int test_sem_take(SemaphoreHandle_t);
bool test_sem_give(SemaphoreHandle_t);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
void vTaskSuspend(void*);
int64_t esp_timer_get_time(void);
int esp_deep_sleep_try_to_start(void);
struct ble_gap_conn_desc {struct {bool encrypted;} sec_state;};
struct os_mbuf {int unused;};
int ble_gap_conn_find(uint16_t,struct ble_gap_conn_desc*);
struct os_mbuf *ble_hs_mbuf_from_flat(const void*,size_t);
int ble_gatts_notify_custom(uint16_t,uint16_t,struct os_mbuf*);
size_t strlcpy(char*,const char*,size_t);
