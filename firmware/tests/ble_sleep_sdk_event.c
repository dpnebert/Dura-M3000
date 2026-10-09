/* Real pinned SDK init/deinit bodies are generated into sdk_event.inc.
 * Model only the OS_MEM_ALLOC pool boundary, whose backing storage is released
 * at nimble_port_deinit. ASan detects reuse of the old opaque event pointer. */
#include "ble_sleep_shim.h"
#define OS_MEM_ALLOC 1
#define BLE_LL_ASSERT assert
static int ble_freertos_ev_pool;
static void *pool_block;
static bool reserved;
static void *os_memblock_get(void *pool) {
    assert(radio_up && !reserved);
    if (!pool_block) pool_block=calloc(1,sizeof(struct ble_npl_event_freertos));
    reserved=true;
    return pool_block;
}
static int os_memblock_put(void *pool,void *block) {
    assert(host_done && radio_up && reserved && block==pool_block);
    reserved=false;
    return 0;
}
#include "sdk_event.inc"
void ble_npl_event_init(struct ble_npl_event *e,ble_npl_event_fn *fn,void *arg) {
    npl_freertos_event_init(e,fn,arg);
}
void ble_npl_event_deinit(struct ble_npl_event *e) {
    npl_freertos_event_deinit(e);
    assert(!e->event);
}
void test_event_pool_teardown(void) {
    assert(host_done && radio_up);
    free(pool_block);pool_block=NULL;reserved=false;
}
