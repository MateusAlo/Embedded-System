# include "utils.h"

#include <stdio.h>
#include <stdarg.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include "esp_log.h"
#include "esp_err.h"

#define IMU_ADDR                    0x68    // Typical I2C address for MPU6050
#define LIGHT_SENSOR_ADDR           0x23    // Typical I2C address for BH1750

// State variables for IMU and dead reckoning
static float ax = 0, ay = 0, az = 0;
static float gx = 0, gy = 0, gz = 0;
static float vx = 0, vy = 0, vz = 0;
static float px = 0, py = 0, pz = 0;

// Your universal, easy-to-use configuration function
esp_err_t setup_gpio(uint8_t pin_num, gpio_mode_t mode, bool pull_up, bool pull_down) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin_num),          // Convert pin number to bit mask automatically
        .mode = mode,                               // Set INPUT, OUTPUT, etc.
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE              // Keep interrupts off by default for simplicity
    };
    
    return gpio_config(&io_conf); // Apply settings and return success/error code
}

esp_err_t i2c_master_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return err;
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

// --- IMU & Dead Reckoning ---
void imu_init(void) {
    // Initialize MPU6050 and configure internal Free Fall interrupt
    uint8_t init_cmds[][2] = {
        {0x6B, 0x00}, // Power management (wake up)
        {0x1D, 0x08}, // Free-fall threshold register
        {0x1E, 0x10}, // Free-fall duration register
        {0x38, 0x80}  // Interrupt enable register (Bit 7 = Free-fall / Motion INT)
    };
    for (int i = 0; i < 4; i++) {
        i2c_master_write_to_device(I2C_MASTER_NUM, IMU_ADDR, init_cmds[i], 2, pdMS_TO_TICKS(100));
    }
}

void imu_read_data(void) {
    // Implementation for reading MPU6050 accelerometer and gyroscope data via I2C
    uint8_t reg = 0x3B; // ACCEL_XOUT_H register
    uint8_t data[14] = {0};   // 14 bytes for 3 Accel, 1 Temp, 3 Gyro (2 bytes each)
    
    esp_err_t err = i2c_master_write_to_device(I2C_MASTER_NUM, IMU_ADDR, &reg, 1, pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        i2c_master_read_from_device(I2C_MASTER_NUM, IMU_ADDR, data, 14, pdMS_TO_TICKS(100));
        
        // Combine raw data
        int16_t raw_ax = (int16_t)((data[0] << 8) | data[1]);
        int16_t raw_ay = (int16_t)((data[2] << 8) | data[3]);
        int16_t raw_az = (int16_t)((data[4] << 8) | data[5]);
        int16_t raw_gx = (int16_t)((data[8] << 8) | data[9]);
        int16_t raw_gy = (int16_t)((data[10] << 8) | data[11]);
        int16_t raw_gz = (int16_t)((data[12] << 8) | data[13]);

        // Convert to G's and deg/s (assuming default full scale +/- 2g and +/- 250 deg/s)
        ax = raw_ax / 16384.0f;
        ay = raw_ay / 16384.0f;
        az = raw_az / 16384.0f;
        gx = raw_gx / 131.0f;
        gy = raw_gy / 131.0f;
        gz = raw_gz / 131.0f;
    }
}

void imu_calculate_dead_reckoning(void) {
    float dt = 0.01f; // Assumes 100Hz loop (10ms)
    
    // Remove 1G from Z axis (assuming device is perfectly flat for this simple math)
    float az_linear = az - 1.0f;

    // Convert Gs to m/s^2, integrate to get velocity (m/s)
    vx += ax * dt * 9.81f;
    vy += ay * dt * 9.81f;
    vz += az_linear * dt * 9.81f;

    // Integrate velocity to get position (meters)
    px += vx * dt;
    py += vy * dt;
    pz += vz * dt;
}

void imu_setup_interrupt(gpio_isr_t isr_handler, void *args) {
    // Configure the ESP32 pin to trigger on the rising edge of the MPU6050 INT pin
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << IMU_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE // MPU6050 INT triggers high by default
    };
    gpio_config(&io_conf);
    
    // Install ISR service (ignoring invalid state if already installed)
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        gpio_isr_handler_add(IMU_INT_PIN, isr_handler, args);
    }
}

bool imu_check_fall_detected(void) {
    // Simple free-fall detection for the polling Super-loop (mainloop.c)
    // If the total acceleration vector falls below ~0.4G, it's likely dropping.
    float acc_mag_sq = ax*ax + ay*ay + az*az;
    if (acc_mag_sq > 0.01f && acc_mag_sq < 0.16f) { // 0.16 is 0.4^2
        return true;
    }
    return false;
}

// --- Serial / PC Communication ---
void serial_send_path_data(void) {
    // Format: "PATH_DATA: X, Y, Z, Velocity" (Easy for a Python script to parse)
    float v_mag = vx*vx + vy*vy + vz*vz; // Sending squared magnitude to avoid math.h overhead
    printf("PATH_DATA: %.3f,%.3f,%.3f,%.3f\n", px, py, pz, v_mag);
}

