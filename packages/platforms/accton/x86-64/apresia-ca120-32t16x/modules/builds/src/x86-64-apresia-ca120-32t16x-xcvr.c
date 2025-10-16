// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CA120-32T16X FPGA/CPLD Transceiver.
 *
 * Copyright (C) 2025 Accton Technology Corporation.
 *
 * Overview:
 * Binds to platform devices created by the ca120-32t16x-mfd core and exposes
 * a data-driven sysfs interface for optical transceiver management.
 *
 * Features:
 * - Presence detection
 * - TX disable control
 * - TX fault status
 * - RX loss of signal (LOS)
 *
 * Design notes:
 * - Attribute creation is table-driven per-port; no runtime string parsing is
 * used in callbacks thanks to pre-wired context structures.
 * - All register I/O is delegated to the MFD core via exported ops which
 * provide serialization and retry; callbacks may sleep.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/mod_devicetable.h>
#include "x86-64-apresia-ca120-32t16x-mfd.h"

/**
 * enum cpld_variant - Device variant selector for platform IDs
 * @FPGA_XCVR: Transceiver block attached to the FPGA
 * @CPLD_XCVR: Transceiver block attached to the CPLD
 */
enum cpld_variant {
    FPGA_XCVR,
    CPLD_XCVR
};

/* Internal enum to identify transceiver form factors */
enum xcvr_type {
    XCVR_TYPE_SFP,
    XCVR_TYPE_MAX
};

/* Internal enum to index attributes within struct port_config. */
enum attr_type {
    ATTR_PRESENT = 0,
    ATTR_TXDISABLE,
    ATTR_TXFAULT,
    ATTR_RXLOS,
    ATTR_MAX
};

/* Bit flags for port capabilities */
#define PORT_CAP_PRESENT    BIT(0)
#define PORT_CAP_TXDISABLE  BIT(1)
#define PORT_CAP_TXFAULT    BIT(2)
#define PORT_CAP_RXLOS      BIT(3)

/**
 * struct port_sysfs_attr - Template for a single sysfs attribute.
 * @name_fmt:	The printf-style format for the attribute name(e.g., "module_present_%02d").
 * @mode:	The file permissions (e.g., 0444 for read-only, 0644 for read-write).
 * @reg_addr:	The CPLD register address for this attribute.
 * @bit_pos:	The bit position within the register.
 * @invert:	True if the hardware bit logic is inverted (e.g., 0 means 'on' or 'present').
 */
struct port_sysfs_attr {
    const char *name_fmt;
    umode_t mode;
    u8 reg_addr;
    u8 bit_pos;
    bool invert;
};

/**
 * struct port_config - Defines the complete hardware layout for a single port.
 * @type:	 The form factor of the transceiver (enum xcvr_type).
 * @port_num:	 The physical port number used for naming sysfs files.
 * @capabilities: A bitmask of supported features for this port (PORT_CAP_*).
 * @attrs:	An array of attribute templates, indexed by enum attr_type.
 */
struct port_config {
    u8 type;
    u8 port_num;
    u8 capabilities;
    struct port_sysfs_attr attrs[ATTR_MAX];
};

/* Helper macros to define port configurations in a data-driven way */
#define SFP_PORT(num, present, txdisable, txfault, rxlos, bit_pos) \
    { .type = XCVR_TYPE_SFP, .port_num = num, .capabilities = PORT_CAP_PRESENT | PORT_CAP_TXDISABLE | PORT_CAP_TXFAULT | PORT_CAP_RXLOS, \
      .attrs = { [ATTR_PRESENT]   = {"module_present_%02d", 0444, present, bit_pos, true}, \
                 [ATTR_TXDISABLE] = {"module_tx_disable_%02d", 0644, txdisable, bit_pos, false}, \
                 [ATTR_TXFAULT]   = {"module_tx_fault_%02d", 0444, txfault, bit_pos, false}, \
                 [ATTR_RXLOS]     = {"module_rx_los_%02d", 0444, rxlos, bit_pos, false} } }


