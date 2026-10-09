#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "nvs.h"

#include "dura_recipe.h"
#include "dura_json_builder.h"

static const char *TAG = "dura_recipe";
static const char *NVS_NAMESPACE = "dura_recipe";
static const char *NVS_KEY_CONFIG = "config";
static const char *NVS_KEY_LIQUID_CATALOG = "liquid_catalog";
static const uint32_t RECIPE_CONFIG_MAGIC = 0x44524350U; /* DRCP */
static const uint16_t RECIPE_CONFIG_VERSION = 1;
static const uint32_t LIQUID_CATALOG_MAGIC = 0x444C4350U; /* DLCP */
static const uint16_t LIQUID_CATALOG_VERSION = 1;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    dura_liquid_config_t local_liquid;
    dura_recipe_t default_recipe;
} dura_recipe_persisted_config_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint16_t next_id;
    uint16_t reserved;
    dura_liquid_profile_t custom[DURA_RECIPE_MAX_CUSTOM_LIQUIDS];
} dura_liquid_catalog_persisted_t;

static const dura_liquid_profile_t s_factory_liquids[] = {
    {.id = 17, .source = DURA_LIQUID_SOURCE_FACTORY, .reference_num = 17, .name = "Water 66F", .calibration_factor = 1.000f},
    {.id = 15, .source = DURA_LIQUID_SOURCE_FACTORY, .reference_num = 15, .name = "Kerosene 69F", .calibration_factor = 0.980f},
    {.id = 11, .source = DURA_LIQUID_SOURCE_FACTORY, .reference_num = 11, .name = "Anti-freeze 71F", .calibration_factor = 0.940f},
    {.id = 7, .source = DURA_LIQUID_SOURCE_FACTORY, .reference_num = 7, .name = "10W Oil 71F", .calibration_factor = 0.900f},
};

static dura_liquid_config_t s_local_liquid;
static dura_recipe_t s_default_recipe;
static dura_liquid_profile_t s_custom_liquids[DURA_RECIPE_MAX_CUSTOM_LIQUIDS];
static size_t s_custom_liquid_count;
static uint16_t s_next_custom_liquid_id = DURA_RECIPE_CUSTOM_LIQUID_ID_BASE;
static bool s_initialized;

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }
    snprintf(dst, dst_len, "%s", src != NULL ? src : "");
}

static void load_defaults(void)
{
    memset(&s_local_liquid, 0, sizeof(s_local_liquid));
    copy_text(s_local_liquid.liquid_name, sizeof(s_local_liquid.liquid_name), "unassigned");
    s_local_liquid.viscosity_cP = 1.0f;
    s_local_liquid.calibration_id = 0;

    memset(&s_default_recipe, 0, sizeof(s_default_recipe));
    copy_text(s_default_recipe.name, sizeof(s_default_recipe.name), "unconfigured");
    s_default_recipe.order = DURA_RECIPE_ORDER_CONFIGURED;
    s_default_recipe.ingredient_count = 0;
}

static bool valid_recipe(const dura_recipe_t *recipe)
{
    if (recipe == NULL || recipe->name[0] == '\0') {
        return false;
    }
    if (recipe->ingredient_count > DURA_RECIPE_MAX_INGREDIENTS) {
        return false;
    }
    for (size_t i = 0; i < recipe->ingredient_count; ++i) {
        if (recipe->ingredients[i].liquid_name[0] == '\0' || recipe->ingredients[i].amount <= 0.0f) {
            return false;
        }
    }
    return true;
}

static bool valid_liquid(const dura_liquid_config_t *liquid)
{
    return liquid != NULL && liquid->liquid_name[0] != '\0' && liquid->viscosity_cP > 0.0f;
}

static const char *liquid_source_name(dura_liquid_source_t source)
{
    switch (source) {
    case DURA_LIQUID_SOURCE_FACTORY:
        return "factory";
    case DURA_LIQUID_SOURCE_CUSTOM:
        return "custom";
    default:
        return "unknown";
    }
}

static bool valid_liquid_profile_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (const char *p = name; *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20U || *p == '\"' || *p == '\\') {
            return false;
        }
    }
    return true;
}

