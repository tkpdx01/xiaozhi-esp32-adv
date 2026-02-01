#include "tca8418_keyboard.h"
#include <esp_log.h>

#define TAG "TCA8418"

// TCA8418 additional registers
#define TCA8418_REG_GPIO_INT_EN_1   0x1A
#define TCA8418_REG_GPIO_INT_EN_2   0x1B
#define TCA8418_REG_GPIO_INT_EN_3   0x1C
#define TCA8418_REG_GPIO_DAT_STAT_1 0x14
#define TCA8418_REG_GPIO_DAT_STAT_2 0x15
#define TCA8418_REG_GPIO_DAT_STAT_3 0x16
#define TCA8418_REG_GPIO_DAT_OUT_1  0x17
#define TCA8418_REG_GPIO_DAT_OUT_2  0x18
#define TCA8418_REG_GPIO_DAT_OUT_3  0x19
#define TCA8418_REG_GPIO_INT_LVL_1  0x20
#define TCA8418_REG_GPIO_INT_LVL_2  0x21
#define TCA8418_REG_GPIO_INT_LVL_3  0x22
#define TCA8418_REG_DEBOUNCE_DIS_1  0x29
#define TCA8418_REG_DEBOUNCE_DIS_2  0x2A
#define TCA8418_REG_DEBOUNCE_DIS_3  0x2B
#define TCA8418_REG_GPIO_PULL_1     0x2C
#define TCA8418_REG_GPIO_PULL_2     0x2D
#define TCA8418_REG_GPIO_PULL_3     0x2E

// Config register bits
#define TCA8418_CFG_AI              0x80  // Auto-increment for read/write
#define TCA8418_CFG_GPI_E_CFG       0x40  // GPI event mode config
#define TCA8418_CFG_OVR_FLOW_M      0x20  // Overflow mode
#define TCA8418_CFG_INT_CFG         0x10  // Interrupt config
#define TCA8418_CFG_OVR_FLOW_IEN    0x08  // Overflow interrupt enable
#define TCA8418_CFG_K_LCK_IEN       0x04  // Keypad lock interrupt enable
#define TCA8418_CFG_GPI_IEN         0x02  // GPI interrupt enable

// Interrupt status bits
#define TCA8418_INT_STAT_CAD_INT    0x10  // CTRL-ALT-DEL interrupt
#define TCA8418_INT_STAT_OVR_FLOW   0x08  // Overflow interrupt
#define TCA8418_INT_STAT_K_LCK_INT  0x04  // Key lock interrupt
#define TCA8418_INT_STAT_GPI_INT    0x02  // GPI interrupt
#define TCA8418_INT_STAT_K_INT      0x01  // Key event interrupt

Tca8418Keyboard::Tca8418Keyboard(i2c_master_bus_handle_t i2c_bus, uint8_t addr, gpio_num_t int_pin)
    : I2cDevice(i2c_bus, addr), int_pin_(int_pin) {
}

Tca8418Keyboard::~Tca8418Keyboard() {
    if (task_handle_) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    gpio_isr_handler_remove(int_pin_);
}

void Tca8418Keyboard::Initialize() {
    ESP_LOGI(TAG, "Initializing TCA8418 keyboard");

    // Configure keyboard matrix
    ConfigureMatrix();

    // Flush any pending events
    FlushEvents();

    // Enable interrupts
    EnableInterrupts();

    // Configure GPIO interrupt pin
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;  // Interrupt on falling edge
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << int_pin_);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    // Install GPIO ISR service if not already installed
    gpio_install_isr_service(0);
    gpio_isr_handler_add(int_pin_, GpioIsrHandler, this);

    // Create keyboard task
    xTaskCreate(KeyboardTask, "keyboard_task", 4096, this, 5, &task_handle_);

    ESP_LOGI(TAG, "TCA8418 keyboard initialized");
}

void Tca8418Keyboard::ConfigureMatrix() {
    // Configure rows 0-3 as keypad rows (R0-R3)
    // Configure cols 0-13 as keypad columns (C0-C9 + R4-R7 as C10-C13)
    // KP_GPIO1: R0-R7 (bits 0-7)
    // KP_GPIO2: C0-C7 (bits 0-7)
    // KP_GPIO3: C8-C9 (bits 0-1)

    // Set rows R0-R3 as keypad rows
    WriteReg(TCA8418_REG_KP_GPIO_1, 0x0F);  // R0-R3 as keypad
    // Set columns C0-C7 as keypad columns
    WriteReg(TCA8418_REG_KP_GPIO_2, 0xFF);  // C0-C7 as keypad
    // Set C8-C9 and R4-R7 as keypad (for extended columns)
    WriteReg(TCA8418_REG_KP_GPIO_3, 0xFF);  // C8-C9 + R4-R7 as keypad
}

