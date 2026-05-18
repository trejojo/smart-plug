#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "board_profile.h"
#include "module_ade7953.h"
#include "module_ledstrip.h"
#include "module_relay.h"

static const char *TAG = "aice_unified";

#define ENABLE_PEAK_HEADROOM_MONITOR  1
/* Set to 1 for high-speed signal tracking. Set to 0 for Safety Watchdog / Calibration */
#define ENABLE_WAVEFORM_STREAM        1  
#define ENABLE_IRQ_DEBUG_READBACK     0

static TaskHandle_t s_wave_task = NULL;
static gpio_num_t s_ade_irq_gpio = GPIO_NUM_NC;

typedef struct {
    int32_t v_wave;
    int32_t i_wave;
} wave_sample_t;

/* --- INTERRUPT HANDLERS --- */

/* 1. High-Speed Waveform ISR (Triggered 7,000 times/sec if STREAM = 1) */
static void IRAM_ATTR ade7953_waveform_irq_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_wave_task != NULL) {
        vTaskNotifyGiveFromISR(s_wave_task, &xHigherPriorityTaskWoken);
    }
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/* 2. Critical Safety ISR (Triggered ONLY if hardware catches fire/overloads when STREAM = 0) */
static void IRAM_ATTR ade7953_safety_irq_handler(void *arg)
{
    /* Emergency Cutoff */
    module_relay_set(false);
    module_ledstrip_set(32, 0, 0); /* Red */
}


/* --- TASKS --- */
static void waveform_stream_task(void *arg)
{
    wave_sample_t sample;

    while (true) {
        /* Wait for the hardware falling edge interrupt */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
#if ENABLE_IRQ_DEBUG_READBACK
            ade7953_events_t events = {0};
            if (module_ade7953_read_events(&events, false) == ESP_OK) {
                int current_gpio = (s_ade_irq_gpio != GPIO_NUM_NC) ? gpio_get_level(s_ade_irq_gpio) : -1;
                ESP_LOGW(TAG, "IRQ status: gpio=%d irq_a=0x%06" PRIX32 " irq_b=0x%06" PRIX32 " wsmp=%d cycend=%d",
                         current_gpio,
                         events.irq_a, events.irq_b,
                         events.waveform_sample_ready, events.line_cycle_end);
                
                /* FAIL-SAFE: If we timed out and the line is physically stuck low, force an unlatch! */
                if (current_gpio == 0) {
                    ESP_LOGE(TAG, "Line stuck LOW! Forcing unlatch recovery.");
                    uint32_t force_clear = 0;
                    (void)module_ade7953_read32(ADE7953_REG_RSTIRQSTATA_32, &force_clear);
                    (void)module_ade7953_read32(ADE7953_REG_RSTIRQSTATB_32, &force_clear);
                }
            }
#endif
            continue;
        }

        /* Normal high-speed execution path */
        if (module_ade7953_read_s32(ADE7953_REG_V_32, &sample.v_wave) == ESP_OK &&
            module_ade7953_read_s32(ADE7953_REG_IA_32, &sample.i_wave) == ESP_OK) {
            printf("%" PRId32 ",%" PRId32 "\n", sample.v_wave, sample.i_wave);
        }

        /* Clear the latched IRQ so the line goes back to 1 and can fire again */
        uint32_t dummy_irq = 0;
        (void)module_ade7953_read32(ADE7953_REG_RSTIRQSTATA_32, &dummy_irq);
    }
}

static void setup_button_init(gpio_num_t gpio_num)
{
    gpio_config_t button_cfg = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_cfg));
}

static void relay_led_set(bool relay_on)
{
    if (relay_on) {
        module_ledstrip_set(0, 32, 0); /* green */
    } else {
        module_ledstrip_set(32, 0, 0); /* red */
    }
}

