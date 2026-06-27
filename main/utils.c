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
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include <math.h>

#define IMU_ADDR                    0x68    // Typical I2C address for MPU6050

// State variables for IMU and dead reckoning
static float ax = 0, ay = 0, az = 0;
static float gx = 0, gy = 0, gz = 0;
static float vx = 0, vy = 0, vz = 0;
static float px = 0, py = 0, pz = 0;

// --- IMU Calibration Offsets ---
// Replace these with the numbers printed by the calibration sequence!
static float ax_offset = 0.041f;
static float ay_offset = -0.015f;
static float az_offset = -0.281f;

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

static void i2c_bus_reset(void) {
    // I2C Bus Recovery Routine (Frees stuck slave devices)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2C_MASTER_SDA_IO) | (1ULL << I2C_MASTER_SCL_IO),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    gpio_set_level(I2C_MASTER_SDA_IO, 1);
    gpio_set_level(I2C_MASTER_SCL_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    for (int i = 0; i < 9; i++) {
        gpio_set_level(I2C_MASTER_SCL_IO, 0);
        ets_delay_us(5000);
        gpio_set_level(I2C_MASTER_SCL_IO, 1);
        ets_delay_us(5000);
    }
    
    // Send STOP condition
    gpio_set_level(I2C_MASTER_SCL_IO, 0);
    ets_delay_us(5000);
    gpio_set_level(I2C_MASTER_SDA_IO, 0);
    ets_delay_us(5000);
    gpio_set_level(I2C_MASTER_SCL_IO, 1);
    ets_delay_us(5000);
    gpio_set_level(I2C_MASTER_SDA_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

esp_err_t i2c_master_init(void) {
    // 1. Force release of the I2C bus in case a sensor is stuck holding SDA low
    i2c_bus_reset();
    
    // 3. Initialize fresh
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
        .clk_flags = 0, // Ensure default clock source in IDF v5
    };
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return err;
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

// --- I2C Probe ---
bool i2c_probe(uint8_t addr) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);
    return (err == ESP_OK);
}

// --- I2C Scanner ---
void i2c_scanner(void) {
    ESP_LOGI("I2C_SCANNER", "Starting I2C bus scan...");
    int devices_found = 0;
    for (uint8_t i = 1; i < 127; i++) {
        if (i2c_probe(i)) {
            ESP_LOGI("I2C_SCANNER", "---> Device found at address: 0x%02X <---", i);
            devices_found++;
        }
    }
    if (devices_found == 0) {
        ESP_LOGW("I2C_SCANNER", "No I2C devices found on the bus! Check wiring, power, and pull-up resistors.");
    } else {
        ESP_LOGI("I2C_SCANNER", "Scan completed. Found %d devices.", devices_found);
    }
}

// --- IMU & Dead Reckoning ---
void imu_init(void) {
    // Initialize MPU6050 and configure internal Free Fall interrupt
    uint8_t init_cmds[][2] = {
        {0x6B, 0x00}, // Power management (wake up)
        {0x1D, 0x20}, // Free-fall threshold register (Set to 0x20 as a balanced middle ground)
        {0x1E, 0x0A}, // Free-fall duration register (10ms)
        {0x38, 0x80}  // Interrupt enable register (Bit 7 = Free-fall / Motion INT)
    };
    for (int i = 0; i < 4; i++) {
        i2c_master_write_to_device(I2C_MASTER_NUM, IMU_ADDR, init_cmds[i], 2, pdMS_TO_TICKS(100));
    }
}

static int64_t virtual_start_time = 0;

bool imu_read_data(void) {
#if USE_VIRTUAL_IMU
    float time_sec = (esp_timer_get_time() - virtual_start_time) / 1000000.0f;
    
    // A 15-millisecond mechanical shock (10G) that happens 5 times per second (every 200ms).
    float time_in_cycle = fmodf(time_sec, 0.2f);
    if (time_in_cycle < 0.015f) { 
        ax = 10.0f; 
    } else {
        ax = 0.0f;
    }
    ay = 0.0f;
    az = 1.0f; // Standard 1G gravity
    gx = 0; gy = 0; gz = 0;
    return true;
#else
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

        return true;
    }
    return false;
#endif
}