void Tca8418Keyboard::EnableInterrupts() {
    // Enable key event interrupt
    uint8_t cfg = TCA8418_CFG_KE_IEN | TCA8418_CFG_OVR_FLOW_M | TCA8418_CFG_INT_CFG;
    WriteReg(TCA8418_REG_CFG, cfg);
}

void Tca8418Keyboard::FlushEvents() {
    // Read and discard all pending key events
    uint8_t event;
    int count = 0;
    while ((event = GetEvent()) != 0 && count < 10) {
        count++;
    }

    // Clear interrupt status
    WriteReg(TCA8418_REG_INT_STAT, 0x1F);
}

uint8_t Tca8418Keyboard::GetEvent() {
    return ReadReg(TCA8418_REG_KEY_EVENT_A);
}

KeyCode Tca8418Keyboard::MapKeyCode(uint8_t row, uint8_t col) {
    // M5Cardputer keyboard matrix (4 rows x 14 columns):
    // Row 2: fn, shift, a, s, d, f, g, h, j, k, l, ;(UP), '(unused), enter
    // Row 3: ctrl, opt, alt, z, x, c, v, b, n, m, ,(LEFT), .(DOWN), /(RIGHT), space

    // Arrow keys mapping based on M5Cardputer layout:
    // UP: ; key - row 2, col 11
    // DOWN: . key - row 3, col 11
    // LEFT: , key - row 3, col 10
    // RIGHT: / key - row 3, col 12
    // ENTER: enter key - row 2, col 13

    if (row == 2 && col == 11) return KEY_UP;      // ; key
    if (row == 3 && col == 11) return KEY_DOWN;    // . key
    if (row == 3 && col == 10) return KEY_LEFT;    // , key
    if (row == 3 && col == 12) return KEY_RIGHT;   // / key
    if (row == 2 && col == 13) return KEY_ENTER;   // Enter key

    return KEY_OTHER;
}

void IRAM_ATTR Tca8418Keyboard::GpioIsrHandler(void* arg) {
    Tca8418Keyboard* keyboard = static_cast<Tca8418Keyboard*>(arg);
    keyboard->isr_flag_ = true;

    // Wake up the keyboard task
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (keyboard->task_handle_) {
        vTaskNotifyGiveFromISR(keyboard->task_handle_, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

void Tca8418Keyboard::KeyboardTask(void* arg) {
    Tca8418Keyboard* keyboard = static_cast<Tca8418Keyboard*>(arg);

    while (true) {
        // Wait for interrupt notification
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!keyboard->isr_flag_) {
            continue;
        }
        keyboard->isr_flag_ = false;

        // Small delay for debounce
        vTaskDelay(pdMS_TO_TICKS(10));

        // Read interrupt status
        uint8_t int_stat = keyboard->ReadReg(TCA8418_REG_INT_STAT);

        if (int_stat & TCA8418_INT_STAT_K_INT) {
            // Read key events
            uint8_t event;
            while ((event = keyboard->GetEvent()) != 0) {
                // Event format: bit 7 = press(1)/release(0), bits 6-0 = key code
                bool pressed = (event & 0x80) != 0;
                uint8_t key_code = event & 0x7F;

                if (pressed && key_code > 0) {
                    // Convert key code to row/col
                    // TCA8418 key code = (row * 10) + col + 1
                    uint8_t row = (key_code - 1) / 10;
                    uint8_t col = (key_code - 1) % 10;

                    // For extended columns (10-13), they use R4-R7 as additional columns
                    // Key codes 41-80 map to extended matrix
                    if (key_code > 40) {
                        row = (key_code - 41) / 10;
                        col = ((key_code - 41) % 10) + 10;
                    }

                    ESP_LOGD(TAG, "Key pressed: code=%d, row=%d, col=%d", key_code, row, col);

                    KeyCode mapped_key = keyboard->MapKeyCode(row, col);
                    if (mapped_key != KEY_OTHER && mapped_key != KEY_NONE) {
                        if (keyboard->key_callback_) {
                            keyboard->key_callback_(mapped_key);
                        }
                    }
                }
            }
        }

        // Clear interrupt status
        keyboard->WriteReg(TCA8418_REG_INT_STAT, int_stat);
    }
}