/* Data table defining all ports on the FPGA */
static const struct port_config fpga_ports[] = {
    SFP_PORT(41, 0x1D, 0x18, 0x20, 0x23, 0),
    SFP_PORT(42, 0x1D, 0x18, 0x20, 0x23, 1),
    SFP_PORT(43, 0x1D, 0x18, 0x20, 0x23, 2),
    SFP_PORT(44, 0x1D, 0x18, 0x20, 0x23, 3),
    SFP_PORT(45, 0x1D, 0x18, 0x20, 0x23, 4),
    SFP_PORT(46, 0x1D, 0x18, 0x20, 0x23, 5),
    SFP_PORT(47, 0x1D, 0x18, 0x20, 0x23, 6),
    SFP_PORT(48, 0x1D, 0x18, 0x20, 0x23, 7),
};
/* Data table defining all ports on the CPLD */
static const struct port_config cpld_ports[] = {
    SFP_PORT(33, 0x2C, 0x14, 0x3C, 0x4C, 0),
    SFP_PORT(34, 0x2C, 0x14, 0x3C, 0x4C, 1),
    SFP_PORT(35, 0x2C, 0x14, 0x3C, 0x4C, 2),
    SFP_PORT(36, 0x2C, 0x14, 0x3C, 0x4C, 3),
    SFP_PORT(37, 0x2C, 0x14, 0x3C, 0x4C, 4),
    SFP_PORT(38, 0x2C, 0x14, 0x3C, 0x4C, 5),
    SFP_PORT(39, 0x2C, 0x14, 0x3C, 0x4C, 6),
    SFP_PORT(40, 0x2C, 0x14, 0x3C, 0x4C, 7),    
};

/**
 * struct ca120_32t16x_xcvr_data - Driver private data
 * @mfd_dev:	Parent MFD device used for register I/O.
 * @ops:	MFD-provided byte access operations (read/write).
 * @ports:	Active port configuration table.
 * @port_count:	Number of entries in @ports.
 * @port_group:	Sysfs group holding all per-port attributes.
 * @port_attr_ptrs: Null-terminated array of pointers to created attributes.
 */
struct ca120_32t16x_xcvr_data {
    struct device *mfd_dev; /* Pointer to the parent MFD device */
    const struct ca120_32t16x_ops *ops;
    const struct port_config *ports;
    int port_count;
    struct attribute_group port_group;
    struct attribute **port_attr_ptrs;
    /*
     * reg_lock - Serialize read-modify-write sequences on shared registers.
     * MFD ops already serialize single SMBus transactions, but not the whole
     * RMW window across attributes that share the same register.
     */
    struct mutex reg_lock;
};

/**
 * struct ca120_32t16x_port_attribute - Custom attribute struct.
 * @dev_attr:  The standard device_attribute struct, must be the first member.
 * @port:      Pointer to the port_config for this attribute's port.
 * @attr_info: Pointer to the port_sysfs_attr template for this attribute.
 *
 * This struct allows passing context (port and attribute info) to the shared
 * show/store callbacks, avoiding runtime string parsing. It is accessed via
 * the container_of() macro.
 */
struct ca120_32t16x_port_attribute {
    struct device_attribute dev_attr;
    const struct port_config *port;
    const struct port_sysfs_attr *attr_info;
};

static ssize_t port_attr_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t port_attr_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);

/**
 * create_port_attributes() - Dynamically create all sysfs attributes.
 * @data: Pointer to the driver's private data.
 * @dev:  Pointer to the platform device.
 *
 * This function iterates through the port configuration table, calculates the
 * total number of required sysfs files, allocates memory for them, and then
 * populates each attribute structure with the correct name, mode, callbacks,
 * and context pointers.
 *
 * @return: 0 on success, or a negative errno on failure.
 */
static int create_port_attributes(struct ca120_32t16x_xcvr_data *data, struct device *dev)
{
    struct ca120_32t16x_port_attribute *port_attrs;
    int i, attr_idx = 0;
    int total_attrs = 0;

    for (i = 0; i < data->port_count; i++) {
        total_attrs += hweight32(data->ports[i].capabilities);
    }

    port_attrs = devm_kcalloc(dev, total_attrs, sizeof(*port_attrs), GFP_KERNEL);
    data->port_attr_ptrs = devm_kcalloc(dev, total_attrs + 1, sizeof(struct attribute *), GFP_KERNEL);
    if (!port_attrs || !data->port_attr_ptrs) {
        return -ENOMEM;
    }

    for (i = 0; i < data->port_count; i++) {
        const struct port_config *port = &data->ports[i];
        enum attr_type type;

        for (type = 0; type < ATTR_MAX; type++) {
            if (port->capabilities & BIT(type)) {
                const struct port_sysfs_attr *tmpl = &port->attrs[type];
                struct ca120_32t16x_port_attribute *f_attr = &port_attrs[attr_idx];

                f_attr->dev_attr.attr.name = devm_kasprintf(dev, GFP_KERNEL,
                                                            tmpl->name_fmt,
                                                            port->port_num);
                if (!f_attr->dev_attr.attr.name) {
                    return -ENOMEM;
                }

                sysfs_attr_init(&f_attr->dev_attr.attr);
                f_attr->dev_attr.attr.mode = tmpl->mode;
                f_attr->dev_attr.show = port_attr_show;
                if (tmpl->mode & S_IWUSR) {
                    f_attr->dev_attr.store = port_attr_store;
                }
                /* Store pointers for direct access in callbacks */
                f_attr->port = port;
                f_attr->attr_info = tmpl;

                data->port_attr_ptrs[attr_idx++] = &f_attr->dev_attr.attr;
            }
        }
    }
    data->port_group.attrs = data->port_attr_ptrs;

    return 0;
}

