/* SDK boundary doubles only. Production OTA, provisioning and handoff C is
 * compiled verbatim by test_ota_recovery.py. No flash or radio is accessed. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <setjmp.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_VERSION 4
#define ESP_ERR_NO_MEM 5
#define ESP_ERR_WIFI_NOT_STARTED 6
#define ESP_ERR_NVS_NO_FREE_PAGES 7
#define ESP_ERR_NVS_NEW_VERSION_FOUND 8
#define ESP_ERR_HTTPS_OTA_IN_PROGRESS 9
#define ESP_OTA_IMG_VALID 1
#define ESP_OTA_IMG_PENDING_VERIFY 2
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 16
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 17
#define CONFIG_DURA_OTA_URL "https://test.invalid/firmware.bin"
#define CONFIG_DURA_OTA_POLL_SECONDS 60
#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1
#define CONFIG_DURA_BOARD_SETTINGS_HOLD_MS 1500
#define DURA_OPERATION_IDLE 0
#define DURA_UI_MAIN_MENU 0
#define DURA_UI_OTA_PROVISION 99
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define tskIDLE_PRIORITY 0
#define WIFI_EVENT 1
#define IP_EVENT 2
#define NETWORK_PROV_EVENT 3
#define WIFI_EVENT_STA_DISCONNECTED 4
#define IP_EVENT_STA_GOT_IP 5
#define NETWORK_PROV_WIFI_CRED_SUCCESS 6
#define NETWORK_PROV_WIFI_CRED_FAIL 7
#define NETWORK_PROV_END 8
#define ESP_EVENT_ANY_ID -1
#define NETWORK_PROV_EVENT_HANDLER_NONE {0}
#define NETWORK_PROV_SECURITY_1 1
#define WIFI_INIT_CONFIG_DEFAULT() {0}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_RETURN_ON_ERROR(x,...) do { int r_=(x); if(r_) return r_; }while(0)
#define ESP_RETURN_ON_FALSE(x,e,...) do { if(!(x)) return (e); }while(0)
#define ESP_ERROR_CHECK(x) do { if((x)!=ESP_OK) longjmp(jump, 2); }while(0)
static jmp_buf jump;
static int sequence, validated_at, cleared_at, reset_calls, rollback_calls;
static int abort_calls, finish_calls, erase_calls, begin_error, desc_error;
static int perform_error, finish_error, valid_error, reset_error, state_error;
static int task_error, task_calls, complete=1, has_invalid;
static int fence, active, prepare_error, shutdown_error, resume_error, provision_error;
static int resume_calls, provision_calls, connects, deinit_calls;
static int nvs_error, nvs_erases, heartbeat_stalled, ble_missing, clock_ms;
static unsigned heartbeat;
static void *s_app_command_queue=(void*)1, *s_app_command_reply_queue=(void*)1, *s_app_command_submit_mutex=(void*)1;
static void (*created_task)(void*);
static void (*deadline_cb)(void*);
static uint64_t deadline_us;
typedef int esp_ota_img_states_t;
static int image_state=ESP_OTA_IMG_VALID;
typedef struct { unsigned subtype,address; } esp_partition_t;
static esp_partition_t slot0={16,0x20000},slot1={17,0x320000};
static const esp_partition_t *running_part=&slot0,*boot_part=&slot0,*next_part=&slot1;
typedef struct { char project_name[32]; uint8_t app_elf_sha256[32]; } esp_app_desc_t;
static esp_app_desc_t running_desc={"dura",{1}},candidate_desc={"dura",{2}},failed_desc={"dura",{3}};
static const esp_partition_t *esp_ota_get_running_partition(void){return running_part;}
static const esp_partition_t *esp_ota_get_boot_partition(void){return boot_part;}
static const esp_partition_t *esp_ota_get_next_update_partition(const void*p){
    return running_part == &slot0 ? &slot1 : &slot0;
}
static int esp_ota_get_state_partition(const void*p,int*s){*s=image_state;return state_error;}
static const esp_app_desc_t *esp_app_get_description(void){return &running_desc;}
static const esp_partition_t *esp_ota_get_last_invalid_partition(void){return has_invalid?&slot1:NULL;}
static int esp_ota_get_partition_description(const void*p,esp_app_desc_t*d){*d=failed_desc;return 0;}
static int esp_ota_mark_app_valid_cancel_rollback(void){validated_at=++sequence;if(!valid_error)image_state=ESP_OTA_IMG_VALID;return valid_error;}
static int esp_ota_mark_app_invalid_rollback_and_reboot(void){++rollback_calls;return ESP_FAIL;}
static void esp_restart(void){++reset_calls;longjmp(jump,1);}
typedef void *esp_timer_handle_t;
typedef struct { void(*callback)(void*);const char *name; } esp_timer_create_args_t;
static int esp_timer_create(const esp_timer_create_args_t*a,void**h){deadline_cb=a->callback;*h=(void*)1;return 0;}
static int esp_timer_start_once(void*h,uint64_t us){deadline_us=us;return 0;}
static int esp_timer_stop(void*h){return 0;}
static int esp_timer_delete(void*h){return 0;}
static void dura_board_set_ota_active(bool v){active=v;}
static unsigned dura_board_healthy_cycles(void){return heartbeat;}
static void dura_board_note_user_activity(void){}
static const char *esp_err_to_name(int x){return "stub";}
static int esp_crt_bundle_attach(void*p){return 0;}
typedef struct {const char*url;int(*crt_bundle_attach)(void*);int timeout_ms,keep_alive_enable,buffer_size,buffer_size_tx;} esp_http_client_config_t;
typedef struct {const esp_http_client_config_t*http_config;} esp_https_ota_config_t;
typedef void *esp_https_ota_handle_t;
static int esp_https_ota_begin(const esp_https_ota_config_t*c,void**h){if(begin_error)return begin_error;*h=(void*)1;return 0;}
static int esp_https_ota_get_img_desc(void*h,esp_app_desc_t*d){*d=candidate_desc;return desc_error;}
static int esp_https_ota_perform(void*h){++erase_calls;return perform_error;}
static int esp_https_ota_is_complete_data_received(void*h){return complete;}
static int esp_https_ota_abort(void*h){assert(h);++abort_calls;return 0;}
static int esp_https_ota_finish(void*h){++finish_calls;if(!finish_error)boot_part=&slot1;return finish_error;}
static int xTaskCreate(void(*fn)(void*),const char*n,int stack,void*a,int priority,void*h){++task_calls;created_task=fn;return task_error?0:pdPASS;}
static void vTaskDelete(void*p){}
static void vTaskDelay(unsigned ms){clock_ms+=ms;if(!heartbeat_stalled)++heartbeat;}
static int dura_meter_prepare_for_sleep(void){fence=1;return prepare_error;}
static int dura_meter_cancel_sleep_prepare(void){fence=0;return 0;}
static int gama_blefob_sleep_shutdown(void){return shutdown_error;}
static int gama_blefob_sleep_resume(void){++resume_calls;return resume_error;}
static int gama_blefob_is_advertising(bool*p){*p=!ble_missing;return 0;}
static int gama_blefob_is_connected(bool*p){*p=false;return 0;}
typedef struct {bool pump_enabled,fault_latched;int operation_mode;} dura_meter_snapshot_t;
static int dura_meter_get_snapshot(dura_meter_snapshot_t*m){memset(m,0,sizeof(*m));return 0;}
static int nvs_flash_init(void){return nvs_error;}
static int nvs_flash_erase(void){++nvs_erases;nvs_error=0;return 0;}
#ifdef TEST_WIFI
static int dura_ota_start_polling(void){++provision_calls;return provision_error;}
typedef int esp_event_base_t;
typedef struct {int unused;} wifi_init_config_t;
typedef struct {int event_cb;} network_prov_event_handler_t;
typedef struct {int scheme;network_prov_event_handler_t scheme_event_handler,app_event_handler;struct {int wifi_conn_attempts;}network_prov_wifi_conn_cfg;}network_prov_mgr_config_t;
static int network_prov_scheme_ble;
static int esp_netif_init(void){return 0;}
static int esp_event_loop_create_default(void){return 0;}
static void *esp_netif_create_default_wifi_sta(void){return (void*)1;}
static int esp_wifi_init(const void*c){return 0;}
static int esp_event_handler_register(int base,int id,void(*fn)(void*,int,int,void*),void*a){return 0;}
static int esp_wifi_stop(void){return ESP_ERR_WIFI_NOT_STARTED;}
static int esp_wifi_restore(void){++reset_calls;return 0;}
static int esp_wifi_connect(void){++connects;return 0;}
static int network_prov_mgr_init(network_prov_mgr_config_t c){assert(c.network_prov_wifi_conn_cfg.wifi_conn_attempts==3);return 0;}
static int network_prov_mgr_start_provisioning(int s,const char*p,const char*n,void*k){return 0;}
static int network_prov_mgr_deinit(void){++deinit_calls;return 0;}
static int network_prov_mgr_reset_wifi_sm_state_on_failure(void){++reset_calls;return 0;}
#else
static int dura_wifi_reset_provisioning(void){cleared_at=++sequence;++reset_calls;return reset_error;}
static int dura_wifi_start_provisioning(void){++provision_calls;return provision_error;}
#endif
