/************************************************************
 * <bsn.cl fy=2014 v=onl>
 *
 *           Copyright 2014 Big Switch Networks, Inc.
 *           Copyright 2014 Accton Technology Corporation.
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
#include <onlplib/file.h>
#include <onlp/platformi/sysi.h>
#include <onlp/platformi/ledi.h>
#include <onlp/platformi/thermali.h>
#include <onlp/platformi/fani.h>
#include <onlp/platformi/psui.h>
#include <onlp/platformi/sfpi.h>
#include "platform_lib.h"

#include "x86_64_accton_as9817_64_int.h"
#include "x86_64_accton_as9817_64_log.h"

#define NUM_OF_CPLD_VER 4
#define NUM_OF_QSFP_PORT 64
#define TEMPERATURE_COMPENSATION (5.0f)
#define BMC_FILE_RETRY_COUNT 3             // Retry count for file read/write operations
#define BMC_FILE_RETRY_DELAY_US 1000000    // Delay between retries (in microseconds, 1s)

typedef struct temp_reader_data {
    int data;
    int index;
} temp_reader_data_t;

int onlp_sysi_get_cpu_temp(temp_reader_data_t *temp);
int onlp_sysi_get_mac_temp(temp_reader_data_t *temp);
int onlp_sysi_get_xcvr_temp(temp_reader_data_t *temp);
int onlp_sysi_over_temp_protector(void);
int onlp_sysi_set_fan_duty_all(int duty);
int onlp_sysi_get_fan_status(void);

enum fan_duty_level {
    FAN_DUTY_MIN = 30,
    FAN_DUTY_MID = 60,
    FAN_DUTY_MAX = 100
};

enum temp_sensors {
    TEMP_SENSOR_CPU = 0,
    TEMP_SENSOR_MAC,
    TEMP_SENSOR_XCVR,
    TEMP_SENSOR_COUNT
};

typedef struct temp_threshold {
    int idle;
    int up_adjust;
    int down_adjust;
    int otp;
} temp_threshold_t;

typedef int (*temp_getter_t)(temp_reader_data_t *temp);
typedef int (*fan_pwm_setter_t)(int pwm);
typedef int (*fan_status_getter_t)(void);
typedef int (*ot_protector_t)(void);

typedef struct temp_handler {
    temp_getter_t    temp_readers[TEMP_SENSOR_COUNT];
    temp_threshold_t thresholds[TEMP_SENSOR_COUNT];
} temp_handler_t;

typedef struct fan_handler {
    fan_pwm_setter_t    pwm_writer;
    fan_status_getter_t status_reader;
} fan_handler_t;

/* over temp protection */
typedef struct otp_handler {
    ot_protector_t  otp_writer;
} otp_handler_t;

struct thermal_policy_manager {
    temp_handler_t  temp_hdlr;
    fan_handler_t   fan_hdlr;
    otp_handler_t   otp_hdlr; /* over temp protector */
};

struct thermal_policy_manager tp_mgr = {
    .temp_hdlr = {
        .thresholds = {
            [TEMP_SENSOR_CPU]  = { .idle = 60000, .up_adjust = 85000, .down_adjust = 75000, .otp = 100000 },
            [TEMP_SENSOR_MAC]  = { .idle = 60000, .up_adjust = 90000, .down_adjust = 80000, .otp = 105000 },
            [TEMP_SENSOR_XCVR] = { .idle = ONLP_STATUS_E_MISSING, .up_adjust = 70000, .down_adjust = 65000 }
        },
        .temp_readers = {
            [TEMP_SENSOR_CPU] = onlp_sysi_get_cpu_temp,
            [TEMP_SENSOR_MAC] = onlp_sysi_get_mac_temp,
            [TEMP_SENSOR_XCVR] = onlp_sysi_get_xcvr_temp
        }
    },
    .fan_hdlr = {
        .pwm_writer = onlp_sysi_set_fan_duty_all,
        .status_reader = onlp_sysi_get_fan_status
    },
    .otp_hdlr = {
        .otp_writer = onlp_sysi_over_temp_protector
    }
};

