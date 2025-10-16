// SPDX-License-Identifier: GPL-2.0
/*
 * CA120-32T16X FPGA/CPLD MFD Core Driver
 *
 * Copyright (C) 2025 Accton Technology Corporation
 *
 *
 * This driver probes the FPGA and CPLD on the I2C bus. It serves as
 * a multi-function device (MFD) core, which means it does not directly control
 * hardware features. Instead, it registers platform devices for each functional
 * unit provided by the CPLDs (e.g., transceivers, hardware monitors, LEDs).
 *
 * It also provides shared I2C register I/O functions that can be used by the
 * child platform drivers to communicate with the CPLD. Finally, it creates
 * top-level sysfs attributes for chip-wide information like versioning and a
 * raw register access tool for debugging.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/err.h>
#include "x86-64-apresia-ca120-32t16x-mfd.h"

#define I2C_RETRY_COUNT 3
#define I2C_RETRY_DELAY 5 /* milliseconds */

/* Version register addresses (specific to the FPGA) */
#define REG_PLD_VERSION_MAJOR   0x00
#define REG_PLD_VERSION_MINOR   0x00
#define REG_PCB_VERSION         0x02

/* System reset control register (active-low fields) */
#define REG_SYS_RESET           0x05

/* Bit definitions for REG_SYS_RESET */
#define MAC_RESET_N_MASK        0x01  /* Bit[0]: 1=Normal, 0=Reset BCM MAC */

#define REG_PLD_VERSION_MAJOR_MASK   0xF0
#define REG_PLD_VERSION_MINOR_MASK   0x0F

#define PCB_VERSION_R0A         0x00
#define PCB_VERSION_R0B         0x01
#define PCB_VERSION_R01         0x02
#define PCB_VERSION_MASK        0x70

/**
 * enum cpld_type - Identifies the CPLD variant.
 * @CPLD_TYPE_FPGA: The FPGA.
 * @CPLD_TYPE_CPLD: The CPLD.
 */
enum cpld_type {
    CPLD_TYPE_FPGA,
    CPLD_TYPE_CPLD,
};

/**
 * struct ca120_32t16x_mfd_data - Shared data for the MFD core and its children.
 * @client: Pointer to the I2C client for this MFD instance.
 * @lock: A mutex to protect concurrent access to the FPGA/CPLD's registers.
 * Child drivers do not need to implement their own locking if they
 * use the exported ca120_32t16x_mfd_read/write_reg() functions.
 * @attr_group: The attribute group actually created for this device.
 * @type:       Cached CPLD type (FPGA or CPLD).
 */
struct ca120_32t16x_mfd_data {
    struct i2c_client *client;
    struct mutex lock;
    const struct attribute_group *attr_group; /* remember which group we created */
    enum cpld_type type;
};

/* Forward declaration for i2c_device_id table */
static const struct i2c_device_id ca120_32t16x_mfd_id[];

/**
 * ca120_32t16x_mfd_read_reg() - Read a byte from a FPGA/CPLD register.
 * @dev:  Device pointer (can be the MFD core device or a child platform device).
 * @reg:  The register address (0x00 - 0xFF) to read from.
 * @val:  Pointer to a u8 to store the read value.
 *
 * This function handles I2C communication with retry logic. It is exported
 * for use by child drivers. Access is protected by a mutex.
 *
 * Return: 0 on success, or a negative errno on failure.
 */
int ca120_32t16x_mfd_read_reg(struct device *dev, u8 reg, u8 *val)
{
    struct ca120_32t16x_mfd_data *data = dev_get_drvdata(dev);
    int ret = -EIO;
    int retry;

    if (!data || !data->client) {
        return -ENODEV;
    }

    mutex_lock(&data->lock);
    for (retry = 0; retry < I2C_RETRY_COUNT; retry++) {
        ret = i2c_smbus_read_byte_data(data->client, reg);
        if (ret >= 0) {
            *val = (u8)ret;
            ret = 0; /* Success */
            break;
        }
        if (retry < I2C_RETRY_COUNT - 1) {
            usleep_range(I2C_RETRY_DELAY * 1000, (I2C_RETRY_DELAY + 1) * 1000);
        }
    }
    mutex_unlock(&data->lock);

    if (ret < 0) {
        dev_err(dev, "MFD failed to read reg 0x%02x: %d\n", reg, ret);
    }

    return ret;
}