/**
 * to_ca120_32t16x_port_attr() - Helper to get the custom attribute struct.
 * @attr: The generic struct attribute pointer from the callback.
 *
 * @return: A pointer to the containing ca120_32t16x_port_attribute struct.
 */
static inline struct ca120_32t16x_port_attribute *to_ca120_32t16x_port_attr(struct attribute *attr)
{
    return container_of(attr, struct ca120_32t16x_port_attribute, dev_attr.attr);
}

/**
 * port_attr_show() - Shared callback for reading all port attributes.
 * @dev:   The device being read.
 * @attr:  The specific device_attribute being read.
 * @buf:   The buffer to write the result into.
 *
 * This function uses container_of() to get the pre-stored context for the
 * attribute, reads the corresponding CPLD register, extracts the bit value,
 * applies inversion logic if needed, and formats the result into the buffer.
 *
 * @return: The number of bytes written to the buffer, or a negative errno.
 */
static ssize_t port_attr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct ca120_32t16x_xcvr_data *data = dev_get_drvdata(dev);
    struct ca120_32t16x_port_attribute *f_attr = to_ca120_32t16x_port_attr(&attr->attr);
    const struct port_sysfs_attr *attr_info;
    u8 reg_val;
    bool bit_val;
    int ret;

    attr_info = f_attr->attr_info;
    ret = data->ops->read(data->mfd_dev, attr_info->reg_addr, &reg_val);
    if (ret < 0) {
        return ret;
    }

    bit_val = !!(reg_val & BIT(attr_info->bit_pos));
    if (attr_info->invert) {
        bit_val = !bit_val;
    }

    return sprintf(buf, "%d\n", bit_val);
}

/**
 * port_attr_store() - Shared callback for writing all port attributes.
 * @dev:   The device being written to.
 * @attr:  The specific device_attribute being written to.
 * @buf:   The user-provided buffer.
 * @count: The number of bytes in the buffer.
 *
 * This function uses container_of() to get attribute context, parses the user
 * input, and performs a read-modify-write cycle on the CPLD register.
 * Locking is handled by the parent MFD driver's I/O functions.
 *
 * @return: The number of bytes consumed, or a negative errno.
 */
static ssize_t port_attr_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct ca120_32t16x_xcvr_data *data = dev_get_drvdata(dev);
    struct ca120_32t16x_port_attribute *f_attr = to_ca120_32t16x_port_attr(&attr->attr);
    const struct port_sysfs_attr *attr_info;
    long val;
    u8 reg_val;
    int ret;

    if (kstrtol(buf, 0, &val) || val < 0 || val > 1) {
        return -EINVAL;
    }

    attr_info = f_attr->attr_info;
    if (attr_info->invert) {
        val = !val;
    }

    /*
     * Protect the entire read-modify-write sequence to avoid lost updates
     * when multiple attributes operate on different bits of the same register.
     */
    mutex_lock(&data->reg_lock);
    ret = data->ops->read(data->mfd_dev, attr_info->reg_addr, &reg_val);
    if (ret == 0) {
        if (val) {
            reg_val |= BIT(attr_info->bit_pos);
        } else {
            reg_val &= ~BIT(attr_info->bit_pos);
        }
        ret = data->ops->write(data->mfd_dev, attr_info->reg_addr, reg_val);
    }
    mutex_unlock(&data->reg_lock);

    return ret < 0 ? ret : count;
}