static bool valid_liquid_profile(const dura_liquid_profile_t *profile)
{
    if (profile == NULL || !valid_liquid_profile_name(profile->name) || profile->calibration_factor <= 0.0f) {
        return false;
    }
    if (profile->source != DURA_LIQUID_SOURCE_FACTORY && profile->source != DURA_LIQUID_SOURCE_CUSTOM) {
        return false;
    }
    return true;
}

static void liquid_profile_to_config(const dura_liquid_profile_t *profile, dura_liquid_config_t *liquid)
{
    memset(liquid, 0, sizeof(*liquid));
    copy_text(liquid->liquid_name, sizeof(liquid->liquid_name), profile->name);
    liquid->viscosity_cP = profile->calibration_factor;
    liquid->calibration_id = profile->id;
}

static bool local_liquid_is_selected_profile(const dura_liquid_profile_t *profile)
{
    return profile != NULL &&
           s_local_liquid.calibration_id == profile->id &&
           strcasecmp(s_local_liquid.liquid_name, profile->name) == 0;
}

static bool local_liquid_matches_catalog(void)
{
    if (strcasecmp(s_local_liquid.liquid_name, "unassigned") == 0) {
        return true;
    }
    for (size_t i = 0; i < sizeof(s_factory_liquids) / sizeof(s_factory_liquids[0]); ++i) {
        if (strcasecmp(s_local_liquid.liquid_name, s_factory_liquids[i].name) == 0) {
            return true;
        }
    }
    for (size_t i = 0; i < s_custom_liquid_count; ++i) {
        if (strcasecmp(s_local_liquid.liquid_name, s_custom_liquids[i].name) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t reconcile_local_liquid_with_catalog(void)
{
    if (local_liquid_matches_catalog()) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "clearing stale non-catalog liquid selection: %s", s_local_liquid.liquid_name);
    copy_text(s_local_liquid.liquid_name, sizeof(s_local_liquid.liquid_name), "unassigned");
    s_local_liquid.viscosity_cP = 1.0f;
    s_local_liquid.calibration_id = 0;
    return dura_recipe_save();
}

static esp_err_t load_custom_liquid_catalog(void)
{
    s_custom_liquid_count = 0;
    s_next_custom_liquid_id = DURA_RECIPE_CUSTOM_LIQUID_ID_BASE;
    memset(s_custom_liquids, 0, sizeof(s_custom_liquids));

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    dura_liquid_catalog_persisted_t persisted = {0};
    size_t len = sizeof(persisted);
    err = nvs_get_blob(nvs, NVS_KEY_LIQUID_CATALOG, &persisted, &len);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    ESP_RETURN_ON_FALSE(len == sizeof(persisted), ESP_ERR_INVALID_SIZE, TAG, "bad liquid catalog size");
    ESP_RETURN_ON_FALSE(persisted.magic == LIQUID_CATALOG_MAGIC && persisted.version == LIQUID_CATALOG_VERSION,
                        ESP_ERR_INVALID_VERSION, TAG, "bad liquid catalog version");
    ESP_RETURN_ON_FALSE(persisted.count <= DURA_RECIPE_MAX_CUSTOM_LIQUIDS,
                        ESP_ERR_INVALID_SIZE, TAG, "bad liquid catalog count");

    s_custom_liquid_count = persisted.count;
    s_next_custom_liquid_id = persisted.next_id < DURA_RECIPE_CUSTOM_LIQUID_ID_BASE ?
                              DURA_RECIPE_CUSTOM_LIQUID_ID_BASE : persisted.next_id;
    for (size_t i = 0; i < s_custom_liquid_count; ++i) {
        s_custom_liquids[i] = persisted.custom[i];
        s_custom_liquids[i].name[sizeof(s_custom_liquids[i].name) - 1] = '\0';
        ESP_RETURN_ON_FALSE(valid_liquid_profile(&s_custom_liquids[i]) &&
                                s_custom_liquids[i].source == DURA_LIQUID_SOURCE_CUSTOM &&
                                s_custom_liquids[i].id >= DURA_RECIPE_CUSTOM_LIQUID_ID_BASE,
                            ESP_ERR_INVALID_ARG, TAG, "bad custom liquid profile");
        if (s_custom_liquids[i].id >= s_next_custom_liquid_id) {
            s_next_custom_liquid_id = (uint16_t)(s_custom_liquids[i].id + 1U);
        }
    }
    return ESP_OK;
}

static esp_err_t save_custom_liquid_catalog(void)
{
    ESP_RETURN_ON_FALSE(s_custom_liquid_count <= DURA_RECIPE_MAX_CUSTOM_LIQUIDS,
                        ESP_ERR_INVALID_STATE, TAG, "bad custom liquid count");

    dura_liquid_catalog_persisted_t persisted = {
        .magic = LIQUID_CATALOG_MAGIC,
        .version = LIQUID_CATALOG_VERSION,
        .count = (uint16_t)s_custom_liquid_count,
        .next_id = s_next_custom_liquid_id,
    };
    for (size_t i = 0; i < s_custom_liquid_count; ++i) {
        persisted.custom[i] = s_custom_liquids[i];
    }

    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open NVS failed");
    esp_err_t err = nvs_set_blob(nvs, NVS_KEY_LIQUID_CATALOG, &persisted, sizeof(persisted));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t dura_recipe_init(void)
{
    load_defaults();
    s_initialized = true;

    esp_err_t err = dura_recipe_load();
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "load persisted recipe config failed");

    err = load_custom_liquid_catalog();
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "load custom liquid catalog failed");

    ESP_RETURN_ON_ERROR(reconcile_local_liquid_with_catalog(), TAG, "reconcile liquid selection failed");

    ESP_LOGI(TAG, "recipe service initialized; max ingredients=%u custom liquids=%u/%u local liquid=%s recipe=%s ingredients=%u",
             (unsigned)DURA_RECIPE_MAX_INGREDIENTS,
             (unsigned)s_custom_liquid_count,
             (unsigned)DURA_RECIPE_MAX_CUSTOM_LIQUIDS,
             s_local_liquid.liquid_name,
             s_default_recipe.name, (unsigned)s_default_recipe.ingredient_count);
    return ESP_OK;
}

esp_err_t dura_recipe_load(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    dura_recipe_persisted_config_t persisted = {0};
    size_t len = sizeof(persisted);
    err = nvs_get_blob(nvs, NVS_KEY_CONFIG, &persisted, &len);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }
    ESP_RETURN_ON_FALSE(len == sizeof(persisted), ESP_ERR_INVALID_SIZE, TAG, "bad persisted size");
    ESP_RETURN_ON_FALSE(persisted.magic == RECIPE_CONFIG_MAGIC && persisted.version == RECIPE_CONFIG_VERSION,
                        ESP_ERR_INVALID_VERSION, TAG, "bad persisted recipe version");
    ESP_RETURN_ON_FALSE(valid_liquid(&persisted.local_liquid), ESP_ERR_INVALID_ARG, TAG, "bad persisted liquid");
    ESP_RETURN_ON_FALSE(valid_recipe(&persisted.default_recipe), ESP_ERR_INVALID_ARG, TAG, "bad persisted recipe");

    s_local_liquid = persisted.local_liquid;
    s_local_liquid.liquid_name[sizeof(s_local_liquid.liquid_name) - 1] = '\0';
    s_default_recipe = persisted.default_recipe;
    s_default_recipe.name[sizeof(s_default_recipe.name) - 1] = '\0';
    for (size_t i = 0; i < s_default_recipe.ingredient_count; ++i) {
        s_default_recipe.ingredients[i].liquid_name[sizeof(s_default_recipe.ingredients[i].liquid_name) - 1] = '\0';
    }
    ESP_LOGI(TAG, "loaded recipe config from NVS");
    return ESP_OK;
}

