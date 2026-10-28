/* Complete production meter linked separately. Reuse only host platform headers.
 * Staged NVS simulator: commit success persists a file for a NEW process boot.
 * This models committed-key reload, NOT ESP NVS multi-key power-loss atomicity.
 * Output callback observes production intent, not GPIO or physical hardware. */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dura_meter.h"
#include "freertos/semphr.h"
#include "nvs.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d %s\n",__func__,__LINE__,#x); exit(1); } } while (0)
_Thread_local unsigned host_critical_depth;
static int64_t clock_us = 1000000;
static atomic_uint output_bits, on_edges;
static bool require_off;
static unsigned queued;
static bool ingress_locked;
static const char *store_path;
static unsigned fail_kind;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static bool block_commit, commit_entered, release_commit;
static unsigned writes, commits;

typedef struct { char key[24]; size_t size; unsigned char bytes[128]; } entry_t;
typedef struct { unsigned used; entry_t items[32]; } database_t;
static database_t committed, staged;
static entry_t *entry(database_t *db, const char *key, bool create)
{
    for (unsigned i=0; i<db->used; ++i)
        if (!strcmp(db->items[i].key,key)) return &db->items[i];
    if (!create) return NULL;
    CHECK(db->used < 32 && strlen(key) < 24);
    entry_t *e = &db->items[db->used++];
    strcpy(e->key,key);
    return e;
}
static void put(database_t *db, const char *key, const void *value, size_t size)
{
    CHECK(size <= sizeof(db->items[0].bytes));
    entry_t *e=entry(db,key,true); e->size=size; memcpy(e->bytes,value,size);
}
static void put_u32(database_t *db, const char *key, uint32_t value)
{ put(db,key,&value,sizeof(value)); }
static void disk_write(void)
{
    FILE *f=fopen(store_path,"wb"); CHECK(f);
    CHECK(fwrite(&committed,sizeof(committed),1,f)==1); CHECK(!fclose(f));
}
static void off(void) { CHECK(output_bits==0); }
static void boundary(void) { CHECK(!host_critical_depth); if (require_off) off(); }
int64_t esp_timer_get_time(void) { return clock_us; }
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *m=malloc(sizeof(*m)); CHECK(m);
    CHECK(!pthread_mutex_init(m,NULL)); return m;
}
int xSemaphoreTake(SemaphoreHandle_t m, unsigned t)
{ (void)t; boundary(); CHECK(!pthread_mutex_lock(m)); return pdTRUE; }
void xSemaphoreGive(SemaphoreHandle_t m) { CHECK(!pthread_mutex_unlock(m)); }
int nvs_open(const char *ns, int mode, nvs_handle_t *h)
{
    boundary(); CHECK(!strcmp(ns,DURA_METER_NVS_NAMESPACE)); *h=mode+1;
    if (mode==NVS_READONLY) return committed.used ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
    if (fail_kind==1) return ESP_FAIL;
    staged=committed; return ESP_OK;
}
static int get(const char *key, void *v, size_t *n)
{
    boundary(); entry_t *e=entry(&committed,key,false);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    CHECK(*n>=e->size); memcpy(v,e->bytes,e->size); *n=e->size; return ESP_OK;
}
int nvs_get_u32(int h,const char *k,uint32_t *v)
{ (void)h; size_t n=sizeof(*v); return get(k,v,&n); }
int nvs_get_u8(int h,const char *k,uint8_t *v)
{ (void)h; size_t n=sizeof(*v); return get(k,v,&n); }
int nvs_get_blob(int h,const char *k,void *v,size_t *n)
{ (void)h; return get(k,v,n); }
static int set(const char *k,const void *v,size_t n)
{
    boundary(); ++writes;
    if (fail_kind==2 || (fail_kind==3 && !strcmp(k,"coef_liter"))) return ESP_FAIL;
    put(&staged,k,v,n); return ESP_OK;
}
int nvs_set_u32(int h,const char *k,uint32_t v) { (void)h; return set(k,&v,sizeof(v)); }
int nvs_set_u8(int h,const char *k,uint8_t v) { (void)h; return set(k,&v,sizeof(v)); }
int nvs_set_blob(int h,const char *k,const void *v,size_t n) { (void)h; return set(k,v,n); }
int nvs_commit(int h)
{
    (void)h; boundary(); ++commits;
    CHECK(!pthread_mutex_lock(&gate));
    if (block_commit) {
        commit_entered=true; CHECK(!pthread_cond_broadcast(&cond));
        while (!release_commit) CHECK(!pthread_cond_wait(&cond,&gate));
        block_commit=false;
    }
    CHECK(!pthread_mutex_unlock(&gate));
    if (fail_kind==4) return ESP_FAIL;
    committed=staged; disk_write(); return ESP_OK;
}
void nvs_close(int h) { (void)h; boundary(); }
static void output(dura_output_intent_t intent)
{
    CHECK(host_critical_depth>0);
    CHECK(!(intent.recirc_ev && intent.inject_ev));
    unsigned value=intent.pump | (intent.recirc_ev<<1) | (intent.inject_ev<<2);
    if (value && !output_bits) ++on_edges;
    output_bits=value;
}
static uint32_t ingress(bool enter)
{
    CHECK(host_critical_depth>0);
    if (enter) { CHECK(!ingress_locked); ingress_locked=true; unsigned n=queued; queued=0; return n; }
    CHECK(ingress_locked); ingress_locked=false; return 0;
}
static dura_meter_snapshot_t snap(void)
{ dura_meter_snapshot_t s; CHECK(dura_meter_get_snapshot(&s)==ESP_OK); return s; }
static void counts(dura_meter_snapshot_t s,uint32_t m,uint32_t b,uint32_t t,uint32_t o)
{
    CHECK(s.meter_total_counts==m); CHECK(s.batch_total_counts==b);
    CHECK(s.total_total_counts==t); CHECK(s.odometer_counts==o);
}
static void fresh(void)
{
    CHECK(dura_meter_init()==ESP_OK);
    CHECK(dura_meter_set_output_handler(output)==ESP_OK); off();
    CHECK(dura_meter_save()==ESP_OK);
    /* Distinct persisted legacy keys ensure unrelated counters cannot be swapped. */
    put_u32(&committed,"meter_counts",11); put_u32(&committed,"batch_counts",22);
    put_u32(&committed,"total_counts",33); put_u32(&committed,"odom_counts",44);
    disk_write(); CHECK(dura_meter_load()==ESP_OK); counts(snap(),11,22,33,44);
}
static void state(const char *mode)
{
    if (!strcmp(mode,"manual")) CHECK(dura_meter_start_manual()==ESP_OK);
    else if (!strcmp(mode,"auto")) CHECK(dura_meter_start_batch(10)==ESP_OK);
    else if (!strcmp(mode,"recirc")) CHECK(dura_meter_start_batch_mode(10,DURA_OPERATION_RECIRC)==ESP_OK);
    else if (!strcmp(mode,"calibration")) {
        CHECK(dura_meter_start_calibration()==ESP_OK);
        CHECK(dura_meter_record_pulse(17)==ESP_OK);
    } else if (!strcmp(mode,"recipe")) {
        uint32_t owner=0; CHECK(dura_meter_claim_recipe(&owner)==ESP_OK);
        CHECK(dura_meter_start_owned_batch(10,owner)==ESP_OK);
    } else if (!strcmp(mode,"paused")) {
        CHECK(dura_meter_start_batch(10)==ESP_OK); CHECK(dura_meter_pause_batch()==ESP_OK);
    } else if (!strcmp(mode,"fault")) CHECK(dura_meter_set_fault(9)==ESP_OK);
    else if (!strcmp(mode,"sleep")) CHECK(dura_meter_prepare_for_sleep()==ESP_OK);
    else CHECK(!strcmp(mode,"idle"));
}
static void same_operation(dura_meter_snapshot_t a,dura_meter_snapshot_t b)
{
    CHECK(a.screen==b.screen); CHECK(a.operation_mode==b.operation_mode);
    CHECK(a.batch_mode==b.batch_mode); CHECK(a.calibration_mode==b.calibration_mode);
    CHECK(a.pump_enabled==b.pump_enabled); CHECK(a.recipe_owner_id==b.recipe_owner_id);
    CHECK(a.preset_batch_counts==b.preset_batch_counts);
    CHECK(a.calibration_counts==b.calibration_counts); CHECK(a.remaining_batch==b.remaining_batch);
    CHECK(a.fault_latched==b.fault_latched); CHECK(a.fault_code==b.fault_code);
    CHECK(!memcmp(&a.working_coefficients,&b.working_coefficients,sizeof(a.working_coefficients)));
    CHECK(!memcmp(a.precal_profiles,b.precal_profiles,sizeof(a.precal_profiles)));
}
static int reset(const char *which)
{
    if (!strcmp(which,"factory")) {
        int e=dura_meter_factory_defaults(); CHECK(e==ESP_OK); off();
        return dura_meter_save(); /* Existing factory API is RAM-only; caller saves. */
    }
    if (!strcmp(which,"batch")) return dura_meter_reset_batch_total();
    if (!strcmp(which,"total")) return dura_meter_reset_total_total();
    CHECK(!strcmp(which,"meter")); return dura_meter_reset_meter_total();
}
static void expected_reset(const char *which,dura_meter_snapshot_t before)
{
    dura_meter_snapshot_t after=snap();
    counts(after,0,(!strcmp(which,"factory") || !strcmp(which,"batch")) ? 0 : before.batch_total_counts,
                  (!strcmp(which,"factory") || !strcmp(which,"total")) ? 0 : before.total_total_counts,
                  before.odometer_counts);
    CHECK(after.meter_total==0);
    if (!strcmp(which,"batch")) CHECK(after.batch_total==0);
    if (!strcmp(which,"total")) CHECK(after.total_total==0);
    if (strcmp(which,"factory")) same_operation(before,after);
    else {
        CHECK(!after.pump_enabled); CHECK(!after.recipe_owner_id); off();
        CHECK(after.operation_mode==DURA_OPERATION_IDLE);
        CHECK(after.batch_mode==DURA_BATCH_IDLE); CHECK(after.calibration_mode==DURA_CAL_IDLE);
    }
}
static void failure_case(const char *which,unsigned failure)
{
    if (!strcmp(which,"factory")) CHECK(dura_meter_start_batch_mode(10,DURA_OPERATION_RECIRC)==ESP_OK);
    dura_meter_snapshot_t before=snap(); database_t old=committed;
    require_off=true; fail_kind=failure;
    CHECK(reset(which)==ESP_FAIL); off(); expected_reset(which,before);
    CHECK(!memcmp(&old,&committed,sizeof(old))); /* failed candidate not committed */
    fail_kind=0;
    CHECK(dura_meter_load()==ESP_OK); counts(snap(),11,22,33,44); off();
    before=snap(); CHECK(reset(which)==ESP_OK); expected_reset(which,before);
}
static void factory_active(const char *mode)
{
    state(mode); dura_meter_snapshot_t before=snap(); require_off=true;
    CHECK(reset("factory")==ESP_OK); expected_reset("factory",before);
    CHECK(dura_meter_resume_batch()==ESP_ERR_INVALID_STATE);
    if (before.recipe_owner_id) {
        CHECK(dura_meter_start_owned_batch(1,before.recipe_owner_id)==ESP_ERR_INVALID_STATE);
        CHECK(dura_meter_release_recipe(before.recipe_owner_id)==ESP_ERR_INVALID_STATE);
        uint32_t next; CHECK(dura_meter_claim_recipe(&next)==ESP_OK);
        CHECK(next!=before.recipe_owner_id); CHECK(dura_meter_release_recipe(next)==ESP_OK);
    }
    if (!strcmp(mode,"sleep")) {
        CHECK(dura_meter_start_manual()==ESP_ERR_INVALID_STATE); off();
        CHECK(dura_meter_cancel_sleep_prepare()==ESP_OK);
    }
}
static int worker_result;
static bool calibration_worker;
static void *writer(void *arg)
{
    (void)arg;
    worker_result=calibration_worker ? dura_meter_finish_calibration(2) : dura_meter_start_batch(10);
    return NULL;
}
static void pending_case(bool calibration)
{
    calibration_worker=calibration;
    if (calibration) {
        CHECK(dura_meter_start_calibration()==ESP_OK);
        CHECK(dura_meter_record_pulse(DURA_MIN_FLOW_COUNTS)==ESP_OK);
        CHECK(dura_meter_stop_calibration()==ESP_OK);
        CHECK(dura_meter_accept_calibration_amount(5)==ESP_OK);
    }
    dura_meter_snapshot_t before=snap(); require_off=true;
    block_commit=true; pthread_t thread; CHECK(!pthread_create(&thread,NULL,writer,NULL));
    CHECK(!pthread_mutex_lock(&gate));
    while (!commit_entered) CHECK(!pthread_cond_wait(&cond,&gate));
    CHECK(!pthread_mutex_unlock(&gate));
    /* No save-mutex wait is allowed before factory OFF/revocation. */
    unsigned edges=on_edges;
    CHECK(dura_meter_factory_defaults()==ESP_OK); off(); expected_reset("factory",before);
    if (calibration) {
        CHECK(dura_meter_start_manual()==ESP_ERR_INVALID_STATE);
        CHECK(dura_meter_apply_quick_cal(1)==ESP_ERR_INVALID_STATE);
        CHECK(dura_meter_load()==ESP_ERR_INVALID_STATE);
    }
    CHECK(!pthread_mutex_lock(&gate)); release_commit=true;
    CHECK(!pthread_cond_broadcast(&cond)); CHECK(!pthread_mutex_unlock(&gate));
    CHECK(!pthread_join(thread,NULL));
    CHECK(worker_result==ESP_ERR_INVALID_STATE); CHECK(on_edges==edges); off();
    CHECK(dura_meter_save()==ESP_OK); expected_reset("factory",before);
    dura_meter_snapshot_t after=snap();
    CHECK(after.working_coefficients.gallon_per_count==0.00892857f);
    CHECK(after.selected_precal_profile==0);
    CHECK(after.precal_profiles[2].gallon_per_count==0);
}
static void ingress_case(const char *which)
{
    CHECK(dura_meter_set_flow_handler(ingress)==ESP_OK); queued=7;
    /* No intervening snapshot: reset's METER_LOCK must drain before mutation. */
    CHECK(reset(which)==ESP_OK);
    counts(snap(),0,!strcmp(which,"batch") || !strcmp(which,"factory") ? 0 : 29,
                   !strcmp(which,"total") || !strcmp(which,"factory") ? 0 : 40,51);
    CHECK(!ingress_locked && queued==0);
    CHECK(dura_meter_record_pulse(3)==ESP_OK);
    CHECK(snap().meter_total_counts==3 && snap().odometer_counts==54);
    CHECK(dura_meter_save()==ESP_OK);
}
static void water_case(void)
{
    CHECK(dura_meter_start_calibration()==ESP_OK);
    CHECK(dura_meter_record_pulse(DURA_MIN_FLOW_COUNTS)==ESP_OK);
    CHECK(dura_meter_stop_calibration()==ESP_OK);
    CHECK(dura_meter_accept_calibration_amount(5)==ESP_OK);
    CHECK(dura_meter_finish_calibration(0)==ESP_OK);
    CHECK(fabsf(snap().precal_profiles[0].gallon_per_count-0.02f)<1e-7f);
    require_off=true; CHECK(reset("factory")==ESP_OK);
    dura_meter_snapshot_t s=snap(); counts(s,0,0,0,44);
    CHECK(s.precal_profiles[0].gallon_per_count==0.00892857f);
    CHECK(s.working_coefficients.gallon_per_count==0.00892857f);
    CHECK(s.selected_precal_profile==0 && s.quick_cal_reference==4);
}
static void rate_characterization(void)
{
    const uint32_t initial_count = snap().meter_total_counts;
    CHECK(dura_meter_record_pulse(1)==ESP_OK);
    CHECK(snap().flow_rate==0);
    clock_us+=1000000; CHECK(dura_meter_record_pulse(4)==ESP_OK);
    /* Candidate44 intentionally removes drain-time rate inference: a grouped
     * count has no physical edge timestamps and must never invent rate samples.
     * Exact timestamp/FIR/timeout/restart behavior is separately exercised by
     * test_legacy_flow_rate.py against the real board ingress and meter. */
    CHECK(snap().flow_rate==0.0f);
    CHECK(snap().meter_total_counts==initial_count+5);
    clock_us+=60000000; CHECK(dura_meter_record_pulse(0)==ESP_OK);
    CHECK(snap().flow_rate==0.0f);
    CHECK(dura_meter_set_units(DURA_UNITS_LITER)==ESP_OK);
    CHECK(snap().flow_rate==0.0f);
    CHECK(snap().meter_total_counts==initial_count+5);
    printf("CHARACTERIZATION count_only_preserves_volume_without_fabricated_rate\n");
}
static void rounding_characterization(void)
{
    const float quotient[]={2.49f,2.5f,2.51f};
    const uint32_t expected[]={2,2,2};
    /* Exactly representable coefficient gives unambiguous host boundaries. */
    float coef=0.5f; put(&committed,"coef_gal",&coef,sizeof(coef));
    CHECK(dura_meter_load()==ESP_OK);
    for (unsigned i=0;i<3;++i) {
        CHECK(dura_meter_start_batch(quotient[i]*coef)==ESP_OK);
        CHECK(snap().preset_batch_counts==expected[i]);
        printf("CHARACTERIZATION quotient=%.9g count=%u\n",(double)quotient[i],expected[i]);
        CHECK(dura_meter_cancel_batch()==ESP_OK);
    }
}
static void reboot_expectation_write(void)
{
    /* Capture only persisted field expectations, never transient run state. */
    dura_meter_snapshot_t s=snap(); char path[4096];
    CHECK(snprintf(path,sizeof(path),"%s.expected",store_path)<(int)sizeof(path));
    FILE *f=fopen(path,"wb"); CHECK(f); CHECK(fwrite(&s,sizeof(s),1,f)==1); CHECK(!fclose(f));
}
static void reboot(void)
{
    char path[4096]; dura_meter_snapshot_t expected;
    FILE *f=fopen(store_path,"rb"); CHECK(f); CHECK(fread(&committed,sizeof(committed),1,f)==1); CHECK(!fclose(f));
    CHECK(snprintf(path,sizeof(path),"%s.expected",store_path)<(int)sizeof(path));
    f=fopen(path,"rb"); CHECK(f); CHECK(fread(&expected,sizeof(expected),1,f)==1); CHECK(!fclose(f));
    CHECK(dura_meter_init()==ESP_OK); CHECK(dura_meter_set_output_handler(output)==ESP_OK); off();
    dura_meter_snapshot_t s=snap();
    counts(s,expected.meter_total_counts,expected.batch_total_counts,expected.total_total_counts,expected.odometer_counts);
    CHECK(s.meter_total==expected.meter_total && s.batch_total==expected.batch_total && s.total_total==expected.total_total);
    CHECK(!memcmp(&s.working_coefficients,&expected.working_coefficients,sizeof(s.working_coefficients)));
    CHECK(!memcmp(s.precal_profiles,expected.precal_profiles,sizeof(s.precal_profiles)));
    CHECK(s.selected_precal_profile==expected.selected_precal_profile);
    CHECK(s.quick_cal_reference==expected.quick_cal_reference);
    CHECK(s.selected_units==expected.selected_units);
    CHECK(s.operation_mode==DURA_OPERATION_IDLE && !s.pump_enabled && !s.recipe_owner_id);
    CHECK(s.batch_mode==DURA_BATCH_IDLE && s.calibration_mode==DURA_CAL_IDLE);
    printf("PASS new-process reboot committed counters=%u,%u,%u,%u outputs=OFF\n",s.meter_total_counts,s.batch_total_counts,s.total_total_counts,s.odometer_counts);
}
int main(int argc,char **argv)
{
    CHECK(argc==3); store_path=argv[2]; const char *name=argv[1];
    if (!strcmp(name,"reboot")) { reboot(); return 0; }
    fresh();
    if (!strncmp(name,"factory_active_",15)) factory_active(name+15);
    else if (!strcmp(name,"factory_pending_activation")) pending_case(false);
    else if (!strcmp(name,"factory_pending_calibration")) pending_case(true);
    else if (!strcmp(name,"factory_water_unchanged")) water_case();
    else if (!strcmp(name,"factory_empty")) {
        committed.used=0; CHECK(dura_meter_load()==ESP_OK);
        CHECK(reset("factory")==ESP_OK); counts(snap(),0,0,0,0);
    } else if (!strcmp(name,"factory_saturation")) {
        CHECK(dura_meter_record_pulse(UINT32_MAX)==ESP_OK);
        dura_meter_snapshot_t before=snap(); CHECK(before.odometer_counts==UINT32_MAX);
        CHECK(reset("factory")==ESP_OK); expected_reset("factory",before);
        CHECK(dura_meter_record_pulse(1)==ESP_OK); CHECK(snap().odometer_counts==UINT32_MAX);
        CHECK(dura_meter_save()==ESP_OK);
    } else if (!strcmp(name,"characterize_rate")) rate_characterization();
    else if (!strcmp(name,"characterize_rounding")) rounding_characterization();
    else {
        char which[32], action[32]; CHECK(sscanf(name,"%31[^_]_%31s",which,action)==2);
        if (!strcmp(action,"ingress")) ingress_case(which);
        else if (!strncmp(action,"failure",7)) failure_case(which,(unsigned)atoi(action+7));
        else {
            state(action); dura_meter_snapshot_t before=snap(); unsigned bits=output_bits, edges=on_edges;
            CHECK(reset(which)==ESP_OK); expected_reset(which,before);
            CHECK(output_bits==bits && on_edges==edges);
            if (!strcmp(action,"fault") || !strcmp(action,"sleep")) {
                CHECK(dura_meter_start_manual()==ESP_ERR_INVALID_STATE); off();
            }
        }
    }
    CHECK(dura_meter_save()==ESP_OK); reboot_expectation_write();
    printf("PASS %s writes=%u commits=%u\n",name,writes,commits); return 0;
}
