#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_rom_gpio.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <hal/gpio_hal.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_sig_map.h>

#include "bq24295.h"
#include "bq27546.h"
#include "fast_hsv2rgb.h"
#include "i2c_bus.h"
#include "lis3dh.h"
#include "neighbour.h"
#include "neighbour_rssi_delay_model.h"
#include "spl06.h"
#include "strutil.h"
#include "util.h"
#include "wireless.h"

static const char *TAG = "main";

#define GPIO_LED1	20
#define GPIO_LED2	21
#define GPIO_POWER_ON	10
#define GPIO_CHARGE_EN	 1

#define NUM_LEDS	16
#define BITS_PER_SYMBOL	4
#define SYMBOL_ZERO	0b0001
#define SYMBOL_ONE	0b0111

#define BYTES_DATA	((NUM_LEDS) * 24 * (BITS_PER_SYMBOL)) / 8
#define BYTES_RESET	(250 / 8)

const uint8_t gamma_table[] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,
    2,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  5,  5,  5,
    5,  6,  6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  9,  9,  9, 10,
   10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16,
   17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25,
   25, 26, 27, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36,
   37, 38, 39, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 50,
   51, 52, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68,
   69, 70, 72, 73, 74, 75, 77, 78, 79, 81, 82, 83, 85, 86, 87, 89,
   90, 92, 93, 95, 96, 98, 99,101,102,104,105,107,109,110,112,114,
  115,117,119,120,122,124,126,127,129,131,133,135,137,138,140,142,
  144,146,148,150,152,154,156,158,160,162,164,167,169,171,173,175,
  177,180,182,184,186,189,191,193,196,198,200,203,205,208,210,213,
  215,218,220,223,225,228,231,233,236,239,241,244,247,249,252,255 };

static uint8_t *led_set_color_component(uint8_t *data, uint8_t val) {
	for (int i = 0; i < 8; i++) {
		bool bit = !!(val & (1 << (7 - i)));
		if (i % 2 == 0) {
			*data &= ~0xf;
			*data |= bit ? SYMBOL_ONE : SYMBOL_ZERO;
		} else {
			*data &= ~0xf0;
			*data |= (bit ? SYMBOL_ONE : SYMBOL_ZERO) << 4;
			data++;
		}
	}
	return data;
}

static uint8_t *led_set_color(uint8_t *data, uint32_t rgb_) {
	data = led_set_color_component(data, (rgb_ & 0x00ff00) >> 8);
	data = led_set_color_component(data, (rgb_ & 0x0000ff) >> 0);
	data = led_set_color_component(data, (rgb_ & 0xff0000) >> 16);
	return data;
}

static void leds_set_color(uint8_t *data, uint32_t rgb_) {
	for (int i = 0; i < NUM_LEDS; i++) {
		data = led_set_color(data, rgb_);
	}
}

typedef struct node_info {
	int64_t uptime_us;
	int16_t battery_voltage_mv;
	int16_t battery_current_ma;
} __attribute__((packed)) node_info_t;

typedef struct click {
	int32_t velocity;
} __attribute__((packed)) click_t;

lis3dh_t accelerometer;
static void IRAM_ATTR led_iomux_enable(spi_transaction_t *trans) {
	esp_rom_gpio_connect_out_signal(3, FSPID_OUT_IDX, false, false);
/*
	gpio_iomux_in(3, FSPID_IN_IDX);
	gpio_iomux_out(3, SPI2_FUNC_NUM, false);
*/
}

static void IRAM_ATTR led_iomux_disable(spi_transaction_t *trans) {
	gpio_set_direction(3, GPIO_MODE_OUTPUT);
	gpio_set_level(3, 0);
	gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[3], PIN_FUNC_GPIO);
}

