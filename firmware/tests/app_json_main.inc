#include "dura_meter.h"
#include "dura_recipe.h"
#include "dura_coordinator.h"
#include "dura_peers.h"
static char s_assigned_system_id[32]="system\"\\\n",s_assigned_system_name[32]="name\"\\\r";
static bool system_assignment_configured(void){return true;}
static const char *dura_board_power_source_name(void){return "battery";}
static bool dura_board_ext_power_present(void){return false;}
static bool dura_board_boost_enabled(void){return false;}
