#include "dura_legacy_protocol.h"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOG_BUFFER_HEX_LEVEL(...) ((void)0)
static char dispatched[128];
void fixture_reset_route(void){dispatched[0]=0;}
const char *fixture_dispatched(void){return dispatched;}
static int gama_blefob_dispatch_text(const char*s){strlcpy(dispatched,s,sizeof(dispatched));return ESP_OK;}
typedef int dura_button_t;
enum { DURA_BUTTON_UP,DURA_BUTTON_DOWN,DURA_BUTTON_SELECT,DURA_BUTTON_BACK };
typedef int dura_board_ui_snapshot_t;
static int dura_board_enqueue_button(int b){return ESP_OK;}
static int dura_board_get_ui_snapshot(int*s){return ESP_OK;}
static int dura_legacy_notify_snapshot(const int*s){return ESP_OK;}