static void color_bend_to_hs(unsigned int bend, uint16_t *h, uint8_t *s) {
	unsigned int initial_sat = 0;
//	unsigned int initial_hue = 348; // of 360
	int initial_hue = 40; // of 1536
//	unsigned int mid_saturation = 31; // of 100
	unsigned int mid_saturation = 220; // of 255
//	unsigned int final_hue = 197; // of 360
	int final_hue = 845; // of 360
	unsigned int final_saturation = 200; // of 255

	if (bend <= 500) {
		*h = initial_hue;
		*s = mid_saturation * bend  / 500;
	} else {
		int local_bend = bend - 500;
		int hue = ((int)(initial_hue - (initial_hue + HSV_HUE_STEPS - final_hue) * local_bend / 500));
		if (hue < 0) {
			hue = HSV_HUE_STEPS + hue;
		}
		*h = hue;
//		*s = 220;
		*s = mid_saturation - (mid_saturation - final_saturation) * local_bend / 500;
	}
}

static uint16_t hue_g = 0;
static uint8_t sat_g = 0, val_g = 25;

void hsv_input_loop(void *arg) {
	char keybuf[32] = { 0 };
	unsigned int bufpos = 0;
	while (1) {
		char c;
		ssize_t ret = fread(&c, 1, 1, stdin);
		if (ret > 0) {
			if (c == '\x0a') {
				keybuf[bufpos] = 0;
				ESP_LOGI(TAG, "%s", keybuf);
				char *first_sep = strchr(keybuf, ' ');
				if (first_sep) {
					*first_sep = 0;
					char *second_sep = strchr(first_sep + 1, ' ');
					if (second_sep) {
						*second_sep = 0;
						int hue = atoi(keybuf);
						int sat = atoi(first_sep + 1);
						int val = atoi(second_sep + 1);

						ESP_LOGI(TAG, "hue: %d sat: %d val: %d", hue, sat, val);
						hue_g = hue;
						sat_g = sat;
						val_g = val;
					}
				}
				bufpos = 0;
			}
			if (c == '\x20' || (c >= '0' && c <= '9')) {
				if (bufpos < sizeof(keybuf) - 1) {
					keybuf[bufpos++] = c;
				}
			}
		}
		vTaskDelay(1);
	}
}

void app_main(void) {
	gpio_reset_pin(0);
	gpio_reset_pin(2);

	gpio_reset_pin(GPIO_CHARGE_EN);
	gpio_set_direction(GPIO_CHARGE_EN, GPIO_MODE_OUTPUT);
	gpio_set_level(GPIO_CHARGE_EN, 0);

	spi_bus_config_t spi_bus_cfg = {
		.mosi_io_num = 7,
		.miso_io_num = 6,
		.sclk_io_num = 8,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 4092,
		.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
		.intr_flags = ESP_INTR_FLAG_IRAM
	};
	ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &spi_bus_cfg, SPI_DMA_CH_AUTO));
	spi_device_interface_config_t dev_cfg = {
		.command_bits = 0,
		.address_bits = 0,
		.dummy_bits = 0,
		.mode = 0,
		.duty_cycle_pos = 0,
		.cs_ena_pretrans = 0,
		.cs_ena_posttrans = 0,
		.clock_speed_hz = 3000000,
		.input_delay_ns = 0,
		.spics_io_num = -1,
		.flags = SPI_DEVICE_BIT_LSBFIRST,
		.queue_size = 1,
		.pre_cb = led_iomux_enable,
		.post_cb = led_iomux_disable,
	};
	spi_device_handle_t dev;
	ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &dev));

	size_t dma_buf_len = ALIGN_UP(BYTES_RESET + BYTES_DATA + BYTES_RESET, 4);
	ESP_LOGI(TAG, "Allocating %zu bytes of DMA memory", dma_buf_len);
	uint8_t *led_data = heap_caps_malloc(dma_buf_len, MALLOC_CAP_DMA);
	ESP_ERROR_CHECK(!led_data);
	memset(led_data, 0, dma_buf_len);

//	leds_set_color(led_data + BYTES_RESET, 0xffffff);
	leds_set_color(led_data + BYTES_RESET, 0x000000);

	spi_transaction_t xfer = {
		.length = dma_buf_len * 8,
		.rxlength = 0,
		.tx_buffer = led_data,
		.rx_buffer = NULL
	};
	ESP_ERROR_CHECK(spi_device_transmit(dev, &xfer));

	gpio_reset_pin(GPIO_LED1);
	gpio_reset_pin(GPIO_LED2);
	gpio_reset_pin(GPIO_POWER_ON);
	gpio_set_direction(GPIO_LED1, GPIO_MODE_OUTPUT);
	gpio_set_direction(GPIO_LED2, GPIO_MODE_OUTPUT);
	gpio_set_direction(GPIO_POWER_ON, GPIO_MODE_INPUT);
	gpio_set_pull_mode(GPIO_POWER_ON, GPIO_PULLDOWN_ONLY);

