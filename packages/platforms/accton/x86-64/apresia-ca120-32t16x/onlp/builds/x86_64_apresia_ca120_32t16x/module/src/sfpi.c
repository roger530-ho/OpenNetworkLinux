/************************************************************
 * <bsn.cl fy=2014 v=onl>
 *
 *           Copyright 2014 Big Switch Networks, Inc.
 *           Copyright 2025 Accton Technology Corporation.
 *
 * Licensed under the Eclipse Public License, Version 1.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *        http://www.eclipse.org/legal/epl-v10.html
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the
 * License.
 *
 * </bsn.cl>
 ************************************************************
 *
 *
 *
 ***********************************************************/
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <onlp/platformi/sfpi.h>
#include <onlplib/i2c.h>
#include <onlplib/file.h>
#include "x86_64_apresia_ca120_32t16x_int.h"
#include "x86_64_apresia_ca120_32t16x_log.h"

#define PORT_EEPROM_FORMAT        "/sys/bus/i2c/devices/%d-0050/eeprom"
#define MODULE_PRESENT_FORMAT_1   "/sys/bus/i2c/devices/2-0064/ca120-32t16x-cxcvr/module_present_%d"
#define MODULE_PRESENT_FORMAT_2   "/sys/bus/i2c/devices/2-0060/ca120-32t16x-fxcvr/module_present_%d"
#define MODULE_RXLOS_FORMAT_1     "/sys/bus/i2c/devices/2-0064/ca120-32t16x-cxcvr/module_rx_los_%d"
#define MODULE_RXLOS_FORMAT_2     "/sys/bus/i2c/devices/2-0060/ca120-32t16x-fxcvr/module_rx_los_%d"
#define MODULE_TXFAULT_FORMAT_1   "/sys/bus/i2c/devices/2-0064/ca120-32t16x-cxcvr/module_tx_fault_%d"
#define MODULE_TXFAULT_FORMAT_2   "/sys/bus/i2c/devices/2-0060/ca120-32t16x-fxcvr/module_tx_fault_%d"
#define MODULE_TXDISABLE_FORMAT_1 "/sys/bus/i2c/devices/2-0064/ca120-32t16x-cxcvr/module_tx_disable_%d"
#define MODULE_TXDISABLE_FORMAT_2 "/sys/bus/i2c/devices/2-0060/ca120-32t16x-fxcvr/module_tx_disable_%d"
#define MODULE_PRESENT_ALL_ATTR_1 "/sys/bus/i2c/devices/2-0064/ca120-32t16x-cxcvr/module_present_all"
#define MODULE_PRESENT_ALL_ATTR_2 "/sys/bus/i2c/devices/2-0060/ca120-32t16x-fxcvr/module_present_all"
#define MODULE_RXLOS_ALL_ATTR_1   "/sys/bus/i2c/devices/2-0064/ca120-32t16x-cxcvr/module_rx_los_all"
#define MODULE_RXLOS_ALL_ATTR_2   "/sys/bus/i2c/devices/2-0060/ca120-32t16x-fxcvr/module_rx_los_all"

/* Port 33..48 map to bus index array below (0x50 eeprom). */
int port_bus_index[] = { 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33 };
#define PORT_BUS_INDEX(port) (port_bus_index[(port) - 32])

#define VALIDATE_SFP(_port) \
    do { \
        /* Valid ports are 33..48 inclusive */ \
        if ((_port) < 32 || (_port) > 47) \
            return ONLP_STATUS_E_UNSUPPORTED; \
    } while(0)

/* Bit base is 0-based (port 1 -> bit 0). */
#define ATTR1_PORT_BASE  32  /* ports 33..40 -> bits 32..39 */
#define ATTR1_PORTS       8
#define ATTR2_PORT_BASE  40  /* ports 41..48 -> bits 40..47 */
#define ATTR2_PORTS       8

/************************************************************
 *
 * SFPI Entry Points
 *
 ***********************************************************/
/**
 * onlp_sfpi_init - Initialize the SFP interface.
 *
 * This function is called during the platform initialization sequence. It
 * can be used to set up any necessary resources for SFP management.
 * In this specific implementation, it does nothing.
 *
 * @return ONLP_STATUS_OK on success.
 */
int
onlp_sfpi_init(void)
{
    /* Called at initialization time */
    return ONLP_STATUS_OK;
}