static char* cpld_ver_path[NUM_OF_CPLD_VER] = {
    "/sys/devices/platform/as9817_64_sys/fpga_version",  /* FPGA */
    "/sys/devices/platform/as9817_64_fpga/cpld1_version", /* CPLD-1 */
    "/sys/devices/platform/as9817_64_fpga/cpld2_version", /* CPLD-2 */
    "/sys/devices/platform/as9817_64_fan/hwmon/hwmon*/version" /* Fan CPLD */
};

const char*
onlp_sysi_platform_get(void)
{
    as9817_64_platform_id_t pid = get_platform_id();

    switch (pid) {
        case AS9817_64O: return "x86-64-accton-as9817-64o-r0";
        case AS9817_64D: return "x86-64-accton-as9817-64d-r0";
        default: break;
    }

    return "Unknown Platform";
}

int
onlp_sysi_onie_data_get(uint8_t** data, int* size)
{
    uint8_t* rdata = aim_zmalloc(256);
    if (onlp_file_read(rdata, 256, size, IDPROM_PATH) == ONLP_STATUS_OK) {
        if(*size == 256) {
            *data = rdata;
            return ONLP_STATUS_OK;
        }
    }

    aim_free(rdata);
    *size = 0;
    return ONLP_STATUS_E_INTERNAL;
}

int
onlp_sysi_oids_get(onlp_oid_t* table, int max)
{
    int i;
    onlp_oid_t* e = table;
    memset(table, 0, max*sizeof(onlp_oid_t));

    /* 7 Thermal sensors on the chassis */
    for (i = 1; i <= CHASSIS_THERMAL_COUNT; i++) {
        *e++ = ONLP_THERMAL_ID_CREATE(i);
    }

    /* 6 LEDs on the chassis */
    for (i = 1; i <= CHASSIS_LED_COUNT; i++) {
        *e++ = ONLP_LED_ID_CREATE(i);
    }

    /* 2 PSUs on the chassis */
    for (i = 1; i <= CHASSIS_PSU_COUNT; i++) {
        *e++ = ONLP_PSU_ID_CREATE(i);
    }

    /* 10 Fans on the chassis */
    for (i = 1; i <= CHASSIS_FAN_COUNT; i++) {
        *e++ = ONLP_FAN_ID_CREATE(i);
    }

    return 0;
}

int
onlp_sysi_platform_info_get(onlp_platform_info_t* pi)
{
    int i, len, ret = ONLP_STATUS_OK;
    char *v[NUM_OF_CPLD_VER] = {NULL};

    for (i = 0; i < AIM_ARRAYSIZE(cpld_ver_path); i++) {
        if (i == 3) {
            int hwmon_idx = onlp_get_fan_hwmon_idx();

            if (hwmon_idx < 0) {
                ret = ONLP_STATUS_E_INTERNAL;
                break;
            }

            len = onlp_file_read_str(&v[i], FAN_SYSFS_FORMAT_1, hwmon_idx, "version");
        }
        else {
            len = onlp_file_read_str(&v[i], cpld_ver_path[i]);
        }

        if (v[i] == NULL || len <= 0) {
            ret = ONLP_STATUS_E_INTERNAL;
            break;
        }
    }

    if (ret == ONLP_STATUS_OK) {
        pi->cpld_versions = aim_fstrdup("\r\nFPGA:%s\r\nCPLD-1:%s"
                                        "\r\nCPLD-2:%s\r\nFan CPLD:%s",
                                        v[0], v[1], v[2], v[3]);
    }

    for (i = 0; i < AIM_ARRAYSIZE(v); i++) {
        AIM_FREE_IF_PTR(v[i]);
    }

    return ret;
}

void
onlp_sysi_platform_info_free(onlp_platform_info_t* pi)
{
    aim_free(pi->cpld_versions);
}

int onlp_sysi_get_cpu_temp(temp_reader_data_t *temp)
{
    int ret;
    onlp_thermal_info_t ti;

    ret = onlp_thermali_info_get(ONLP_THERMAL_ID_CREATE(THERMAL_CPU_CORE), &ti);
    if (ret != ONLP_STATUS_OK) {
        return ret;
    }

    temp->data = ti.mcelsius;
    return ONLP_STATUS_OK;
}