static esp_err_t ade7953_startup_config(void)
{
    const smartplug_board_pins_t *pins = smartplug_board_pins_get();
    s_ade_irq_gpio = pins->ade_irq_gpio;

    ESP_ERROR_CHECK(module_ade7953_set_hpf(true));
    ESP_ERROR_CHECK(module_ade7953_set_integrators(false, false));
    ESP_ERROR_CHECK(module_ade7953_set_pga(ADE7953_PGA_GAIN_2,
                                           ADE7953_PGA_GAIN_1,
                                           ADE7953_PGA_GAIN_1));

    ESP_ERROR_CHECK(module_ade7953_set_zero_cross_timeout_ms(100.0f));

#if ENABLE_WAVEFORM_STREAM
    /* HIGH SPEED STREAMING SETUP */
    if (xTaskCreate(waveform_stream_task, "ade7953_wave", 4096, NULL, 20, &s_wave_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create waveform stream task");
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(module_ade7953_configure_irq_masks(ADE7953_IRQ_A_WSMP, 0));

    ade7953_events_t startup_events = {0};
    ESP_ERROR_CHECK(module_ade7953_read_events(&startup_events, true));

    gpio_config_t irq_pin_cfg = {
        .pin_bit_mask = 1ULL << pins->ade_irq_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&irq_pin_cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(pins->ade_irq_gpio, ade7953_waveform_irq_handler, NULL));

#else
    /* SAFETY WATCHDOG SETUP */
    const uint32_t raw_safe_irq_a = ADE7953_IRQ_A_SAG | ADE7953_IRQ_A_OV | ADE7953_IRQ_A_OIA;
    const uint32_t raw_safe_irq_b = 0;
    ESP_ERROR_CHECK(module_ade7953_configure_irq_masks(raw_safe_irq_a, raw_safe_irq_b));

    /* Force scrub latches so the line initializes to 1 (High) */
    uint32_t flush_irq = 0;
    for (int i = 0; i < 3; i++) {
        (void)module_ade7953_read32(ADE7953_REG_RSTIRQSTATA_32, &flush_irq);
        (void)module_ade7953_read32(ADE7953_REG_RSTIRQSTATB_32, &flush_irq);
    }

    gpio_config_t irq_pin_cfg = {
        .pin_bit_mask = 1ULL << pins->ade_irq_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // Reinforce external 10k resistor
        .intr_type = GPIO_INTR_NEGEDGE,   // Fire instantly if an emergency drops the line
    };
    ESP_ERROR_CHECK(gpio_config(&irq_pin_cfg));
    
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret == ESP_OK || isr_ret == ESP_ERR_INVALID_STATE) {
        gpio_isr_handler_add(pins->ade_irq_gpio, ade7953_safety_irq_handler, NULL);
    }
#endif

    module_ade7953_reset_energy_totals();
    return ESP_OK;
}

void app_main(void)
{
    const smartplug_board_pins_t *pins = smartplug_board_pins_get();
    
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("aice_unified", ESP_LOG_INFO);

    ESP_LOGI(TAG, "AICE Firmware Booted");

    ESP_ERROR_CHECK(module_relay_init());
    ESP_ERROR_CHECK(module_ledstrip_init());
    setup_button_init(pins->setup_bt_button);

    if (module_ade7953_init() == ESP_OK) {
        if (ade7953_startup_config() != ESP_OK) {
            ESP_LOGE(TAG, "ADE7953 startup config failed! Halting.");
            while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
        }
        ESP_LOGI(TAG, "ADE7953 Initialized Successfully.");
    }

    bool relay_state = true;
    module_relay_set(relay_state);
    relay_led_set(relay_state);
    
    int last_button_level = 1;

#if ENABLE_WAVEFORM_STREAM
    printf("WAVE_V,WAVE_I\n");
    while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
#else
    /* Standard Calibration & Watchdog Loop */
#if ENABLE_PEAK_HEADROOM_MONITOR
    printf("t_ms,vrms_raw,irmsa_raw,irmsb_raw,awatt_raw,avar_raw,ava_raw,aenergy_raw,pf_raw,period_raw,vpeak_raw,iapeak_raw\n");
#else
    printf("t_ms,vrms_raw,irmsa_raw,irmsb_raw,awatt_raw,avar_raw,ava_raw,aenergy_raw,pf_raw,period_raw\n");
#endif

    while (true) {
        int current_button_level = gpio_get_level(pins->setup_bt_button);
        if (current_button_level == 0 && last_button_level == 1) {
            relay_state = !relay_state;
            module_relay_set(relay_state);
            relay_led_set(relay_state);
        }
        last_button_level = current_button_level;

        ade7953_measurement_t m = {0};
        uint32_t vpeak = 0, iapeak = 0, ibpeak = 0;

        /* Note: clear_irq_status must be false here so we can read it without wiping the safety trip flags */
        if (module_ade7953_read_measurement(&m, true, false) == ESP_OK) {
            uint64_t t_ms = esp_timer_get_time() / 1000ULL;
            
            /* If the chip latched a critical trip, clear it manually so the line can recover */
            if ((m.raw.irq_a & (ADE7953_IRQ_A_SAG | ADE7953_IRQ_A_OV | ADE7953_IRQ_A_OIA)) != 0) {
                ESP_LOGE(TAG, "Hardware Safety Trip Triggered! IRQ_A: 0x%06" PRIX32, m.raw.irq_a);
                uint32_t clr = 0;
                (void)module_ade7953_read32(ADE7953_REG_RSTIRQSTATA_32, &clr);
            }

#if ENABLE_PEAK_HEADROOM_MONITOR
            (void)module_ade7953_read_peaks(&vpeak, &iapeak, &ibpeak, true);
            printf("%llu,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId16 ",%u,%" PRIu32 ",%" PRIu32 "\n",
                   t_ms, m.raw.vrms, m.raw.irmsa, m.raw.irmsb, m.raw.awatt, m.raw.avar, m.raw.ava, m.raw.aenergy_a_delta, m.raw.pfa, m.raw.period, vpeak, iapeak);
#else
            printf("%llu,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId16 ",%u\n",
                   t_ms, m.raw.vrms, m.raw.irmsa, m.raw.irmsb, m.raw.awatt, m.raw.avar, m.raw.ava, m.raw.aenergy_a_delta, m.raw.pfa, m.raw.period);
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#endif
}