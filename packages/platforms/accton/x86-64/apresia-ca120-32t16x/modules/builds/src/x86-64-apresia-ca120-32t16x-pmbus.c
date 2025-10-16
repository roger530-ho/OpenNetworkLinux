// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CA120-32T16X Power Management Bus (PMBus).
 *
 * Copyright (C) 2025 Accton Technology Corporation.
 *
 * Based on ad7414.c
 * Copyright 2006 Stefan Roese <sr at denx.de>, DENX Software Engineering
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/version.h>

#define DRVNAME "ca120-32t16x-pmbus"

/* STATUS_WORD bits we care about (PMBus) */
#define PMBUS_STATUS_PG_N      0x0800  /* POWER_GOOD_N, 1 = not good */
#define PMBUS_STATUS_POWER_OFF 0x0040  /* bit set -> power off */

#define MAX_FAN_DUTY_CYCLE      100
#define I2C_RW_RETRY_COUNT      10
#define I2C_RW_RETRY_INTERVAL   60 /* ms */

/* Set to 0 if I2C_FUNC_SMBUS_I2C_BLOCK is not supported */
static int support_i2c_block = 1;

/* PMBus ASCII command codes used by this driver */
#define PMBUS_MFR_ID        0x99
#define PMBUS_MFR_MODEL     0x9A
#define PMBUS_MFR_REVISION  0x9B /* not always used here, but common */
#define PMBUS_MFR_SERIAL    0x9E

/* Addresses scanned */
static const unsigned short normal_i2c[] = { I2C_CLIENT_END };

enum chips {
    PMBUS_G1394,
};

/**
 * struct ca120_32t16x_pmbus_data - Driver private data for each client
 * @hwmon_dev:	  Pointer to the registered hwmon device.
 * @update_lock:  Mutex to protect access to the data.
 * @valid:	  True if the data is valid.
 * @last_updated: Jiffies time of last update.
 * @client:	  Back pointer to the i2c client.
 * @chip:	  Chip identifier from enum chips.
 * @capability:	  Cached value of CAPABILITY register.
 * @status_word:  Cached value of STATUS_WORD register.
 * @fan_fault:	  Cached value of fan fault status register.
 * @v_in:	  Cached value of input voltage.
 * @i_in:	  Cached value of input current.
 * @p_in:	  Cached value of input power.
 * @v_out:	  Cached value of output voltage.
 * @i_out:	  Cached value of output current.
 * @p_out:	  Cached value of output power.
 * @vout_mode:	  Cached value of VOUT_MODE register.
 * @temp:	  Array of cached temperature values.
 * @fan_speed:	  Cached value of fan speed.
 * @fan_dir:	  Array of cached fan direction values.
 * @pmbus_revision: Cached PMBus revision.
 * @mfr_id:	  Cached manufacturer ID string.
 * @mfr_model:	  Cached manufacturer model string.
 * @mfr_revsion:  Cached manufacturer revision string.
 * @mfr_serial:	  Cached manufacturer serial number string.
 * @mfr_vin_min:  Cached manufacturer minimum input voltage.
 * @mfr_vin_max:  Cached manufacturer maximum input voltage.
 * @mfr_iin_max:  Cached manufacturer maximum input current.
 * @mfr_iout_max: Cached manufacturer maximum output current.
 * @mfr_pin_max:  Cached manufacturer maximum input power.
 * @mfr_pout_max: Cached manufacturer maximum output power.
 * @mfr_vout_min: Cached manufacturer minimum output voltage.
 * @mfr_vout_max: Cached manufacturer maximum output voltage.
 */
struct ca120_32t16x_pmbus_data {
    struct device     *hwmon_dev;
    struct mutex        update_lock;
    bool                 valid;
    unsigned long      last_updated;
    struct i2c_client  *client;
    u8   chip;
    u8   capability;
    u16  status_word;
    u8   fan_fault;
    u16  v_in;
    u16  i_in;
    u16  p_in;
    u16  v_out;
    u16  i_out;
    u16  p_out;
    u8   vout_mode;
    u16  temp[3];
    u16  fan_speed;
    u8   fan_dir[5];
    u8   pmbus_revision;
    u8   mfr_id[10];
    u8   mfr_model[15];
    u8   mfr_revsion[3];
    u8   mfr_serial[21];
    u16  mfr_vin_min;
    u16  mfr_vin_max;
    u16  mfr_iin_max;
    u16  mfr_iout_max;
    u16  mfr_pin_max;
    u16  mfr_pout_max;
    u16  mfr_vout_min;
    u16  mfr_vout_max;
};
static ssize_t show_word(struct device *dev, struct device_attribute *da,
             char *buf);
