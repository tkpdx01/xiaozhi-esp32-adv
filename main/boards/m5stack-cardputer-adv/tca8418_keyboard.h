#ifndef TCA8418_KEYBOARD_H
#define TCA8418_KEYBOARD_H

#include "i2c_device.h"
#include <functional>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// TCA8418 Register definitions
#define TCA8418_REG_CFG             0x01
#define TCA8418_REG_INT_STAT        0x02
#define TCA8418_REG_KEY_LCK_EC      0x03
#define TCA8418_REG_KEY_EVENT_A     0x04
#define TCA8418_REG_KP_GPIO_1       0x1D
#define TCA8418_REG_KP_GPIO_2       0x1E
#define TCA8418_REG_KP_GPIO_3       0x1F

// Config register bits
#define TCA8418_CFG_KE_IEN          0x01  // Key events interrupt enable

// Key codes for arrow keys (based on keyboard matrix position)
// Row 2: fn, shift, a, s, d, f, g, h, j, k, l, ;, ', enter
// Row 3: ctrl, opt, alt, z, x, c, v, b, n, m, ,, ., /, space
enum KeyCode {
    KEY_NONE = 0,
    KEY_UP,      // ; key position
    KEY_DOWN,    // . key position
    KEY_LEFT,    // , key position
    KEY_RIGHT,   // / key position
    KEY_ENTER,   // Enter key
    KEY_OTHER
};

class Tca8418Keyboard : public I2cDevice {
public:
    using KeyCallback = std::function<void(KeyCode key)>;

    Tca8418Keyboard(i2c_master_bus_handle_t i2c_bus, uint8_t addr, gpio_num_t int_pin);
    ~Tca8418Keyboard();

    void Initialize();
    void SetKeyCallback(KeyCallback callback) { key_callback_ = callback; }

private:
    gpio_num_t int_pin_;
    KeyCallback key_callback_;
    TaskHandle_t task_handle_ = nullptr;
    volatile bool isr_flag_ = false;

    void ConfigureMatrix();
    void EnableInterrupts();
    void FlushEvents();
    uint8_t GetEvent();
    KeyCode MapKeyCode(uint8_t row, uint8_t col);

    static void IRAM_ATTR GpioIsrHandler(void* arg);
    static void KeyboardTask(void* arg);
};

#endif // TCA8418_KEYBOARD_H
