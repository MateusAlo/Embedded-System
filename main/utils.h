#ifndef UTILS_H
#define UTILS_H

#include "driver/gpio.h"
#include <stdbool.h>

// Set this to 1 to use a simulated 5Hz Sine Wave IMU for presentation/debugging.
// Set to 0 to use the real hardware I2C MPU6050.
#define USE_VIRTUAL_IMU 1

// --- Hardware Config Defines ---
#define I2C_MASTER_SCL_IO           9       // GPIO for I2C SCL (Changed from 35)
#define I2C_MASTER_SDA_IO           8       // GPIO for I2C SDA (Changed from 33)
#define I2C_MASTER_NUM              0       // I2C port number
#define I2C_MASTER_FREQ_HZ          100000  // I2C frequency (100kHz for Standard Mode)
#define DHT11_PIN                   GPIO_NUM_2 // GPIO for DHT11 1-wire data (S2 Mini)
#define IMU_INT_PIN                 GPIO_NUM_14 // GPIO for MPU6050 hardware interrupt


// --- Base Hardware ---
esp_err_t setup_gpio(uint8_t pin_num, gpio_mode_t mode, bool pull_up, bool pull_down);
esp_err_t i2c_master_init(void);
void i2c_scanner(void);

// --- IMU & Dead Reckoning ---
void imu_init(void);
bool imu_read_data(void);
void imu_calculate_dead_reckoning(void);
void imu_reset_data(void);
void imu_setup_interrupt(gpio_isr_t isr_handler, void *args);
void imu_clear_interrupt(void);
bool imu_check_fall_detected(void);
void imu_get_raw_data(float *ax_out, float *ay_out, float *az_out, float *gx_out, float *gy_out, float *gz_out);

// --- Serial / PC Communication ---
void serial_send_path_data(void);

// --- Environment Sensors (Temp, Humidity, Light) ---
void sensors_read_temp_humidity(float *temperature, float *humidity);
void sensors_read_light(int *light_level);

// --- OLED Display ---
void oled_init(void);
void oled_update_display(int light_level, float temperature, float humidity);
void oled_fill_screen(uint8_t pattern);
void oled_draw_bar(uint8_t percent);

// --- Debug ---
void debug_sensor_init(void);

#endif /* UTILS_H */