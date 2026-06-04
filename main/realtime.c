#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "utils.h"

static const char *TAG = "REALTIME";

// Shared data structure and its Mutex for thread-safe access
typedef struct {
    float temp;
    float hum;
    int light;
} sensor_data_t;

static sensor_data_t shared_env_data = {0};
static SemaphoreHandle_t env_data_mutex;

// Hardware Interrupt Semaphore
static SemaphoreHandle_t fall_semaphore;

// --- Interrupt Service Routine (ISR) ---
// This runs in hardware space. No blocking functions (like printf or vTaskDelay) are allowed here!
static void IRAM_ATTR imu_isr_handler(void* arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(fall_semaphore, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR(); // Instantly jump to the high-priority alarm task
    }
}

// --- Emergency High Priority Task ---
void fall_detection_task(void *pvParameters) {
    while (1) {
        // Block indefinitely until the hardware interrupt gives the semaphore
        if (xSemaphoreTake(fall_semaphore, portMAX_DELAY) == pdTRUE) {
            ESP_LOGE(TAG, "EMERGENCY: FALL DETECTED VIA HARDWARE INTERRUPT!");
            // Trigger alarms, buzzers, send Wi-Fi SOS, etc.
        }
    }
}

// --- High Priority: IMU Task ---
// Priority 5 (Highest). Dead reckoning requires exact timing (dt) for velocity/position integration.
// Using vTaskDelayUntil guarantees this runs exactly every 10ms (100Hz) regardless of other tasks.
void imu_task(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(10);

    while (1) {
        imu_read_data();
        imu_calculate_dead_reckoning();
        
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

// --- Low Priority: Environment Task ---
// Priority 2. The DHT22 blocks for 25ms+, but since it has a low priority, the RTOS
// will automatically pause it to let the IMU task run exactly when it needs to.
void environment_task(void *pvParameters) {
    while (1) {
        float t, h;
        int l;
        sensors_read_temp_humidity(&t, &h);
        sensors_read_light(&l);

        // Safely update shared variables
        if (xSemaphoreTake(env_data_mutex, portMAX_DELAY)) {
            shared_env_data.temp = t;
            shared_env_data.hum = h;
            shared_env_data.light = l;
            xSemaphoreGive(env_data_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(2000)); // Only needs updating every 2 seconds
    }
}

// --- Medium/Low Priority: Display Task ---
// Priority 3. Screen updates take time over I2C but aren't extremely critical.
void display_task(void *pvParameters) {
    while (1) {
        float t = 0, h = 0;
        int l = 0;

        if (xSemaphoreTake(env_data_mutex, portMAX_DELAY)) {
            t = shared_env_data.temp;
            h = shared_env_data.hum;
            l = shared_env_data.light;
            xSemaphoreGive(env_data_mutex);
        }

        oled_update_display(l, t, h);
        vTaskDelay(pdMS_TO_TICKS(100)); // Update UI at ~10 FPS
    }
}

// --- Lowest Priority: Serial Data Task ---
// Priority 1. Print data out to the PC
void serial_task(void *pvParameters) {
    while (1) {
        serial_send_path_data();
        vTaskDelay(pdMS_TO_TICKS(500)); // Stream data to PC at 2Hz
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "--- FreeRTOS Real-Time Setup ---");
    
    debug_sensor_init();
    imu_init();
    oled_init();

    env_data_mutex = xSemaphoreCreateMutex();
    fall_semaphore = xSemaphoreCreateBinary();

    // Attach our hardware interrupt
    imu_setup_interrupt(imu_isr_handler, NULL);

    // Spawning tasks with explicitly defined priorities
    xTaskCreate(serial_task,      "Serial_Task",  4096, NULL, 1, NULL);
    xTaskCreate(environment_task, "Env_Task",     4096, NULL, 2, NULL);
    xTaskCreate(display_task,     "Display_Task", 4096, NULL, 3, NULL);
    xTaskCreate(imu_task,         "IMU_Task",     4096, NULL, 5, NULL); // Highest priority!
    xTaskCreate(fall_detection_task, "Fall_Task", 4096, NULL, 6, NULL); // Top emergency priority!

    ESP_LOGI(TAG, "System Tasks Spawned successfully");
}