esp_err_t dura_recipe_save(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(valid_liquid(&s_local_liquid), ESP_ERR_INVALID_STATE, TAG, "bad liquid config");
    ESP_RETURN_ON_FALSE(valid_recipe(&s_default_recipe), ESP_ERR_INVALID_STATE, TAG, "bad recipe config");

    dura_recipe_persisted_config_t persisted = {
        .magic = RECIPE_CONFIG_MAGIC,
        .version = RECIPE_CONFIG_VERSION,
        .local_liquid = s_local_liquid,
        .default_recipe = s_default_recipe,
    };

    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open NVS failed");
    esp_err_t err = nvs_set_blob(nvs, NVS_KEY_CONFIG, &persisted, sizeof(persisted));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t dura_recipe_get_local_liquid(dura_liquid_config_t *liquid)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(liquid != NULL, ESP_ERR_INVALID_ARG, TAG, "liquid is NULL");
    *liquid = s_local_liquid;
    return ESP_OK;
}

esp_err_t dura_recipe_set_local_liquid(const dura_liquid_config_t *liquid)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(valid_liquid(liquid), ESP_ERR_INVALID_ARG, TAG, "invalid liquid");

    s_local_liquid = *liquid;
    s_local_liquid.liquid_name[sizeof(s_local_liquid.liquid_name) - 1] = '\0';
    return dura_recipe_save();
}