static ssize_t show_linear(struct device *dev, struct device_attribute *da,
             char *buf);
static ssize_t show_vout(struct device *dev, struct device_attribute *da, char *buf);
static ssize_t show_fan_fault(struct device *dev, struct device_attribute *da,
             char *buf);
static ssize_t show_ascii(struct device *dev, struct device_attribute *da,
             char *buf);
static struct ca120_32t16x_pmbus_data *ca120_32t16x_pmbus_update_device(struct device *dev);
static int ca120_32t16x_pmbus_read_block(struct i2c_client *client, u8 command, u8 *data, int data_len);
static int ca120_32t16x_pmbus_read_ascii(struct i2c_client *client, u8 command, u8 *dst, size_t dstsz);

enum ca120_32t16x_pmbus_sysfs_attributes {
    PSU_POWER_ON = 0,
    PSU_POWER_GOOD,
    PSU_FAN1_FAULT,
    PSU_V_IN,
    PSU_I_IN,
    PSU_P_IN,
    PSU_V_OUT,
    PSU_I_OUT,
    PSU_P_OUT,
    PSU_TEMP1_INPUT,
    PSU_TEMP2_INPUT,
    PSU_TEMP3_INPUT,
    PSU_FAN1_SPEED,
    PSU_MFR_ID,
    PSU_MFR_MODEL,
    PSU_MFR_SERIAL,
    PSU_MFR_VIN_MIN,
    PSU_MFR_VIN_MAX,
    PSU_MFR_VOUT_MIN,
    PSU_MFR_VOUT_MAX,
    PSU_MFR_IIN_MAX,
    PSU_MFR_IOUT_MAX,
    PSU_MFR_PIN_MAX,
    PSU_MFR_POUT_MAX
};

/* sysfs attributes for hwmon */
static SENSOR_DEVICE_ATTR(psu_power_on,     S_IRUGO, show_word,   NULL, PSU_POWER_ON);
static SENSOR_DEVICE_ATTR(psu_power_good,   S_IRUGO, show_word,   NULL, PSU_POWER_GOOD);
static SENSOR_DEVICE_ATTR(psu_fan1_fault,   S_IRUGO, show_fan_fault, NULL, PSU_FAN1_FAULT);
static SENSOR_DEVICE_ATTR(psu_v_in,     S_IRUGO, show_linear,   NULL, PSU_V_IN);
static SENSOR_DEVICE_ATTR(psu_i_in,     S_IRUGO, show_linear,   NULL, PSU_I_IN);
static SENSOR_DEVICE_ATTR(psu_p_in,     S_IRUGO, show_linear,   NULL, PSU_P_IN);
static SENSOR_DEVICE_ATTR(psu_v_out,        S_IRUGO, show_vout,     NULL, PSU_V_OUT);
static SENSOR_DEVICE_ATTR(psu_i_out,        S_IRUGO, show_linear,   NULL, PSU_I_OUT);
static SENSOR_DEVICE_ATTR(psu_p_out,        S_IRUGO, show_linear,   NULL, PSU_P_OUT);
static SENSOR_DEVICE_ATTR(psu_temp1_input,  S_IRUGO, show_linear,   NULL, PSU_TEMP1_INPUT);
static SENSOR_DEVICE_ATTR(psu_temp2_input,  S_IRUGO, show_linear,   NULL, PSU_TEMP2_INPUT);
static SENSOR_DEVICE_ATTR(psu_temp3_input,  S_IRUGO, show_linear,   NULL, PSU_TEMP3_INPUT);
static SENSOR_DEVICE_ATTR(psu_fan1_speed_rpm, S_IRUGO, show_linear, NULL, PSU_FAN1_SPEED);
static SENSOR_DEVICE_ATTR(psu_mfr_id,       S_IRUGO, show_ascii,  NULL, PSU_MFR_ID);
static SENSOR_DEVICE_ATTR(psu_mfr_model,    S_IRUGO, show_ascii,  NULL, PSU_MFR_MODEL);
static SENSOR_DEVICE_ATTR(psu_mfr_serial,   S_IRUGO, show_ascii, NULL, PSU_MFR_SERIAL);
static SENSOR_DEVICE_ATTR(psu_mfr_vin_min,  S_IRUGO, show_linear, NULL, PSU_MFR_VIN_MIN);
static SENSOR_DEVICE_ATTR(psu_mfr_vin_max,  S_IRUGO, show_linear, NULL, PSU_MFR_VIN_MAX);
static SENSOR_DEVICE_ATTR(psu_mfr_vout_min, S_IRUGO, show_vout, NULL, PSU_MFR_VOUT_MIN);
static SENSOR_DEVICE_ATTR(psu_mfr_vout_max, S_IRUGO, show_vout, NULL, PSU_MFR_VOUT_MAX);
static SENSOR_DEVICE_ATTR(psu_mfr_iin_max,  S_IRUGO, show_linear, NULL, PSU_MFR_IIN_MAX);
static SENSOR_DEVICE_ATTR(psu_mfr_iout_max, S_IRUGO, show_linear, NULL, PSU_MFR_IOUT_MAX);
static SENSOR_DEVICE_ATTR(psu_mfr_pin_max,  S_IRUGO, show_linear, NULL, PSU_MFR_PIN_MAX);
static SENSOR_DEVICE_ATTR(psu_mfr_pout_max, S_IRUGO, show_linear, NULL, PSU_MFR_POUT_MAX);

