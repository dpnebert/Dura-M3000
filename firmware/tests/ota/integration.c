int main(int argc,char**argv)
{
    assert(argc==2);const char*t=argv[1];
    if(!strcmp(t,"ota_sleep_veto")){
        assert(sleep_attempt_current(0));active=true;assert(!sleep_attempt_current(0));
        active=false;s_ble_connected=true;assert(!sleep_attempt_current(0));
    }else if(!strncmp(t,"handoff",7)){
        if(!strcmp(t,"handoff_veto"))shutdown_error=ESP_ERR_INVALID_STATE;
        if(!strcmp(t,"handoff_partial")){shutdown_error=ESP_FAIL;resume_error=ESP_FAIL;}
        if(!strcmp(t,"handoff_start_failure"))provision_error=ESP_FAIL;
        if(!strcmp(t,"handoff_ok")){
            assert(!start_ota_wifi_provisioning());assert(fence&&active&&provision_calls==1);
        }else if(!strcmp(t,"handoff_veto")){
            assert(start_ota_wifi_provisioning()==ESP_ERR_INVALID_STATE);
            assert(!provision_calls&&resume_calls==1&&!fence&&!active);
        }else{
            if(!setjmp(jump))start_ota_wifi_provisioning();
            assert(reset_calls==1&&fence&&active);
            if(!strcmp(t,"handoff_partial"))assert(!provision_calls);
        }
    }else if(!strcmp(t,"nvs_preserved")){
        s_pending_verify=true;nvs_error=ESP_ERR_NVS_NEW_VERSION_FOUND;
        if(!setjmp(jump))init_nvs();assert(!nvs_erases);
    }else if(!strncmp(t,"gesture",7)){
        assert(handle_ota_gesture(true,true,0));assert(!gesture_calls);
        if(!strcmp(t,"gesture_cancel")){
            assert(handle_ota_gesture(true,false,100000));
            assert(handle_ota_gesture(true,true,2000000));assert(!gesture_calls);
            assert(!handle_ota_gesture(false,false,2100000));
        }else{
            if(!strcmp(t,"gesture_veto"))gesture_result=ESP_ERR_INVALID_STATE;
            assert(handle_ota_gesture(true,true,1500000));assert(gesture_calls==1);
            assert(s_ui_screen==(!gesture_result?DURA_UI_OTA_PROVISION:DURA_UI_MAIN_MENU));
            assert(handle_ota_gesture(false,true,1600000));
            assert(handle_ota_gesture(true,true,3000000));assert(gesture_calls==1);
            assert(!handle_ota_gesture(false,false,3100000));
        }
    }else{
        if(!strcmp(t,"stalled_board"))heartbeat_stalled=1;
        if(!strcmp(t,"missing_ble"))ble_missing=1;
        if(!strcmp(t,"missing_commands"))s_app_command_queue=NULL;
        bool result=controller_boot_healthy(true);
        assert(result==(!strcmp(t,"healthy")));
        assert(clock_ms<=5000);
    }
    puts(t);return 0;
}