/**
 * ca120_32t16x_mfd_write_reg() - Write a byte to a FPGA/CPLD register.
 * @dev:  Device pointer (can be the MFD core device or a child platform device).
 * @reg:  The register address (0x00 - 0xFF) to write to.
 * @val:  The u8 value to write.
 *
 * This function handles I2C communication with retry logic. It is exported
 * for use by child drivers. Access is protected by a mutex.
 *
 * Return: 0 on success, or a negative errno on failure.
 */
int ca120_32t16x_mfd_write_reg(struct device *dev, u8 reg, u8 val)
{
    struct ca120_32t16x_mfd_data *data = dev_get_drvdata(dev);
    int ret = -EIO;
    int retry;

    if (!data || !data->client) {
        return -ENODEV;
    }

    mutex_lock(&data->lock);
    for (retry = 0; retry < I2C_RETRY_COUNT; retry++) {
        ret = i2c_smbus_write_byte_data(data->client, reg, val);
        if (ret == 0) {
            break; /* Success */
        }

        if (retry < I2C_RETRY_COUNT - 1) {
            usleep_range(I2C_RETRY_DELAY * 1000, (I2C_RETRY_DELAY + 1) * 1000);
        }
    }
    mutex_unlock(&data->lock);

    if (ret < 0) {
        dev_err(dev, "MFD failed to write reg 0x%02x with 0x%02x: %d\n", reg, val, ret);
    }

    return ret;
}

static struct ca120_32t16x_ops ca120_32t16x_mfd_ops = {
    .read  = ca120_32t16x_mfd_read_reg,
    .write = ca120_32t16x_mfd_write_reg,
};

/*
 * Sysfs Attributes
 */ 
 
/**
 * get_pcb_version_string() - Convert PCB version register value to a string.
 * @pcb_reg_value: The raw 8-bit value from the PCB version register.
 *
 * Return: A pointer to a constant string representing the PCB revision.
 */
static const char *get_pcb_version_string(u8 pcb_reg_value)
{
    switch (pcb_reg_value & PCB_VERSION_MASK) {
    case PCB_VERSION_R0A:
        return "R0A";
    case PCB_VERSION_R0B:
        return "R0B";
    case PCB_VERSION_R01:
        return "R01";
    default:
        return "Unknown";
    }
}

/**
 * cpld_name_show() - Sysfs callback to show the CPLD/FPGA name.
 * @dev:  Device pointer.
 * @attr: Device attribute pointer.
 * @buf:  Buffer to write the name to.
 *
 * Return: Number of bytes written, or a negative errno.
 */
static ssize_t cpld_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct ca120_32t16x_mfd_data *data = dev_get_drvdata(dev);
    const char *name;

    if (!data) {
        return -ENODEV;
    }

    name = (data->type == CPLD_TYPE_FPGA) ? "fpga" : "cpld";
    return scnprintf(buf, PAGE_SIZE, "%s\n", name);
}
static DEVICE_ATTR_RO(cpld_name);

/**
 * cpld_version_show() - Sysfs callback to show the CPLD/FPGA version.
 * @dev:  Device pointer.
 * @attr: Device attribute pointer.
 * @buf:  Buffer to write the version to.
 *
 * Return: Number of bytes written, or a negative errno.
 */
static ssize_t cpld_version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    u8 val_major, val_minor;
    int ret;

    ret = ca120_32t16x_mfd_read_reg(dev, REG_PLD_VERSION_MAJOR, &val_major);
    if (ret < 0) {
        dev_err(dev, "Failed to read major version register: %d\n", ret);
        return ret;
    }

    ret = ca120_32t16x_mfd_read_reg(dev, REG_PLD_VERSION_MINOR, &val_minor);
    if (ret < 0) {
        dev_err(dev, "Failed to read minor version register: %d\n", ret);
        return ret;
    }

    return sprintf(buf, "%d.%d\n", val_major & REG_PLD_VERSION_MAJOR_MASK, 
                        val_minor & REG_PLD_VERSION_MINOR_MASK);
}
static DEVICE_ATTR_RO(cpld_version);

