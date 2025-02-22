#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#ifdef ARCH_CV182X
#include <linux/cvi_vip_snsr.h>
#include "cvi_comm_video.h"
#else
#include <linux/cvi_comm_video.h>
#endif
#include "cvi_sns_ctrl.h"
#include "q03_cmos_ex.h"

#define Q03_CHIP_ID_HI_ADDR		0x0A
#define Q03_CHIP_ID_LO_ADDR		0x0B
#define Q03_CHIP_ID			0x0507

static void q03_linear_1296p30_init(VI_PIPE ViPipe);

CVI_U8 q03_i2c_addr = 0x46;        /* I2C Address of Q03 */
const CVI_U32 q03_addr_byte = 1;
const CVI_U32 q03_data_byte = 1;
static int g_fd[VI_MAX_PIPE_NUM] = {[0 ... (VI_MAX_PIPE_NUM - 1)] = -1};

int q03_i2c_init(VI_PIPE ViPipe)
{
	char acDevFile[16] = {0};
	CVI_U8 u8DevNum;

	if (g_fd[ViPipe] >= 0)
		return CVI_SUCCESS;
	int ret;

	u8DevNum = g_aunQ03_BusInfo[ViPipe].s8I2cDev;
	snprintf(acDevFile, sizeof(acDevFile),  "/dev/i2c-%u", u8DevNum);

	g_fd[ViPipe] = open(acDevFile, O_RDWR, 0600);

	if (g_fd[ViPipe] < 0) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "Open /dev/cvi_i2c_drv-%u error!\n", u8DevNum);
		return CVI_FAILURE;
	}

	ret = ioctl(g_fd[ViPipe], I2C_SLAVE_FORCE, q03_i2c_addr);
	if (ret < 0) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "I2C_SLAVE_FORCE error!\n");
		close(g_fd[ViPipe]);
		g_fd[ViPipe] = -1;
		return ret;
	}

	return CVI_SUCCESS;
}

int q03_i2c_exit(VI_PIPE ViPipe)
{
	if (g_fd[ViPipe] >= 0) {
		close(g_fd[ViPipe]);
		g_fd[ViPipe] = -1;
		return CVI_SUCCESS;
	}
	return CVI_FAILURE;
}

int q03_read_register(VI_PIPE ViPipe, int addr)
{
	int ret, data;
	CVI_U8 buf[8];
	CVI_U8 idx = 0;

	if (g_fd[ViPipe] < 0)
		return CVI_FAILURE;

	// add address byte 0
	buf[idx++] = addr & 0xff;

	ret = write(g_fd[ViPipe], buf, q03_addr_byte);
	if (ret < 0) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "I2C_WRITE error!\n");
		return ret;
	}

	buf[0] = 0;
	ret = read(g_fd[ViPipe], buf, q03_data_byte);
	if (ret < 0) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "I2C_READ error!\n");
		return ret;
	}

	// pack read back data
	data = 0;
	if (q03_data_byte == 1) {
		data = buf[0];
	}

	syslog(LOG_DEBUG, "i2c r 0x%x = 0x%x\n", addr, data);
	return data;
}

int q03_write_register(VI_PIPE ViPipe, int addr, int data)
{
	CVI_U8 idx = 0;
	int ret;
	CVI_U8 buf[8];

	if (g_fd[ViPipe] < 0)
		return CVI_SUCCESS;

	if (q03_addr_byte == 1) {
		buf[idx] = addr & 0xff;
		idx++;
	}

	if (q03_data_byte == 1) {
		buf[idx] = data & 0xff;
		idx++;
	}

	ret = write(g_fd[ViPipe], buf, q03_addr_byte + q03_data_byte);
	if (ret < 0) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "I2C_WRITE error!\n");
		return CVI_FAILURE;
	}
	syslog(LOG_DEBUG, "i2c w 0x%x 0x%x\n", addr, data);
	return CVI_SUCCESS;
}

static void delay_ms(int ms)
{
	usleep(ms * 1000);
}

void q03_standby(VI_PIPE ViPipe)
{
	q03_write_register(ViPipe, 0x12, 0x40);
}

void q03_restart(VI_PIPE ViPipe)
{
	q03_write_register(ViPipe, 0x12, 0x40);
	delay_ms(20);
	q03_write_register(ViPipe, 0x12, 0x00);
}

void q03_default_reg_init(VI_PIPE ViPipe)
{
	CVI_U32 i;

	for (i = 0; i < g_pastQ03[ViPipe]->astSyncInfo[0].snsCfg.u32RegNum; i++) {
		q03_write_register(ViPipe,
				g_pastQ03[ViPipe]->astSyncInfo[0].snsCfg.astI2cData[i].u32RegAddr,
				g_pastQ03[ViPipe]->astSyncInfo[0].snsCfg.astI2cData[i].u32Data);
	}
}