size_t dura_recipe_get_liquid_profile_count(void)
{
    return (sizeof(s_factory_liquids) / sizeof(s_factory_liquids[0])) + s_custom_liquid_count;
}

esp_err_t dura_recipe_get_liquid_profile(size_t index, dura_liquid_profile_t *profile)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(profile != NULL, ESP_ERR_INVALID_ARG, TAG, "profile is NULL");

    size_t factory_count = sizeof(s_factory_liquids) / sizeof(s_factory_liquids[0]);
    if (index < factory_count) {
        *profile = s_factory_liquids[index];
        return ESP_OK;
    }
    index -= factory_count;
    if (index < s_custom_liquid_count) {
        *profile = s_custom_liquids[index];
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t dura_recipe_find_liquid_profile_by_id(uint16_t id, dura_liquid_profile_t *profile)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(profile != NULL, ESP_ERR_INVALID_ARG, TAG, "profile is NULL");

    for (size_t i = 0; i < sizeof(s_factory_liquids) / sizeof(s_factory_liquids[0]); ++i) {
        if (s_factory_liquids[i].id == id) {
            *profile = s_factory_liquids[i];
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < s_custom_liquid_count; ++i) {
        if (s_custom_liquids[i].id == id) {
            *profile = s_custom_liquids[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t dura_recipe_select_liquid_profile(uint16_t id)
{
    dura_liquid_profile_t profile = {0};
    dura_liquid_config_t liquid = {0};
    ESP_RETURN_ON_ERROR(dura_recipe_find_liquid_profile_by_id(id, &profile), TAG, "liquid profile not found");
    liquid_profile_to_config(&profile, &liquid);
    return dura_recipe_set_local_liquid(&liquid);
}

esp_err_t dura_recipe_add_custom_liquid(const char *name, float calibration_factor, uint8_t reference_num, uint16_t *id_out)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(s_custom_liquid_count < DURA_RECIPE_MAX_CUSTOM_LIQUIDS,
                        ESP_ERR_NO_MEM, TAG, "custom liquid catalog full");
    ESP_RETURN_ON_FALSE(valid_liquid_profile_name(name) && calibration_factor > 0.0f,
                        ESP_ERR_INVALID_ARG, TAG, "invalid custom liquid");

    for (size_t i = 0; i < dura_recipe_get_liquid_profile_count(); ++i) {
        dura_liquid_profile_t existing = {0};
        if (dura_recipe_get_liquid_profile(i, &existing) == ESP_OK && strcasecmp(existing.name, name) == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (s_next_custom_liquid_id < DURA_RECIPE_CUSTOM_LIQUID_ID_BASE) {
        s_next_custom_liquid_id = DURA_RECIPE_CUSTOM_LIQUID_ID_BASE;
    }

    dura_liquid_profile_t *profile = &s_custom_liquids[s_custom_liquid_count];
    memset(profile, 0, sizeof(*profile));
    profile->id = s_next_custom_liquid_id++;
    profile->source = DURA_LIQUID_SOURCE_CUSTOM;
    profile->reference_num = reference_num;
    copy_text(profile->name, sizeof(profile->name), name);
    profile->calibration_factor = calibration_factor;
    s_custom_liquid_count++;

    esp_err_t err = save_custom_liquid_catalog();
    if (err != ESP_OK) {
        s_custom_liquid_count--;
        memset(profile, 0, sizeof(*profile));
        return err;
    }
    if (id_out != NULL) {
        *id_out = profile->id;
    }
    return ESP_OK;
}

esp_err_t dura_recipe_delete_custom_liquid(uint16_t id)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(id >= DURA_RECIPE_CUSTOM_LIQUID_ID_BASE,
                        ESP_ERR_INVALID_ARG, TAG, "cannot delete factory liquid");

    for (size_t i = 0; i < s_custom_liquid_count; ++i) {
        if (s_custom_liquids[i].id != id) {
            continue;
        }
        if (local_liquid_is_selected_profile(&s_custom_liquids[i])) {
            return ESP_ERR_INVALID_STATE;
        }
        for (size_t j = i; j + 1 < s_custom_liquid_count; ++j) {
            s_custom_liquids[j] = s_custom_liquids[j + 1];
        }
        s_custom_liquid_count--;
        memset(&s_custom_liquids[s_custom_liquid_count], 0, sizeof(s_custom_liquids[s_custom_liquid_count]));
        return save_custom_liquid_catalog();
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t dura_recipe_format_liquid_catalog_json(char *json, size_t json_len)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(json != NULL && json_len > 0, ESP_ERR_INVALID_ARG, TAG, "bad json buffer");

    int written = dura_json_snprintf(json, json_len,
                           "{\"selected\":\"%s\",\"selected_id\":%lu,\"custom_count\":%u,\"max_custom\":%u,\"liquids\":[",
                           s_local_liquid.liquid_name,
                           (unsigned long)s_local_liquid.calibration_id,
                           (unsigned)s_custom_liquid_count,
                           (unsigned)DURA_RECIPE_MAX_CUSTOM_LIQUIDS);
    if (written <= 0 || (size_t)written >= json_len) {
        return dura_json_fail(json, json_len, written);
    }
    size_t used = (size_t)written;
    for (size_t i = 0; i < dura_recipe_get_liquid_profile_count(); ++i) {
        dura_liquid_profile_t profile = {0};
        ESP_RETURN_ON_ERROR(dura_recipe_get_liquid_profile(i, &profile), TAG, "get liquid profile failed");
        written = dura_json_snprintf(json + used, json_len - used,
                           "%s{\"id\":%u,\"source\":\"%s\",\"reference_num\":%u,\"name\":\"%s\",\"calibration_factor\":%.3f,\"selected\":%s}",
                           i == 0 ? "" : ",",
                           (unsigned)profile.id,
                           liquid_source_name(profile.source),
                           (unsigned)profile.reference_num,
                           profile.name,
                           (double)profile.calibration_factor,
                           local_liquid_is_selected_profile(&profile) ? "true" : "false");
        if (written <= 0 || (size_t)written >= json_len - used) {
            return dura_json_fail(json, json_len, written);
        }
        used += (size_t)written;
    }
    written = dura_json_snprintf(json + used, json_len - used, "]}");
    return dura_json_result(json, json_len, written);
}

esp_err_t dura_recipe_get_default(dura_recipe_t *recipe)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(recipe != NULL, ESP_ERR_INVALID_ARG, TAG, "recipe is NULL");
    *recipe = s_default_recipe;
    return ESP_OK;
}

esp_err_t dura_recipe_set_default(const dura_recipe_t *recipe)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(valid_recipe(recipe), ESP_ERR_INVALID_ARG, TAG, "invalid recipe");

    s_default_recipe = *recipe;
    s_default_recipe.name[sizeof(s_default_recipe.name) - 1] = '\0';
    for (size_t i = 0; i < s_default_recipe.ingredient_count; ++i) {
        s_default_recipe.ingredients[i].liquid_name[sizeof(s_default_recipe.ingredients[i].liquid_name) - 1] = '\0';
        if (s_default_recipe.ingredients[i].sequence == 0) {
            s_default_recipe.ingredients[i].sequence = (uint8_t)(i + 1);
        }
    }
    return dura_recipe_save();
}

esp_err_t dura_recipe_reset_default(const char *name, dura_recipe_order_t order)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(name != NULL && name[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "recipe name required");

    memset(&s_default_recipe, 0, sizeof(s_default_recipe));
    copy_text(s_default_recipe.name, sizeof(s_default_recipe.name), name);
    s_default_recipe.order = order;
    return dura_recipe_save();
}

esp_err_t dura_recipe_clear_default(void)
{
    return dura_recipe_reset_default("unconfigured", DURA_RECIPE_ORDER_CONFIGURED);
}

esp_err_t dura_recipe_factory_defaults(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    load_defaults();
    return dura_recipe_save();
}

esp_err_t dura_recipe_add_default_ingredient(const char *liquid_name, float amount, uint8_t sequence)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(liquid_name != NULL && liquid_name[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "liquid name required");
    ESP_RETURN_ON_FALSE(amount > 0.0f, ESP_ERR_INVALID_ARG, TAG, "amount must be positive");
    ESP_RETURN_ON_FALSE(s_default_recipe.ingredient_count < DURA_RECIPE_MAX_INGREDIENTS,
                        ESP_ERR_NO_MEM, TAG, "recipe ingredient limit reached");

    size_t idx = s_default_recipe.ingredient_count++;
    dura_recipe_ingredient_t *ingredient = &s_default_recipe.ingredients[idx];
    memset(ingredient, 0, sizeof(*ingredient));
    copy_text(ingredient->liquid_name, sizeof(ingredient->liquid_name), liquid_name);
    ingredient->amount = amount;
    ingredient->sequence = sequence == 0 ? (uint8_t)(idx + 1) : sequence;
    return dura_recipe_save();
}

esp_err_t dura_recipe_find(const char *name, dura_recipe_t *recipe)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(recipe != NULL, ESP_ERR_INVALID_ARG, TAG, "recipe is NULL");
    if (name == NULL || name[0] == '\0' || strcasecmp(name, s_default_recipe.name) == 0) {
        *recipe = s_default_recipe;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

const char *dura_recipe_order_name(dura_recipe_order_t order)
{
    switch (order) {
    case DURA_RECIPE_ORDER_CONFIGURED:
        return "configured";
    case DURA_RECIPE_ORDER_LARGEST_FIRST:
        return "largest";
    case DURA_RECIPE_ORDER_SMALLEST_FIRST:
        return "smallest";
    default:
        return "unknown";
    }
}

esp_err_t dura_recipe_format_status_json(char *json, size_t json_len)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(json != NULL && json_len > 0, ESP_ERR_INVALID_ARG, TAG, "bad json buffer");

    int written = dura_json_snprintf(json, json_len,
                           "{\"local_liquid\":\"%s\",\"local_liquid_id\":%lu,\"viscosity_cP\":%.2f,\"calibration_id\":%lu,\"default_recipe\":\"%s\",\"ingredients\":%u,\"max_ingredients\":%u,\"order\":\"%s\"}",
                           s_local_liquid.liquid_name,
                           (unsigned long)s_local_liquid.calibration_id,
                           (double)s_local_liquid.viscosity_cP,
                           (unsigned long)s_local_liquid.calibration_id,
                           s_default_recipe.name,
                           (unsigned)s_default_recipe.ingredient_count,
                           (unsigned)DURA_RECIPE_MAX_INGREDIENTS,
                           dura_recipe_order_name(s_default_recipe.order));
    return dura_json_result(json, json_len, written);
}

esp_err_t dura_recipe_format_detail_json(char *json, size_t json_len)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(json != NULL && json_len > 0, ESP_ERR_INVALID_ARG, TAG, "bad json buffer");

    int written = dura_json_snprintf(json, json_len,
                           "{\"name\":\"%s\",\"order\":\"%s\",\"ingredient_count\":%u,\"ingredients\":[",
                           s_default_recipe.name,
                           dura_recipe_order_name(s_default_recipe.order),
                           (unsigned)s_default_recipe.ingredient_count);
    if (written <= 0 || (size_t)written >= json_len) {
        return dura_json_fail(json, json_len, written);
    }

    size_t used = (size_t)written;
    for (size_t i = 0; i < s_default_recipe.ingredient_count; ++i) {
        const dura_recipe_ingredient_t *ingredient = &s_default_recipe.ingredients[i];
        written = dura_json_snprintf(json + used, json_len - used,
                           "%s{\"liquid\":\"%s\",\"amount\":%.2f,\"sequence\":%u}",
                           i == 0 ? "" : ",",
                           ingredient->liquid_name,
                           (double)ingredient->amount,
                           (unsigned)ingredient->sequence);
        if (written <= 0 || (size_t)written >= json_len - used) {
            return dura_json_fail(json, json_len, written);
        }
        used += (size_t)written;
    }

    written = dura_json_snprintf(json + used, json_len - used, "]}");
    return dura_json_result(json, json_len, written);
}
