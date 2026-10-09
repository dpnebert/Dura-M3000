void fixture_coordinator(void){
    memset(s_snapshot.active_recipe,1,sizeof(s_snapshot.active_recipe)-1);
    memset(s_snapshot.last_failed_liquid,1,sizeof(s_snapshot.last_failed_liquid)-1);
    s_snapshot.active_transaction_id=s_snapshot.pending_ack_count=s_snapshot.acked_count=s_snapshot.completed_count=s_snapshot.timeout_count=s_snapshot.last_failed_transaction_id=UINT32_MAX;
}