/**
 * pcb_version_show() - Sysfs callback to show the PCB version.
 * @dev:  Device pointer.
 * @attr: Device attribute pointer.
 * @buf:  Buffer to write the version to.
 *
 * Return: Number of bytes written, or a negative errno.
 */
static ssize_t pcb_version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    u8 val_pcb;
    int ret;

    ret = ca120_32t16x_mfd_read_reg(dev, REG_PCB_VERSION, &val_pcb);
    if (ret < 0) {
        dev_err(dev, "Failed to read PCB version register: %d\n", ret);
        return ret;
    }

    return sprintf(buf, "%s\n", get_pcb_version_string(val_pcb));
}
static DEVICE_ATTR_RO(pcb_version);

/**
 * version_info_show() - Sysfs callback for aggregated version info.
 * @dev:  Device pointer.
 * @attr: Device attribute pointer.
 * @buf:  Buffer to write the formatted info to.
 *
 * Return: Number of bytes written.
 */
static ssize_t version_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct ca120_32t16x_mfd_data *data = dev_get_drvdata(dev);

    u8 val_major = 0, val_minor = 0, val_pcb = 0;
    int len = 0;
    int ret_major, ret_minor, ret_pcb;
    size_t remain = PAGE_SIZE;

    if (!data) {
        return scnprintf(buf, PAGE_SIZE, "Version Information:\n(no driver data)\n");
    }

    /*
     * Read all registers needed for this file. It is acceptable to do
     * multiple reads for a single sysfs cat, but for performance-critical
     * paths, this should be minimized.
     */
    ret_major = ca120_32t16x_mfd_read_reg(dev, REG_PLD_VERSION_MAJOR, &val_major);
    ret_minor = ca120_32t16x_mfd_read_reg(dev, REG_PLD_VERSION_MINOR, &val_minor);
    ret_pcb   = ca120_32t16x_mfd_read_reg(dev, REG_PCB_VERSION, &val_pcb);

    /* Aggregated output; use scnprintf for kernels prior to sysfs_emit(). */
    len += scnprintf(buf + len, remain - len, "Version Information:\n");
    len += scnprintf(buf + len, remain - len, "========================\n");

    if (ret_major == 0) {
        u8 major_masked = val_major & REG_PLD_VERSION_MAJOR_MASK;
        len += scnprintf(buf + len, remain - len,
                         "Major Version: 0x%02x (%u)\n", val_major, major_masked);
    } else {
        len += scnprintf(buf + len, remain - len,
                         "Major Version: ERROR (%d)\n", ret_major);
    }

    if (ret_minor == 0) {
        u8 minor_masked = val_minor & REG_PLD_VERSION_MINOR_MASK;
        len += scnprintf(buf + len, remain - len,
                         "Minor Version: 0x%02x (%u)\n", val_minor, minor_masked);
    } else {
        len += scnprintf(buf + len, remain - len,
                         "Minor Version: ERROR (%d)\n", ret_minor);
    }

    /* Only the main FPGA exposes PCB version info. Decide by cached type. */
    if (data->type == CPLD_TYPE_FPGA) {
        if (ret_pcb == 0) {
            len += scnprintf(buf + len, remain - len,
                             "PCB Version: 0x%02x (%s)\n",
                             val_pcb, get_pcb_version_string(val_pcb));
        } else {
            len += scnprintf(buf + len, remain - len,
                             "PCB Version: ERROR (%d)\n", ret_pcb);
        }
    }

    return len;
}
static DEVICE_ATTR_RO(version_info);

/**
 * reset_mac_show() - Show the current MAC reset state via sysfs.
 * @dev:  Device pointer.
 * @attr: Device attribute pointer.
 * @buf:  Output buffer.
 *
 * This attribute reflects the logical reset state (not the raw active-low bit):
 *   Returns "1\n" when MAC is in Reset  (MAC_RESET_N=0),
 *   Returns "0\n" when MAC is Normal (MAC_RESET_N=1).
 *
 * Return: Number of bytes written to @buf, or a negative errno on error.
 */
