#ifndef TEST_SPI_H
#define TEST_SPI_H
#include <stdint.h>
#include <stddef.h>
typedef struct { unsigned id; } spi_inst_t;
extern spi_inst_t *spi0;
extern spi_inst_t *spi1;
#define SPI_CPOL_0 0
#define SPI_CPHA_1 1
#define SPI_MSB_FIRST 0
unsigned spi_init(spi_inst_t *spi, unsigned baud);
void spi_set_format(spi_inst_t *spi, unsigned bits, int cpol, int cpha, int order);
int spi_write_read_blocking(spi_inst_t *spi, const uint8_t *tx, uint8_t *rx, size_t len);
int spi_write_blocking(spi_inst_t *spi, const uint8_t *tx, size_t len);
int spi_read_blocking(spi_inst_t *spi, uint8_t fill, uint8_t *rx, size_t len);
#endif
