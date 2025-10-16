from onl.platform.base import *
from onl.platform.accton import *

class OnlPlatform_x86_64_apresia_ca120_32t16x_r0(OnlPlatformAccton,
                                              OnlPlatformPortConfig_32x1_16x10):

    PLATFORM='x86-64-apresia-ca120-32t16x-r0'
    MODEL="APRESIA-CA120-32T16X"
    SYS_OBJECT_ID=".120.48.1"

    def baseconfig(self):
        os.system("modprobe i2c-ismt")
        os.system("modprobe at24")
        self.insmod('optoe')

        for m in [ 'mfd', 'xcvr', 'psu', 'fan', 'leds', 'pmbus' ]:
            self.insmod("x86-64-apresia-ca120-32t16x-%s.ko" % m)

        ########### initialize I2C bus 0, bus 1 ###########
        self.new_i2c_devices([
            # initialize multiplexer (PCA9548)
            ('pca9548', 0x77, 1),
            ('pca9548', 0x70, 3),
            ('pca9548', 0x75, 14),
            ('pca9548', 0x76, 15),
            ('pca9548', 0x76, 2),
            ('pca9548', 0x71, 41)
            ])

        # initialize pca9548 idle_state in kernel 5.4.40 version
        subprocess.call('echo -2 | tee /sys/bus/i2c/drivers/pca954x/*-00*/idle_state > /dev/null', shell=True)

        self.new_i2c_devices([
            #initiate FPGA/CPLD
            ('fpga', 0x60, 2),
            ('cpld', 0x64, 2),
            ])

        self.new_i2c_devices([
            # inititate LM77
            ('lm77', 0x48, 41),
            ('lm77', 0x49, 41),
            ('lm77', 0x4a, 49),
            ('lm77', 0x4b, 41)
            ])

        self.new_i2c_devices([
            # initiate PSU-1
            ('g1394', 0x59, 37),
            # initiate PSU-2
            ('g1394', 0x58, 37),
            ])

        # initialize SFP port 33~48
        for port in range(33, 49):
            self.new_i2c_device('optoe2', 0x50, port-15)
            subprocess.call('echo port%d > /sys/bus/i2c/devices/%d-0050/port_name' % (port, port-15), shell=True)

        self.new_i2c_device('24c02', 0x50, 39)
        self.new_i2c_device('24c02', 0x51, 39)
        return True