/**
 * onlp_sfpi_bitmap_get - Get the bitmap of available SFP ports.
 *
 * Populates the provided SFP bitmap with the ports that are physically
 * present on the platform. For this platform, it marks ports 33 to 48
 * (bits 32 to 47) as valid.
 *
 * @bmap: A pointer to the SFP bitmap structure to be populated.
 *
 * @return ONLP_STATUS_OK on success.
 */
int
onlp_sfpi_bitmap_get(onlp_sfp_bitmap_t* bmap)
{
    /* Mark valid SFP ports: 33..48 => bits 32..47 */
    int p;

    for (p = 32; p < 48; p++) {
        AIM_BITMAP_SET(bmap, p);
    }

    return ONLP_STATUS_OK;
}

/**
 * onlp_sfpi_is_present - Check if an SFP module is present in a specific port.
 *
 * Reads the presence status of an SFP module from a sysfs file. The file
 * path is determined by the port number.
 *
 * @port: The port number to check.
 *
 * @return 1 if present, 0 if not present, or a negative ONLP_STATUS
 * code on error.
 */
int
onlp_sfpi_is_present(int port)
{
    /* Return 1 if present, 0 if not present, <0 on error. */
    int present;
    const char* fmt;
    VALIDATE_SFP(port);

    /* Ports 33..40 use *_FORMAT_1, ports 41..48 use *_FORMAT_2. */
    fmt = (port < 40) ? MODULE_PRESENT_FORMAT_1 : MODULE_PRESENT_FORMAT_2;
    if (onlp_file_read_int(&present, fmt, port + 1) < 0) {
        AIM_LOG_ERROR("Unable to read present status from port(%d)", port);
        return ONLP_STATUS_E_INTERNAL;
    }

    return present;
}

/**
 * read_hex_bitmap_file_lsb_bytes - Parse a sysfs hex bitmap file with LSB-first byte ordering.
 *
 * Reads a string of hexadecimal characters from a sysfs file, where each
 * pair of characters represents a byte. The file is expected to have
 * byte 0 printed first. The function parses up to 8 bytes (64 bits) and
 * constructs a uint64_t value with the first byte read placed in the
 * least-significant position.
 *
 * @path: The path to the sysfs file.
 * @out_val: A pointer to a uint64_t variable to store the parsed result.
 *
 * Context: process context.
 * @return ONLP_STATUS_OK on success, or a negative ONLP_STATUS code on error.
 */
static int
read_hex_bitmap_file_lsb_bytes(const char* path, uint64_t* out_val)
{
    FILE* fp;
    char buf[32];
    const char* p;
    uint64_t val = 0ULL;
    int byte_index = 0;

    if (out_val == NULL) {
        return ONLP_STATUS_E_PARAM;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        AIM_LOG_ERROR("Unable to open '%s'", path);
        return ONLP_STATUS_E_INTERNAL;
    }

    if (fgets(buf, sizeof(buf), fp) == NULL) {
        fclose(fp);
        AIM_LOG_ERROR("Unable to read from '%s'", path);
        return ONLP_STATUS_E_INTERNAL;
    }
    fclose(fp);

    /* Walk the string and parse two hex digits at a time. */
    p = buf;
    while (*p != '\0') {
        unsigned int bytev = 0U;
        int consumed = 0;
        int r;

        /* Skip whitespace (newline, spaces, tabs). */
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        if (byte_index >= 8) {
            AIM_LOG_ERROR("Bitmap exceeds 64 bits in '%s'", path);
            return ONLP_STATUS_E_INTERNAL;
        }

        /* Read exactly up to 2 hex chars into 'bytev'; report chars consumed in 'consumed'. */
        r = sscanf(p, "%2x%n", &bytev, &consumed);
        if (r != 1 || consumed <= 0) {
            AIM_LOG_ERROR("Invalid hex byte near '%.8s' in '%s'", p, path);
            return ONLP_STATUS_E_INTERNAL;
        }

        /* Place byte0 at LSB, byte1 at bits [15:8], etc. */
        val |= ((uint64_t)(bytev & 0xFFU)) << (8 * byte_index);
        byte_index++;
        p += consumed;
    }

    if (byte_index == 0) {
        AIM_LOG_ERROR("No hex data in '%s'", path);
        return ONLP_STATUS_E_INTERNAL;
    }

    *out_val = val;
    return ONLP_STATUS_OK;
}

