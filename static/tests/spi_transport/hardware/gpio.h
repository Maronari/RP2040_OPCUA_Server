#ifndef TEST_GPIO_H
#define TEST_GPIO_H
#define GPIO_FUNC_SPI 1
#define GPIO_OUT 1
void gpio_init(unsigned pin);
void gpio_set_dir(unsigned pin, int dir);
void gpio_set_function(unsigned pin, int function);
void gpio_put(unsigned pin, int value);
#endif
