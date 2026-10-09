#include <assert.h>
void fixture_recipe(void) {
    char name[32]; memset(name,1,31); name[31]=0;
    dura_liquid_config_t local={.viscosity_cP=FLT_MAX,.calibration_id=UINT32_MAX};
    memcpy(local.liquid_name,name,32);
    assert(dura_recipe_set_local_liquid(&local)==ESP_OK);
    assert(dura_recipe_reset_default(name,DURA_RECIPE_ORDER_CONFIGURED)==ESP_OK);
    for(size_t i=0;i<DURA_RECIPE_MAX_INGREDIENTS;i++)
        assert(dura_recipe_add_default_ingredient(name,FLT_MAX,255)==ESP_OK);
    /* Advance identity to the largest still-valid contiguous ID range. No
     * semantic changes to allocation/persistence; actual accepted add/get APIs. */
    s_next_custom_liquid_id=UINT16_MAX-DURA_RECIPE_MAX_CUSTOM_LIQUIDS;
    for(size_t i=0;i<DURA_RECIPE_MAX_CUSTOM_LIQUIDS;i++) {
        memset(name,'x',31);name[31]=0;name[0]=(char)('A'+i/10);name[1]=(char)('0'+i%10);
        uint16_t id;assert(dura_recipe_add_custom_liquid(name,FLT_MAX,255,&id)==ESP_OK);
    }
    assert(dura_recipe_get_liquid_profile_count()==DURA_RECIPE_MAX_CUSTOM_LIQUIDS+4);
}
void fixture_recipe_nonfinite(void){s_default_recipe.ingredients[0].amount=INFINITY;}
