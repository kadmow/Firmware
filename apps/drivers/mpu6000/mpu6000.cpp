/****************************************************************************
 *
 *   Copyright (C) 2012 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mpu6000.cpp
 *
 * Driver for the Invensense MPU6000 connected via SPI.
 */

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <semaphore.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

#include <systemlib/perf_counter.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>

#include <arch/board/board.h>
#include <arch/board/up_hrt.h>

#include <drivers/device/spi.h>
#include <drivers/drv_accel.h>
#include <drivers/drv_gyro.h>

#define DIR_READ			0x80
#define DIR_WRITE			0x00

// MPU 6000 registers
#define MPUREG_WHOAMI			0x75
#define MPUREG_SMPLRT_DIV		0x19
#define MPUREG_CONFIG			0x1A
#define MPUREG_GYRO_CONFIG		0x1B
#define MPUREG_ACCEL_CONFIG		0x1C
#define MPUREG_FIFO_EN			0x23
#define MPUREG_INT_PIN_CFG		0x37
#define MPUREG_INT_ENABLE		0x38
#define MPUREG_INT_STATUS		0x3A
#define MPUREG_ACCEL_XOUT_H		0x3B
#define MPUREG_ACCEL_XOUT_L		0x3C
#define MPUREG_ACCEL_YOUT_H		0x3D
#define MPUREG_ACCEL_YOUT_L		0x3E
#define MPUREG_ACCEL_ZOUT_H		0x3F
#define MPUREG_ACCEL_ZOUT_L		0x40
#define MPUREG_TEMP_OUT_H		0x41
#define MPUREG_TEMP_OUT_L		0x42
#define MPUREG_GYRO_XOUT_H		0x43
#define MPUREG_GYRO_XOUT_L		0x44
#define MPUREG_GYRO_YOUT_H		0x45
#define MPUREG_GYRO_YOUT_L		0x46
#define MPUREG_GYRO_ZOUT_H		0x47
#define MPUREG_GYRO_ZOUT_L		0x48
#define MPUREG_USER_CTRL		0x6A
#define MPUREG_PWR_MGMT_1		0x6B
#define MPUREG_PWR_MGMT_2		0x6C
#define MPUREG_FIFO_COUNTH		0x72
#define MPUREG_FIFO_COUNTL		0x73
#define MPUREG_FIFO_R_W			0x74
#define MPUREG_PRODUCT_ID		0x0C

// Configuration bits MPU 3000 and MPU 6000 (not revised)?
#define BIT_SLEEP			0x40
#define BIT_H_RESET			0x80
#define BITS_CLKSEL			0x07
#define MPU_CLK_SEL_PLLGYROX		0x01
#define MPU_CLK_SEL_PLLGYROZ		0x03
#define MPU_EXT_SYNC_GYROX		0x02
#define BITS_FS_250DPS			0x00
#define BITS_FS_500DPS			0x08
#define BITS_FS_1000DPS			0x10
#define BITS_FS_2000DPS			0x18
#define BITS_FS_MASK			0x18
#define BITS_DLPF_CFG_256HZ_NOLPF2	0x00
#define BITS_DLPF_CFG_188HZ		0x01
#define BITS_DLPF_CFG_98HZ		0x02
#define BITS_DLPF_CFG_42HZ		0x03
#define BITS_DLPF_CFG_20HZ		0x04
#define BITS_DLPF_CFG_10HZ		0x05
#define BITS_DLPF_CFG_5HZ		0x06
#define BITS_DLPF_CFG_2100HZ_NOLPF	0x07
#define BITS_DLPF_CFG_MASK		0x07
#define BIT_INT_ANYRD_2CLEAR		0x10
#define BIT_RAW_RDY_EN			0x01
#define BIT_I2C_IF_DIS			0x10
#define BIT_INT_STATUS_DATA		0x01

// Product ID Description for MPU6000
// high 4 bits 	low 4 bits
// Product Name	Product Revision
#define MPU6000ES_REV_C4		0x14
#define MPU6000ES_REV_C5		0x15
#define MPU6000ES_REV_D6		0x16
#define MPU6000ES_REV_D7		0x17
#define MPU6000ES_REV_D8		0x18
#define MPU6000_REV_C4			0x54
#define MPU6000_REV_C5			0x55
#define MPU6000_REV_D6			0x56
#define MPU6000_REV_D7			0x57
#define MPU6000_REV_D8			0x58
#define MPU6000_REV_D9			0x59
#define MPU6000_REV_D10			0x5A