/**
 * Reads a 32-bit frequency from FPGA via I2C and calculates temperature in Celsius.
 *
 * Datasheet Formula:
 *   Period = (Data + 1) x 80 ns
 *   Period = 1 / freq
 *   -> Data = (1 / freq / 80e-9) - 1
 *   -> Temp = -0.317704 x Data + 476.359
 *
 * @param clk_addr_start  I2C register address to start reading (e.g. 0xA0 for min, 0xA4 for max)
 * @param bus             I2C bus number (e.g. 9)
 * @param addr            I2C device address (e.g. 0x58)
 * @param temp            Pointer to store the resulting temperature in Celsius
 * @return                ONLP_STATUS_OK if successful;
 *                        ONLP_STATUS_E_MISSING if I2C read fails, frequency is 0,
 *                        or calculated Data is out of expected range.
 */
int get_mac_temperature_from_fpga(uint8_t clk_addr_start, int bus, uint8_t addr, float *temp)
{
    uint8_t bytes[4] = {0, 0, 0, 0};
    uint32_t freq = 0;
    int ret;
    double period, data;

    if (temp == NULL) {
        AIM_LOG_ERROR("Null pointer passed for temperature result\n");
        return ONLP_STATUS_E_MISSING;
    }

    ret = i2cget_bytes(bus, addr, clk_addr_start, bytes, sizeof(bytes));
    if (ret != ONLP_STATUS_OK) {
        AIM_LOG_ERROR("Failed to read 4-byte frequency from 0x%02X\n", clk_addr_start);
        return ONLP_STATUS_E_MISSING;
    }

    freq = ((uint32_t)bytes[0]) |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);

    if (freq == 0) {
        AIM_LOG_ERROR("Invalid frequency (0 Hz)\n");
        return ONLP_STATUS_E_MISSING;
    }

    period = 1.0 / (double)freq;
    data = (period / 80e-9) - 1.0;

    if (data < 0.0 || data > 2047.0) {
        AIM_LOG_ERROR("ADC data %.2f out of range\n", data);
        return ONLP_STATUS_E_MISSING;
    }

    *temp = (float)(-0.317704 * data + 476.359);
    return ONLP_STATUS_OK;
}

int onlp_sysi_get_mac_temp(temp_reader_data_t *temp)
{
    int ret;
    int bus = 0;
    uint8_t addr = 0x60;
    uint8_t reg_min = 0xA0;
    uint8_t reg_max = 0xA4;
    float min_temp, max_temp;

    ret = get_mac_temperature_from_fpga(reg_min, bus, addr, &min_temp);
    if (ret != ONLP_STATUS_OK) {
        return ret;
    }
    ret = get_mac_temperature_from_fpga(reg_max, bus, addr, &max_temp);
    if (ret != ONLP_STATUS_OK) {
        return ret;
    }

    temp->data = (int)(((min_temp + max_temp) / 2.0f) + TEMPERATURE_COMPENSATION);
    temp->data *= 1000;

    return ONLP_STATUS_OK;
}

int onlp_sysi_get_xcvr_presence(void)
{
    onlp_sfp_bitmap_t bitmap;
    onlp_sfp_bitmap_t_init(&bitmap);
    onlp_sfp_presence_bitmap_get(&bitmap);

    /* Ignore SFP */
    AIM_BITMAP_CLR(&bitmap, 65);
    AIM_BITMAP_CLR(&bitmap, 66);
    return !(AIM_BITMAP_COUNT(&bitmap) == 0);
}

