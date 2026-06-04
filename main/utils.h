#ifndef UTILS_H
#define UTILS_H

#include "driver/gpio.h"
#include <stdbool.h>

// --- Hardware Config Defines ---
#define I2C_MASTER_SCL_IO           22      // GPIO for I2C SCL
#define I2C_MASTER_SDA_IO           21      // GPIO for I2C SDA
#define I2C_MASTER_NUM              0       // I2C port number
#define I2C_MASTER_FREQ_HZ          400000  // I2C frequency (400kHz for Fast Mode)
#define DHT22_PIN                   GPIO_NUM_4 // GPIO for DHT22 1-wire data
#define IMU_INT_PIN                 GPIO_NUM_14 // GPIO for MPU6050 hardware interrupt


// --- Base Hardware ---
esp_err_t setup_gpio(uint8_t pin_num, gpio_mode_t mode, bool pull_up, bool pull_down);
esp_err_t i2c_master_init(void);

// --- IMU & Dead Reckoning ---
void imu_init(void);
void imu_read_data(void);
void imu_calculate_dead_reckoning(void);
void imu_setup_interrupt(gpio_isr_t isr_handler, void *args);
bool imu_check_fall_detected(void);

// --- Serial / PC Communication ---
void serial_send_path_data(void);

// --- Environment Sensors (Temp, Humidity, Light) ---
void sensors_read_temp_humidity(float *temperature, float *humidity);
void sensors_read_light(int *light_level);

// --- OLED Display ---
void oled_init(void);
void oled_update_display(int light_level, float temperature, float humidity);

// --- Debug ---
void debug_sensor_init(void);

#endif /* UTILS_H */