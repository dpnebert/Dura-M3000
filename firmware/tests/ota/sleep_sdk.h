/* Additional SDK boundaries for the real final sleep-admission check. */
#define portENTER_CRITICAL(p) ((void)0)
#define portEXIT_CRITICAL(p) ((void)0)
#define s_ota_active active
#define DURA_BUTTON_NONE 0
#define DURA_BUTTON_ACTIVE_LEVEL 0
static int s_ui_snapshot_mux;
static bool s_ble_connected;
static uint64_t s_activity_generation,s_sleep_touched_mask;
static void *s_button_queue,*s_flow_event_queue;
static struct {int gpio;}s_buttons[]={{0},{1},{2},{3}};
static unsigned uxQueueMessagesWaiting(void*p){return 0;}
static int rtc_gpio_get_level(int gpio){return 1;}
static int dura_board_poll_button(void){return DURA_BUTTON_NONE;}