int onlp_sysi_get_sff8436_temp(int port, int *temp)
{
    int value;
    int16_t port_temp;

    /* Read memory model */
    value = onlp_sfpi_dev_readb(port, 0x50, 0x2);
    if (value & 0x04) {
        *temp = ONLP_STATUS_E_MISSING;
        return ONLP_STATUS_OK;
    }

    value = onlp_sfpi_dev_readb(port, 0x50, 22);
    if (value < 0) {
        *temp = ONLP_STATUS_E_MISSING;
        return ONLP_STATUS_OK;
    }
    port_temp = (int16_t)((value & 0xFF) << 8);

    value = onlp_sfpi_dev_readb(port, 0x50, 23);
    if (value < 0) {
        *temp = ONLP_STATUS_E_MISSING;
        return ONLP_STATUS_OK;
    }
    port_temp = (port_temp | (int16_t)(value & 0xFF));

    *temp = (int)port_temp * 1000 / 256;
    return ONLP_STATUS_OK;
}

int onlp_sysi_get_cmis_temp(int port, int *temp)
{
    int value;
    int16_t port_temp;

    /* Read memory model */
    value = onlp_sfpi_dev_readb(port, 0x50, 0x2);
    if (value & 0x80) {
        *temp = ONLP_STATUS_E_MISSING;
        return ONLP_STATUS_OK;
    }

    value = onlp_sfpi_dev_readb(port, 0x50, 14);
    if (value < 0) {
        *temp = ONLP_STATUS_E_MISSING;
        return ONLP_STATUS_OK;
    }
    port_temp = (int16_t)((value & 0xFF) << 8);

    value = onlp_sfpi_dev_readb(port, 0x50, 15);
    if (value < 0) {
        *temp = ONLP_STATUS_E_MISSING;
        return ONLP_STATUS_OK;
    }
    port_temp = (port_temp | (int16_t)(value & 0xFF));

    *temp = (int)port_temp * 1000 / 256;
    return ONLP_STATUS_OK;
}

int onlp_sysi_get_xcvr_temp(temp_reader_data_t *temp)
{
    int ret = ONLP_STATUS_OK;
    int value, port;
    int port_temp = ONLP_STATUS_E_MISSING, max_temp = ONLP_STATUS_E_MISSING;
    int max_port = ONLP_STATUS_E_MISSING;

    temp->data = 0;
    temp->index = 0;

    if (!onlp_sysi_get_xcvr_presence()) {
        return ONLP_STATUS_OK;
    }

    for (port = 1; port <= NUM_OF_QSFP_PORT; port++) {
        if (!onlp_sfpi_is_present(port)) {
            continue;
        }

        value = onlp_sfpi_dev_readb(port, 0x50, 0);
        if (value < 0) {
            AIM_LOG_ERROR("Unable to get read port(%d) eeprom\r\n", port);
            continue;
        }

        if (value == 0x18 || value == 0x19 || value == 0x1E) {
            ret = onlp_sysi_get_cmis_temp(port, &port_temp);
            if (ret != ONLP_STATUS_OK) {
                continue;
            }
        }
        else if (value == 0x0C || value == 0x0D || value == 0x11 || value ==  0xE1) {
            ret = onlp_sysi_get_sff8436_temp(port, &port_temp);
            if (ret != ONLP_STATUS_OK) {
                continue;
            }
        }
        else {
            continue;
        }

        if (port_temp > max_temp) {
            max_temp = port_temp;
            max_port = port;
        }
    }

    if (max_temp != ONLP_STATUS_E_MISSING && 
        max_port != ONLP_STATUS_E_MISSING) {
        temp->data = max_temp;
        temp->index = max_port;
    }

    return ONLP_STATUS_OK;
}

int onlp_sysi_reset_front_port(void)
{
    int port, ret;

    for (port = 1; port <= NUM_OF_QSFP_PORT; port++) {
        if (ONLP_STATUS_OK != onlp_sfpi_control_set(port, ONLP_SFP_CONTROL_RESET_STATE, 1)) {
            ret = ONLP_STATUS_E_INTERNAL;
        }
    }

    return ret;
}

int onlp_sysi_over_temp_protector(void)
{
    AIM_SYSLOG_CRIT("Temperature critical", "Temperature critical",
                    "Alarm for temperature critical is detected; performing OTP protect action!");
    system("sync;sync;sync");
    onlp_sysi_reset_front_port();
    onlp_file_write_int(1, "/sys/devices/platform/as9817_64_sys/otp_protect");
    return ONLP_STATUS_OK;
}

