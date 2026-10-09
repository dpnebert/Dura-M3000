void fixture_meter(void){
    s_meter.meter_total_counts=s_meter.batch_total_counts=s_meter.total_total_counts=s_meter.odometer_counts=UINT32_MAX;
    s_meter.preset_batch_counts=UINT32_MAX;s_meter.fault_code=UINT32_MAX;
    s_meter.batch_preset_gal=FLT_MAX;
    s_meter.working_coef_gal=s_meter.working_coef_liter=s_meter.working_coef_ounce=1.0f;
    s_meter.flow_rate=FLT_MAX;
}
void fixture_recipe_nonfinite(void);
void fixture_nonfinite(void){fixture_recipe_nonfinite();s_meter.working_coef_gal=INFINITY;s_meter.flow_rate=NAN;}