static float last_dt_ms = 0.0f;

void imu_calculate_dead_reckoning(void) {
    static int64_t last_time = 0;
    int64_t now = esp_timer_get_time();
    if (last_time > 0) {
        last_dt_ms = (now - last_time) / 1000.0f;
    }
    last_time = now;

    float dt = 0.01f; // Assumes exactly 100Hz loop (10ms) for mathematical integration
    
    // Apply calibration offsets
    float ax_cal = ax - ax_offset;
    float ay_cal = ay - ay_offset;
    float az_linear = az - 1.0f - az_offset; // Remove gravity and offset

    // Apply a small deadzone to prevent micro-noise from drifting the velocity
    if (ax_cal > -0.04f && ax_cal < 0.04f) ax_cal = 0;
    if (ay_cal > -0.04f && ay_cal < 0.04f) ay_cal = 0;
    if (az_linear > -0.06f && az_linear < 0.06f) az_linear = 0;

    // Convert Gs to m/s^2, integrate to get velocity (m/s)
    vx += ax_cal * dt * 9.81f;
    vy += ay_cal * dt * 9.81f;
    vz += az_linear * dt * 9.81f;

    // Calculate how much real time has passed to scale the damping correctly
    float real_dt_sec = last_dt_ms / 1000.0f;
    if (real_dt_sec <= 0.0f) real_dt_sec = 0.01f;
    float time_scale = real_dt_sec / 0.01f; // Ratio of real time compared to the math dt

    // Apply Velocity Damping (Leaky Integrator) scaled to real time
    float vel_damping = powf(0.95f, time_scale);
    vx *= vel_damping;
    vy *= vel_damping;
    vz *= vel_damping;

    // Integrate velocity to get position (meters)
    px += vx * dt;
    py += vy * dt;
    pz += vz * dt;

    // Apply Position Damping scaled to real time
    float pos_damping = powf(0.98f, time_scale);
    px *= pos_damping;
    py *= pos_damping;
    pz *= pos_damping;
}