class MPU6000_gyro;

class MPU6000 : public device::SPI
{
public:
	MPU6000(int bus, spi_dev_e device);
	~MPU6000();

	virtual int		init();

	virtual ssize_t		read(struct file *filp, char *buffer, size_t buflen);
	virtual int		ioctl(struct file *filp, int cmd, unsigned long arg);

	virtual int		open_first(struct file *filp);
	virtual int		close_last(struct file *filp);

	/**
	 * Diagnostics - print some basic information about the driver.
	 */
	void			print_info();

protected:
	virtual int		probe();

	friend class MPU6000_gyro;

	virtual ssize_t		gyro_read(struct file *filp, char *buffer, size_t buflen);
	virtual int		gyro_ioctl(struct file *filp, int cmd, unsigned long arg);

private:
	MPU6000_gyro		*_gyro;
	uint8_t			_product;	/** product code */

	struct hrt_call		_call;
	unsigned		_call_interval;

	struct accel_report	_accel_report;
	struct accel_scale	_accel_scale;
	float			_accel_range_scale;
	int			_accel_topic;

	struct gyro_report	_gyro_report;
	struct gyro_scale	_gyro_scale;
	float			_gyro_range_scale;
	int			_gyro_topic;

	unsigned		_reads;
	perf_counter_t		_sample_perf;

	/**
	 * Start automatic measurement.
	 */
	void			start();

	/**
	 * Stop automatic measurement.
	 */
	void			stop();

	/**
	 * Static trampoline from the hrt_call context; because we don't have a
	 * generic hrt wrapper yet.
	 *
	 * Called by the HRT in interrupt context at the specified rate if
	 * automatic polling is enabled.
	 *
	 * @param arg		Instance pointer for the driver that is polling.
	 */
	static void		measure_trampoline(void *arg);

	/**
	 * Fetch measurements from the sensor and update the report ring.
	 */
	void			measure();

	/**
	 * Read a register from the MPU6000
	 *
	 * @param		The register to read.
	 * @return		The value that was read.
	 */
	uint8_t			read_reg(unsigned reg);
	uint16_t		read_reg16(unsigned reg);

	/**
	 * Write a register in the MPU6000
	 *
	 * @param reg		The register to write.
	 * @param value		The new value to write.
	 */
	void			write_reg(unsigned reg, uint8_t value);

	/**
	 * Modify a register in the MPU6000
	 *
	 * Bits are cleared before bits are set.
	 *
	 * @param reg		The register to modify.
	 * @param clearbits	Bits in the register to clear.
	 * @param setbits	Bits in the register to set.
	 */
	void			modify_reg(unsigned reg, uint8_t clearbits, uint8_t setbits);

	/**
	 * Set the MPU6000 measurement range.
	 *
	 * @param max_g		The maximum G value the range must support.
	 * @return		OK if the value can be supported, -ERANGE otherwise.
	 */
	int			set_range(unsigned max_g);

	/**
	 * Swap a 16-bit value read from the MPU6000 to native byte order.
	 */
	uint16_t		swap16(uint16_t val) { return (val >> 8) | (val << 8);	}

};

/**
 * Helper class implementing the gyro driver node.
 */
class MPU6000_gyro : public device::CDev
{
public:
	MPU6000_gyro(MPU6000 *parent);
	~MPU6000_gyro();

	virtual ssize_t		read(struct file *filp, char *buffer, size_t buflen);
	virtual int		ioctl(struct file *filp, int cmd, unsigned long arg);

protected:
	friend class MPU6000;

	void			parent_poll_notify();
private:
	MPU6000			*_parent;
};

/** driver 'main' command */
extern "C" { __EXPORT int mpu6000_main(int argc, char *argv[]); }

