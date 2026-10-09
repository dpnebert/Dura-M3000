/* Reuse NVS and output observations; production meter is separately compiled. */
#define main safe_output_existing_main
#include "safe_output.c"
#undef main
void test_board_bind(void) {}
int dura_board_sync_outputs(void) { return dura_meter_sync_outputs(); }
#include "board_flow.inc"
static unsigned reed;
static void ordered_output(dura_output_intent_t intent) {
 (void)intent;
 int rc=pthread_mutex_trylock(&s_flow_boundary_mux);
 if(rc==0) pthread_mutex_unlock(&s_flow_boundary_mux);
 CHECK(rc!=0); /* Output publication cannot open an ISR/transition gap. */
 CHECK(host_critical_depth>=2);
}
static void edges(unsigned n) { while(n--) { reed=reed==1?2:1; levels=reed; flow_isr((void*)(uintptr_t)reed); } }
static void cal_prefix(void) {
 CHECK(dura_meter_start_calibration()==ESP_OK);
 unsigned n=DURA_MIN_FLOW_COUNTS-1;
 while(n) { unsigned k=n>20?20:n; edges(k); drain(); n-=k; }
}
int main(int argc,char **argv) {
 CHECK(argc==2); CHECK(dura_meter_init()==ESP_OK); bind_flow(); const char*t=argv[1];
 if(!strcmp(t,"boundary_exclusion")) {
  CHECK(dura_meter_set_output_handler(ordered_output)==ESP_OK);
  edges(1); CHECK(dura_meter_start_calibration()==ESP_OK); edges(2); drain();
  CHECK(snap().calibration_counts==2); CHECK(dura_meter_stop_calibration()==ESP_OK);
  edges(1); drain();
 } else if(!strcmp(t,"before_batch")) {
  edges(2); CHECK(dura_meter_start_batch(10)==ESP_OK); unsigned n=snap().preset_batch_counts; edges(3); drain(); CHECK(snap().preset_batch_counts==n-3); CHECK(snap().total_total_counts==5);
 } else if(!strcmp(t,"before_cal")) {
  edges(2); CHECK(dura_meter_start_calibration()==ESP_OK); edges(3); drain(); CHECK(snap().calibration_counts==3); CHECK(snap().total_total_counts==2);
 } else if(!strcmp(t,"before_pause")) {
  CHECK(dura_meter_start_batch(10)==ESP_OK); unsigned n=snap().preset_batch_counts; edges(3); CHECK(dura_meter_pause_batch()==ESP_OK); edges(2); drain(); CHECK(snap().preset_batch_counts==n-3); CHECK(snap().total_total_counts==5);
 } else if(!strcmp(t,"paused_resume")) {
  CHECK(dura_meter_start_batch(10)==ESP_OK); unsigned n=snap().preset_batch_counts; CHECK(dura_meter_pause_batch()==ESP_OK); edges(2); CHECK(dura_meter_resume_batch()==ESP_OK); edges(3); drain(); CHECK(snap().preset_batch_counts==n-3);
 } else if(!strcmp(t,"cal_stop")||!strcmp(t,"cal_continue")||!strcmp(t,"cal_fault")||!strcmp(t,"cal_sleep")) {
  cal_prefix(); edges(1);
  if(!strcmp(t,"cal_stop")) CHECK(dura_meter_stop_calibration()==ESP_OK);
  else if(!strcmp(t,"cal_continue")) CHECK(dura_meter_continue_calibration()==ESP_OK);
  else if(!strcmp(t,"cal_fault")) CHECK(dura_meter_set_fault(9)==ESP_OK);
  else CHECK(dura_meter_prepare_for_sleep()==ESP_OK);
  edges(2); drain(); CHECK(snap().calibration_counts==DURA_MIN_FLOW_COUNTS); CHECK(snap().calibration_mode==DURA_CAL_WAIT_MEASURED_AMOUNT); CHECK(!snap().pump_enabled); CHECK(snap().total_total_counts==2);
  if(!strcmp(t,"cal_stop")||!strcmp(t,"cal_continue")) { CHECK(dura_meter_accept_calibration_amount(2)==ESP_OK); CHECK(dura_meter_finish_calibration(1)==ESP_OK); CHECK(fabsf(snap().working_coefficients.gallon_per_count-2.0f/DURA_MIN_FLOW_COUNTS)<0.000001f); }
 } else if(!strcmp(t,"cal_reset")) {
  CHECK(dura_meter_start_calibration()==ESP_OK); edges(3); CHECK(dura_meter_reset_calibration()==ESP_OK); edges(2); drain(); CHECK(snap().calibration_counts==2);
 } else if(!strcmp(t,"cal_cancel_restart")) {
  CHECK(dura_meter_start_calibration()==ESP_OK); edges(3); CHECK(dura_meter_cancel_calibration()==ESP_OK); edges(2); CHECK(dura_meter_start_calibration()==ESP_OK); edges(4); drain(); CHECK(snap().calibration_counts==4); CHECK(snap().total_total_counts==2);
 } else if(!strcmp(t,"manual_to_cal")) {
  CHECK(dura_meter_start_manual()==ESP_OK); edges(3); CHECK(dura_meter_stop_manual()==ESP_OK); edges(2); CHECK(dura_meter_start_calibration()==ESP_OK); edges(4); drain(); CHECK(snap().calibration_counts==4); CHECK(snap().total_total_counts==5);
 } else if(!strcmp(t,"cal_to_batch")) {
  cal_prefix(); edges(1); CHECK(dura_meter_stop_calibration()==ESP_OK); CHECK(dura_meter_accept_calibration_amount(2)==ESP_OK); CHECK(dura_meter_finish_calibration(1)==ESP_OK); edges(2); CHECK(dura_meter_start_batch(10)==ESP_OK); unsigned n=snap().preset_batch_counts; edges(3); drain(); CHECK(snap().preset_batch_counts==n-3); CHECK(snap().total_total_counts==5);
 } else if(!strcmp(t,"delayed_completion")) {
  CHECK(dura_meter_start_batch(0.1f)==ESP_OK); unsigned n=snap().preset_batch_counts; CHECK(n<30); edges(n); drain(); CHECK(!snap().pump_enabled); CHECK(snap().batch_mode==DURA_BATCH_DONE); CHECK(snap().total_total_counts==n); drain(); CHECK(snap().total_total_counts==n);
 } else if(!strcmp(t,"qualification")) {
  levels=0; flow_isr((void*)1); edges(1); flow_isr((void*)(uintptr_t)reed); CHECK(dura_meter_start_calibration()==ESP_OK); flow_isr((void*)(uintptr_t)reed); edges(1); drain(); CHECK(snap().calibration_counts==1); CHECK(snap().total_total_counts==1);
 } else CHECK(false);
 CHECK(rd==wr); printf("PASS %s\n",t); return 0;
}