static struct attribute *ca120_32t16x_pmbus_attributes[] = {
    &sensor_dev_attr_psu_power_on.dev_attr.attr,
    &sensor_dev_attr_psu_power_good.dev_attr.attr,
    &sensor_dev_attr_psu_fan1_fault.dev_attr.attr,
    &sensor_dev_attr_psu_v_in.dev_attr.attr,
    &sensor_dev_attr_psu_i_in.dev_attr.attr,
    &sensor_dev_attr_psu_p_in.dev_attr.attr,
    &sensor_dev_attr_psu_v_out.dev_attr.attr,
    &sensor_dev_attr_psu_i_out.dev_attr.attr,
    &sensor_dev_attr_psu_p_out.dev_attr.attr,
    &sensor_dev_attr_psu_temp1_input.dev_attr.attr,
    &sensor_dev_attr_psu_temp2_input.dev_attr.attr,
    &sensor_dev_attr_psu_temp3_input.dev_attr.attr,
    &sensor_dev_attr_psu_fan1_speed_rpm.dev_attr.attr,
    &sensor_dev_attr_psu_mfr_id.dev_attr.attr,
    &sensor_dev_attr_psu_mfr_model.dev_attr.attr,
    &sensor_dev_attr_psu_mfr_serial.dev_attr.attr,
    &sensor_dev_attr_psu_mfr_vin_min.dev_attr.attr,
    &sensor_dev_attr_psu_mfr_vin_max.dev_attr.attr,
    &sensor_dev_attr_psu_mfr_pout_max.dev_attr.attr,
    &sensor_dev_attr_psu_mfr_iin_max.dev_attr.attr,
    &sensor_dev_attr_psu_mfr_pin_max.dev_attr.attr,
    &sensor_dev_attr_psu_mfr_vout_min.dev_attr.attr,
    &sensor_dev_attr_psu_mfr_vout_max.dev_attr.attr,
    &sensor_dev_attr_psu_mfr_iout_max.dev_attr.attr,
    NULL
};

/**
 * show_word() - Sysfs callback to show word-based status values.
 * @dev:  Device pointer.
 * @da:   Device attribute pointer.
 * @buf:  Buffer to write the value to.
 *
 * This function reads from the cached status_word and returns a boolean
 * value based on the specific attribute being requested.
 *
 * @return: Number of bytes written to buf, or 0 on failure.
 */
static ssize_t show_word(struct device *dev, struct device_attribute *da,
             char *buf)
{
    struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
    struct ca120_32t16x_pmbus_data *data = ca120_32t16x_pmbus_update_device(dev);
    u16 status = 0;

    if (!data || !data->valid) {
        return 0;
    }

    switch (attr->index) {
    case PSU_POWER_ON: /* psu_power_on, low byte bit 6 of status_word, 0=>ON, 1=>OFF */
        /* bit set -> OFF */
        status = (data->status_word & PMBUS_STATUS_POWER_OFF) ? 0 : 1;
        break;  
    case PSU_POWER_GOOD: /* psu_power_good, high byte bit 3 of status_word, 0=>OK, 1=>FAIL */
         /* POWER_GOOD_N: 1 -> not good */
        status = (data->status_word & PMBUS_STATUS_PG_N) ? 0 : 1;
        break;
    default:
        return 0;
    }

    return sprintf(buf, "%d\n", status);
}