MPU6000::MPU6000(int bus, spi_dev_e device) :
	SPI("MPU6000", ACCEL_DEVICE_PATH, bus, device, SPIDEV_MODE3, 10000000),
	_gyro(new MPU6000_gyro(this)),
	_product(0),
	_call_interval(0),
	_accel_range_scale(1.0f),
	_accel_topic(-1),
	_gyro_range_scale(1.0f),
	_gyro_topic(-1),
	_reads(0),
	_sample_perf(perf_alloc(PC_ELAPSED, "mpu6000_read"))
{
	// enable debug() calls
	_debug_enabled = true;

	// default accel scale factors
	_accel_scale.x_offset = 0;
	_accel_scale.x_scale  = 1.0f;
	_accel_scale.y_offset = 0;
	_accel_scale.y_scale  = 1.0f;
	_accel_scale.z_offset = 0;
	_accel_scale.z_scale  = 1.0f;

	// default gyro scale factors
	_gyro_scale.x_offset = 0;
	_gyro_scale.x_scale  = 1.0f;
	_gyro_scale.y_offset = 0;
	_gyro_scale.y_scale  = 1.0f;
	_gyro_scale.z_offset = 0;
	_gyro_scale.z_scale  = 1.0f;

	memset(&_accel_report, 0, sizeof(_accel_report));
	memset(&_gyro_report, 0, sizeof(_gyro_report));
}

MPU6000::~MPU6000()
{
	/* make sure we are truly inactive */
	stop();

	/* delete the gyro subdriver */
	delete _gyro;

	/* delete the perf counter */
	perf_free(_sample_perf);
}

int
MPU6000::init()
{
	int ret;

	/* do SPI init (and probe) first */
	ret = SPI::init();

	/* if probe/setup failed, bail now */
	if (ret != OK) {
		debug("SPI setup failed");
		return ret;
	}

	/* advertise sensor topics */
	_accel_topic = orb_advertise(ORB_ID(sensor_accel), &_accel_report);
	_gyro_topic = orb_advertise(ORB_ID(sensor_gyro), &_gyro_report);

	// Chip reset
	write_reg(MPUREG_PWR_MGMT_1, BIT_H_RESET);
	up_udelay(10000);

	// Wake up device and select GyroZ clock (better performance)
	write_reg(MPUREG_PWR_MGMT_1, MPU_CLK_SEL_PLLGYROZ);
	up_udelay(1000);

	// Disable I2C bus (recommended on datasheet)
	write_reg(MPUREG_USER_CTRL, BIT_I2C_IF_DIS);
	up_udelay(1000);

	// SAMPLE RATE
	write_reg(MPUREG_SMPLRT_DIV, 0x04);     // Sample rate = 200Hz    Fsample= 1Khz/(4+1) = 200Hz
	usleep(1000);

	// FS & DLPF   FS=2000¼/s, DLPF = 98Hz (low pass filter)
	write_reg(MPUREG_CONFIG, BITS_DLPF_CFG_98HZ);
	usleep(1000);
	write_reg(MPUREG_GYRO_CONFIG, BITS_FS_2000DPS); // Gyro scale 2000¼/s
	usleep(1000);

	// product-specific scaling
	switch (_product) {
	case MPU6000ES_REV_C4:
	case MPU6000ES_REV_C5:
	case MPU6000_REV_C4:
	case MPU6000_REV_C5:
		// Accel scale 8g (4096 LSB/g)
		// Rev C has different scaling than rev D
		write_reg(MPUREG_ACCEL_CONFIG, 1 << 3);
		break;

	case MPU6000ES_REV_D6:
	case MPU6000ES_REV_D7:
	case MPU6000ES_REV_D8:
	case MPU6000_REV_D6:
	case MPU6000_REV_D7:
	case MPU6000_REV_D8:
	case MPU6000_REV_D9:
	case MPU6000_REV_D10:
		// Accel scale 8g (4096 LSB/g)
		write_reg(MPUREG_ACCEL_CONFIG, 2 << 3);
		break;
	}

	usleep(1000);

	// INT CFG => Interrupt on Data Ready
	write_reg(MPUREG_INT_ENABLE, BIT_RAW_RDY_EN);        // INT: Raw data ready
	usleep(1000);
	write_reg(MPUREG_INT_PIN_CFG, BIT_INT_ANYRD_2CLEAR); // INT: Clear on any read
	usleep(1000);

	// Oscillator set
	// write_reg(MPUREG_PWR_MGMT_1,MPU_CLK_SEL_PLLGYROZ);
	usleep(1000);


	return ret;
}