/**
 * onlp_sfpi_presence_bitmap_get - Get a bitmap of all present SFP modules.
 *
 * Aggregates the presence status for all supported SFP ports (33-48) into a
 * single bitmap. It reads from two separate sysfs files, each corresponding
 * to a group of 8 ports. It updates only the bits for the relevant ports in
 * the destination bitmap.
 *
 * @dst: A pointer to the destination SFP bitmap to be updated.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP_STATUS code on error.
 */
int
onlp_sfpi_presence_bitmap_get(onlp_sfp_bitmap_t* dst)
{
    uint64_t v1;
    uint64_t v2;
    int rc;
    int i;

    /* Clear only the target range [32..47] to avoid clobbering other ports. */
    for (i = ATTR1_PORT_BASE; i < (ATTR1_PORT_BASE + ATTR1_PORTS + ATTR2_PORTS); i++) {
        AIM_BITMAP_MOD(dst, i, 0);
    }

    /* Read ports 33..40 (byte0-first hex like "01", LSB maps to port 33). */
    rc = read_hex_bitmap_file_lsb_bytes(MODULE_PRESENT_ALL_ATTR_1, &v1);
    if (rc != ONLP_STATUS_OK) {
        return rc;
    }
    v1 &= 0xFFULL; /* attribute is one byte today; mask for safety */

    for (i = 0; i < ATTR1_PORTS; i++) {
        int bit = (int)((v1 >> i) & 0x1ULL);
        AIM_BITMAP_MOD(dst, ATTR1_PORT_BASE + i, bit);
    }

    /* Read ports 41..48 (same format; LSB maps to port 41). */
    rc = read_hex_bitmap_file_lsb_bytes(MODULE_PRESENT_ALL_ATTR_2, &v2);
    if (rc != ONLP_STATUS_OK) {
        return rc;
    }
    v2 &= 0xFFULL;

    for (i = 0; i < ATTR2_PORTS; i++) {
        int bit = (int)((v2 >> i) & 0x1ULL);
        AIM_BITMAP_MOD(dst, ATTR2_PORT_BASE + i, bit);
    }

    return ONLP_STATUS_OK;
}

/**
 * onlp_sfpi_rx_los_bitmap_get - Get a bitmap of SFP modules with RX Loss of Signal (LOS).
 *
 * Aggregates the RX LOS status for all supported SFP ports (33-48) into a
 * single bitmap. It reads from two separate sysfs files. A set bit indicates
 * that the corresponding port has an RX LOS condition.
 *
 * @dst: A pointer to the destination SFP bitmap to be updated.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP_STATUS code on error.
 */
int
onlp_sfpi_rx_los_bitmap_get(onlp_sfp_bitmap_t* dst)
{
    uint64_t v1;
    uint64_t v2;
    int rc;
    int i;

    /* Clear only the target range [32..47] so other ranges (1..32, 49..) stay intact. */
    for (i = ATTR1_PORT_BASE; i < (ATTR1_PORT_BASE + ATTR1_PORTS + ATTR2_PORTS); i++) {
        AIM_BITMAP_MOD(dst, i, 0);
    }

    /* Ports 33..40. */
    rc = read_hex_bitmap_file_lsb_bytes(MODULE_RXLOS_ALL_ATTR_1, &v1);
    if (rc != ONLP_STATUS_OK) {
        return rc;
    }
    v1 &= 0xFFULL; /* keep the lowest 8 bits */

    for (i = 0; i < ATTR1_PORTS; i++) {
        int bit = (int)((v1 >> i) & 0x1ULL);  /* bit0 -> port33 */
        AIM_BITMAP_MOD(dst, ATTR1_PORT_BASE + i, bit);
    }

    /* Ports 41..48. */
    rc = read_hex_bitmap_file_lsb_bytes(MODULE_RXLOS_ALL_ATTR_2, &v2);
    if (rc != ONLP_STATUS_OK) {
        return rc;
    }
    v2 &= 0xFFULL;

    for (i = 0; i < ATTR2_PORTS; i++) {
        int bit = (int)((v2 >> i) & 0x1ULL);  /* bit0 -> port41 */
        AIM_BITMAP_MOD(dst, ATTR2_PORT_BASE + i, bit);
    }

    return ONLP_STATUS_OK;
}

/**
 * onlp_sfpi_eeprom_read - Read the SFP EEPROM data.
 *
 * Reads the first 256 bytes (A0h) of the EEPROM data for a specified
 * SFP port from a sysfs file.
 *
 * @port: The port number from which to read the EEPROM.
 * @data: A 256-byte buffer to store the EEPROM data.
 *
 * @return ONLP_STATUS_OK on success, or a negative ONLP_STATUS code on error.
 */
