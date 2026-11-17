int main(int argc,char**argv)
{
    assert(argc==2);const char*t=argv[1];
    if(!strcmp(t,"credential_reset")){
        assert(!dura_wifi_reset_provisioning());assert(s_wifi_initialized&&s_stopping&&reset_calls==1);
    }else if(!strcmp(t,"disconnect")){
        s_provisioning_manager_initialized=true;
        wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);assert(!connects);
        s_provisioning_manager_initialized=false;
        wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);assert(connects==1);
        s_stopping=true;
        wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);assert(connects==1);
    }else{
        assert(!dura_wifi_start_provisioning());
        if(!strcmp(t,"failure")){
            provisioning_event_handler(NULL,NETWORK_PROV_EVENT,NETWORK_PROV_WIFI_CRED_FAIL,NULL);
            assert(reset_calls==1&&!provision_calls&&!s_credentials_accepted);
        }else{
            if(!strcmp(t,"success"))provisioning_event_handler(NULL,NETWORK_PROV_EVENT,NETWORK_PROV_WIFI_CRED_SUCCESS,NULL);
            provisioning_event_handler(NULL,NETWORK_PROV_EVENT,NETWORK_PROV_END,NULL);
            provisioning_event_handler(NULL,NETWORK_PROV_EVENT,NETWORK_PROV_END,NULL);
            assert(task_calls==1);
            if(!setjmp(jump))created_task(NULL);
            assert(deinit_calls==1);
            if(!strcmp(t,"success"))assert(provision_calls==1&&!reset_calls&&connects==1);
            else assert(!provision_calls&&reset_calls==1);
        }
    }
    puts(t);return 0;
}