int
MPU6000::open_first(struct file *filp)
{
	/* reset to manual-poll mode */
	_call_interval = 0;

	/* XXX set default sampling/acquisition parameters */

	return OK;
}

int
MPU6000::close_last(struct file *filp)
{
	/* stop measurement */
	stop();

	return OK;
}

int
MPU6000::probe()
{

	/* look for a product ID we recognise */
	_product = read_reg(MPUREG_PRODUCT_ID);

	// verify product revision
	switch (_product) {
	case MPU6000ES_REV_C4:
	case MPU6000ES_REV_C5:
	case MPU6000_REV_C4:
	case MPU6000_REV_C5:
	case MPU6000ES_REV_D6:
	case MPU6000ES_REV_D7:
	case MPU6000ES_REV_D8:
	case MPU6000_REV_D6:
	case MPU6000_REV_D7:
	case MPU6000_REV_D8:
	case MPU6000_REV_D9:
	case MPU6000_REV_D10:
		log("ID 0x%02x", _product);
		return OK;
	}

	debug("unexpected ID 0x%02x", _product);
	return -EIO;
}

ssize_t
MPU6000::read(struct file *filp, char *buffer, size_t buflen)
{
	int ret = 0;

	/* buffer must be large enough */
	if (buflen < sizeof(_accel_report))
		return -ENOSPC;

	/* if automatic measurement is not enabled */
	if (_call_interval == 0)
		measure();

	/* copy out the latest report */
	memcpy(buffer, &_accel_report, sizeof(_accel_report));
	ret = sizeof(_accel_report);

	return ret;
}

ssize_t
MPU6000::gyro_read(struct file *filp, char *buffer, size_t buflen)
{
	int ret = 0;

	/* buffer must be large enough */
	if (buflen < sizeof(_gyro_report))
		return -ENOSPC;

	/* if automatic measurement is not enabled */
	if (_call_interval == 0)
		measure();

	/* copy out the latest report */
	memcpy(buffer, &_gyro_report, sizeof(_gyro_report));
	ret = sizeof(_gyro_report);

	return ret;
}

int
MPU6000::ioctl(struct file *filp, int cmd, unsigned long arg)
{
	switch (cmd) {

	case ACCELIOCSPOLLRATE: {
			switch (arg) {

				/* switching to manual polling */
			case ACC_POLLRATE_MANUAL:
				stop();
				_call_interval = 0;
				return OK;

				/* external signalling not supported */
			case ACC_POLLRATE_EXTERNAL:

				/* zero would be bad */
			case 0:
				return -EINVAL;

				/* adjust to a legal polling interval in Hz */
			default: {
					/* do we need to start internal polling? */
					bool want_start = (_call_interval == 0);

					/* convert hz to hrt interval via microseconds */
					unsigned ticks = 1000000 / arg;

					/* check against maximum sane rate */
					if (ticks < 1000)
						return -EINVAL;

					/* update interval for next measurement */
					/* XXX this is a bit shady, but no other way to adjust... */
					_call.period = _call_interval;

					/* if we need to start the poll state machine, do it */
					if (want_start)
						start();

					return OK;
				}
			}
		}

	case ACCELIOCSQUEUEDEPTH:
		/* XXX not implemented */
		return -EINVAL;

	case ACCELIOCSLOWPASS:
		/* XXX not implemented */
		return -EINVAL;

	case ACCELIORANGE:
		/* XXX not implemented really */
		return set_range(arg);

	case ACCELIOCSSAMPLERATE:	/* sensor sample rate is not (really) adjustable */
	case ACCELIOCSREPORTFORMAT:	/* no alternate report formats */
		return -EINVAL;

	default:
		/* give it to the superclass */
		return SPI::ioctl(filp, cmd, arg);
	}
}

