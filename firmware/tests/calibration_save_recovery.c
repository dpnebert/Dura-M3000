/* F01: reuse real locks and GPIO/NVS stub infrastructure from the safety suite.
 * Link the complete production meter; include verbatim board UI/output extracts.
 * No target hardware, power-loss atomicity, or physical LCD claim. */
#define main safe_output_suite_main
#define nvs_open base_nvs_open
#define nvs_set_u32 base_nvs_set_u32
#define nvs_set_u8 base_nvs_set_u8
#define nvs_set_blob base_nvs_set_blob
#define nvs_commit base_nvs_commit
#define xSemaphoreTake base_take
#include "safe_output.c"
#undef main
#undef nvs_open
#undef nvs_set_u32
#undef nvs_set_u8
#undef nvs_set_blob
#undef nvs_commit
#undef xSemaphoreTake
#include "board_calibration_ui.inc"

static dura_meter_snapshot_t before, staged, committed;
static const char *late_failure_key;
static unsigned open_calls, commit_calls;
static bool observe_pending, quick_mode;
static _Thread_local bool cal_thread;
static bool cal_take_entered;
static SemaphoreHandle_t save_lock;

static void same_coefficients(dura_cal_coefficients_t a, dura_cal_coefficients_t b)
{
    CHECK(a.gallon_per_count == b.gallon_per_count);
    CHECK(a.liter_per_count == b.liter_per_count);
    CHECK(a.ounce_per_count == b.ounce_per_count);
}
static void unpublished(void)
{
    dura_meter_snapshot_t s = snap();
    same_coefficients(s.working_coefficients, before.working_coefficients);
    CHECK(!memcmp(s.precal_profiles, before.precal_profiles, sizeof(s.precal_profiles)));
    CHECK(s.selected_precal_profile == before.selected_precal_profile);
    CHECK(s.quick_cal_reference == before.quick_cal_reference);
    off();
}
static void excluded_operations(void)
{
    uint32_t owner = 99;
    CHECK(dura_meter_start_manual() == ESP_ERR_INVALID_STATE);
    CHECK(dura_meter_start_batch(10) == ESP_ERR_INVALID_STATE);
    CHECK(dura_meter_start_batch_mode(10, DURA_OPERATION_RECIRC) == ESP_ERR_INVALID_STATE);
    CHECK(dura_meter_start_calibration() == ESP_ERR_INVALID_STATE);
    CHECK(dura_meter_claim_recipe(&owner) == ESP_ERR_INVALID_STATE);
    CHECK(owner == 0);
    CHECK(dura_meter_set_units(DURA_UNITS_LITER) == ESP_ERR_INVALID_STATE);
    CHECK(dura_meter_select_precal_profile(0) == ESP_ERR_INVALID_STATE);
    CHECK(dura_meter_store_working_precal_profile(0) == ESP_ERR_INVALID_STATE);
    off();
}
static void excluded(void)
{
    excluded_operations();
    CHECK(dura_meter_apply_quick_cal(1) == ESP_ERR_INVALID_STATE);
}
static void boundary(void)
{
    CHECK(!host_critical_depth);
    if (observe_pending) { unpublished(); excluded(); }
}
int xSemaphoreTake(SemaphoreHandle_t m, unsigned t)
{
    if (!save_lock) save_lock = m;
    if (cal_thread) {
        boundary();
        pthread_mutex_lock(&gate);
        cal_take_entered = true;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&gate);
    }
    return base_take(m, t);
}
int nvs_open(const char *s, int mode, nvs_handle_t *h)
{
    boundary();
    if (mode == NVS_READWRITE) { ++open_calls; staged = committed; }
    return base_nvs_open(s, mode, h);
}
int nvs_set_u32(int h, const char *k, uint32_t v)
{
    boundary();
    if (late_failure_key && !strcmp(k, late_failure_key)) return ESP_FAIL;
    return base_nvs_set_u32(h, k, v);
}
int nvs_set_u8(int h, const char *k, uint8_t v)
{
    int e = nvs_set_u32(h, k, v);
    if (!e && !strcmp(k, "precal_sel")) staged.selected_precal_profile = v;
    if (!e && !strcmp(k, "quick_ref")) staged.quick_cal_reference = v;
    return e;
}
int nvs_set_blob(int h, const char *k, const void *v, size_t n)
{
    int e = nvs_set_u32(h, k, 0);
    if (!e) {
        if (!strcmp(k, "precal")) { CHECK(n == sizeof(staged.precal_profiles)); memcpy(staged.precal_profiles, v, n); }
        if (!strcmp(k, "coef_gal")) memcpy(&staged.working_coefficients.gallon_per_count, v, n);
        if (!strcmp(k, "coef_liter")) memcpy(&staged.working_coefficients.liter_per_count, v, n);
        if (!strcmp(k, "coef_ounce")) memcpy(&staged.working_coefficients.ounce_per_count, v, n);
    }
    return e;
}
int nvs_commit(int h)
{
    boundary();
    ++commit_calls;
    int e = base_nvs_commit(h);
    if (!e) committed = staged;
    return e;
}
static void bucket_ready(dura_units_t units)
{
    CHECK(dura_meter_set_units(units) == ESP_OK);
    s_ui_screen = DURA_UI_CAL_START;
    ui_button(DURA_BUTTON_DOWN);
    CHECK(snap().calibration_mode == DURA_CAL_RUNNING);
    CHECK(dura_meter_record_pulse(DURA_MIN_FLOW_COUNTS) == ESP_OK);
    ui_button(DURA_BUTTON_BACK);
    ui_refresh();
    CHECK(s_ui_screen == DURA_UI_CAL_ADJUST);
    ui_button(DURA_BUTTON_BACK);
    CHECK(s_ui_screen == DURA_UI_CAL_SAVE_1);
    CHECK(snap().calibration_mode == DURA_CAL_WAIT_SAVE);
    before = snap();
    off();
}
static void quick_ready(void)
{
    s_ui_screen = DURA_UI_CAL_START;
    ui_button(DURA_BUTTON_BACK);
    CHECK(s_ui_screen == DURA_UI_QUICK_CAL);
    while (s_ui_quick_ref > 1) ui_button(DURA_BUTTON_DOWN);
    before = snap();
}
static void choose_slot(unsigned slot)
{
    if (slot >= 3 && s_ui_screen == DURA_UI_CAL_SAVE_1) ui_button(DURA_BUTTON_BACK);
    ui_button((dura_button_t)(1 + slot % 3));
}
static void successful(bool quick, unsigned slot)
{
    dura_meter_snapshot_t s = snap();
    CHECK(s_ui_screen == DURA_UI_HOME);
    CHECK(s.calibration_mode == DURA_CAL_IDLE);
    CHECK(s.operation_mode == DURA_OPERATION_IDLE);
    CHECK(s.working_coefficients.gallon_per_count != before.working_coefficients.gallon_per_count);
    same_coefficients(s.working_coefficients, committed.working_coefficients);
    if (quick) {
        CHECK(s.quick_cal_reference == 1 && committed.quick_cal_reference == 1);
        CHECK(fabsf(s.working_coefficients.gallon_per_count - before.precal_profiles[0].gallon_per_count * 0.84f) < 1e-8f);
        CHECK(!memcmp(s.precal_profiles, before.precal_profiles, sizeof(s.precal_profiles)));
    } else {
        CHECK(s.selected_precal_profile == slot && committed.selected_precal_profile == slot);
        same_coefficients(s.working_coefficients, s.precal_profiles[slot]);
        same_coefficients(s.working_coefficients, committed.precal_profiles[slot]);
        CHECK(fabsf(s.working_coefficients.gallon_per_count - 0.02f) < 1e-7f);
        for (unsigned i=0; i<DURA_PRECAL_PROFILE_COUNT; ++i)
            if (i != slot) same_coefficients(s.precal_profiles[i], before.precal_profiles[i]);
    }
    off();
}
static void failure_case(const char *name)
{
    if (quick_mode) quick_ready(); else bucket_ready(DURA_UNITS_GALLON);
    if (strstr(name,"open")) fail_open=1;
    else if (strstr(name,"write")) fail_write=1;
    else fail_commit=1;
    if (quick_mode) ui_button(DURA_BUTTON_BACK); else choose_slot(4);
    unpublished();
    ui_refresh();
    CHECK(s_ui_screen == (quick_mode ? DURA_UI_QUICK_CAL : DURA_UI_CAL_SAVE_2));
    if (!quick_mode) { CHECK(snap().calibration_mode == DURA_CAL_WAIT_SAVE); excluded(); }
    else {
        /* Retry is the owner's permitted action; competing operations are not. */
        excluded_operations();
        ui_refresh();
        CHECK(s_ui_screen == DURA_UI_QUICK_CAL);
    }
    if (strstr(name,"cancel")) {
        ui_button(quick_mode ? DURA_BUTTON_UP : DURA_BUTTON_BACK);
        CHECK(s_ui_screen == DURA_UI_CAL_START);
        unpublished();
        CHECK(snap().calibration_mode == DURA_CAL_IDLE);
        CHECK(dura_meter_finish_calibration(4) == ESP_ERR_INVALID_STATE);
        CHECK(dura_meter_start_manual() == ESP_OK);
    } else {
        fail_open=fail_write=fail_commit=0;
        if (quick_mode) ui_button(DURA_BUTTON_BACK); else choose_slot(4);
        successful(quick_mode,4);
    }
}
static int finish_result;
static void *finish_worker(void *p)
{
    cal_thread=true;
    finish_result = quick_mode ? dura_meter_apply_quick_cal(1) : dura_meter_finish_calibration(4);
    return NULL;
}
static const char *invalidate_action;
static void *invalidate_worker(void *p)
{
    stop_thread=true;
    if (!strcmp(invalidate_action,"fault")) stop_result=dura_meter_set_fault(7);
    else if (!strcmp(invalidate_action,"sleep")) stop_result=dura_meter_prepare_for_sleep();
    else stop_result=dura_meter_set_screen_home();
    return NULL;
}
static void release_save(void)
{
    pthread_mutex_lock(&gate); release_commit=true; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&gate);
}
static void blocked_case(const char *action)
{
    if (quick_mode) quick_ready(); else bucket_ready(DURA_UNITS_GALLON);
    block_commit=true;
    observe_pending=!strcmp(action,"observe");
    pthread_t worker, invalidator;
    CHECK(!pthread_create(&worker,NULL,finish_worker,NULL));
    pthread_mutex_lock(&gate); while(!commit_entered) pthread_cond_wait(&cond,&gate); pthread_mutex_unlock(&gate);
    if (!strcmp(action,"observe")) {
        unpublished();
        excluded();
        if (!quick_mode) CHECK(snap().calibration_mode==DURA_CAL_WAIT_SAVE);
    } else if (!strcmp(action,"cancel")) {
        CHECK(dura_meter_cancel_calibration()==ESP_OK);
        CHECK(snap().calibration_mode==DURA_CAL_IDLE);
        excluded(); /* Reservation outlives cancellation until writer returns. */
    } else if (!strcmp(action,"defaults")) {
        CHECK(dura_meter_factory_defaults()==ESP_OK);
        excluded();
    } else if (!strcmp(action,"load")) {
        /* Do not read partially written calibration keys into live RAM. */
        CHECK(dura_meter_load()==ESP_ERR_INVALID_STATE);
        unpublished();
    } else {
        invalidate_action=action;
        CHECK(!pthread_create(&invalidator,NULL,invalidate_worker,NULL));
        pthread_mutex_lock(&gate); while(!stop_waiting) pthread_cond_wait(&cond,&gate); pthread_mutex_unlock(&gate);
        off();
    }
    release_save();
    CHECK(!pthread_join(worker,NULL));
    if (!strcmp(action,"home") || !strcmp(action,"fault") || !strcmp(action,"sleep"))
        CHECK(!pthread_join(invalidator,NULL));
    if (!strcmp(action,"observe") || !strcmp(action,"load")) {
        CHECK(finish_result==ESP_OK);
        same_coefficients(snap().working_coefficients,committed.working_coefficients);
    } else {
        CHECK(finish_result==ESP_ERR_INVALID_STATE);
        unpublished();
        if (!strcmp(action,"fault")) CHECK(snap().fault_latched);
        if (!strcmp(action,"cancel") || !strcmp(action,"defaults") || !strcmp(action,"load") || !strcmp(action,"home")) {
            CHECK(dura_meter_start_manual()==ESP_OK);
            CHECK(dura_meter_stop_manual()==ESP_OK);
            if (!quick_mode) {
                bucket_ready(DURA_UNITS_GALLON);
                choose_slot(2);
                successful(false,2);
            }
        }
    }
    off();
}
static void waiting_case(void)
{
    CHECK(dura_meter_save()==ESP_OK);
    if (quick_mode) quick_ready(); else bucket_ready(DURA_UNITS_GALLON);
    CHECK(!pthread_mutex_lock(save_lock));
    pthread_t worker, invalidator;
    CHECK(!pthread_create(&worker,NULL,finish_worker,NULL));
    pthread_mutex_lock(&gate); while(!cal_take_entered) pthread_cond_wait(&cond,&gate); pthread_mutex_unlock(&gate);
    unsigned n=open_calls;
    if (quick_mode) {
        invalidate_action="home";
        CHECK(!pthread_create(&invalidator,NULL,invalidate_worker,NULL));
        pthread_mutex_lock(&gate); while(!stop_waiting) pthread_cond_wait(&cond,&gate); pthread_mutex_unlock(&gate);
    } else CHECK(dura_meter_cancel_calibration()==ESP_OK);
    CHECK(!pthread_mutex_unlock(save_lock));
    CHECK(!pthread_join(worker,NULL));
    if (quick_mode) CHECK(!pthread_join(invalidator,NULL));
    CHECK(finish_result==ESP_ERR_INVALID_STATE);
    CHECK(open_calls==n+(quick_mode ? 1u : 0u)); /* no canceled candidate write */
    unpublished();
}
int main(int argc,char **argv)
{
    CHECK(argc==2);
    fresh();
    const char *name=argv[1];
    quick_mode=!strncmp(name,"quick",5);
    const char *blocked=strstr(name,"_blocked_");
    if (blocked) blocked_case(blocked+9);
    else if (strstr(name,"_waiting_")) waiting_case();
    else if (strstr(name,"_retry") || strstr(name,"_cancel")) {
        if (!strcmp(name,"late_write_retry")) {
            bucket_ready(DURA_UNITS_GALLON);
            late_failure_key="coef_liter";
            choose_slot(5); unpublished();
            CHECK(s_ui_screen==DURA_UI_CAL_SAVE_2);
            late_failure_key=NULL; choose_slot(5); successful(false,5);
        } else failure_case(name);
    } else if (!strncmp(name,"bucket_slot_",12)) {
        bucket_ready(DURA_UNITS_GALLON);
        unsigned slot=(unsigned)atoi(name+12);
        choose_slot(slot); successful(false,slot);
    } else if (!strcmp(name,"bucket_liter") || !strcmp(name,"bucket_ounce")) {
        bucket_ready(!strcmp(name,"bucket_liter") ? DURA_UNITS_LITER : DURA_UNITS_OUNCE);
        fail_commit=1; choose_slot(3); unpublished(); fail_commit=0;
        choose_slot(3); successful(false,3);
    } else if (!strcmp(name,"bucket_double_finish")) {
        bucket_ready(DURA_UNITS_GALLON);
        block_commit=true;
        pthread_t worker; CHECK(!pthread_create(&worker,NULL,finish_worker,NULL));
        pthread_mutex_lock(&gate); while(!commit_entered) pthread_cond_wait(&cond,&gate); pthread_mutex_unlock(&gate);
        CHECK(dura_meter_finish_calibration(5)==ESP_ERR_INVALID_STATE);
        release_save(); CHECK(!pthread_join(worker,NULL)); CHECK(finish_result==ESP_OK);
        CHECK(snap().selected_precal_profile==4);
    } else if (!strcmp(name,"invalid_finish")) {
        CHECK(dura_meter_finish_calibration(0)==ESP_ERR_INVALID_STATE);
        bucket_ready(DURA_UNITS_GALLON);
        unsigned n=open_calls;
        CHECK(dura_meter_finish_calibration(DURA_PRECAL_PROFILE_COUNT)==ESP_ERR_INVALID_ARG);
        CHECK(open_calls==n); unpublished();
        choose_slot(0); successful(false,0);
    } else CHECK(false);
    printf("PASS %s\n",name);
    return 0;
}