int
onlp_sfpi_eeprom_read(int port, uint8_t data[256])
{
    /*
     * Read the SFP eeprom into data[]
     *
     * Return MISSING if SFP is missing.
     * Return OK if eeprom is read
     */
    int size = 0;
    VALIDATE_SFP(port);

    memset(data, 0, 256);

    if (onlp_file_read(data, 256, &size, PORT_EEPROM_FORMAT,
                       PORT_BUS_INDEX(port)) != ONLP_STATUS_OK) {
        AIM_LOG_ERROR("Unable to read eeprom from port(%d)", port);
        return ONLP_STATUS_E_INTERNAL;
    }

    if (size != 256) {
        AIM_LOG_ERROR("Unable to read eeprom from port(%d), size mismatch", port);
        return ONLP_STATUS_E_INTERNAL;
    }

    return ONLP_STATUS_OK;
}

/**
 * read_a2_per_byte_with_probe - Read SFF-8472 A2h (I2C 0x51) with presence probe.
 * @port: Logical port index to access the transceiver on.
 * @data: Caller-provided buffer (256 bytes) to receive A2h contents.
 *
 * Rationale:
 *   Per SFF-8472 for SFP/SFP+, identity/static fields are at A0h (0x50) and
 *   DOM/DDM fields are at A2h (0x51). Some platforms (e.g., certain 'optoe'
 *   variants) do NOT expose a stitched sysfs window that maps 0x51 to offset
 *   256 of the 0x50 "eeprom" file, or they gate that mapping on A0[92] bit 6.
 *   When the stitched view is unavailable, reading offset 256 from 0x50 will
 *   hit EOF/short-read. In such cases, DOM must be obtained by directly
 *   addressing 0x51 at the I2C level.
 *
 * Behavior:
 *   This helper first PROBES 0x51 by reading a safe byte (offset 0) with
 *   a small number of retries and backoff to tolerate transient NACK/bus
 *   contention. If 0x51 does not respond, the function returns
 *   ONLP_STATUS_E_UNSUPPORTED (typical for copper/RJ45 SFPs without DDM).
 *   If 0x51 responds, it optionally selects page 0 (write 0x7F <- 0x00;
 *   failures are ignored as many modules already default to page 0), and
 *   then reads all 256 bytes (offsets 0..255) via per-byte API calls.
 *
 * Return:
 *   ONLP_STATUS_OK            on success (all 256 bytes read into @data).
 *   ONLP_STATUS_E_UNSUPPORTED if 0x51 is absent/not responding.
 *   ONLP_STATUS_E_INTERNAL    on I/O errors during the per-byte reads.
 */
static int
read_a2_per_byte_with_probe(int port, uint8_t data[256])
{
    int r;
    int v;
    int i;
    const int retries = 3;
    const int delay_us = 30000; /* 30 ms */

    /* Probe presence of 0x51 by reading offset 0 with small retries. */
    for (r = 0; r < retries; r++) {
        v = onlp_sfpi_dev_readb(port, 0x51, 0);
        if (v >= 0 && v <= 0xFF) {
            break;
        }
        usleep(delay_us);
    }
    if (r == retries) {
        AIM_LOG_ERROR("A2(0x51) not present or not responding on port(%d)", port);
        return ONLP_STATUS_E_UNSUPPORTED;
    }

    /* Optional vendor quirk: select page 0. */
    onlp_sfpi_dev_writeb(port, 0x51, 0x7F, 0x00);

    /* Read full 256-byte A2h space via per-byte ONLP API. */
    for (i = 0; i < 256; i++) {
        v = onlp_sfpi_dev_readb(port, 0x51, i);
        if (v < 0 || v > 0xFF) {
            AIM_LOG_ERROR("Unable to read A2(0x51) on port(%d) at off=%d, v=%d", port, i, v);
            return ONLP_STATUS_E_INTERNAL;
        }
        data[i] = (uint8_t) v;
    }

    return ONLP_STATUS_OK;
}