int
MPU6000::gyro_ioctl(struct file *filp, int cmd, unsigned long arg)
{
	switch (cmd) {

	case GYROIOCSPOLLRATE:
		/* gyro and accel poll rates are shared */
		return ioctl(filp, ACCELIOCSPOLLRATE, arg);

	case GYROIOCSQUEUEDEPTH:
		/* XXX not implemented */
		return -EINVAL;

	case GYROIOCSLOWPASS:
		/* XXX not implemented */
		return -EINVAL;

	case GYROIOCSSCALE:
		/* XXX not implemented really */
		return set_range(arg);

	case GYROIOCSSAMPLERATE:	/* sensor sample rate is not (really) adjustable */
	case GYROIOCSREPORTFORMAT:	/* no alternate report formats */
		return -EINVAL;

	default:
		/* give it to the superclass */
		return SPI::ioctl(filp, cmd, arg);
	}
}

uint8_t
MPU6000::read_reg(unsigned reg)
{
	uint8_t cmd[2];

	cmd[0] = reg | DIR_READ;

	transfer(cmd, cmd, sizeof(cmd));

	return cmd[1];
}

uint16_t
MPU6000::read_reg16(unsigned reg)
{
	uint8_t cmd[3];

	cmd[0] = reg | DIR_READ;

	transfer(cmd, cmd, sizeof(cmd));

	return (uint16_t)(cmd[1] << 8) | cmd[2];
}

void
MPU6000::write_reg(unsigned reg, uint8_t value)
{
	uint8_t	cmd[2];

	cmd[0] = reg | DIR_WRITE;
	cmd[1] = value;

	transfer(cmd, nullptr, sizeof(cmd));
}

void
MPU6000::modify_reg(unsigned reg, uint8_t clearbits, uint8_t setbits)
{
	uint8_t	val;

	val = read_reg(reg);
	val &= ~clearbits;
	val |= setbits;
	write_reg(reg, val);
}

int
MPU6000::set_range(unsigned max_g)
{
#if 0
	uint8_t rangebits;
	float rangescale;

	if (max_g > 16) {
		return -ERANGE;

	} else if (max_g > 8) {		/* 16G */
		rangebits = OFFSET_LSB1_RANGE_16G;
		rangescale = 1.98;

	} else if (max_g > 4) {		/* 8G */
		rangebits = OFFSET_LSB1_RANGE_8G;
		rangescale = 0.99;

	} else if (max_g > 3) {		/* 4G */
		rangebits = OFFSET_LSB1_RANGE_4G;
		rangescale = 0.5;

	} else if (max_g > 2) {		/* 3G */
		rangebits = OFFSET_LSB1_RANGE_3G;
		rangescale = 0.38;

	} else if (max_g > 1) {		/* 2G */
		rangebits = OFFSET_LSB1_RANGE_2G;
		rangescale = 0.25;

	} else {			/* 1G */
		rangebits = OFFSET_LSB1_RANGE_1G;
		rangescale = 0.13;
	}

	/* adjust sensor configuration */
	modify_reg(ADDR_OFFSET_LSB1, OFFSET_LSB1_RANGE_MASK, rangebits);
	_range_scale = rangescale;
#endif
	return OK;
}

void
MPU6000::start()
{
	/* make sure we are stopped first */
	stop();

	/* start polling at the specified rate */
	hrt_call_every(&_call, 1000, _call_interval, (hrt_callout)&MPU6000::measure_trampoline, this);
}

void
MPU6000::stop()
{
	hrt_cancel(&_call);
}

void
MPU6000::measure_trampoline(void *arg)
{
	MPU6000 *dev = (MPU6000 *)arg;

	/* make another measurement */
	dev->measure();
}