void sensors_read_temp_humidity(float *temperature, float *humidity) {
    uint8_t data[5] = {0};

    // 1. Send start signal
    gpio_set_level(DHT22_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20)); // Pull low for 20ms (DHT11 requires at least 18ms)
    gpio_set_level(DHT22_PIN, 1);
    ets_delay_us(40);   // Pull high for 40us

    // 2. Wait for response
    int timeout = 0;
    while (gpio_get_level(DHT22_PIN) == 1 && timeout++ < 100) ets_delay_us(1);
    timeout = 0;
    while (gpio_get_level(DHT22_PIN) == 0 && timeout++ < 100) ets_delay_us(1);
    timeout = 0;
    while (gpio_get_level(DHT22_PIN) == 1 && timeout++ < 100) ets_delay_us(1);

    // 3. Read 40 bits of data
    for (int i = 0; i < 40; i++) {
        timeout = 0;
        while (gpio_get_level(DHT22_PIN) == 0 && timeout++ < 100) ets_delay_us(1);
        uint32_t t = 0;
        while (gpio_get_level(DHT22_PIN) == 1 && t++ < 100) ets_delay_us(1);
        if (t > 40) {
            data[i / 8] |= (1 << (7 - (i % 8)));
        }
    }

    // 4. Verify Checksum
    if (data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        // DHT11 format: Byte 0 = Hum Integral, Byte 1 = Hum Decimal, Byte 2 = Temp Integral, Byte 3 = Temp Decimal
        if (humidity) *humidity = (float)data[0] + ((float)data[1] / 10.0f);
        if (temperature) *temperature = (float)data[2] + ((float)data[3] / 10.0f);
    }
}

void sensors_read_light(int *light_level) {
    // Implementation for I2C Light Sensor BH1750
    uint8_t cmd = 0x20;  // One-time High-Res Mode
    uint8_t data[2] = {0};
    
    i2c_master_write_to_device(I2C_MASTER_NUM, LIGHT_SENSOR_ADDR, &cmd, 1, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(120)); // Wait for measurement to complete (BH1750 needs ~120ms)
    
    esp_err_t err = i2c_master_read_from_device(I2C_MASTER_NUM, LIGHT_SENSOR_ADDR, data, 2, pdMS_TO_TICKS(100));
    if (err == ESP_OK && light_level) {
        *light_level = ((data[0] << 8) | data[1]) / 1.2f;
    }
}

// --- OLED Display ---
void oled_init(void) {
    uint8_t oled_addr = 0x3C; // Standard SSD1306 I2C address
    uint8_t init_sequence[] = {
        0x00,       // Command byte indicator
        0xAE,       // Display OFF
        0x20, 0x00, // Memory addressing mode (Horizontal)
        0x21, 0x00, 0x7F, // Column address (0-127)
        0x22, 0x00, 0x07, // Page address (0-7)
        0x81, 0x7F, // Contrast
        0xA1,       // Segment remap
        0xC8,       // COM output scan direction
        0xA6,       // Normal display
        0xAF        // Display ON
    };
    
    // Send the initialization commands to the OLED
    i2c_master_write_to_device(I2C_MASTER_NUM, oled_addr, init_sequence, sizeof(init_sequence), pdMS_TO_TICKS(100));
}

void oled_update_display(int light_level, float temperature, float humidity) {
    // In a real application, you would generate a bitmap array using a font library.
    // For now, we reset the addressing pointers and log the data internally.
    uint8_t oled_addr = 0x3C;
    uint8_t cmds[] = {
        0x00,         // Command byte indicator
        0x21, 0, 127, // Reset column address
        0x22, 0, 7    // Reset page address
    };
    
    i2c_master_write_to_device(I2C_MASTER_NUM, oled_addr, cmds, sizeof(cmds), pdMS_TO_TICKS(50));
    
    ESP_LOGD("OLED", "Simulated Display Update -> L: %d lx, T: %.1f C, H: %.1f %%", light_level, temperature, humidity);
}

// --- Debug ---
void debug_sensor_init(void) {
    const char *TAG = "SENSOR_DEBUG";
    ESP_LOGI(TAG, "Starting sensor initialization and debug probe...");
    
    // 1. Init I2C Bus
    esp_err_t err = i2c_master_init();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "[OK] I2C Master initialized successfully.");
    } else {
        ESP_LOGE(TAG, "[FAIL] I2C Master init failed: %s", esp_err_to_name(err));
    }

    // 2. Init DHT22 GPIO
    err = setup_gpio(DHT22_PIN, GPIO_MODE_INPUT_OUTPUT_OD, true, false);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[OK] DHT22 GPIO initialized successfully.");
    } else {
        ESP_LOGE(TAG, "[FAIL] DHT22 GPIO init failed: %s", esp_err_to_name(err));
    }

    // 3. Probe I2C Devices
    uint8_t dummy_data;
    
    err = i2c_master_write_to_device(I2C_MASTER_NUM, IMU_ADDR, &dummy_data, 0, pdMS_TO_TICKS(100));
    if (err == ESP_OK) ESP_LOGI(TAG, "[OK] IMU (MPU6050) detected at I2C address 0x%02X.", IMU_ADDR);
    else ESP_LOGW(TAG, "[FAIL] IMU (MPU6050) NOT detected at I2C address 0x%02X!", IMU_ADDR);

    err = i2c_master_write_to_device(I2C_MASTER_NUM, LIGHT_SENSOR_ADDR, &dummy_data, 0, pdMS_TO_TICKS(100));
    if (err == ESP_OK) ESP_LOGI(TAG, "[OK] Light Sensor (BH1750) detected at I2C address 0x%02X.", LIGHT_SENSOR_ADDR);
    else ESP_LOGW(TAG, "[FAIL] Light Sensor (BH1750) NOT detected at I2C address 0x%02X!", LIGHT_SENSOR_ADDR);

    uint8_t oled_addr = 0x3C; // Default OLED I2C address
    err = i2c_master_write_to_device(I2C_MASTER_NUM, oled_addr, &dummy_data, 0, pdMS_TO_TICKS(100));
    if (err == ESP_OK) ESP_LOGI(TAG, "[OK] OLED Display detected at I2C address 0x%02X.", oled_addr);
    else ESP_LOGW(TAG, "[FAIL] OLED Display NOT detected at I2C address 0x%02X!", oled_addr);

    ESP_LOGI(TAG, "Sensor debug sequence completed.");
}