//	vTaskDelay(pdMS_TO_TICKS(2000));

	i2c_bus_t i2c_bus;
	i2c_bus_init(&i2c_bus, I2C_NUM_0, 0, 2, 100000);
//	i2c_detect(&i2c_bus);

	bq24295_t charger;
	// Reset charger to default settings
	bq24295_init(&charger, &i2c_bus);
	ESP_ERROR_CHECK(bq24295_reset(&charger));
	vTaskDelay(pdMS_TO_TICKS(10));

	// Setup charger settings
	// Min system voltage 3.0V
	ESP_ERROR_CHECK(bq24295_set_min_system_voltage(&charger, 3000));
	// Boost voltage 4.55V
	ESP_ERROR_CHECK(bq24295_set_boost_voltage(&charger, 4550));
	// Set input current limit to 1A
	ESP_ERROR_CHECK(bq24295_set_input_current_limit(&charger, 1000));
	// Set charging current to 1024mA
	ESP_ERROR_CHECK(bq24295_set_charge_current(&charger, 1024));
	// Terminate charge at 128mA
	ESP_ERROR_CHECK(bq24295_set_termination_current(&charger, 128));
	// Assert battery low at 2.8V
	ESP_ERROR_CHECK(bq24295_set_battery_low_threshold(&charger, BQ24295_BATTERY_LOW_THRESHOLD_2_8V));
	// Recharge battery if 300mV below charging voltage after charging
	ESP_ERROR_CHECK(bq24295_set_recharge_threshold(&charger, BQ24295_RECHARGE_THRESHOLD_300MV));

	bq27546_t gauge;
	ESP_ERROR_CHECK(bq27546_init(&gauge, &i2c_bus));
	ESP_LOGI(TAG, "Battery voltage: %dmV", bq27546_get_voltage_mv(&gauge));
	int current_ma;
	ESP_ERROR_CHECK(bq27546_get_current_ma(&gauge, &current_ma));
	ESP_LOGI(TAG, "Battery current: %dmA", current_ma);

	ESP_ERROR_CHECK(wireless_init());

	neighbour_init();

	spl06_t barometer;
	ESP_ERROR_CHECK(spl06_init(&barometer, SPI2_HOST, 9));

	ESP_ERROR_CHECK(lis3dh_init(&accelerometer, &i2c_bus, 0x18));