/**
 * two_complement_to_int() - Convert two's complement to integer.
 * @data:      The raw data containing the two's complement value.
 * @valid_bit: The number of valid bits for the value.
 * @mask:      The mask to extract the valid data bits.
 *
 * @return: The converted integer value.
 */
static int two_complement_to_int(u16 data, u8 valid_bit, int mask)
{
    u16  valid_data  = data & mask;
    bool is_negative = valid_data >> (valid_bit - 1);

    return is_negative ? (-(((~valid_data) & mask) + 1)) : valid_data;
}

/**
 * show_linear() - Sysfs callback for values in LINEAR11 format.
 * @dev:  Device pointer.
 * @da:   Device attribute pointer.
 * @buf:  Buffer to write the value to.
 *
 * This function decodes a value from LINEAR11 format (a 16-bit word with a
 * 5-bit exponent and 11-bit mantissa) and returns it as a string.
 *
 * @return: Number of bytes written to buf, or 0 on failure.
 */
static ssize_t show_linear(struct device *dev, struct device_attribute *da,
             char *buf)
{
    struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
    struct ca120_32t16x_pmbus_data *data = ca120_32t16x_pmbus_update_device(dev);

    u16 value = 0;
    int exponent, mantissa;
    int multiplier = 1000;

    if (!data || !data->valid) {
        return 0;
    }

    switch (attr->index) {
    case PSU_V_IN:
        value = data->v_in;
        break;
    case PSU_I_IN:
        value = data->i_in;
        break;
    case PSU_P_IN:
        value = data->p_in;
        break;
    case PSU_V_OUT:
        value = data->v_out;
        break;
    case PSU_I_OUT:
        value = data->i_out;
        break;
    case PSU_P_OUT:
        value = data->p_out;
        break;
    case PSU_TEMP1_INPUT:
    case PSU_TEMP2_INPUT:
    case PSU_TEMP3_INPUT:
        value = data->temp[attr->index-PSU_TEMP1_INPUT];
        break;
    case PSU_FAN1_SPEED:
        value = data->fan_speed;
        multiplier = 1;
        break;
    case PSU_MFR_VIN_MIN:
        value = data->mfr_vin_min;
        break;
    case PSU_MFR_VIN_MAX:
        value = data->mfr_vin_max;
        break;
    case PSU_MFR_VOUT_MIN:
        value = data->mfr_vout_min;
        break;
    case PSU_MFR_VOUT_MAX:
        value = data->mfr_vout_max;
        break;
    case PSU_MFR_PIN_MAX:
        value = data->mfr_pin_max;
        break;
    case PSU_MFR_POUT_MAX:
        value = data->mfr_pout_max;
        break;
    case PSU_MFR_IOUT_MAX:
        value = data->mfr_iout_max;
        break;
    case PSU_MFR_IIN_MAX:
        value = data->mfr_iin_max;
        break;
    default:
        return 0;
    }

    exponent = two_complement_to_int(value >> 11, 5, 0x1f);
    mantissa = two_complement_to_int(value & 0x7ff, 11, 0x7ff);

    return (exponent >= 0) ? sprintf(buf, "%d\n", (mantissa << exponent) * multiplier) :
                             sprintf(buf, "%d\n", (mantissa * multiplier) / (1 << -exponent));
}

/**
 * show_fan_fault() - Sysfs callback to show fan fault status.
 * @dev:  Device pointer.
 * @da:   Device attribute pointer.
 * @buf:  Buffer to write the value to.
 *
 * @return: Number of bytes written to buf, or 0 on failure.
 */
static ssize_t show_fan_fault(struct device *dev, struct device_attribute *da,
             char *buf)
{
    struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
    struct ca120_32t16x_pmbus_data *data = ca120_32t16x_pmbus_update_device(dev);
    u8 shift;

    if (!data->valid) {
        return 0;
    }

    shift = (attr->index == PSU_FAN1_FAULT) ? 7 : 6;

    return sprintf(buf, "%d\n", (data->fan_fault >> shift) & 0x1);
}

/**
 * show_ascii() - Sysfs callback to show ASCII string values.
 * @dev:  Device pointer.
 * @da:   Device attribute pointer.
 * @buf:  Buffer to write the value to.
 *
 * This function reads from cached manufacturer data which is stored as a
 * length-prefixed string and returns the null-terminated string content.
 *
 * @return: Number of bytes written to buf, or 0 on failure.
 */