void q03_mirror_flip(VI_PIPE ViPipe, ISP_SNS_MIRRORFLIP_TYPE_E eSnsMirrorFlip)
{
	CVI_U8 val = q03_read_register(ViPipe, 0x12) & ~0x30;

	switch (eSnsMirrorFlip) {
	case ISP_SNS_NORMAL:
		break;
	case ISP_SNS_MIRROR:
		val |= 0x20;
		break;
	case ISP_SNS_FLIP:
		val |= 0x10;
		break;
	case ISP_SNS_MIRROR_FLIP:
		val |= 0x30;
		break;
	default:
		return;
	}

	q03_write_register(ViPipe, 0x12, val);
}

int q03_probe(VI_PIPE ViPipe)
{
	int nVal;
	CVI_U16 chip_id;

	delay_ms(4);
	if (q03_i2c_init(ViPipe) != CVI_SUCCESS)
		return CVI_FAILURE;

	nVal = q03_read_register(ViPipe, Q03_CHIP_ID_HI_ADDR);
	if (nVal < 0) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "read sensor id error.\n");
		return nVal;
	}
	chip_id = (nVal & 0xFF) << 8;
	nVal = q03_read_register(ViPipe, Q03_CHIP_ID_LO_ADDR);
	if (nVal < 0) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "read sensor id error.\n");
		return nVal;
	}
	chip_id |= (nVal & 0xFF);

	if (chip_id != Q03_CHIP_ID) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "Sensor ID Mismatch! Use the wrong sensor??\n");
		return CVI_FAILURE;
	}

	return CVI_SUCCESS;
}

void q03_init(VI_PIPE ViPipe)
{
	q03_i2c_init(ViPipe);

	q03_linear_1296p30_init(ViPipe);

	g_pastQ03[ViPipe]->bInit = CVI_TRUE;
}

void q03_exit(VI_PIPE ViPipe)
{
	q03_i2c_exit(ViPipe);
}