//	ESP_ERROR_CHECK(xTaskCreate(hsv_input_loop, "hsv_input_loop", 4096, NULL, 10, NULL) != pdPASS);

	bool level = true;
	uint8_t bright = 0;
	unsigned int offset = 0;
	bool shutdown = false;
	unsigned loop_interval_ms = 20;
	bool transaction_pending = false;
	uint64_t loops = 0;
	int64_t click_time_us = 0;
	uint32_t active_velocity = 0;
	unsigned int color_bend = 0; // max 1000
	int32_t last_pressure = -1;
	int32_t pressure_at_rest = -1;
	unsigned int pressure_samples = 0;
	while (1) {
		int64_t time_loop_start_us = esp_timer_get_time();
		gpio_set_level(GPIO_LED1, level);
		gpio_set_level(GPIO_LED2, !level);
		level = !level;

		if (!gpio_get_level(GPIO_POWER_ON)) {
			if (shutdown) {
				// Disable watchdog and other timers
				ESP_ERROR_CHECK(bq24295_set_watchdog_timeout(&charger, BQ24295_WATCHDOG_TIMEOUT_DISABLED));
				// Disable BATFET
				ESP_ERROR_CHECK(bq24295_set_shutdown(&charger, true));
			}
			ESP_LOGI(TAG, "Shutdown requested");
			shutdown = true;
		} else {
			shutdown = false;
		}

		uint8_t r, g, b;
		neighbour_t *clock_source;
		int64_t global_clock_us = neighbour_get_global_clock_and_source(&clock_source);
		neighbour_rssi_delay_model_t delay_model = {
			-20,
			2000,
			-60,
			0
		};
		int64_t clock_delay = neighbour_calculate_rssi_delay(&delay_model, clock_source);
		int64_t delayed_clock = global_clock_us - clock_delay;
		unsigned int hue = ((((uint64_t)delayed_clock / 1000UL) * HSV_HUE_STEPS) / 5000) % HSV_HUE_STEPS;
//		ESP_LOGI(TAG, "Hue: %u", hue * 360 / HSV_HUE_STEPS);
		fast_hsv2rgb_32bit(hue, HSV_SAT_MAX, HSV_VAL_MAX / 10, &r, &g, &b);
		spi_transaction_t *xfer_;
		if (transaction_pending) {
			spi_device_get_trans_result(dev, &xfer_, portMAX_DELAY);
			transaction_pending = false;
		}

		spl06_update(&barometer);
		int32_t pressure = spl06_get_pressure(&barometer);
//		ESP_LOGI(TAG, "Pressure: %ld", pressure);
		if (pressure_samples < 20) {
			if (pressure_samples == 3) {
				pressure_at_rest = pressure;
			} else if (pressure_samples > 3) {
				pressure_at_rest = (pressure_at_rest * 8 + pressure * 2) / 10;
			}
			pressure_samples++;
		} else {
			int32_t delta = pressure_at_rest - pressure;

			if (delta > 0) {
				color_bend = MIN(color_bend + delta / 8192, 1000);
			}
		}
/*
		if (last_pressure != -1) {
			int32_t delta = pressure - last_pressure;

			if (delta > 0) {
				color_bend = MIN(color_bend + delta / 256, 1000);
			}
		}
*/
		last_pressure = pressure;

//		leds_set_color(led_data + BYTES_RESET, (uint32_t)b << 16 | (uint32_t)g << 8 | r);
/*
		if (delayed_clock % 1000000UL <= 100000UL) {
			leds_set_color(led_data + BYTES_RESET, 0x101010);
		} else {
			leds_set_color(led_data + BYTES_RESET, 0);
		}
*/
		wireless_packet_t packet;
		if (xQueueReceive(wireless_get_rx_queue(), &packet, 0)) {
			ESP_LOGD(TAG, "Dequeued packet, size: %u bytes", packet.len);
			if (packet.len == sizeof(node_info_t)) {
				node_info_t node_info;
				memcpy(&node_info, packet.data, sizeof(node_info));

				ESP_LOGI(TAG, "<"MACSTR"> Uptime: %lldus, Battery voltage: %umV, Battery current: %dmA",
					 MAC2STR(packet.src_addr), node_info.uptime_us, (unsigned int)node_info.battery_voltage_mv, (int)node_info.battery_current_ma);
			} else if (packet.len == sizeof(neighbour_advertisement_t)) {
				neighbour_advertisement_t adv;
				memcpy(&adv, packet.data, sizeof(neighbour_advertisement_t));
				neighbour_update(packet.src_addr, packet.rx_timestamp, &adv);
			} else if (packet.len == sizeof(click_t)) {
				click_time_us = global_clock_us;
				click_t click;
				memcpy(&click, packet.data, sizeof(click));
				active_velocity = MAX(active_velocity, ABS(click.velocity));

				int32_t max_brightness = 255;
				int32_t min_brightness = 16;
				int32_t max_velocity = 20000;
				unsigned int brightness = min_brightness + (max_brightness - min_brightness) * active_velocity / max_velocity;
				if (brightness > max_brightness) {
					brightness = max_brightness;
				}

				leds_set_color(led_data + BYTES_RESET, 0x010101 * brightness);
//				leds_set_color(led_data + BYTES_RESET, 0x101010);
			}
		}

		lis3dh_update(&accelerometer);
		if (lis3dh_has_click_been_detected(&accelerometer)) {
			int32_t velocity = lis3dh_get_click_velocity(&accelerometer);
			ESP_LOGI(TAG, "click! velocity: %ld", (long int)velocity);
			click_time_us = global_clock_us;
			active_velocity = MAX(active_velocity, ABS(velocity));
			int32_t max_brightness = 255;
			int32_t min_brightness = 16;
			int32_t max_velocity = 20000;
			unsigned int brightness = min_brightness + (max_brightness - min_brightness) * active_velocity / max_velocity;
			if (brightness > max_brightness) {
				brightness = max_brightness;
			}

			leds_set_color(led_data + BYTES_RESET, 0x010101 * brightness);
//			leds_set_color(led_data + BYTES_RESET, 0x101010);
			click_t click = { velocity };
			wireless_broadcast((uint8_t*)&click, sizeof(click));
		}
		if (click_time_us + 100000UL < global_clock_us) {
			leds_set_color(led_data + BYTES_RESET, 0);
			active_velocity = 0;
		}

		if (color_bend > 0) {
			color_bend -= MIN(color_bend, 4);
		}

		uint16_t h;
		uint8_t s;
		color_bend_to_hs(color_bend, &h, &s);
//		fast_hsv2rgb_32bit(hue_g, sat_g, val_g, &r, &g, &b);
		fast_hsv2rgb_32bit(h, s, HSV_VAL_MAX / 10, &r, &g, &b);
		leds_set_color(led_data + BYTES_RESET, (uint32_t)b << 16 | (uint32_t)g << 8 | r);
//		leds_set_color(led_data + BYTES_RESET, (uint32_t)gamma_table[b] << 16 | (uint32_t)gamma_table[g] << 8 | gamma_table[r]);

		bright += 10;
		xfer.length = dma_buf_len * 8;
		xfer.rxlength = 0;
		xfer.tx_buffer = led_data;
		xfer.rx_buffer = NULL;
		ESP_ERROR_CHECK(spi_device_queue_trans(dev, &xfer, 0));
		transaction_pending = true;

		if (loops % 500 == 0) {
			// Charger watchdog reset
			bq24295_watchdog_reset(&charger);

			unsigned int battery_voltage_mv = 0;
			int val = bq27546_get_voltage_mv(&gauge);
			if (val > 0) {
				battery_voltage_mv = val;
			}
			int battery_current_ma = 0;
			bq27546_get_current_ma(&gauge, &battery_current_ma);

			node_info_t node_info = {
				.uptime_us = esp_timer_get_time(),
				.battery_voltage_mv = battery_voltage_mv,
				.battery_current_ma = battery_current_ma
			};

			esp_err_t err = wireless_broadcast((uint8_t*)&node_info, sizeof(node_info));
			if (err) {
				ESP_LOGE(TAG, "Failed to send status inforamtion: %d", err);
			} else {
				ESP_LOGI(TAG, "Status information sent");
			}
		}

		if (loops % 500 == 250) {
			wireless_scan_aps();
		}

		if (wireless_is_scan_done()) {
			unsigned int num_results = wireless_get_num_scan_results();
			ESP_LOGI(TAG, "Scan complete, found %u APs", num_results);
			wifi_ap_record_t *scan_results = calloc(num_results, sizeof(wifi_ap_record_t));
			if (scan_results) {
				esp_err_t err = wireless_get_scan_results(scan_results, &num_results);
				if (!err) {
					for (int i = 0; i < num_results; i++) {
						wifi_ap_record_t *scan_result = &scan_results[i];
						if (!str_starts_with((const char *)scan_result->ssid, "blinkekatze_")) {
							continue;
						}

						neighbour_update_rssi(scan_result->bssid, scan_result->rssi);
					}
				}
				free(scan_results);
			}
			wireless_clear_scan_results();
		}

		neighbour_housekeeping();

		offset++;
		offset %= HSV_HUE_STEPS;
//		color_bend += 5;
//		color_bend %= 1000;
		int64_t time_loop_end_us = esp_timer_get_time();
		int dt_ms = DIV_ROUND(time_loop_end_us - time_loop_start_us, 1000);
		if (dt_ms > loop_interval_ms) {
			ESP_LOGW(TAG, "Can't keep up, update took %d ms", dt_ms);
			vTaskDelay(pdMS_TO_TICKS(0));
		} else {
			vTaskDelay(pdMS_TO_TICKS(loop_interval_ms - dt_ms));
		}
		loops++;
	}
}