static ssize_t show_ascii(struct device *dev, struct device_attribute *da,
             char *buf)
{
    struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
    struct ca120_32t16x_pmbus_data *data = ca120_32t16x_pmbus_update_device(dev);
    u8 *ptr = NULL; /* points to string content after count byte */

    if (!data || !data->valid) {
        return 0;
    }

    switch (attr->index) {  
    case PSU_MFR_ID: /* psu_mfr_id */
        ptr = data->mfr_id + 1; /* first byte is count */
        break;
    case PSU_MFR_MODEL: /* psu_mfr_model */
        ptr = data->mfr_model + 1; /* first byte is count */
        break;  
    case PSU_MFR_SERIAL: /* psu_mfr_serial */
        ptr = data->mfr_serial + 1; /* first byte is count */
        break;
    default:
        return 0;
    }

    return sprintf(buf, "%s\n", ptr);
}

/**
 * show_vout_by_mode() - Sysfs callback for VOUT values based on VOUT_MODE.
 * @dev:  Device pointer.
 * @da:   Device attribute pointer.
 * @buf:  Buffer to write the value to.
 *
 * This function decodes a VOUT value using the exponent from the VOUT_MODE
 * register, which is a PMBus standard way of representing output voltage.
 *
 * @return: Number of bytes written to buf, or 0 on failure.
 */
static ssize_t show_vout_by_mode(struct device *dev, struct device_attribute *da,
             char *buf)
{
    struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
    struct ca120_32t16x_pmbus_data *data = ca120_32t16x_pmbus_update_device(dev);
    int exponent, mantissa;
    int multiplier = 1000;

    if (!data || !data->valid) {
        return 0;
    }

    exponent = two_complement_to_int(data->vout_mode, 5, 0x1f);
    switch (attr->index) {
    case PSU_MFR_VOUT_MIN:
        mantissa = data->mfr_vout_min;
        break;
    case PSU_MFR_VOUT_MAX:
        mantissa = data->mfr_vout_max;
        break;
    case PSU_V_OUT:
        mantissa = data->v_out;
        break;
    default:
        return 0;
    }

    return (exponent > 0) ? sprintf(buf, "%d\n", (mantissa << exponent) * multiplier) :
                            sprintf(buf, "%d\n", (mantissa * multiplier) / (1 << -exponent));
}

/**
 * show_vout() - Sysfs callback dispatcher for VOUT.
 * @dev:  Device pointer.
 * @da:   Device attribute pointer.
 * @buf:  Buffer to write the value to.
 *
 * This function checks the chip type and calls the appropriate VOUT decoding
 * function (either standard LINEAR11 or VOUT_MODE based).
 *
 * @return: Result of the called show function.
 */
static ssize_t show_vout(struct device *dev, struct device_attribute *da,
             char *buf)
{
    struct ca120_32t16x_pmbus_data *data = dev_get_drvdata(dev);

    if (data && data->chip == PMBUS_G1394) {
        return show_vout_by_mode(dev, da, buf);
    }
    else {
        return show_linear(dev, da, buf);
    }
}

static const struct attribute_group ca120_32t16x_pmbus_group = {
    .attrs = ca120_32t16x_pmbus_attributes,
};

static const struct attribute_group *ca120_32t16x_pmbus_groups[] = {
    &ca120_32t16x_pmbus_group,
    NULL
};

/**
 * ca120_32t16x_pmbus_probe() - Probe function for the I2C driver.
 * @client: The I2C client device.
 * @dev_id: The I2C device ID.
 *
 * This is called when the I2C core finds a device that matches our driver.
 * It allocates driver data, initializes it, and registers the hwmon device
 * with its sysfs attributes.
 *
 * @return: 0 on success, or a negative errno on failure.
 */
