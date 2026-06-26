#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "utils.h"
#include "esp_timer.h"

static const char *TAG = "mainloop";
#define LED_PIN GPIO_NUM_15
#define BUTTON_PIN GPIO_NUM_12

void setup(void) {
    ESP_LOGI(TAG, "--- Super-loop Setup ---");
    setup_gpio(LED_PIN, GPIO_MODE_OUTPUT, false, false);
    setup_gpio(BUTTON_PIN, GPIO_MODE_INPUT, false, true); // No pull-up, enable pull-down
    
    // This debug call initializes our I2C and DHT11 pins and probes the bus
    debug_sensor_init();
    
    imu_init();
    oled_init();
    
    ESP_LOGI(TAG, "System Ready");
}

void loop(void) {
    static int64_t led_on_time = 0;
    static bool last_btn_state = false;

    // Reset IMU data if button is pressed (Active High + Edge Detection)
    bool current_btn = (gpio_get_level(BUTTON_PIN) == 1);
    if (current_btn && !last_btn_state) {
        imu_reset_data();
        ESP_LOGI(TAG, "Virtual IMU Reset via Button!");
    }
    last_btn_state = current_btn;

    // --- 1. IMU and Dead Reckoning ---
    // FLAW: This needs to run consistently (e.g., exactly every 10ms) for math integration.
    // But in this super-loop, it gets delayed by all the other slow functions below.
    if (imu_read_data()) {
        imu_calculate_dead_reckoning();
        if (imu_check_fall_detected()) {
            ESP_LOGW(TAG, "FALL DETECTED!");
            gpio_set_level(LED_PIN, 1);
            led_on_time = esp_timer_get_time();
        }
    } else {
        ESP_LOGE(TAG, "CRITICAL: IMU Read Failed! (I2C Bus locked or IMU crashed)");
    }
    
    // Check if 1 second has passed since the LED was turned on
    if (led_on_time > 0 && (esp_timer_get_time() - led_on_time) > 1000000) {
        gpio_set_level(LED_PIN, 0);
        led_on_time = 0;
    }

    // --- 2. Environment Sensors ---
    // FLAW: DHT11 1-wire protocol takes over 25+ milliseconds to complete.
    // During this time, the CPU is blocked and we miss IMU data.
    float temp, hum;
    int light;
    sensors_read_temp_humidity(&temp, &hum);
    // ESP_LOGI(TAG, "Temp: %.1f C, Humidity: %.1f%%", temp, hum); // Commented out to prevent terminal trashing
    sensors_read_light(&light);

    // --- 3. Update Display ---
    // FLAW: I2C screen writing takes time and delays the loop further.
    oled_update_display(light, temp, hum);

    // --- 4. Serial Data Transmission ---
    // FLAW: printf blocks until the UART buffer is flushed!
    serial_send_path_data();
}

void app_main(void) {
    // --- FLASHING FIX ---
    // Wait 2 seconds before starting. This keeps the USB port perfectly silent on boot,
    // allowing esptool.py to cleanly software-reset the board without you needing to press any buttons!
    vTaskDelay(pdMS_TO_TICKS(2000));

    setup();
    while (1) {
        loop();
        
        // Minimal delay to prevent task watchdog trigger, but our loop 
        // frequency is completely unpredictable due to the blocking functions.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}