int onlp_sysi_set_fan_duty_all(int duty)
{
    int fid, ret = ONLP_STATUS_OK;

    for (fid = 1; fid <= CHASSIS_FAN_COUNT; fid++) {
        if (ONLP_STATUS_OK != onlp_fani_percentage_set(ONLP_FAN_ID_CREATE(fid), duty)) {
            ret = ONLP_STATUS_E_INTERNAL;
        }
    }

    return ret;
}

int onlp_sysi_get_fan_status(void)
{
    int i, ret;
    onlp_fan_info_t fi[CHASSIS_FAN_COUNT];
    memset(fi, 0, sizeof(fi));

    for (i = 0; i < CHASSIS_FAN_COUNT; i++) {
        ret = onlp_fani_info_get(ONLP_FAN_ID_CREATE(i+1), &fi[i]);
        if (ret != ONLP_STATUS_OK) {
			AIM_LOG_ERROR("Unable to get fan(%d) status\r\n", i+1);
            return ONLP_STATUS_E_INTERNAL;
        }

        if (!(fi[i].status & ONLP_FAN_STATUS_PRESENT)) {
            AIM_LOG_ERROR("Fan(%d) is NOT present\r\n", i+1);
            return ONLP_STATUS_E_INTERNAL;
        }

        if (fi[i].status & ONLP_FAN_STATUS_FAILED) {
            AIM_LOG_ERROR("Fan(%d) is NOT operational\r\n", i+1);
            return ONLP_STATUS_E_INTERNAL;
        }
    }

    return ONLP_STATUS_OK;
}

int control_thermal_policy_via_cpu(void)
{
    int i, ret;
    temp_reader_data_t temp[TEMP_SENSOR_COUNT] = {0};
    static int fan_duty = 60;

    /* Get fan status
     * Bring fan speed to FAN_DUTY_MAX if any fan is not present or operational
     */
    if (tp_mgr.fan_hdlr.status_reader() != ONLP_STATUS_OK) {
        fan_duty = FAN_DUTY_MAX;
        tp_mgr.fan_hdlr.pwm_writer(fan_duty);
        return ONLP_STATUS_E_INTERNAL;
    }

    for (i = 0; i < AIM_ARRAYSIZE(temp); i++) {
        ret = tp_mgr.temp_hdlr.temp_readers[i](&temp[i]);
        if (ret != ONLP_STATUS_OK) {
            fan_duty = FAN_DUTY_MAX;
            tp_mgr.fan_hdlr.pwm_writer(fan_duty);
            return ret;
        }
    }

    /* Adjust fan pwm based on current temperature status */
    if (!onlp_sysi_get_xcvr_presence() &&
        temp[TEMP_SENSOR_CPU].data < tp_mgr.temp_hdlr.thresholds[TEMP_SENSOR_CPU].idle &&
        temp[TEMP_SENSOR_MAC].data < tp_mgr.temp_hdlr.thresholds[TEMP_SENSOR_MAC].idle) {
        fan_duty = FAN_DUTY_MIN;
    }
    else if (temp[TEMP_SENSOR_CPU].data > tp_mgr.temp_hdlr.thresholds[TEMP_SENSOR_CPU].up_adjust ||
             temp[TEMP_SENSOR_MAC].data > tp_mgr.temp_hdlr.thresholds[TEMP_SENSOR_MAC].up_adjust ||
             temp[TEMP_SENSOR_XCVR].data > tp_mgr.temp_hdlr.thresholds[TEMP_SENSOR_XCVR].up_adjust) {
        fan_duty = FAN_DUTY_MAX;
    }
    else if (temp[TEMP_SENSOR_CPU].data < tp_mgr.temp_hdlr.thresholds[TEMP_SENSOR_CPU].down_adjust &&
             temp[TEMP_SENSOR_MAC].data < tp_mgr.temp_hdlr.thresholds[TEMP_SENSOR_MAC].down_adjust &&
             temp[TEMP_SENSOR_XCVR].data < tp_mgr.temp_hdlr.thresholds[TEMP_SENSOR_XCVR].down_adjust) {
        fan_duty = FAN_DUTY_MID;
    }

    tp_mgr.fan_hdlr.pwm_writer(fan_duty);

    /* Handle over temp condition */
    if (temp[TEMP_SENSOR_CPU].data > tp_mgr.temp_hdlr.thresholds[TEMP_SENSOR_CPU].otp ||
        temp[TEMP_SENSOR_MAC].data > tp_mgr.temp_hdlr.thresholds[TEMP_SENSOR_MAC].otp) {
        tp_mgr.otp_hdlr.otp_writer();
    }

    return ONLP_STATUS_OK;
}

