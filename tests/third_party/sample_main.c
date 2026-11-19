/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Calls QNX's own I2C client library (librpi_i2c from
 * gitlab.com/qnx/projects/hardware-component-samples, built unchanged)
 * against /dev/i2c0 served by i2c-ichsmb. Nothing here talks to the driver
 * directly; every transfer goes through the sample library's functions.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rpi_i2c.h"

#define BUS     0
#define EEPROM  0x56
#define SENSOR  0x48
#define EMPTY   0x20

static int failures;

static void check(const char *what, int ok)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

int main(void)
{
    const uint8_t pattern[8] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };
    uint8_t v = 0, buf[8] = { 0 }, temp[2] = { 0 };
    int rc;

    rc = smbus_write_byte_data(BUS, EEPROM, 0x05, 0x5a);
    check("smbus_write_byte_data(0x56, reg 0x05, 0x5a)", rc == I2C_SUCCESS);
    rc = smbus_read_byte_data(BUS, EEPROM, 0x05, &v);
    printf("      read back 0x%02x\n", v);
    check("smbus_read_byte_data returns 0x5a", rc == I2C_SUCCESS && v == 0x5a);

    rc = smbus_write_block_data(BUS, EEPROM, 0x20, pattern, sizeof(pattern));
    check("smbus_write_block_data(0x56, reg 0x20, 8 bytes)", rc == I2C_SUCCESS);
    rc = smbus_read_block_data(BUS, EEPROM, 0x20, buf, sizeof(buf));
    check("smbus_read_block_data returns the same 8 bytes", rc == I2C_SUCCESS && memcmp(buf, pattern, 8) == 0);

    rc = smbus_read_block_data(BUS, SENSOR, 0x00, temp, 2);
    printf("      sensor raw 0x%02x%02x = %.1f C\n", temp[0], temp[1], (int16_t)((temp[0] << 8) | temp[1]) / 256.0);
    check("smbus_read_block_data(0x48, reg 0, 2) reads the sensor", rc == I2C_SUCCESS);

    rc = smbus_read_byte_data(BUS, EMPTY, 0x00, &v);
    check("smbus_read_byte_data on an empty address fails", rc == I2C_ERROR_OPERATION_FAILED);

    smbus_cleanup(BUS);
    printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