void
MPU6000::measure()
{
#pragma pack(push, 1)
	/**
	 * Report conversation within the MPU6000, including command byte and
	 * interrupt status.
	 */
	struct Report {
		uint8_t		cmd;
		uint8_t		status;
		uint16_t	accel_x;
		uint16_t	accel_y;
		uint16_t	accel_z;
		uint16_t	temp;
		uint16_t	gyro_x;
		uint16_t	gyro_y;
		uint16_t	gyro_z;
	} report;
#pragma pack(pop)

	/* start measuring */
	perf_begin(_sample_perf);

	/*
	 * Fetch the full set of measurements from the MPU6000 in one pass.
	 */
	report.cmd = DIR_READ | MPUREG_INT_STATUS;
	transfer((uint8_t *)&report, (uint8_t *)&report, sizeof(report));

	/*
	 * Adjust and scale results to mg.
	 */
	_gyro_report.timestamp = _accel_report.timestamp = hrt_absolute_time();

	_accel_report.x = report.accel_x * _accel_range_scale;
	_accel_report.y = report.accel_y * _accel_range_scale;
	_accel_report.z = report.accel_z * _accel_range_scale;

	_gyro_report.x = report.gyro_x * _gyro_range_scale;
	_gyro_report.y = report.gyro_y * _gyro_range_scale;
	_gyro_report.z = report.gyro_z * _gyro_range_scale;

	/* notify anyone waiting for data */
	poll_notify(POLLIN);
	_gyro->parent_poll_notify();

	/* and publish for subscribers */
	orb_publish(ORB_ID(sensor_accel), _accel_topic, &_accel_report);
	orb_publish(ORB_ID(sensor_gyro), _gyro_topic, &_gyro_report);

	/* stop measuring */
	perf_end(_sample_perf);
}

void
MPU6000::print_info()
{
	printf("reads:          %u\n", _reads);
}

MPU6000_gyro::MPU6000_gyro(MPU6000 *parent) :
	CDev("MPU6000_gyro", GYRO_DEVICE_PATH),
	_parent(parent)
{
}

MPU6000_gyro::~MPU6000_gyro()
{
}

void
MPU6000_gyro::parent_poll_notify()
{
	poll_notify(POLLIN);
}

ssize_t
MPU6000_gyro::read(struct file *filp, char *buffer, size_t buflen)
{
	return _parent->gyro_read(filp, buffer, buflen);
}

int
MPU6000_gyro::ioctl(struct file *filp, int cmd, unsigned long arg)
{
	return _parent->gyro_ioctl(filp, cmd, arg);
}

/**
 * Local functions in support of the shell command.
 */
namespace
{

MPU6000	*g_dev;

/*
 * XXX this should just be part of the generic sensors test...
 */

int
test()
{
	int fd = -1;
	struct accel_report report;
	ssize_t sz;
	const char *reason = "test OK";

	do {

		/* get the driver */
		fd = open(ACCEL_DEVICE_PATH, O_RDONLY);

		if (fd < 0) {
			reason = "can't open driver";
			break;
		}

		/* do a simple demand read */
		sz = read(fd, &report, sizeof(report));

		if (sz != sizeof(report)) {
			reason = "immediate read failed";
			break;
		}

		printf("single read\n");
		fflush(stdout);
		printf("time:        %lld\n", report.timestamp);
		printf("x:           %f\n", report.x);
		printf("y:           %f\n", report.y);
		printf("z:           %f\n", report.z);

	} while (0);

	printf("MPU6000: %s\n", reason);

	return OK;
}

int
info()
{
	if (g_dev == nullptr) {
		fprintf(stderr, "MPU6000: driver not running\n");
		return -ENOENT;
	}

	printf("state @ %p\n", g_dev);
	g_dev->print_info();

	return OK;
}


} // namespace

int
mpu6000_main(int argc, char *argv[])
{
	/*
	 * Start/load the driver.
	 *
	 * XXX it would be nice to have a wrapper for this...
	 */
	if (!strcmp(argv[1], "start")) {

		if (g_dev != nullptr) {
			fprintf(stderr, "MPU6000: already loaded\n");
			return -EBUSY;
		}

		/* create the driver */
		g_dev = new MPU6000(1 /* XXX magic number */, (spi_dev_e)PX4_SPIDEV_MPU);

		if (g_dev == nullptr) {
			fprintf(stderr, "MPU6000: driver alloc failed\n");
			return -ENOMEM;
		}

		if (OK != g_dev->init()) {
			fprintf(stderr, "MPU6000: driver init failed\n");
			usleep(100000);
			delete g_dev;
			g_dev = nullptr;
			return -EIO;
		}

		return OK;
	}

	/*
	 * Test the driver/device.
	 */
	if (!strcmp(argv[1], "test"))
		return test();

	/*
	 * Print driver information.
	 */
	if (!strcmp(argv[1], "info"))
		return info();

	fprintf(stderr, "unrecognised command, try 'start', 'test' or 'info'\n");
	return -EINVAL;
}