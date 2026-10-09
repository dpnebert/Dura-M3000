/* Board transaction above is extracted verbatim; assertions inspect effects. */
static int rendered_fail;
static void linked(void*p){CHECK(dura_board_set_ble_connected(true)==ESP_OK);}
static void unlinked(void*p){CHECK(dura_board_set_ble_connected(false)==ESP_OK);}
static int command_dispatches;
static esp_err_t control_command(const uint8_t *data,size_t len,void *ctx){
 CHECK(!strcmp((const char*)data,"manual_start"));
 ++command_dispatches;
 return dura_meter_start_manual();
}
void ble_shell_handle_command(char *data){CHECK(control_command((const uint8_t*)data,strlen(data),NULL)==ESP_OK);}
void test_worker_interleave(void){
 /* Deterministic preemption inside dequeue, after queue ownership transfers;
  * worker_peek instead pauses before ownership, so the shutdown drain wins. */
 CHECK(dura_board_enter_deep_sleep()!=ESP_OK);
 CHECK(!s_sleep_recovery_pending&&radio_up);
}
void test_shutdown_injection(void){
 CHECK(s_sleep_recovery_pending);CHECK(dura_meter_start_manual()==ESP_ERR_INVALID_STATE);
 const int before=adv_calls;
 if(!strcmp(test,"racing")){test_connect();test_disconnect();CHECK(!s_ble_connected);}
 ble_shell_open_pairing_window(60,"late external pair request");
 CHECK(adv_calls==before);
 const uint8_t frame[]={1,2,3};
 CHECK(gama_blefob_notify_binary(frame,sizeof(frame))==ESP_ERR_INVALID_STATE);
 bool active=true;CHECK(gama_blefob_is_advertising(&active)==ESP_ERR_INVALID_STATE&&!active);
 if(!strcmp(test,"activity_shutdown"))dura_board_note_activity();
}
int esp_deep_sleep_try_to_start(void){
 CHECK(!radio_up&&stop_calls&&deinit_calls&&host_done);
 CHECK(!light&&!output[0]&&!output[1]&&!output[2]);
 expected_recovery_light=false; /* Transaction blanking must survive RTC rollback. */
 CHECK(dura_meter_start_manual()==ESP_ERR_INVALID_STATE);
 ++sleep_calls;
 if(!strcmp(test,"shutdown")||!strcmp(test,"racing")||!strcmp(test,"retry_success"))longjmp(slept,1);
 if(!strcmp(test,"restore_fail_radio"))restore_error=1;
 return ESP_FAIL;
}
int main(int argc,char**argv){
 CHECK(argc==2);test=argv[1];ble_test=test;
 CHECK(dura_meter_init()==ESP_OK);CHECK(dura_meter_set_output_handler(outputs)==ESP_OK);
 /* Reproduce board boot's independent clock initialization. */
 dura_board_note_user_activity();
 const bool dark_off=strstr(test,"off_reject")!=NULL;
 const bool dark_expired=strstr(test,"expired_reject")!=NULL;
 if(dark_off)CHECK(dura_meter_set_backlight_timeout(9)==ESP_OK);
 if(dark_expired)now+=31000000;
 expected_policy_light=expected_recovery_light=!(dark_off||dark_expired);
 update_backlight();light_on_calls=0;
 const int64_t backlight_before=s_backlight_activity_us;
 if(!strcmp(test,"paused_reject")||dark_off||dark_expired){
   CHECK(dura_meter_start_batch_mode(10,DURA_OPERATION_RECIRC)==ESP_OK);
   CHECK(dura_meter_pause_batch()==ESP_OK);
 }
 CHECK(gama_blefob_init(NULL)==ESP_OK);
 s_app_cfg.connected_handler=linked;s_app_cfg.disconnected_handler=unlinked;
 if(!strncmp(test,"worker_",7)){
   s_app_cfg.binary_write_handler=control_command;
   test_worker_run(strcmp(test,"worker_text")!=0);
   printf("worker dispatches=%d completed_sleep_rollbacks=%d\n",command_dispatches,sleep_calls);
   if(!strcmp(test,"worker_peek")){
     CHECK(sleep_calls==1&&command_dispatches==0&&!output[0]);
     /* Empty nonblocking receive must release its lease, not poison the next
      * admission. Also exercises another SDK pool generation. */
     now+=400000000;
     CHECK(dura_board_enter_deep_sleep()!=ESP_OK);
     CHECK(sleep_calls==2&&!s_sleep_recovery_pending&&radio_up);
   }
   else CHECK(sleep_calls==0&&stop_calls==0&&command_dispatches==1&&output[0]);
   CHECK(ble_shell_stack_enter());ble_shell_stack_exit();
   puts("PASS worker ownership interleaving");return 0;
 }
 if(!strcmp(test,"disabled")){s_cfg.ble_advertising_enabled=false;ble_shell_close_pairing_window();}
 if(!strcmp(test,"established"))test_connect();
 if(!strcmp(test,"host_established"))host_link=true; /* table populated, callback pending */
 if(!strcmp(test,"busy_user"))CHECK(ble_shell_stack_enter());
 if(!strcmp(test,"veto_retry")){
   CHECK(ble_shell_stack_enter());
   CHECK(dura_board_enter_deep_sleep()!=ESP_OK);
   CHECK(!stop_calls&&!sleep_calls&&radio_up);
   ble_shell_stack_exit();now+=400000000;
   /* The retry delay expires illumination without accepted user activity. */
   expected_policy_light=expected_recovery_light=false;
   update_backlight(); /* Execute the owner tick after advancing the fake clock. */
 }
 int e=0;
 if(!setjmp(slept))e=dura_board_enter_deep_sleep();
 if(!strcmp(test,"established")||!strcmp(test,"host_established")||!strcmp(test,"busy_user")){
   CHECK(!sleep_calls&&!stop_calls&&!deinit_calls&&e!=ESP_OK&&radio_up);
   if(!strcmp(test,"busy_user"))ble_shell_stack_exit();
   return 0;
 }
 CHECK(stop_calls==1); /* baseline RED: board never invokes normal shutdown */
 if(!strcmp(test,"shutdown")||!strcmp(test,"racing")){
   CHECK(sleep_calls==1&&deinit_calls==1&&!radio_up&&!light);return 0;
 }
 CHECK(e!=ESP_OK);
 const bool failed_radio=!strcmp(test,"stop_fail")||!strcmp(test,"deinit_fail")||!strcmp(test,"init_fail")||!strcmp(test,"sync_fail")||!strcmp(test,"adv_fail");
 if(failed_radio||!strcmp(test,"restore_fail_radio")||!strcmp(test,"render_fail_radio")){
   CHECK(s_sleep_recovery_pending);CHECK(dura_meter_start_manual()==ESP_ERR_INVALID_STATE);
 }else{
   CHECK(!s_sleep_recovery_pending);CHECK(radio_up&&s_ble_synced);CHECK(light==expected_policy_light&&!ext1);
   CHECK(s_backlight_activity_us==backlight_before);
   if(dark_off||dark_expired)CHECK(light_on_calls==0);
   for(int i=0;i<6;i++)CHECK(!rtc[i]&&!hold[i]&&intr[i]==(i<4?0:3));
   if(strcmp(test,"disabled"))CHECK(ble_gap_adv_active());
   else CHECK(!ble_gap_adv_active());
   CHECK(!output[0]&&!output[1]&&!output[2]);
   if(!strcmp(test,"paused_reject")||dark_off||dark_expired){dura_meter_snapshot_t m;CHECK(dura_meter_get_snapshot(&m)==ESP_OK);CHECK(m.batch_mode==DURA_BATCH_PAUSED&&m.operation_mode==DURA_OPERATION_RECIRC&&!m.pump_enabled);}
   test_connect();CHECK(s_ble_connected);test_disconnect();CHECK(!s_ble_connected);
 }
 if(!strcmp(test,"reject_retry")||!strcmp(test,"veto_retry")){
   test=ble_test="retry_success";now+=400000000;
   if(!setjmp(slept))dura_board_enter_deep_sleep();
   CHECK(sleep_calls==2&&stop_calls==2&&deinit_calls==2&&init_calls==2&&worker_starts==1);
 }
 puts("PASS real board/BLE sleep path");return 0;
}