/**
 * onlp_sfpi_dom_read - Read the SFP Digital Optical Monitoring (DOM) data.
 *
 * Reads the 256 bytes of DOM/DDM data (SFF-8472 A2h, I2C 0x51). Some drivers
 * expose a "stitched" sysfs view where 0x50 appears at offsets 0..255 and 0x51
 * at 256..511. Others do NOT stitch (or gate stitching by A0[92] bit 6), so reading
 * from offset 256 on the 0x50 file hits EOF/short read. In that case, we fall
 * back to direct per-byte reads from 0x51 after probing its presence.
 *
 * @port: The port number from which to read the DOM data.
 * @data: A 256-byte buffer to store the DOM data.
 *
 * @return ONLP_STATUS_OK on success;
 *         ONLP_STATUS_E_UNSUPPORTED if 0x51 is not present/responding;
 *         ONLP_STATUS_E_INTERNAL on other I/O errors.
 */
int
onlp_sfpi_dom_read(int port, uint8_t data[256])
{
    FILE* fp;
    char file[64] = {0};
    int ret;

    VALIDATE_SFP(port);

    /* Attempt 1: stitched view (0x50 sysfs, offset 256). */
    sprintf(file, PORT_EEPROM_FORMAT, PORT_BUS_INDEX(port));
    fp = fopen(file, "rb");
    if (fp != NULL) {
        if (fseek(fp, 256, SEEK_CUR) == 0) {
            ret = fread(data, 1, 256, fp);
            fclose(fp);
            if (ret == 256) {
                return ONLP_STATUS_OK;
            }
        } else {
            fclose(fp);
            AIM_LOG_ERROR("fseek(256) failed on 0x50/eeprom of port(%d)", port);
        }
    } else {
        AIM_LOG_ERROR("Unable to open the eeprom device file of port(%d)", port);
    }

    /* Attempt 2: direct per-byte read from 0x51 (normative A2h). */
    return read_a2_per_byte_with_probe(port, data);
}

/**
 * onlp_sfpi_control_set - Set an SFP control parameter.
 *
 * Sets a controllable parameter for a specified SFP port. Currently, only
 * supports disabling the transmitter (ONLP_SFP_CONTROL_TX_DISABLE).
 *
 * @port: The port number to control.
 * @control: The control parameter to set.
 * @value: The value to set for the control parameter.
 *
 * @return ONLP_STATUS_OK on success, ONLP_STATUS_E_UNSUPPORTED for
 * unsupported controls, or another negative ONLP_STATUS code on error.
 */
int
onlp_sfpi_control_set(int port, onlp_sfp_control_t control, int value)
{
    const char* fmt = NULL;

    switch(control) {
    case ONLP_SFP_CONTROL_TX_DISABLE: {
        VALIDATE_SFP(port);
        fmt = (port < 40) ? MODULE_TXDISABLE_FORMAT_1 : MODULE_TXDISABLE_FORMAT_2;
        if (onlp_file_write_int(value, fmt, port + 1) < 0) {
            AIM_LOG_ERROR("Unable to set tx_disable status to port(%d)", port);
            return ONLP_STATUS_E_INTERNAL;
        }
        return ONLP_STATUS_OK;
    }
    default:
        break;
    }

    return ONLP_STATUS_E_UNSUPPORTED;
}

/**
 * onlp_sfpi_control_get - Get an SFP control or status parameter.
 *
 * Retrieves the value of a control or status parameter for a specified
 * SFP port. Supports ONLP_SFP_CONTROL_RX_LOS, ONLP_SFP_CONTROL_TX_FAULT,
 * and ONLP_SFP_CONTROL_TX_DISABLE.
 *
 * @port: The port number to query.
 * @control: The control parameter to get.
 * @value: A pointer to an integer to store the retrieved value.
 *
 * @return ONLP_STATUS_OK on success, ONLP_STATUS_E_UNSUPPORTED for
 * unsupported controls, or another negative ONLP_STATUS code on error.
 */
