#pragma once
#include <stddef.h>
#include <stdint.h>

/* Candidate47 contract: explicit saved-Water multipliers (not compounded).
 * Named anchors are customer/reference-backed; unnamed endpoints 1.03/.83
 * complete the approved uniform .01 steps, intentionally not the old range.
 * This is an editor selection, never evidence of the active calibration. */
#define DURA_QUICK_CAL_DEFAULT 4u
#define DURA_QUICK_CAL_UNMAPPED 0u
#define DURA_QUICK_CAL_NVS_KEY "quick_ref_v2"
typedef struct {
    uint8_t number;
    uint8_t pointer_y;
    float water_factor;
    const char *product;
    const char *temperature;
} dura_quick_cal_entry_t;

static inline const dura_quick_cal_entry_t *dura_quick_cal_entry(uint8_t number)
{
    static const dura_quick_cal_entry_t entries[] = {
        {1, 2, 1.03f, NULL, NULL},
        {2, 4, 1.02f, NULL, NULL},
        {3, 6, 1.01f, NULL, NULL},
        {4, 8, 1.00f, "Water", "66 F"},
        {5, 10, .99f, NULL, NULL},
        {6, 12, .98f, "Kerosene", "69 F"},
        {7, 14, .97f, NULL, NULL},
        {8, 16, .96f, NULL, NULL},
        {9, 18, .95f, NULL, NULL},
        {10, 20, .94f, "Anti-freeze", "71 F"},
        {11, 22, .93f, NULL, NULL},
        {12, 24, .92f, NULL, NULL},
        {13, 26, .91f, NULL, NULL},
        {14, 28, .90f, "10W Oil", "71 F"},
        {15, 30, .89f, NULL, NULL},
        {16, 32, .88f, NULL, NULL},
        {17, 34, .87f, NULL, NULL},
        {18, 36, .86f, NULL, NULL},
        {19, 38, .85f, NULL, NULL},
        {20, 40, .84f, NULL, NULL},
        {21, 42, .83f, NULL, NULL},
    };
    return number >= 1 && number <= 21 ? &entries[number - 1] : NULL;
}
