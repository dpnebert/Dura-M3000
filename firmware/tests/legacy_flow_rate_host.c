/* Real meter + extracted board ISR/qualification/drain. Only platform seams. */
#define main safe_output_existing_main
#define esp_timer_get_time constant_fixture_time
#include "safe_output.c"
#undef esp_timer_get_time
#undef main
static int64_t now_us;
int64_t esp_timer_get_time(void) { return now_us; }
void test_board_bind(void) {}
int dura_board_sync_outputs(void) { return dura_meter_sync_outputs(); }
#include "board_flow.inc"
static void observe_outputs(dura_output_intent_t v) {
 CHECK(host_critical_depth >= 2);
 CHECK(!(v.recirc_ev && v.inject_ev));
 int rc=pthread_mutex_trylock(&s_flow_boundary_mux);
 if(!rc) pthread_mutex_unlock(&s_flow_boundary_mux);
 CHECK(rc != 0);
 gpio_set_level(0,v.pump); gpio_set_level(1,v.recirc_ev); gpio_set_level(2,v.inject_ev);
}
int main(int argc, char **argv) {
 CHECK(argc==2); now_us=strtoll(argv[1],NULL,10); CHECK(now_us>=0);
 CHECK(dura_meter_init()==ESP_OK); bind_flow();
 CHECK(dura_meter_set_output_handler(observe_outputs)==ESP_OK); off();
 char op[32]; long long t; unsigned a,b;
 while(scanf("%31s %lld",op,&t)==2) {
  CHECK(t>=now_us); now_us=t;
  if(!strcmp(op,"init")) { CHECK(t==strtoll(argv[1],NULL,10)); }
  else if(!strcmp(op,"edge")) { CHECK(scanf("%u %u",&a,&b)==2); levels=b; flow_isr((void*)(uintptr_t)a); }
  else if(!strcmp(op,"poll")) drain();
  else if(!strcmp(op,"read")) {
   dura_meter_snapshot_t s=snap();
   printf("S %lld %.9g %.9g %.9g %.9g %u %u %u %u %u %u %u\n",t,
    (double)s.flow_rate,(double)s.working_coefficients.gallon_per_count,
    (double)s.working_coefficients.liter_per_count,(double)s.working_coefficients.ounce_per_count,
    s.meter_total_counts,s.batch_total_counts,s.total_total_counts,s.odometer_counts,
    s.calibration_counts,s.preset_batch_counts,s.pump_enabled);
  }
  else if(!strcmp(op,"units")) { CHECK(scanf("%u",&a)==1); CHECK(dura_meter_set_units(a)==ESP_OK); }
  else if(!strcmp(op,"quick")) { CHECK(scanf("%u",&a)==1); CHECK(dura_meter_apply_quick_cal(a)==ESP_OK); }
  else if(!strcmp(op,"count")) { CHECK(scanf("%u",&a)==1); CHECK(dura_meter_record_pulse(a)==ESP_OK); }
  else if(!strcmp(op,"cal_start")) CHECK(dura_meter_start_calibration()==ESP_OK);
  else if(!strcmp(op,"cal_cancel")) { safe_required=true; CHECK(dura_meter_cancel_calibration()==ESP_OK); safe_required=false; off(); }
  else if(!strcmp(op,"manual")) CHECK(dura_meter_start_manual()==ESP_OK);
  else if(!strcmp(op,"stop")) { safe_required=true; CHECK(dura_meter_stop_manual()==ESP_OK); safe_required=false; off(); }
  else if(!strcmp(op,"batch")) { CHECK(scanf("%u",&a)==1); CHECK(dura_meter_start_batch_mode(0.1f,a?DURA_OPERATION_RECIRC:DURA_OPERATION_AUTO)==ESP_OK); }
  else if(!strcmp(op,"safe")) safe_required=true;
  else if(!strcmp(op,"reset_meter")) CHECK(dura_meter_reset_meter_total()==ESP_OK);
  else if(!strcmp(op,"reset_batch")) CHECK(dura_meter_reset_batch_total()==ESP_OK);
  else if(!strcmp(op,"reset_total")) CHECK(dura_meter_reset_total_total()==ESP_OK);
  else if(!strcmp(op,"fault")) {safe_required=true; CHECK(dura_meter_set_fault(9)==ESP_OK); off();}
  else if(!strcmp(op,"defaults")) {safe_required=true; CHECK(dura_meter_factory_defaults()==ESP_OK); off(); safe_required=false;}
  else CHECK(false);
 }
 return 0;
}