static int ca120_32t16x_pmbus_probe(struct i2c_client *client,
            const struct i2c_device_id *dev_id)
{
    struct ca120_32t16x_pmbus_data *data;
    int status;

    if (!i2c_check_functionality(client->adapter,
        I2C_FUNC_SMBUS_BYTE_DATA |
        I2C_FUNC_SMBUS_WORD_DATA )) {
        status = -EIO;
        goto exit;
    }

    if (!i2c_check_functionality(client->adapter,
        I2C_FUNC_SMBUS_I2C_BLOCK)) {
        support_i2c_block = 0;
    }

    data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
    if (!data) {
        status = -ENOMEM;
        goto exit;
    }

    i2c_set_clientdata(client, data);
    mutex_init(&data->update_lock);
    data->client = client;
    data->chip = dev_id->driver_data;
    dev_info(&client->dev, "chip found\n");

    data->hwmon_dev = devm_hwmon_device_register_with_groups(&client->dev, "ca120_32t16x_pmbus",
                                                             data, ca120_32t16x_pmbus_groups);
    if (IS_ERR(data->hwmon_dev)) {
        status = PTR_ERR(data->hwmon_dev);
        goto exit;
    }

    dev_info(&client->dev, "%s: psu '%s'\n",
         dev_name(data->hwmon_dev), client->name);

    return 0;

exit:

    return status;
}

/**
 * ca120_32t16x_pmbus_remove() - Remove function for the I2C driver.
 * @client: The I2C client device.
 *
 * All resources are devm-managed, so there is nothing to do here.
 *
 * @return: 0 always.
 */
static int ca120_32t16x_pmbus_remove(struct i2c_client *client)
{
    /* All resources are devm-managed now. Nothing to do. */
    return 0;
}

static const struct i2c_device_id ca120_32t16x_pmbus_id[] = {
    { "g1394", PMBUS_G1394 },
    {}
};
MODULE_DEVICE_TABLE(i2c, ca120_32t16x_pmbus_id);

static struct i2c_driver ca120_32t16x_pmbus_driver = {
    .class      = I2C_CLASS_HWMON,
    .driver = {
        .name   = DRVNAME,
    },
    .probe    = ca120_32t16x_pmbus_probe,
    .remove   = ca120_32t16x_pmbus_remove,
    .id_table = ca120_32t16x_pmbus_id,
    .address_list = normal_i2c,
};

/**
 * ca120_32t16x_pmbus_read_byte() - I2C byte read with retry logic.
 * @client: The I2C client device.
 * @reg:    The register to read from.
 *
 * @return: The byte read on success, or a negative errno on failure.
 */
static int ca120_32t16x_pmbus_read_byte(struct i2c_client *client, u8 reg)
{
    int status = 0, retry = I2C_RW_RETRY_COUNT;

    while (retry) {
        status = i2c_smbus_read_byte_data(client, reg);
        if (unlikely(status < 0)) {
            msleep(I2C_RW_RETRY_INTERVAL);
            retry--;
            continue;
        }

        break;
    }

    return status;
}

/**
 * ca120_32t16x_pmbus_read_word() - I2C word read with retry logic.
 * @client: The I2C client device.
 * @reg:    The register to read from.
 *
 * @return: The word read on success, or a negative errno on failure.
 */
static int ca120_32t16x_pmbus_read_word(struct i2c_client *client, u8 reg)
{
    int status = 0, retry = I2C_RW_RETRY_COUNT;

    while (retry) {
        status = i2c_smbus_read_word_data(client, reg);
        if (unlikely(status < 0)) {
            msleep(I2C_RW_RETRY_INTERVAL);
            retry--;
            continue;
        }

        break;
    }

    return status;
}

/**
 * ca120_32t16x_pmbus_read_block() - I2C block read with retry logic.
 * @client:   The I2C client device.
 * @command:  The command/register to read from.
 * @data:     Buffer to store the read data.
 * @data_len: The number of bytes to read.
 *
 * @return: The number of bytes read on success, or a negative errno.
 */
static int ca120_32t16x_pmbus_read_block(struct i2c_client *client, u8 command, u8 *data,
              int data_len)
{
    int status = 0, retry = I2C_RW_RETRY_COUNT;

    while (retry) {
        status = i2c_smbus_read_i2c_block_data(client, command, data_len, data);
        if (unlikely(status < 0)) {
            msleep(I2C_RW_RETRY_INTERVAL);
            retry--;
            continue;
        }

        break;
    }

    return status;
}

/**
 * ca120_32t16x_pmbus_read_ascii() - Safe SMBus block read for PMBus ASCII strings.
 * @client:  I2C client
 * @command: PMBus command code (e.g. 0x99 MFR_ID)
 * @dst:     Destination buffer (contains count byte at [0], payload at [1..])
 * @dstsz:   Size of @dst in bytes
 *
 * This helper reads the first length byte, clamps the read size to @dst
 * capacity (leaving one byte for NUL), then reads the block data and
 * ensures the resulting string is NUL-terminated.
 *
 * @return: 0 on success, or a negative errno.
 */
