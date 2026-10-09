/* Real board owner + meter platform seams are shared with board policy tests. */
#define main board_fixture_main
#include "backlight_policy.c"
#undef main
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <strings.h>
#undef CONFIG_DURA_BOARD_SETTINGS_HOLD_MS
#include "dura_recipe.h"
#define dura_board_dump_button_io_config(...) abort()
#include "dura_coordinator.h"
typedef int dura_test_recipe_preset_t;
static char s_assigned_system_id[32],s_assigned_system_name[32];
static int gama_blefob_set_fault_latched(bool b){return ESP_OK;}
static int gama_blefob_set_fault_code(uint32_t c){return ESP_OK;}
#include "app_leaves.inc"
#include "app_activity.inc"
int main(int argc,char **argv){
 CHECK(argc==2);test=argv[1];CHECK(dura_meter_init()==ESP_OK);
 off_setting=is("off_command");CHECK(dura_meter_set_backlight_timeout(off_setting?9:10)==ESP_OK);
 now=1000000;CHECK(dura_board_init()==ESP_OK);now+=40000000;update_backlight();CHECK(!light);
 const char *cmd=NULL;bool accepted=true;
 if(is("start_batch")||is("off_command"))cmd="start_batch 10";
 else if(is("stop_batch")){CHECK(dura_meter_start_batch(10)==ESP_OK);cmd="stop_batch";}
 else if(is("start_cal"))cmd="start_cal";
 else if(is("stop_cal")){CHECK(dura_meter_start_calibration()==ESP_OK);cmd="stop_cal";}
 else if(is("auto_batch"))cmd="auto_batch on";
 else if(is("sim_pulse"))cmd="sim_pulse 2";
 else if(is("set_fault"))cmd="set_fault 7";
 else if(is("clear_fault")){CHECK(dura_meter_set_fault(7)==ESP_OK);cmd="clear_fault";}
 else if(is("defaults"))cmd="meter_defaults";
 else if(is("save"))cmd="meter_save";
 else if(is("load"))cmd="meter_load";
 else {accepted=false;
  if(is("status"))cmd="meter_status";
  else if(is("config"))cmd="meter_config";
  else if(is("help"))cmd="help";
  else if(is("bad_amount"))cmd="start_batch -1";
  else if(is("bad_bool"))cmd="auto_batch perhaps";
  else if(is("bad_count"))cmd="sim_pulse 1x";
  else if(is("unknown"))cmd="not_a_command";
  else if(is("busy")){CHECK(dura_meter_start_calibration()==ESP_OK);cmd="start_batch 10";}
 }
 CHECK(cmd);int64_t before=s_backlight_activity_us;char response[4096]={0};
 int err=dura_execute_command(cmd,response,sizeof(response),NULL);
 CHECK(err==ESP_OK||is("unknown"));
 if(is("load")&&strstr(response,"error:"))accepted=false;
 if(is("busy"))CHECK(strstr(response,"error:"));
 CHECK((s_backlight_activity_us!=before)==accepted);
 update_backlight();CHECK(light==(accepted&&!off_setting));
 printf("PASS %s: %s\n",test,response);return 0;
}