int
onlp_sfpi_control_get(int port, onlp_sfp_control_t control, int* value)
{
    const char* fmt = NULL;

    switch(control) {
    case ONLP_SFP_CONTROL_RX_LOS: {
        VALIDATE_SFP(port);
        fmt = (port < 40) ? MODULE_RXLOS_FORMAT_1 : MODULE_RXLOS_FORMAT_2;
        if (onlp_file_read_int(value, fmt, port + 1) < 0) {
            AIM_LOG_ERROR("Unable to read rx_loss status from port(%d)", port);
            return ONLP_STATUS_E_INTERNAL;
        }
        return ONLP_STATUS_OK;
    }

    case ONLP_SFP_CONTROL_TX_FAULT: {
        VALIDATE_SFP(port);
        fmt = (port < 40) ? MODULE_TXFAULT_FORMAT_1 : MODULE_TXFAULT_FORMAT_2;
        if (onlp_file_read_int(value, fmt, port + 1) < 0) {
            AIM_LOG_ERROR("Unable to read tx_fault status from port(%d)", port);
            return ONLP_STATUS_E_INTERNAL;
        }
        return ONLP_STATUS_OK;
    }

    case ONLP_SFP_CONTROL_TX_DISABLE: {
        VALIDATE_SFP(port);
        fmt = (port < 40) ? MODULE_TXDISABLE_FORMAT_1 : MODULE_TXDISABLE_FORMAT_2;
        if (onlp_file_read_int(value, fmt, port + 1) < 0) {
            AIM_LOG_ERROR("Unable to read tx_disabled status from port(%d)", port);
            return ONLP_STATUS_E_INTERNAL;
        }
        return ONLP_STATUS_OK;
    }
    default:
        break;
    }

    return ONLP_STATUS_E_UNSUPPORTED;
}

/**
 * onlp_sfpi_dev_readb - Read a byte from an I2C device on an SFP port's bus.
 *
 * Performs a raw byte read from a specified I2C device address and
 * register address on the I2C bus associated with the given SFP port.
 *
 * @port: The SFP port number, used to determine the I2C bus.
 * @devaddr: The 7-bit I2C device address.
 * @addr: The register address within the I2C device.
 *
 * @return The read byte value (0-255) on success, or a negative value on error.
 */
int
onlp_sfpi_dev_readb(int port, uint8_t devaddr, uint8_t addr)
{
    int bus = PORT_BUS_INDEX(port);
    return onlp_i2c_readb(bus, devaddr, addr, ONLP_I2C_F_FORCE);
}

/**
 * onlp_sfpi_dev_writeb - Write a byte to an I2C device on an SFP port's bus.
 *
 * Performs a raw byte write to a specified I2C device address and
 * register address on the I2C bus associated with the given SFP port.
 *
 * @port: The SFP port number, used to determine the I2C bus.
 * @devaddr: The 7-bit I2C device address.
 * @addr: The register address within the I2C device.
 * @value: The byte value to write.
 *
 * @return 0 on success, or a negative value on error.
 */
int
onlp_sfpi_dev_writeb(int port, uint8_t devaddr, uint8_t addr, uint8_t value)
{
    int bus = PORT_BUS_INDEX(port);
    return onlp_i2c_writeb(bus, devaddr, addr, value, ONLP_I2C_F_FORCE);
}

/**
 * onlp_sfpi_dev_readw - Read a word from an I2C device on an SFP port's bus.
 *
 * Performs a raw word (16-bit) read from a specified I2C device address
 * and register address on the I2C bus associated with the given SFP port.
 *
 * @port: The SFP port number, used to determine the I2C bus.
 * @devaddr: The 7-bit I2C device address.
 * @addr: The register address within the I2C device.
 *
 * @return The read word value (0-65535) on success, or a negative value on error.
 */
int
onlp_sfpi_dev_readw(int port, uint8_t devaddr, uint8_t addr)
{
    int bus = PORT_BUS_INDEX(port);
    return onlp_i2c_readw(bus, devaddr, addr, ONLP_I2C_F_FORCE);
}

/**
 * onlp_sfpi_dev_writew - Write a word to an I2C device on an SFP port's bus.
 *
 * Performs a raw word (16-bit) write to a specified I2C device address
 * and register address on the I2C bus associated with the given SFP port.
 *
 * @port: The SFP port number, used to determine the I2C bus.
 * @devaddr: The 7-bit I2C device address.
 * @addr: The register address within the I2C device.
 * @value: The word value to write.
 *
 * @return 0 on success, or a negative value on error.
 */
int
onlp_sfpi_dev_writew(int port, uint8_t devaddr, uint8_t addr, uint16_t value)
{
    int bus = PORT_BUS_INDEX(port);
    return onlp_i2c_writew(bus, devaddr, addr, value, ONLP_I2C_F_FORCE);
}

/**
 * onlp_sfpi_denit - De-initialize the SFP interface.
 *
 * This function is called during platform shutdown. It can be used to
 * release any resources acquired by onlp_sfpi_init. In this specific
 * implementation, it does nothing.
 *
 * @return ONLP_STATUS_OK on success.
 */
int
onlp_sfpi_denit(void)
{
    return ONLP_STATUS_OK;
}
