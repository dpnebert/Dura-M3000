static void activity_text(const char *cmd,bool accepted){
 int64_t before=s_backlight_activity_us;++now;
 (void)write_bytes((const uint8_t*)cmd,strlen(cmd));
 assert((s_backlight_activity_us!=before)==accepted);
}
int main(int argc,char **argv){
 assert(argc==2);bind_app();s_cfg.ping_debug_enabled=true;
 const char *test=argv[1];
 if(!strcmp(test,"passive")){
  const char *cmds[]={"ping","info","status","config","help","get_status","get_config"};
  for(unsigned i=0;i<sizeof(cmds)/sizeof(*cmds);i++)activity_text(cmds[i],false);
  const uint8_t connect[]={1,10,245},poll[]={1,28,227};
  assert(write_bytes(connect,3)==ESP_OK);assert(write_bytes(poll,3)==ESP_OK);assert(!s_backlight_activity_us);
 }else if(!strcmp(test,"invalid")){
  activity_text("set rf_button 3",false);activity_text("set rf_button 1x",false);activity_text("unknown",false);
  const uint8_t bad[]={2,30,1,0};assert(write_bytes(bad,4)!=ESP_OK);assert(!s_backlight_activity_us);
 }else if(!strcmp(test,"buttons")){
  activity_text("set rf_button 1",true);activity_text("clear rf_button 1",false);
  int64_t before=s_backlight_activity_us;++now;
  const uint8_t press[]={2,30,2,(uint8_t)~(30^2)};assert(write_bytes(press,4)==ESP_OK);assert(s_backlight_activity_us!=before);
 }else if(!strcmp(test,"full")){
  queue_full=true;activity_text("set rf_button 1",false);
  const uint8_t press[]={2,30,2,(uint8_t)~(30^2)};assert(write_bytes(press,4)!=ESP_OK);assert(!s_backlight_activity_us);
 }else if(!strcmp(test,"generic")){
  const char *cmds[]={"unlock test","lock","pair","stop_advertise","clear_faults","start_diagnostic","stop_diagnostic","set_name Meter","set_serial Serial","set_pair_window 30","ble_enable on","diag_enable on","ping_debug off"};
  for(unsigned i=0;i<sizeof(cmds)/sizeof(*cmds);i++)activity_text(cmds[i],true);
 }else if(!strcmp(test,"denied")){
  s_app_cfg.security_mode=GAMA_BLEFOB_SECURITY_PAIR_REQUIRED;
  activity_text("pair",false);activity_text("set_name Meter",false);activity_text("unlock wrong",false);
 }else if(!strcmp(test,"invalid_generic")){
  activity_text("set_pair_window 30x",false);activity_text("set_pair_window 0",false);
  activity_text("ble_enable rubbish",false);activity_text("diag_enable rubbish",false);activity_text("ping_debug rubbish",false);
 }else abort();
 puts("BLE_ACTIVITY_PASS");return 0;
}
