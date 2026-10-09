#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_DURA_RECIPE_MAX_NAME_LEN
#define CONFIG_DURA_RECIPE_MAX_NAME_LEN 32
#endif
#ifndef CONFIG_DURA_RECIPE_MAX_LIQUID_NAME_LEN
#define CONFIG_DURA_RECIPE_MAX_LIQUID_NAME_LEN 32
#endif
#ifndef CONFIG_DURA_RECIPE_MAX_INGREDIENTS
#define CONFIG_DURA_RECIPE_MAX_INGREDIENTS 16
#endif
#ifndef CONFIG_DURA_RECIPE_MAX_CUSTOM_LIQUIDS
#define CONFIG_DURA_RECIPE_MAX_CUSTOM_LIQUIDS 32
#endif

#define DURA_RECIPE_MAX_NAME_LEN          CONFIG_DURA_RECIPE_MAX_NAME_LEN
#define DURA_RECIPE_MAX_LIQUID_NAME_LEN   CONFIG_DURA_RECIPE_MAX_LIQUID_NAME_LEN
#define DURA_RECIPE_MAX_INGREDIENTS       CONFIG_DURA_RECIPE_MAX_INGREDIENTS
#define DURA_RECIPE_MAX_CUSTOM_LIQUIDS    CONFIG_DURA_RECIPE_MAX_CUSTOM_LIQUIDS
#define DURA_RECIPE_CUSTOM_LIQUID_ID_BASE 100U

typedef enum {
    DURA_RECIPE_ORDER_CONFIGURED = 0,
    DURA_RECIPE_ORDER_LARGEST_FIRST,
    DURA_RECIPE_ORDER_SMALLEST_FIRST,
} dura_recipe_order_t;

typedef enum {
    DURA_LIQUID_SOURCE_FACTORY = 0,
    DURA_LIQUID_SOURCE_CUSTOM = 1,
} dura_liquid_source_t;

typedef struct {
    uint16_t id;
    dura_liquid_source_t source;
    uint8_t reference_num;
    char name[DURA_RECIPE_MAX_LIQUID_NAME_LEN];
    float calibration_factor;
} dura_liquid_profile_t;

typedef struct {
    char liquid_name[DURA_RECIPE_MAX_LIQUID_NAME_LEN];
    float viscosity_cP;
    uint32_t calibration_id;
} dura_liquid_config_t;

typedef struct {
    char liquid_name[DURA_RECIPE_MAX_LIQUID_NAME_LEN];
    float amount;
    uint8_t sequence;
} dura_recipe_ingredient_t;

typedef struct {
    char name[DURA_RECIPE_MAX_NAME_LEN];
    dura_recipe_order_t order;
    size_t ingredient_count;
    dura_recipe_ingredient_t ingredients[DURA_RECIPE_MAX_INGREDIENTS];
} dura_recipe_t;

esp_err_t dura_recipe_init(void);
esp_err_t dura_recipe_load(void);
esp_err_t dura_recipe_save(void);
esp_err_t dura_recipe_get_local_liquid(dura_liquid_config_t *liquid);
esp_err_t dura_recipe_set_local_liquid(const dura_liquid_config_t *liquid);
size_t dura_recipe_get_liquid_profile_count(void);
esp_err_t dura_recipe_get_liquid_profile(size_t index, dura_liquid_profile_t *profile);
esp_err_t dura_recipe_find_liquid_profile_by_id(uint16_t id, dura_liquid_profile_t *profile);
esp_err_t dura_recipe_select_liquid_profile(uint16_t id);
esp_err_t dura_recipe_add_custom_liquid(const char *name, float calibration_factor, uint8_t reference_num, uint16_t *id_out);
esp_err_t dura_recipe_delete_custom_liquid(uint16_t id);
esp_err_t dura_recipe_format_liquid_catalog_json(char *json, size_t json_len);
esp_err_t dura_recipe_get_default(dura_recipe_t *recipe);
esp_err_t dura_recipe_set_default(const dura_recipe_t *recipe);
esp_err_t dura_recipe_factory_defaults(void);
esp_err_t dura_recipe_reset_default(const char *name, dura_recipe_order_t order);
esp_err_t dura_recipe_clear_default(void);
esp_err_t dura_recipe_add_default_ingredient(const char *liquid_name, float amount, uint8_t sequence);
esp_err_t dura_recipe_find(const char *name, dura_recipe_t *recipe);
const char *dura_recipe_order_name(dura_recipe_order_t order);
esp_err_t dura_recipe_format_status_json(char *json, size_t json_len);
esp_err_t dura_recipe_format_detail_json(char *json, size_t json_len);

#ifdef __cplusplus
}
#endif
