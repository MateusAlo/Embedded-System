#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "utils.h"

#define LED_PIN GPIO_NUM_15
#define BUTTON_PIN GPIO_NUM_12

static const char *TAG = "TEST_HW";

void app_main(void) {
    ESP_LOGI(TAG, "Starting Hardware Test Program");

    // 1. Setup basic GPIOs (LED output, Button input)
    setup_gpio(LED_PIN, GPIO_MODE_OUTPUT, false, false);
    setup_gpio(BUTTON_PIN, GPIO_MODE_INPUT, true, false);

    // 2. Initialize and probe sensors using utility functions
    debug_sensor_init();
    imu_init();
    oled_init();

    // 3. Test OLED Screen (Rule out bad connection by lighting up screen)
    ESP_LOGI(TAG, "OLED Test: Filling screen with SOLID WHITE (0xFF)...");
    oled_fill_screen(0xFF);
    vTaskDelay(pdMS_TO_TICKS(1500));

    ESP_LOGI(TAG, "OLED Test: Filling screen with STRIPES (0x55)...");
    oled_fill_screen(0x55);
    vTaskDelay(pdMS_TO_TICKS(1500));

    ESP_LOGI(TAG, "OLED Test: Clearing screen (0x00)...");
    oled_fill_screen(0x00);
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "--- Starting IMU Calibration ---");
    ESP_LOGI(TAG, "Keep the breadboard perfectly still and flat for 2 seconds!");
    float sum_x = 0, sum_y = 0, sum_z = 0;
    int samples = 100;
    
    // Discard first 10 readings
    for (int i = 0; i < 10; i++) { 
        imu_read_data(); 
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
    
    int valid_samples = 0;
    for (int i = 0; i < samples; i++) {
        if (imu_read_data()) {
            float ax, ay, az, gx, gy, gz;
            imu_get_raw_data(&ax, &ay, &az, &gx, &gy, &gz);
            sum_x += ax;
            sum_y += ay;
            sum_z += az;
            valid_samples++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    if (valid_samples > 0) {
        float off_x = sum_x / valid_samples;
        float off_y = sum_y / valid_samples;
        float off_z = (sum_z / valid_samples) - 1.0f; // Subtract 1.0G
        
        ESP_LOGI(TAG, "================================================");
        ESP_LOGI(TAG, "CALIBRATION COMPLETE!");
        ESP_LOGI(TAG, "Copy these 3 numbers into the top of utils.c:");
        ESP_LOGI(TAG, "ax_offset = %.3f", off_x);
        ESP_LOGI(TAG, "ay_offset = %.3f", off_y);
        ESP_LOGI(TAG, "az_offset = %.3f", off_z);
        ESP_LOGI(TAG, "================================================");
    } else {
        ESP_LOGE(TAG, "Calibration Failed! IMU did not respond.");
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    bool led_state = false;

    while (1) {
        // Toggle status LED
        led_state = !led_state;
        gpio_set_level(LED_PIN, led_state);

        // Read Button
        int btn = gpio_get_level(BUTTON_PIN);

        // Read Light Sensor
        int light = 0;
        sensors_read_light(&light);

        // Read IMU raw values (ax, ay, az, gx, gy, gz)
        float ax = 0.0f, ay = 0.0f, az = 0.0f;
        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
        bool imu_ok = imu_read_data();
        if (imu_ok) {
            imu_get_raw_data(&ax, &ay, &az, &gx, &gy, &gz);
        }

        // Print results to serial console
        ESP_LOGI(TAG, "--- Sensor Readings ---");
        ESP_LOGI(TAG, "Button: %s | Light: %d lx", btn ? "RELEASED" : "PRESSED", light);
        
        if (imu_ok) {
            ESP_LOGI(TAG, "IMU Accel: X=%.3f G, Y=%.3f G, Z=%.3f G", ax, ay, az);
            ESP_LOGI(TAG, "IMU Gyro:  X=%.1f d/s, Y=%.1f d/s, Z=%.1f d/s", gx, gy, gz);
        } else {
            ESP_LOGW(TAG, "IMU: Read Failed (Check wiring / I2C connection)");
        }

        // OLED Live Update: Draw a live horizontal bar representing Light Sensor (0 to 1000 lx -> 0% to 100%)
        int percent = (light * 100) / 1000;
        if (percent > 100) percent = 100;
        if (percent < 0) percent = 0;

        oled_draw_bar(percent);

        // Non-blocking fast loop (250ms interval)
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}