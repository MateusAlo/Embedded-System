#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "utils.h"

static const char *TAG = "mainloop";
#define LED_PIN GPIO_NUM_13
#define BUTTON_PIN GPIO_NUM_26

void setup(void) {
    ESP_LOGI(TAG, "--- Super-loop Setup ---");
    setup_gpio(LED_PIN, GPIO_MODE_OUTPUT, false, false);
    setup_gpio(BUTTON_PIN, GPIO_MODE_INPUT, true, false);
    
    // This debug call initializes our I2C and DHT22 pins and probes the bus
    debug_sensor_init();
    
    //imu_init();
    //oled_init();
    
    ESP_LOGI(TAG, "System Ready");
}

void loop(void) {
    // --- 1. IMU and Dead Reckoning ---
    // FLAW: This needs to run consistently (e.g., exactly every 10ms) for math integration.
    // But in this super-loop, it gets delayed by all the other slow functions below.
    //imu_read_data();
    //imu_calculate_dead_reckoning();
    if (imu_check_fall_detected()) {
        ESP_LOGW(TAG, "FALL DETECTED!");
    }

    // --- 2. Environment Sensors ---
    // FLAW: DHT22 1-wire protocol takes over 25+ milliseconds to complete.
    // During this time, the CPU is blocked and we miss IMU data.
    float temp, hum;
    int light;
    sensors_read_temp_humidity(&temp, &hum);
    ESP_LOGI(TAG, "Temp: %.1f C, Humidity: %.1f%%", temp, hum);
    //sensors_read_light(&light);

    // --- 3. Update Display ---
    // FLAW: I2C screen writing takes time and delays the loop further.
    //oled_update_display(light, temp, hum);

    // --- 4. Memory Storage ---
    // FLAW: Flash memory (SPIFFS/NVS) writes have huge latency spikes.
    //memory_save_path_data();
}

void app_main(void) {
    setup();
    while (1) {
        loop();
        
        // Minimal delay to prevent task watchdog trigger, but our loop 
        // frequency is completely unpredictable due to the blocking functions.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}