static int ca120_32t16x_pmbus_read_ascii(struct i2c_client *client, u8 command, u8 *dst, size_t dstsz)
{
    int status;
    u8 count = 0;
    int need;

    if (!dst || dstsz < 2)
        return -EINVAL;

    /* Read first byte to determine the block length */
    status = ca120_32t16x_pmbus_read_block(client, command, &count, 1);
    if (status < 0)
        return status;

    /* Need (count + 1) bytes including the count byte at offset 0 */
    need = count + 1;
    if (need > dstsz - 1)
        need = dstsz - 1; /* keep space for trailing NUL */

    status = ca120_32t16x_pmbus_read_block(client, command, dst, need);
    if (status < 0)
        return status;

    dst[need] = '\0';
    return 0;
}

struct reg_data_byte {
    u8   reg;
    u8  *value;
};

struct reg_data_word {
    u8   reg;
    u16 *value;
};

/**
 * ca120_32t16x_pmbus_update_device() - Update the cached data from the device.
 * @dev: The device.
 *
 * This function reads all the sensor values from the I2C device. It is
 * called by the sysfs show functions. The data is cached for 2.5 seconds.
 *
 * @return: A pointer to the updated driver data, or NULL on failure.
 */
static struct ca120_32t16x_pmbus_data *ca120_32t16x_pmbus_update_device(struct device *dev)
{
    struct ca120_32t16x_pmbus_data *data = dev_get_drvdata(dev);
    struct i2c_client *client = NULL;

    if (!data)
        return NULL;
    client = data->client;

    mutex_lock(&data->update_lock);