static ssize_t reset_mac_show(struct device *dev,
                              struct device_attribute *attr,
                              char *buf)
{
    u8 val;
    int ret;

    ret = ca120_32t16x_mfd_read_reg(dev, REG_SYS_RESET, &val);
    if (ret < 0) {
        return ret;
    }

    if ((val & MAC_RESET_N_MASK) != 0) {
        return scnprintf(buf, PAGE_SIZE, "0\n"); /* Normal */
    } else {
        return scnprintf(buf, PAGE_SIZE, "1\n"); /* In reset */
    }
}

/**
 * reset_mac_store() - Set MAC reset state via sysfs.
 * @dev:   Device pointer.
 * @attr:  Device attribute pointer.
 * @buf:   User input buffer ("0" or "1").
 * @count: Number of bytes in @buf.
 *
 * Semantics (maps logical request to active-low register bit):
 *   Write "1": Assert  reset (clear MAC_RESET_N bit -> Reset).
 *   Write "0": Deassert reset (set MAC_RESET_N bit -> Normal).
 *
 * The function performs a read-modify-write to preserve other bits in
 * REG_SYS_RESET.
 *
 * Return: @count on success, or a negative errno on error.
 */
static ssize_t reset_mac_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf, size_t count)
{
    unsigned long v;
    u8 regv;
    int ret;

    ret = kstrtoul(buf, 0, &v);
    if (ret < 0) {
        return ret;
    }
    if (v != 0 && v != 1) {
        return -EINVAL;
    }

    ret = ca120_32t16x_mfd_read_reg(dev, REG_SYS_RESET, &regv);
    if (ret < 0) {
        return ret;
    }

    if (v == 1) {
        regv &= (u8)~MAC_RESET_N_MASK; /* Reset: assert reset (active-low) */
    } else {
        regv |= MAC_RESET_N_MASK;      /* Normal: deassert reset */
    }

    ret = ca120_32t16x_mfd_write_reg(dev, REG_SYS_RESET, regv);
    if (ret < 0) {
        return ret;
    }
    return count;
}
static DEVICE_ATTR_RW(reset_mac);

/**
 * reset_mac_pulse_ms_store() - Pulse MAC reset for a given duration (ms).
 * @dev:   Device pointer.
 * @attr:  Device attribute pointer.
 * @buf:   User input buffer holding an integer milliseconds (e.g. "10").
 * @count: Number of bytes in @buf.
 *
 * Behavior:
 * - Assert reset by clearing REG_SYS_RESET bit[0] (active-low).
 * - Sleep for the requested milliseconds.
 * - Deassert reset by setting REG_SYS_RESET bit[0].
 *
 * Notes:
 * - Other bits in REG_SYS_RESET are preserved via read-modify-write.
 * - Accepts values in [1, 5000]. Value 0 is treated as 10 ms default.
 *
 * Return: @count on success, or a negative errno on error.
 */
static ssize_t reset_mac_pulse_ms_store(struct device *dev,
                                        struct device_attribute *attr,
                                        const char *buf, size_t count)
{
    unsigned long ms;
    u8 regv;
    int ret;

    ret = kstrtoul(buf, 0, &ms);
    if (ret < 0) {
        return ret;
    }

    /* Clamp and normalize: 0 -> 10 ms default, cap to 5000 ms */
    if (ms == 0) {
        ms = 10;
    }
    if (ms > 5000) {
        ms = 5000;
    }

    /* Assert reset (clear active-low bit) */
    ret = ca120_32t16x_mfd_read_reg(dev, REG_SYS_RESET, &regv);
    if (ret < 0) {
        return ret;
    }
    regv &= (u8)~MAC_RESET_N_MASK;
    ret = ca120_32t16x_mfd_write_reg(dev, REG_SYS_RESET, regv);
    if (ret < 0) {
        return ret;
    }

    msleep((unsigned int)ms);

    /* Deassert reset (set active-low bit) */
    ret = ca120_32t16x_mfd_read_reg(dev, REG_SYS_RESET, &regv);
    if (ret < 0) {
        return ret;
    }
    regv |= MAC_RESET_N_MASK;
    ret = ca120_32t16x_mfd_write_reg(dev, REG_SYS_RESET, regv);
    if (ret < 0) {
        return ret;
    }
    return count;
}
static DEVICE_ATTR_WO(reset_mac_pulse_ms);