void imu_reset_data(void) {
    vx = 0; vy = 0; vz = 0;
    px = 0; py = 0; pz = 0;
    virtual_start_time = esp_timer_get_time();
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

void imu_clear_interrupt(void) {
    // The MPU6050 requires reading the INT_STATUS register (0x3A) to clear the interrupt flag.
    // If not cleared, the INT pin will remain high or stop firing.
    uint8_t reg = 0x3A;
    uint8_t status = 0;
    esp_err_t err = i2c_master_write_to_device(I2C_MASTER_NUM, IMU_ADDR, &reg, 1, pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        i2c_master_read_from_device(I2C_MASTER_NUM, IMU_ADDR, &status, 1, pdMS_TO_TICKS(100));
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

void imu_get_raw_data(float *ax_out, float *ay_out, float *az_out, float *gx_out, float *gy_out, float *gz_out) {
    if (ax_out) *ax_out = ax;
    if (ay_out) *ay_out = ay;
    if (az_out) *az_out = az;
    if (gx_out) *gx_out = gx;
    if (gy_out) *gy_out = gy;
    if (gz_out) *gz_out = gz;
}

// --- Serial / PC Communication ---
void serial_send_path_data(void) {
    float v_mag = vx*vx + vy*vy + vz*vz; 
    // Include actual Loop Time (Jitter) to compare Mainloop vs RTOS
    printf("PATH_DATA: %.3f,%.3f,%.3f,%.3f | LOOP_TIME: %.1f ms\n", px, py, pz, v_mag, last_dt_ms);
}

void sensors_read_temp_humidity(float *temperature, float *humidity) {
    uint8_t data[5] = {0};

    // 1. Send start signal
    gpio_set_level(DHT11_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20)); // Pull low for 20ms (DHT11 requires at least 18ms)
    gpio_set_level(DHT11_PIN, 1);
    ets_delay_us(40);   // Pull high for 40us

    // 2. Wait for response
    int timeout = 0;
    while (gpio_get_level(DHT11_PIN) == 1 && timeout++ < 100) ets_delay_us(1);
    timeout = 0;
    while (gpio_get_level(DHT11_PIN) == 0 && timeout++ < 100) ets_delay_us(1);
    timeout = 0;
    while (gpio_get_level(DHT11_PIN) == 1 && timeout++ < 100) ets_delay_us(1);

    // 3. Read 40 bits of data
    for (int i = 0; i < 40; i++) {
        timeout = 0;
        while (gpio_get_level(DHT11_PIN) == 0 && timeout++ < 100) ets_delay_us(1);
        uint32_t t = 0;
        while (gpio_get_level(DHT11_PIN) == 1 && t++ < 100) ets_delay_us(1);
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

static adc_oneshot_unit_handle_t adc1_handle = NULL;
#define PHOTO_ADC_CHANNEL ADC_CHANNEL_0 // GPIO 1 on ESP32-S2

void sensors_read_light(int *light_level) {
    if (adc1_handle == NULL) {
        adc_oneshot_unit_init_cfg_t init_config1 = {
            .unit_id = ADC_UNIT_1,
        };
        esp_err_t err = adc_oneshot_new_unit(&init_config1, &adc1_handle);
        if (err == ESP_OK) {
            adc_oneshot_chan_cfg_t config = {
                .bitwidth = ADC_BITWIDTH_DEFAULT,
                .atten = ADC_ATTEN_DB_12,
            };
            adc_oneshot_config_channel(adc1_handle, PHOTO_ADC_CHANNEL, &config);
        }
    }
    
    if (adc1_handle && light_level) {
        int raw_val = 0;
        esp_err_t err = adc_oneshot_read(adc1_handle, PHOTO_ADC_CHANNEL, &raw_val);
        if (err == ESP_OK) {
            // Scale 12-bit ADC raw value (0-4095) to match expected 0-1000 scale in test_hardware.c
            *light_level = (raw_val * 1000) / 4095;
        } else {
            *light_level = 0;
        }
    }
}

// --- OLED Display ---
void oled_init(void) {
    uint8_t oled_addr = 0x3C; // Standard SSD1306 I2C address
    uint8_t init_sequence[] = {
        0x00,       // Command byte indicator
        0xAE,       // Display OFF
        0xD5, 0x80, // Set display clock divide ratio
        0xA8, 0x3F, // Set multiplex ratio (64 lines)
        0xD3, 0x00, // Set display offset
        0x40,       // Set start line
        0x8D, 0x14, // ENABLE CHARGE PUMP (CRITICAL: Screen will not turn on without this!)
        0x20, 0x00, // Memory addressing mode (Horizontal)
        0xA1,       // Segment remap
        0xC8,       // COM output scan direction
        0xDA, 0x12, // Set COM pins hardware configuration
        0x81, 0x01, // Contrast (Set to minimum 0x01 to prevent power brownouts!)
        0xD9, 0xF1, // Set pre-charge period
        0xDB, 0x40, // Set Vcomh deselect level
        0xA4,       // Output follows RAM content
        0xA6,       // Normal display
        0xAF        // Display ON
    };
    
    // Send the initialization commands to the OLED
    i2c_master_write_to_device(I2C_MASTER_NUM, oled_addr, init_sequence, sizeof(init_sequence), pdMS_TO_TICKS(100));
    
    // Clear the screen to remove any random junk (static) in the GRAM from startup
    oled_fill_screen(0x00);
}

// Minimal 5x8 ASCII font (space to 'z')
static const uint8_t font5x8[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5f, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7f, 0x14, 0x7f, 0x14},
    {0x24, 0x2a, 0x7f, 0x2a, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1c, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1c, 0x00}, {0x14, 0x08, 0x3e, 0x08, 0x14}, {0x08, 0x08, 0x3e, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31},
    {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1e}, {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3e}, {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36}, {0x3e, 0x41, 0x41, 0x41, 0x22},
    {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01}, {0x3e, 0x41, 0x49, 0x49, 0x7a},
    {0x7f, 0x08, 0x08, 0x08, 0x7f}, {0x00, 0x41, 0x7f, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41},
    {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f}, {0x3e, 0x41, 0x41, 0x41, 0x3e},
    {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7f, 0x01, 0x01}, {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7f, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7f, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78}, {0x7f, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7f}, {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7e, 0x09, 0x01, 0x02}, {0x18, 0xa4, 0xa4, 0xa4, 0x7c},
    {0x7f, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7d, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3d, 0x00}, {0x7f, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7f, 0x40, 0x00}, {0x7c, 0x04, 0x18, 0x04, 0x78}, {0x7c, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0xfc, 0x24, 0x24, 0x24, 0x18}, {0x18, 0x24, 0x24, 0x18, 0xfc}, {0x7c, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3f, 0x44, 0x40, 0x20}, {0x3c, 0x40, 0x40, 0x20, 0x7c}, {0x1c, 0x20, 0x40, 0x20, 0x1c}, {0x3c, 0x40, 0x30, 0x40, 0x3c},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x1c, 0xa0, 0xa0, 0xa0, 0x7c}, {0x44, 0x64, 0x54, 0x4c, 0x44}
};

static void oled_draw_text(uint8_t page, uint8_t col, const char *text) {
    uint8_t oled_addr = 0x3C;
    uint8_t cmds[] = {
        0x00,                 // Command indicator
        0x21, col, 127,       // Set column address
        0x22, page, page      // Set page address (single page)
    };
    i2c_master_write_to_device(I2C_MASTER_NUM, oled_addr, cmds, sizeof(cmds), pdMS_TO_TICKS(50));
    
    // Max 128 pixels wide (approx 21 chars at 6 pixels each)
    uint8_t data_buf[129];
    data_buf[0] = 0x40; // Data mode indicator
    int buf_idx = 1;

    while (*text && buf_idx < 128) {
        char c = *text;
        if (c < ' ' || c > 'z') c = ' '; // Invalid char
        
        int font_idx = c - ' ';
        for (int i = 0; i < 5 && buf_idx < 128; i++) {
            data_buf[buf_idx++] = font5x8[font_idx][i];
        }
        // Spacing between chars
        if (buf_idx < 128) {
            data_buf[buf_idx++] = 0x00; 
        }
        text++;
    }
    
    // Fill the rest of the line with blank space to clear old text
    while (buf_idx <= 128) {
        data_buf[buf_idx++] = 0x00;
    }

    i2c_master_write_to_device(I2C_MASTER_NUM, oled_addr, data_buf, buf_idx, pdMS_TO_TICKS(50));
}

void oled_update_display(int light_level, float temperature, float humidity) {
    char buf[32];
    
    sprintf(buf, "Temp:  %.1f C", temperature);
    oled_draw_text(0, 0, buf);

    sprintf(buf, "Hum:   %.1f %%", humidity);
    oled_draw_text(2, 0, buf);
    
    // Clear line 4 (Light) so it doesn't leave remnants
    oled_draw_text(4, 0, "               ");

    ESP_LOGD("OLED", "Display Updated -> L: %d lx, T: %.1f C, H: %.1f %%", light_level, temperature, humidity);
}

void oled_fill_screen(uint8_t pattern) {
    uint8_t oled_addr = 0x3C;
    uint8_t cmds[] = {
        0x00,         // Command indicator
        0x21, 0, 127, // Reset column address range
        0x22, 0, 7    // Reset page address range
    };
    i2c_master_write_to_device(I2C_MASTER_NUM, oled_addr, cmds, sizeof(cmds), pdMS_TO_TICKS(50));
    
    uint8_t data_buf[129];
    data_buf[0] = 0x40; // Data mode indicator
    for (int i = 1; i <= 128; i++) {
        data_buf[i] = pattern;
    }
    
    for (int p = 0; p < 8; p++) {
        i2c_master_write_to_device(I2C_MASTER_NUM, oled_addr, data_buf, sizeof(data_buf), pdMS_TO_TICKS(50));
    }
}

void oled_draw_bar(uint8_t percent) {
    if (percent > 100) percent = 100;
    
    uint8_t oled_addr = 0x3C;
    uint8_t cmds[] = {
        0x00,         // Command indicator
        0x21, 0, 127, // Reset column address range
        0x22, 0, 7    // Reset page address range
    };
    i2c_master_write_to_device(I2C_MASTER_NUM, oled_addr, cmds, sizeof(cmds), pdMS_TO_TICKS(100));

    uint8_t data_buf[129];
    data_buf[0] = 0x40; // Data mode
    int fill_limit = (percent * 128) / 100;

    for (int p = 0; p < 8; p++) {
        // Draw bar on pages 3 and 4 (middle rows of OLED)
        if (p == 3 || p == 4) {
            for (int col = 1; col <= 128; col++) {
                data_buf[col] = (col - 1 < fill_limit) ? 0xFF : 0x00;
            }
        } else {
            for (int col = 1; col <= 128; col++) {
                data_buf[col] = 0x00;
            }
        }
        i2c_master_write_to_device(I2C_MASTER_NUM, oled_addr, data_buf, sizeof(data_buf), pdMS_TO_TICKS(100));
    }
}

// --- Debug ---
void debug_sensor_init(void) {
    const char *TAG = "SENSOR_DEBUG";
    ESP_LOGI(TAG, "Starting sensor initialization and debug probe...");
    
    // Give sensors time to power up (POR delay) before scanning
    vTaskDelay(pdMS_TO_TICKS(150));
    
    // 1. Init I2C Bus
    esp_err_t err = i2c_master_init();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "[OK] I2C Master initialized successfully.");
    } else {
        ESP_LOGE(TAG, "[FAIL] I2C Master init failed: %s", esp_err_to_name(err));
    }

    // Run a full bus scan to see what is physically responding
    i2c_scanner();

    // 2. Init DHT11 GPIO
    err = setup_gpio(DHT11_PIN, GPIO_MODE_INPUT_OUTPUT_OD, true, false);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[OK] DHT11 GPIO initialized successfully.");
    } else {
        ESP_LOGE(TAG, "[FAIL] DHT11 GPIO init failed: %s", esp_err_to_name(err));
    }

    // 3. Probe I2C Devices
    if (i2c_probe(IMU_ADDR)) ESP_LOGI(TAG, "[OK] IMU (MPU6050) detected at I2C address 0x%02X.", IMU_ADDR);
    else ESP_LOGW(TAG, "[FAIL] IMU (MPU6050) NOT detected at I2C address 0x%02X!", IMU_ADDR);

    // Read photoresistor (GPIO 34 / ADC1 Channel 6) to verify functionality
    int photo_val = 0;
    sensors_read_light(&photo_val);
    ESP_LOGI(TAG, "[INFO] Photoresistor detected. Debug read value: %d (0-1000 scale).", photo_val);

    uint8_t oled_addr = 0x3C; // Default OLED I2C address
    if (i2c_probe(oled_addr)) ESP_LOGI(TAG, "[OK] OLED Display detected at I2C address 0x%02X.", oled_addr);
    else ESP_LOGW(TAG, "[FAIL] OLED Display NOT detected at I2C address 0x%02X!", oled_addr);

    ESP_LOGI(TAG, "Sensor debug sequence completed.");
}