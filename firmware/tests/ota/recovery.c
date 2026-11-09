int main(int argc,char **argv)
{
    assert(argc==2);
    const char *t=argv[1];
    atomic_store(&s_polling,true);
    if(!strcmp(t,"fresh_boot")){state_error=ESP_ERR_NOT_FOUND;assert(!dura_ota_boot_begin());assert(!deadline_us);}
    else if(!strcmp(t,"verify_good")){
        running_part=&slot1;image_state=ESP_OTA_IMG_PENDING_VERIFY;
        assert(!dura_ota_boot_begin());assert(active&&deadline_us==30000000);
        assert(!dura_ota_boot_finish(true));assert(validated_at&&cleared_at>validated_at&&!active);
    }else if(!strcmp(t,"verify_bad")){
        image_state=ESP_OTA_IMG_PENDING_VERIFY;assert(!dura_ota_boot_begin());
        assert(dura_ota_boot_finish(false)!=0);assert(rollback_calls==1&&!cleared_at&&s_pending_verify);
    }else if(!strcmp(t,"verify_mark_failure")){
        image_state=ESP_OTA_IMG_PENDING_VERIFY;assert(!dura_ota_boot_begin());valid_error=ESP_FAIL;
        assert(dura_ota_boot_finish(true)!=0);assert(!cleared_at&&s_pending_verify);
    }else if(!strcmp(t,"verify_deadline")){
        image_state=ESP_OTA_IMG_PENDING_VERIFY;assert(!dura_ota_boot_begin());
        if(!setjmp(jump))deadline_cb(NULL);assert(reset_calls==1&&!validated_at&&!cleared_at);
    }else if(!strcmp(t,"undefined_cleanup")){
        running_part=&slot1;image_state=0;assert(!dura_ota_boot_begin());
        assert(dura_ota_boot_finish(true)==ESP_ERR_INVALID_STATE);assert(!cleared_at);
    }else if(!strcmp(t,"cleanup_retry")){
        running_part=&slot1;assert(!dura_ota_boot_begin());reset_error=ESP_FAIL;
        assert(dura_ota_boot_finish(true)==ESP_FAIL);reset_error=0;
        assert(!dura_ota_boot_finish(true));assert(reset_calls==2&&!validated_at);
    }else if(!strcmp(t,"slot_gate")){
        assert(dura_ota_update_allowed());
        running_part=&slot1;boot_part=&slot1;assert(dura_ota_update_allowed());
        running_part=&slot0;boot_part=&slot1;assert(!dura_ota_update_allowed());
        boot_part=&slot0;s_pending_verify=true;assert(!dura_ota_update_allowed());
    }else if(!strcmp(t,"task_failure")){
        atomic_store(&s_polling,false);task_error=1;assert(dura_ota_start_polling()==ESP_ERR_NO_MEM);
        assert(!atomic_load(&s_polling));task_error=0;assert(!dura_ota_start_polling());
        assert(task_calls==2);assert(!dura_ota_start_polling());assert(task_calls==2);
    }else{
        if(!strcmp(t,"same_build"))candidate_desc=running_desc;
        if(!strcmp(t,"failed_build")){has_invalid=1;candidate_desc=failed_desc;}
        if(!strcmp(t,"wrong_project"))strcpy(candidate_desc.project_name,"other");
        if(!strcmp(t,"descriptor_failure"))desc_error=ESP_FAIL;
        if(!strcmp(t,"begin_failure"))begin_error=ESP_FAIL;
        if(!strcmp(t,"interrupted"))perform_error=ESP_FAIL;
        if(!strcmp(t,"incomplete"))complete=0;
        if(!strcmp(t,"finish_failure"))finish_error=ESP_FAIL;
        if(!strcmp(t,"success")){
            if(!setjmp(jump))perform_ota();
            assert(boot_part==&slot1&&reset_calls==1&&!cleared_at&&!validated_at&&finish_calls==1&&!abort_calls);
        }else{
            assert(perform_ota()!=ESP_OK);assert(boot_part==&slot0&&!cleared_at&&!validated_at);
            if(!strcmp(t,"begin_failure")||!strcmp(t,"finish_failure"))assert(!abort_calls);
            else assert(abort_calls==1);
            if(!strcmp(t,"same_build")||!strcmp(t,"failed_build")||!strcmp(t,"wrong_project"))assert(!erase_calls);
        }
    }
    puts(t);return 0;
}