/**
 * debug_reg_show() - Sysfs show callback for the debug register tool.
 * @dev:  Device pointer.
 * @attr: Device attribute pointer.
 * @buf:  Buffer to write usage instructions.
 *
 * Return: Number of bytes written.
 */
static ssize_t debug_reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sprintf(buf,
                      "Usage:\n"
                      "  Read:  echo <reg> > debug_reg\n"
                      "  Write: echo <reg> <val> > debug_reg\n"
                      "  -> View results with 'dmesg' or 'journalctl -k'\n");
}

/**
 * debug_reg_store() - Sysfs store callback for the debug register tool.
 * @dev:   Device pointer.
 * @attr:  Device attribute pointer.
 * @buf:   Buffer containing the command string from the user.
 * @count: Number of bytes in the buffer.
 *
 * Return: Number of bytes written, or a negative errno.
 */
static ssize_t debug_reg_store(struct device *dev, struct device_attribute *attr,
                                const char *buf, size_t count)
{
    unsigned int reg, val;
    int args, ret;

    /* Use sscanf to parse one or two arguments from the buffer */
    args = sscanf(buf, "%i %i", &reg, &val);
    if (args < 1 || reg > 0xFF || (args == 2 && val > 0xFF)) {
        return -EINVAL;
    }

    if (args == 1) { /* Read operation */
        u8 read_val;
        ret = ca120_32t16x_mfd_read_reg(dev, (u8)reg, &read_val);
        if (ret == 0) {
            dev_info(dev, "Read reg 0x%02x = 0x%02x\n", (u8)reg, read_val);
        }
    } else { /* Write operation */
        ret = ca120_32t16x_mfd_write_reg(dev, (u8)reg, (u8)val);
        if (ret == 0) {
            dev_info(dev, "Write reg 0x%02x = 0x%02x\n", (u8)reg, (u8)val);
        }
    }

    return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(debug_reg);

static struct attribute *fpga_attrs[] = {
    &dev_attr_cpld_name.attr,
    &dev_attr_cpld_version.attr,
    &dev_attr_pcb_version.attr,
    &dev_attr_version_info.attr,
    &dev_attr_reset_mac.attr,
    &dev_attr_reset_mac_pulse_ms.attr,
    &dev_attr_debug_reg.attr,
    NULL,
};
static const struct attribute_group fpga_attr_group = {
    .attrs = fpga_attrs,
};

/* A single attribute list for FPGA and CPLD */
static struct attribute *cpld_attrs[] = {
    &dev_attr_cpld_name.attr,
    &dev_attr_cpld_version.attr,
    &dev_attr_version_info.attr,
    &dev_attr_debug_reg.attr,
    NULL,
};
static const struct attribute_group cpld_attr_group = {
    .attrs = cpld_attrs,
};

/* MFD cell definitions */

/* Define the functional units on the FPGA */
static const struct mfd_cell fpga_cells[] = {
    {
        .name = FXCVR_DEVNAME,
        .platform_data = &ca120_32t16x_mfd_ops,
        .pdata_size = sizeof(ca120_32t16x_mfd_ops)
    },
    {
        .name = FAN_DRVNAME,
        .platform_data = &ca120_32t16x_mfd_ops,
        .pdata_size = sizeof(ca120_32t16x_mfd_ops)
    },
    {
        .name = PSU1_DEVNAME,
        .platform_data = &ca120_32t16x_mfd_ops,
        .pdata_size = sizeof(ca120_32t16x_mfd_ops)
    },
    {
        .name = PSU2_DEVNAME,
        .platform_data = &ca120_32t16x_mfd_ops,
        .pdata_size = sizeof(ca120_32t16x_mfd_ops)
    },
    {
        .name = LED_DRVNAME,
        .platform_data = &ca120_32t16x_mfd_ops,
        .pdata_size = sizeof(ca120_32t16x_mfd_ops)
    }
};

/* Define the functional units on the CPLD */
static const struct mfd_cell cpld_cells[] = {
    {
        .name = CXCVR_DEVNAME,
        .platform_data = &ca120_32t16x_mfd_ops,
        .pdata_size = sizeof(ca120_32t16x_mfd_ops) 
    }
};

/**
 * ca120_32t16x_mfd_probe() - Probe method for the I2C MFD driver.
 * @client: The I2C client device being probed.
 * @id:     The I2C device ID that matched for this probe.
 *
 * This function is called by the I2C core when a device matching the driver's
 * ID table is found. It allocates the driver's private data structure,
 * initializes the mutex, creates the common sysfs attributes, and finally
 * registers the child platform devices based on which FPGA/CPLD was detected.
 *
 * Return: 0 on success, or a negative errno on failure.
 */
static int ca120_32t16x_mfd_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct ca120_32t16x_mfd_data *data;
    const struct mfd_cell *cells;
    int n_cells, ret;

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
        dev_err(&client->dev, "I2C adapter doesn't support SMBUS_BYTE_DATA\n");
        return -EIO;
    }

    data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
    if (!data) {
        return -ENOMEM;
    }

    data->client = client;
    mutex_init(&data->lock);
    i2c_set_clientdata(client, data);
    data->attr_group = NULL;
    data->type = (enum cpld_type)id->driver_data;    

    /* Select the cell array and create sysfs attributes based on the probed device ID */
    if (id->driver_data == CPLD_TYPE_FPGA) {
        cells = fpga_cells;
        n_cells = ARRAY_SIZE(fpga_cells);
        dev_info(&client->dev, "Probed FPGA, registering children\n");

        ret = sysfs_create_group(&client->dev.kobj, &fpga_attr_group);
        if (ret < 0) {
            dev_err(&client->dev, "Failed to create FPGA sysfs group: %d\n", ret);
            return ret;
        }
        data->attr_group = &fpga_attr_group;
        data->type = CPLD_TYPE_FPGA;
    } else {
        cells = cpld_cells;
        n_cells = ARRAY_SIZE(cpld_cells);
        dev_info(&client->dev, "Probed CPLD, registering children\n");

        ret = sysfs_create_group(&client->dev.kobj, &cpld_attr_group);
        if (ret < 0) {
            dev_err(&client->dev, "Failed to create CPLD sysfs group: %d\n", ret);
            return ret;
        }
        data->attr_group = &cpld_attr_group;
        data->type = CPLD_TYPE_CPLD;
    }

    /*
     * Register platform devices for all functions on this chip.
     * By passing '0' as the platform device ID base instead of
     * 'PLATFORM_DEVID_AUTO', we ensure predictable device names like
     * 'ca120-ext-xcvr.0' instead of a dynamically assigned ID.
     */
    return devm_mfd_add_devices(&client->dev, PLATFORM_DEVID_NONE, cells, n_cells, NULL, 0, NULL);
}

