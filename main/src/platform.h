#pragma once

#include <stdint.h>

#include <esp_err.h>

#include "bq24295.h"
#include "bq27546.h"
#include "lis3dh.h"
#include "ltr_303als.h"
#include "platform.h"
#include "spl06.h"

typedef struct platform platform_t;

typedef struct platform_ops {
	void (*pre_schedule)(platform_t *platform);
	void (*set_rgb_led_color)(platform_t *platform, uint16_t r, uint16_t g, uint16_t b);
} platform_ops_t;

struct platform {
	bq27546_t *gauge;
	bq24295_t *charger;
	ltr_303als_t *als;
	lis3dh_t *accelerometer;
	spl06_t *barometer;
	const platform_ops_t *ops;
};

void platform_init(platform_t *plat, const platform_ops_t *ops);
esp_err_t platform_probe(platform_t **platform);
void platform_pre_schedule(platform_t *platform);
void platform_set_rgb_led_color(platform_t *platform, uint16_t r, uint16_t g, uint16_t b);