/*
 * Check if BMC thermal policy is currently enabled.
 *
 * Equivalent to running:
 *     ipmitool raw 0x34 0x66
 *
 * Returns ONLP_STATUS_OK only if the status value is 3.
 *
 * @return ONLP_STATUS_OK        if thermal policy is enabled
 *         ONLP_STATUS_E_MISSING if policy is not enabled (status != 3)
 *         ONLP_STATUS_E_INTERNAL on read failure
 */
int is_bmc_thermal_policy_enabled(void)
{
    int status;
    int ret = ONLP_STATUS_E_INTERNAL;

    for (int i = 0; i < BMC_FILE_RETRY_COUNT; i++) {
        ret = onlp_file_read_int(&status, BMC_FAN_CONTROLLER_PATH);
        if (ret == ONLP_STATUS_OK) {
            break;
        }
        usleep(BMC_FILE_RETRY_DELAY_US);
    }
    if (ret != ONLP_STATUS_OK) {
        AIM_LOG_ERROR("Failed to read %s", BMC_FAN_CONTROLLER_PATH);
        return ONLP_STATUS_E_INTERNAL;
    }

    return (status == 3) ? ONLP_STATUS_OK : ONLP_STATUS_E_MISSING;
}

/*
 * Enable the BMC thermal policy by writing "1" to the controller interface.
 *
 * Equivalent to:
 *     ipmitool raw 0x34 0x67 0 3
 *
 * @return ONLP_STATUS_OK        on success
 *         ONLP_STATUS_E_MISSING on write failure after retries
 */
int enable_bmc_thermal_policy(void)
{
    char buf[4] = "1";
    int ret = ONLP_STATUS_E_MISSING;

    for (int i = 0; i < BMC_FILE_RETRY_COUNT; i++) {
        ret = onlp_file_write_str(buf, BMC_FAN_CONTROLLER_PATH);
        if (ret == ONLP_STATUS_OK) {
            return ONLP_STATUS_OK;
        }
        usleep(BMC_FILE_RETRY_DELAY_US);
    }

    AIM_LOG_ERROR("Failed to write '%s' to %s", buf, BMC_FAN_CONTROLLER_PATH);
    return ONLP_STATUS_E_MISSING;
}

/*
 * Send thermal data (MAC temp, XCVR temp, port number) to BMC.
 *
 * This writes to the BMC thermal policy interface, equivalent to:
 *     ipmitool raw 0x34 0x13 <mac_temp> <xcvr_temp> <xcvr_num>
 *
 * Temperatures are in millidegree Celsius and converted to degrees Celsius before sending.
 *
 * @param mac_temp   MAC sensor temperature in milli-degrees Celsius
 * @param xcvr_temp  Transceiver temperature in milli-degrees Celsius
 * @param xcvr_num   Transceiver port number
 *
 * @return ONLP_STATUS_OK         on success
 *         ONLP_STATUS_E_INTERNAL if formatting fails
 *         ONLP_STATUS_E_MISSING  if writing to BMC fails
 */