static void q03_linear_1296p30_init(VI_PIPE ViPipe)
{
	q03_write_register(ViPipe, 0x12, 0x40);
	q03_write_register(ViPipe, 0x48, 0x96);
	q03_write_register(ViPipe, 0x48, 0x16);
	q03_write_register(ViPipe, 0x0E, 0x11);
	q03_write_register(ViPipe, 0x0F, 0x14);
	q03_write_register(ViPipe, 0x10, 0x36);
	q03_write_register(ViPipe, 0x11, 0x80);
	q03_write_register(ViPipe, 0x0D, 0xB0);
	q03_write_register(ViPipe, 0x5F, 0x42);
	q03_write_register(ViPipe, 0x60, 0x2B);
	q03_write_register(ViPipe, 0x58, 0x1A);
	q03_write_register(ViPipe, 0x57, 0x60);
	q03_write_register(ViPipe, 0x20, 0x84);
	q03_write_register(ViPipe, 0x21, 0x03);
	q03_write_register(ViPipe, 0x22, 0x46);
	q03_write_register(ViPipe, 0x23, 0x05);
	q03_write_register(ViPipe, 0x24, 0x42);
	q03_write_register(ViPipe, 0x25, 0x10);
	q03_write_register(ViPipe, 0x26, 0x52);
	q03_write_register(ViPipe, 0x27, 0x53);
	q03_write_register(ViPipe, 0x28, 0x15);
	q03_write_register(ViPipe, 0x29, 0x03);
	q03_write_register(ViPipe, 0x2A, 0x4E);
	q03_write_register(ViPipe, 0x2B, 0x13);
	q03_write_register(ViPipe, 0x2C, 0x00);
	q03_write_register(ViPipe, 0x2D, 0x00);
	q03_write_register(ViPipe, 0x2E, 0x4A);
	q03_write_register(ViPipe, 0x2F, 0x64);
	q03_write_register(ViPipe, 0x41, 0x8A);
	q03_write_register(ViPipe, 0x42, 0x14);
	q03_write_register(ViPipe, 0x47, 0x42);
	q03_write_register(ViPipe, 0x76, 0x4A);
	q03_write_register(ViPipe, 0x77, 0x0B);
	q03_write_register(ViPipe, 0x80, 0x00);
	q03_write_register(ViPipe, 0xAF, 0x22);
	q03_write_register(ViPipe, 0xAB, 0x00);
	q03_write_register(ViPipe, 0x46, 0x00);
	q03_write_register(ViPipe, 0x1D, 0x00);
	q03_write_register(ViPipe, 0x1E, 0x04);
	q03_write_register(ViPipe, 0x6C, 0x40);
	q03_write_register(ViPipe, 0x6E, 0x2C);
	q03_write_register(ViPipe, 0x70, 0xD9);
	q03_write_register(ViPipe, 0x71, 0xD0);
	q03_write_register(ViPipe, 0x72, 0xD5);
	q03_write_register(ViPipe, 0x73, 0x59);
	q03_write_register(ViPipe, 0x74, 0x02);
	q03_write_register(ViPipe, 0x78, 0x96);
	q03_write_register(ViPipe, 0x89, 0x01);
	q03_write_register(ViPipe, 0x6B, 0x20);
	q03_write_register(ViPipe, 0x86, 0x40);
	q03_write_register(ViPipe, 0x31, 0x0A);
	q03_write_register(ViPipe, 0x32, 0x21);
	q03_write_register(ViPipe, 0x33, 0x5C);
	q03_write_register(ViPipe, 0x34, 0x44);
	q03_write_register(ViPipe, 0x35, 0x40);
	q03_write_register(ViPipe, 0x3A, 0xA0);
	q03_write_register(ViPipe, 0x3B, 0x38);
	q03_write_register(ViPipe, 0x3C, 0x50);
	q03_write_register(ViPipe, 0x3D, 0x5B);
	q03_write_register(ViPipe, 0x3E, 0xFF);
	q03_write_register(ViPipe, 0x3F, 0x54);
	q03_write_register(ViPipe, 0x40, 0xFF);
	q03_write_register(ViPipe, 0x56, 0xB2);
	q03_write_register(ViPipe, 0x59, 0x50);
	q03_write_register(ViPipe, 0x5A, 0x04);
	q03_write_register(ViPipe, 0x85, 0x34);
	q03_write_register(ViPipe, 0x8A, 0x04);
	q03_write_register(ViPipe, 0x91, 0x08);
	q03_write_register(ViPipe, 0xA9, 0x08);
	q03_write_register(ViPipe, 0x9C, 0xE1);
	q03_write_register(ViPipe, 0x5B, 0xA0);
	q03_write_register(ViPipe, 0x5C, 0x80);
	q03_write_register(ViPipe, 0x5D, 0xEF);
	q03_write_register(ViPipe, 0x5E, 0x14);
	q03_write_register(ViPipe, 0x64, 0xE0);
	q03_write_register(ViPipe, 0x66, 0x04);
	q03_write_register(ViPipe, 0x67, 0x77);
	q03_write_register(ViPipe, 0x68, 0x00);
	q03_write_register(ViPipe, 0x69, 0x41);
	q03_write_register(ViPipe, 0x7A, 0xA0);
	q03_write_register(ViPipe, 0x8F, 0x91);
	q03_write_register(ViPipe, 0x9D, 0x70);
	q03_write_register(ViPipe, 0xAE, 0x30);
	q03_write_register(ViPipe, 0x13, 0x81);
	q03_write_register(ViPipe, 0x96, 0x04);
	q03_write_register(ViPipe, 0x4A, 0x01);
	q03_write_register(ViPipe, 0x7E, 0xC9);
	q03_write_register(ViPipe, 0x50, 0x02);
	q03_write_register(ViPipe, 0x49, 0x10);
	q03_write_register(ViPipe, 0x7B, 0x4A);
	q03_write_register(ViPipe, 0x7C, 0x0F);
	q03_write_register(ViPipe, 0x7F, 0x57);
	q03_write_register(ViPipe, 0x62, 0x21);
	q03_write_register(ViPipe, 0x90, 0x00);
	q03_write_register(ViPipe, 0x8C, 0xFF);
	q03_write_register(ViPipe, 0x8D, 0xC7);
	q03_write_register(ViPipe, 0x8E, 0x00);
	q03_write_register(ViPipe, 0x8B, 0x01);
	q03_write_register(ViPipe, 0x0C, 0x00);
	q03_write_register(ViPipe, 0xBB, 0x11);
	q03_write_register(ViPipe, 0x6A, 0x12);
	q03_write_register(ViPipe, 0x65, 0x32);
	q03_write_register(ViPipe, 0x82, 0x01);
	q03_write_register(ViPipe, 0xA3, 0x20);
	q03_write_register(ViPipe, 0xA0, 0x01);
	q03_write_register(ViPipe, 0x81, 0x74);
	q03_write_register(ViPipe, 0xA2, 0xFF);
	q03_write_register(ViPipe, 0x19, 0x20);

	q03_default_reg_init(ViPipe);

	q03_write_register(ViPipe, 0x12, 0x00);
	q03_write_register(ViPipe, 0x48, 0x96);
	q03_write_register(ViPipe, 0x48, 0x16);

	printf("ViPipe:%d,===Q03 1296P 30fps 10bit LINE FPGA Init OK!===\n", ViPipe);
}