/**
 * ca120_32t16x_mfd_remove() - Remove method for the I2C MFD driver.
 * @client: The I2C client device being removed.
 *
 * This function is called by the I2C core when the device is unbound from
 * the driver. It cleans up resources allocated in the probe function.
 * Thanks to devm-managed resources, it only needs to explicitly remove
 * the sysfs group. The MFD core will automatically unregister the child
 * platform devices, and the device core will free the memory allocated
 * by devm_kzalloc.
 *
 * Return: Always 0.
 */
static int ca120_32t16x_mfd_remove(struct i2c_client *client)
{
    struct ca120_32t16x_mfd_data *data = i2c_get_clientdata(client);

    /* Be defensive: only remove what we actually created. */
    if (data && data->attr_group) {
        sysfs_remove_group(&client->dev.kobj, data->attr_group);
    }
    
    return 0;
}

static const struct i2c_device_id ca120_32t16x_mfd_id[] = {
    { "fpga", CPLD_TYPE_FPGA },
    { "cpld", CPLD_TYPE_CPLD },
    {  /* Sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, ca120_32t16x_mfd_id);

static struct i2c_driver ca120_32t16x_mfd_driver = {
    .driver = {
        .name = "ca120-32t16x-mfd"
    },
    .probe  = ca120_32t16x_mfd_probe,
    .remove = ca120_32t16x_mfd_remove,
    .id_table = ca120_32t16x_mfd_id,
};
module_i2c_driver(ca120_32t16x_mfd_driver);

MODULE_AUTHOR("Roger Ho <roger530_ho@accton.com>");
MODULE_DESCRIPTION("CA120-32T16X FPGA/CPLD MFD Core Driver");
MODULE_LICENSE("GPL");