    if (time_after(jiffies, data->last_updated + HZ + HZ / 2)
        || !data->valid) {
        int i, status, length;
        bool pg_ok = false;
        u8 command, buf;
        struct reg_data_byte regs_byte[] = { {0x19, &data->capability},
                                             {0x20, &data->vout_mode},
                                             {0x81, &data->fan_fault},
                                             {0x98, &data->pmbus_revision}};
        struct reg_data_word regs_word[] = { {0x79, &data->status_word},
                                             {0x88, &data->v_in},
                                             {0x8b, &data->v_out},
                                             {0x89, &data->i_in},
                                             {0x8c, &data->i_out},
                                             {0x97, &data->p_in},
                                             {0x96, &data->p_out},
                                             {0x8d, &(data->temp[0])},
                                             {0x8e, &(data->temp[1])},
                                             {0x8f, &(data->temp[2])},
                                             {0x90, &data->fan_speed},
                                             {0xa0, &data->mfr_vin_min},
                                             {0xa1, &data->mfr_vin_max},
                                             {0xa2, &data->mfr_iin_max},
                                             {0xa3, &data->mfr_pin_max},
                                             {0xa4, &data->mfr_vout_min},
                                             {0xa5, &data->mfr_vout_max},
                                             {0xa6, &data->mfr_iout_max},
                                             {0xa7, &data->mfr_pout_max}};

        dev_dbg(&client->dev, "Starting pmbus update\n");
        data->valid = 0;

        /* Always read STATUS_WORD first (0x79) */
        status = ca120_32t16x_pmbus_read_word(client, 0x79);
        if (status < 0) {
            dev_dbg(&client->dev, "reg %d, err %d\n", 0x79, status);
            goto exit;
        } else {
            data->status_word = status;
        }

        /*
         * Decide if we should read the remaining word registers:
         * - PMBUS_STATUS_PG_N (bit means "not good"): must be 0
         * - PMBUS_STATUS_POWER_OFF: must be 0 (i.e., power is ON)
         */
        pg_ok = !(data->status_word & PMBUS_STATUS_PG_N) &&
                !(data->status_word & PMBUS_STATUS_POWER_OFF);


        /* Read byte data */
        for (i = 0; i < ARRAY_SIZE(regs_byte); i++) {
            status = ca120_32t16x_pmbus_read_byte(client, regs_byte[i].reg);

            if (status < 0) {
                dev_dbg(&client->dev, "reg 0x%02x, err %d\n",
                        regs_byte[i].reg, status);
                goto exit;
            }
            else {
                *(regs_byte[i].value) = status;
            }
        }

        if (pg_ok) {
            /* Read word data only when POWER_GOOD is asserted */
            for (i = 0; i < ARRAY_SIZE(regs_word); i++) {
                status = ca120_32t16x_pmbus_read_word(client, regs_word[i].reg);
                if (status < 0) {
                    dev_dbg(&client->dev, "reg 0x%02x, err %d\n",
                            regs_word[i].reg, status);
                    goto exit;
                } else {
                    *(regs_word[i].value) = status;
                }
            }
        } else {
            dev_dbg(&client->dev, "POWER_GOOD not asserted; skip reading remaining word registers\n");
            /*
             * PG is NOT OK:
             * Clear cached word registers to avoid showing stale data.
             * These are exactly the fields that would be updated only when PG is OK.
             */
            for (i = 0; i < ARRAY_SIZE(regs_word); i++) {
                if (regs_word[i].value) {
                    *(regs_word[i].value) = 0;
                }
            }
        }

        if (support_i2c_block) {
            /* Read MFR_ID (0x99), MFR_MODEL (0x9A), MFR_SERIAL (0x9E) */
            status = ca120_32t16x_pmbus_read_ascii(client, PMBUS_MFR_ID,
                                            data->mfr_id, ARRAY_SIZE(data->mfr_id));
            if (status < 0) {
                dev_dbg(&client->dev, "reg 0x%02x, err %d\n", PMBUS_MFR_ID, status);
                goto exit;
            }

            /* Read mfr_model (0x9A) */
            command = PMBUS_MFR_MODEL;
            length  = 1;

            /* Read first byte to determine the length of data */
            status = ca120_32t16x_pmbus_read_block(client, command, &buf, length);
            if (status < 0) {
                dev_dbg(&client->dev, "reg 0x%02x, err %d\n", command, status);
                goto exit;
            }

            /*
             * Clamp to buffer capacity (count byte + content), keep one byte for NUL.
             */
            length = buf + 1;
            if (length > (int)ARRAY_SIZE(data->mfr_model) - 1)
                length = (int)ARRAY_SIZE(data->mfr_model) - 1;
            status = ca120_32t16x_pmbus_read_block(client, command, data->mfr_model, length);
            if (status < 0) {
                dev_dbg(&client->dev, "reg 0x%02x, err %d\n", command, status);
                goto exit;
            }

            /* Read mfr_serial (0x9E) */
            command = PMBUS_MFR_SERIAL;
            length  = 1;

            /* Read first byte to determine the length of data */
            status = ca120_32t16x_pmbus_read_block(client, command, &buf, length);
            if (status < 0) {
                dev_dbg(&client->dev, "reg 0x%02x, err %d\n", command, status);
                goto exit;
            }

            /*
             * Clamp to buffer capacity (count byte + content), keep one byte for NUL.
             */
            length = buf + 1;
            if (length > (int)ARRAY_SIZE(data->mfr_serial) - 1)
                length = (int)ARRAY_SIZE(data->mfr_serial) - 1;
            status = ca120_32t16x_pmbus_read_block(client, command, data->mfr_serial, length);
            if (status < 0) {
                dev_dbg(&client->dev, "reg 0x%02x, err %d\n", command, status);
                goto exit;
            }
        }

        /* Ensure all ASCII buffers are NUL-terminated even if not supported */
        data->mfr_model[ARRAY_SIZE(data->mfr_model) - 1] = '\0';
        data->mfr_serial[ARRAY_SIZE(data->mfr_serial) - 1] = '\0';
        data->last_updated = jiffies;
        data->valid = 1;
    }

exit:
    mutex_unlock(&data->update_lock);

    return data;
}

/**
 * ca120_32t16x_pmbus_init() - Module initialization function.
 *
 * This function registers the I2C driver.
 *
 * @return: Result of i2c_add_driver().
 */
static int __init ca120_32t16x_pmbus_init(void)
{
    return i2c_add_driver(&ca120_32t16x_pmbus_driver);
}

/**
 * ca120_32t16x_pmbus_exit() - Module exit function.
 *
 * This function unregisters the I2C driver.
 */
static void __exit ca120_32t16x_pmbus_exit(void)
{
    i2c_del_driver(&ca120_32t16x_pmbus_driver);
}

MODULE_AUTHOR("Roger Ho <roger530_ho@accton.com>");
MODULE_DESCRIPTION("CA120-32T16X PMBus Driver");
MODULE_LICENSE("GPL");

module_init(ca120_32t16x_pmbus_init);
module_exit(ca120_32t16x_pmbus_exit);