int send_thermal_data_to_bmc(int mac_temp, int xcvr_temp, int xcvr_num)
{
    char data[32];
    int ret = ONLP_STATUS_E_INTERNAL;

    if (mac_temp == tp_mgr.temp_hdlr.thresholds[TEMP_SENSOR_MAC].up_adjust) {
        mac_temp = 80000;
    }
    if (xcvr_temp == 0) {
        xcvr_num = 0;
    }

    ret = snprintf(data, sizeof(data), "%d %d %d",
                   (mac_temp / 1000), (xcvr_temp / 1000), xcvr_num);
    if (ret < 0 || ret >= (int)sizeof(data)) {
        AIM_LOG_WARN("snprintf failed or truncated: mac=%d xcvr=%d port=%d (ret=%d)\n",
                     mac_temp, xcvr_temp, xcvr_num, ret);
        return ONLP_STATUS_E_INTERNAL;
    }

    for (int i = 0; i < BMC_FILE_RETRY_COUNT; i++) {
        ret = onlp_file_write_str(data, BMC_THERMAL_DATA_PATH);
        if (ret == ONLP_STATUS_OK) {
            return ONLP_STATUS_OK;
        }
        usleep(BMC_FILE_RETRY_DELAY_US);
    }

    AIM_LOG_ERROR("Failed to write '%s' to %s", data , BMC_THERMAL_DATA_PATH);
    return ONLP_STATUS_E_MISSING;
}

/*
 * Control BMC thermal policy by collecting and sending temperature data.
 *
 * This function checks if the BMC thermal policy is enabled, and enables it
 * if necessary. It then reads MAC and transceiver temperatures using the
 * registered temperature readers. If a reading fails, a fallback value is
 * used (up_adjust threshold for that sensor). Finally, the collected data 
 * is sent to the BMC.
 *
 * @return ONLP_STATUS_OK on success,
 *         ONLP_STATUS_E_MISSING if enabling policy or sending data fails.
 */
int control_thermal_policy_via_bmc(void)
{
    int i, ret;
    temp_reader_data_t temp[TEMP_SENSOR_COUNT] = {0};

    if (is_bmc_thermal_policy_enabled() != ONLP_STATUS_OK) {
        if (enable_bmc_thermal_policy() != ONLP_STATUS_OK) {
            return ONLP_STATUS_E_MISSING;
        }
    }

    for (i = 0; i < AIM_ARRAYSIZE(temp); i++) {
        ret = tp_mgr.temp_hdlr.temp_readers[i](&temp[i]);
        if (ret != ONLP_STATUS_OK) {
            temp[i].data = tp_mgr.temp_hdlr.thresholds[i].up_adjust;
        }
    }

    return send_thermal_data_to_bmc(temp[TEMP_SENSOR_MAC].data, 
                                    temp[TEMP_SENSOR_XCVR].data, 
                                    temp[TEMP_SENSOR_XCVR].index);
}

static atomic_bool is_thermal_policy_running = false;
/*
 * Background thread function to execute thermal policy logic.
 */
void *run_thermal_policy(void *arg)
{
    int ret;

    ret = control_thermal_policy_via_bmc();
    if (ret != ONLP_STATUS_OK) {
        AIM_LOG_WARN("Fallback to CPU-controlled thermal policy");
        control_thermal_policy_via_cpu();
    }

    atomic_store(&is_thermal_policy_running, false);
    return NULL;
}

/*
 * Launch thermal policy evaluation asynchronously in a background thread.
 * Ensures only one thread runs at a time.
 */
void run_thermal_policy_async(void)
{
    pthread_t tid;

    if (atomic_load(&is_thermal_policy_running)) {
        AIM_LOG_INFO("Thermal policy thread already running; skipping re-launch.");
        return;
    }

    atomic_store(&is_thermal_policy_running, true);

    /* Launch background thread to execute run_thermal_policy(). */
    if (pthread_create(&tid, NULL, run_thermal_policy, NULL) != 0) {
        AIM_LOG_ERROR("Failed to create thermal retry thread");
        atomic_store(&is_thermal_policy_running, false);
        return;
    }

    pthread_detach(tid);  /* Allow thread to clean itself up */
}

int onlp_sysi_platform_manage_fans(void)
{
    run_thermal_policy_async();

    return ONLP_STATUS_OK;
}