/**
 * module_present_all_show() - Show presence for all modules as a hex bitmap.
 * @dev:   The device being read.
 * @attr:  The specific device_attribute being read.
 * @buf:   The buffer to write the result into.
 *
 * Aggregates the presence status of all managed ports into a compact
 * hexadecimal bitmap.
 *
 * Bitmap Semantics:
 * Bit 'i' corresponds to the port at index 'i' in the driver's port
 * configuration table. A value of '1' indicates a module is present, after
 * applying the per-port inversion logic. The output prints bytes in ascending
 * order (byte 0 first), each as a two-digit hex value with no prefix,
 * followed by a newline (e.g., "3f00\n").
 *
 * @return: The number of bytes written to the buffer, or a negative errno on
 * failure.
 */
static ssize_t module_present_all_show(struct device *dev,
                                       struct device_attribute *attr,
                                       char *buf)
{
    struct ca120_32t16x_xcvr_data *data = dev_get_drvdata(dev);
    size_t bytes = (data->port_count + 7) / 8;
    u8 *bm;
    int i, len = 0, ret;
    u8 reg_val;
    bool bit_val;

    bm = kcalloc(bytes, sizeof(*bm), GFP_KERNEL);
    if (!bm)
        return -ENOMEM;

    /*
     * Take a consistent snapshot across all ports.
     * Block writers (store() RMW) while we read per-port bits.
     */
    mutex_lock(&data->reg_lock);
    for (i = 0; i < data->port_count; i++) {
        const struct port_config *port = &data->ports[i];
        const struct port_sysfs_attr *tmpl = &port->attrs[ATTR_PRESENT];

        ret = data->ops->read(data->mfd_dev, tmpl->reg_addr, &reg_val);
        if (ret < 0) {
            kfree(bm);
            return ret;
        }

        bit_val = !!(reg_val & BIT(tmpl->bit_pos));
        if (tmpl->invert)
            bit_val = !bit_val;

        if (bit_val)
            bm[i >> 3] |= (1U << (i & 7));
    }
    mutex_unlock(&data->reg_lock);

    for (i = 0; i < bytes; i++) {
        len += scnprintf(buf + len, PAGE_SIZE - len, "%.2x", bm[i]);
        if (len >= PAGE_SIZE) /* Truncate gracefully */
            break;
    }
    if (len < PAGE_SIZE)
        len += scnprintf(buf + len, PAGE_SIZE - len, "\n");

    kfree(bm);
    return len;
}

/**
 * module_rx_los_all_show() - Show RX LOS status for all modules as a hex bitmap.
 * @dev:   The device being read.
 * @attr:  The specific device_attribute being read.
 * @buf:   The buffer to write the result into.
 *
 * Aggregates the RX Loss of Signal (LOS) status of all managed ports into a
 * compact hexadecimal bitmap.
 *
 * Bitmap Semantics:
 * Bit 'i' corresponds to the port at index 'i' in the driver's port
 * configuration table. A value of '1' indicates that RX LOS is asserted. The
 * output prints bytes in ascending order (byte 0 first), each as a two-digit
 * hex value with no prefix, followed by a newline (e.g., "0001\n").
 *
 * @return: The number of bytes written to the buffer, or a negative errno on
 * failure.
 */
static ssize_t module_rx_los_all_show(struct device *dev,
                                      struct device_attribute *attr,
                                      char *buf)
{
    struct ca120_32t16x_xcvr_data *data = dev_get_drvdata(dev);
    size_t bytes = (data->port_count + 7) / 8;
    u8 *bm;
    int i, len = 0, ret;
    u8 reg_val;

    bm = kcalloc(bytes, sizeof(*bm), GFP_KERNEL);
    if (!bm)
        return -ENOMEM;

    /*
     * Take a consistent snapshot across all ports.
     * Block writers (store() RMW) while we read per-port bits.
     */
    mutex_lock(&data->reg_lock);
    for (i = 0; i < data->port_count; i++) {
        const struct port_config *port = &data->ports[i];
        const struct port_sysfs_attr *tmpl = &port->attrs[ATTR_RXLOS];

        ret = data->ops->read(data->mfd_dev, tmpl->reg_addr, &reg_val);
        if (ret < 0) {
            kfree(bm);
            return ret;
        }

        if (reg_val & BIT(tmpl->bit_pos))
            bm[i >> 3] |= (1U << (i & 7));
    }
    mutex_unlock(&data->reg_lock);

    for (i = 0; i < bytes; i++) {
        len += scnprintf(buf + len, PAGE_SIZE - len, "%.2x", bm[i]);
        if (len >= PAGE_SIZE) /* Truncate gracefully */
            break;
    }
    if (len < PAGE_SIZE)
        len += scnprintf(buf + len, PAGE_SIZE - len, "\n");

    kfree(bm);
    return len;
}

/* Read-only aggregated sysfs attributes (hex bitmap) */
static DEVICE_ATTR_RO(module_present_all);
static DEVICE_ATTR_RO(module_rx_los_all);

/**
 * ca120_32t16x_xcvr_probe() - Probe function for the XCVR platform driver.
 * @pdev: The platform device instance.
 *
 * This function is called by the platform core when a device matching the
 * driver's ID table is found. It allocates driver data, determines which
 * CPLD it is attached to (FPGA or CPLD), points to the correct port
 * configuration table, and then calls create_port_attributes() to build and
 * register the sysfs interface.
 *
 * @return: 0 on success, or a negative errno on failure.
 */
static int ca120_32t16x_xcvr_probe(struct platform_device *pdev)
{
    const struct platform_device_id *id = platform_get_device_id(pdev);
    struct ca120_32t16x_xcvr_data *data;
    int ret;

    data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
    if (!data) {
        return -ENOMEM;
    }

    data->mfd_dev = pdev->dev.parent;
    data->ops = dev_get_platdata(&pdev->dev);
    if (!data->ops || !data->ops->read || !data->ops->write) {
        dev_err(&pdev->dev, "missing ca120-32t16x ops from parent\n");
        return -ENODEV;
    }
    platform_set_drvdata(pdev, data);
    mutex_init(&data->reg_lock);

    if (id->driver_data == FPGA_XCVR) {
        data->ports = fpga_ports;
        data->port_count = ARRAY_SIZE(fpga_ports);
    } else {
        data->ports = cpld_ports;
        data->port_count = ARRAY_SIZE(cpld_ports);
    }

    ret = create_port_attributes(data, &pdev->dev);
    if (ret < 0) {
        return ret;
    }

    ret = sysfs_create_group(&pdev->dev.kobj, &data->port_group);
    if (ret < 0) {
        return ret;
    }

    /* Create aggregated "all" attributes at device level (hex bitmaps). */
    ret = device_create_file(&pdev->dev, &dev_attr_module_present_all);
    if (ret < 0) {
        sysfs_remove_group(&pdev->dev.kobj, &data->port_group);
        return ret;
    }

    ret = device_create_file(&pdev->dev, &dev_attr_module_rx_los_all);
    if (ret < 0) {
        device_remove_file(&pdev->dev, &dev_attr_module_present_all);
        sysfs_remove_group(&pdev->dev.kobj, &data->port_group);
        return ret;
    }

    dev_info(&pdev->dev, "ca120-32t16x XCVR driver for %s initialized with %d ports\n", 
                         (id->driver_data == FPGA_XCVR) ? "fpga" : "cpld", data->port_count);
    return 0;
}

/**
 * ca120_32t16x_xcvr_remove() - Remove function for the XCVR platform driver.
 * @pdev: The platform device instance.
 *
 * This function is called by the platform core when the device is being
 * removed. It removes the sysfs group. All memory allocated with devm_*
 * functions in probe() is automatically freed by the framework.
 *
 * @return: 0 always.
 */
static int ca120_32t16x_xcvr_remove(struct platform_device *pdev)
{
    struct ca120_32t16x_xcvr_data *data = platform_get_drvdata(pdev);

    /* Remove aggregated attributes */
    device_remove_file(&pdev->dev, &dev_attr_module_rx_los_all);
    device_remove_file(&pdev->dev, &dev_attr_module_present_all);

    sysfs_remove_group(&pdev->dev.kobj, &data->port_group);

    return 0;
}

static const struct platform_device_id ca120_32t16x_xcvr_ids[] = {
    { .name = FXCVR_DEVNAME, .driver_data = FPGA_XCVR },
    { .name = CXCVR_DEVNAME, .driver_data = CPLD_XCVR },
    { /* Sentinal */ }
};
MODULE_DEVICE_TABLE(platform, ca120_32t16x_xcvr_ids);

static struct platform_driver ca120_32t16x_xcvr_driver = {
    .probe  = ca120_32t16x_xcvr_probe,
    .remove = ca120_32t16x_xcvr_remove,
    .id_table = ca120_32t16x_xcvr_ids,
    .driver = {
        .name = XCVR_DRVNAME
    },
};
module_platform_driver(ca120_32t16x_xcvr_driver);

MODULE_AUTHOR("Roger Ho <roger530_ho@accton.com>");
MODULE_DESCRIPTION("CA120-32T16X XCVR Driver");
MODULE_LICENSE("GPL");
