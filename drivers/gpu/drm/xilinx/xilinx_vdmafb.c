/*
 * xilinx_vdmafb.c - Framebuffer driver based on Xilinx VDMA IP Core
 *
 * Copyright (C) 2019 Guangzhou Alientek Electronic Technology Co., Ltd.
 * Author: Deng Tao <773904075@qq.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/dma/xilinx_dma.h>
#include <linux/of_dma.h>
#include <linux/clk.h>
#include <linux/of_gpio.h>
#include <video/of_display_timing.h>
#include <video/of_videomode.h>
#include <video/videomode.h>
#include "xilinx_vtc.h"

#define  DRIVER_NAME "xilinx_vdmafb"
#define  ATK_LCD_MAX_XRES    1920
#define  ATK_LCD_MIN_XRES    480
#define  ATK_LCD_MAX_YRES    1080
#define  ATK_LCD_MIN_YRES    272

enum vdma_pixel_format {
	VDMA_PIXEL_FORMAT_NONE   = -1,
	VDMA_PIXEL_FORMAT_RGB565 = 0,
	VDMA_PIXEL_FORMAT_BGR565 = 1,
	VDMA_PIXEL_FORMAT_RGB888 = 2,
	VDMA_PIXEL_FORMAT_BGR888 = 3,
};

struct vdma_video_format_desc {
	const char *name;
	unsigned int depth;
	unsigned int bpp;
	enum vdma_pixel_format pixel_format;
};

static const struct vdma_video_format_desc video_formats_table[] = {
	{ "rgb565", 16, 16, VDMA_PIXEL_FORMAT_RGB565 },
	{ "bgr565", 16, 16, VDMA_PIXEL_FORMAT_BGR565 },
	{ "rgb888", 24, 24, VDMA_PIXEL_FORMAT_RGB888 },
	{ "bgr888", 24, 24, VDMA_PIXEL_FORMAT_BGR888 },
};

struct xilinx_vdmafb_videomode {
	struct videomode vmode;
	struct fb_videomode fb_vmode;
	const struct vdma_video_format_desc *desc;
};

struct xilinx_vdmafb_dev {
	struct fb_info *info;
	struct platform_device *pdev;
	struct clk *pclk;
	struct xilinx_vtc *vtc;
	struct dma_chan *dma;
	struct dma_interleaved_template *dma_template;
	int is_hdmi;
	//unsigned int bl_gpio;
	unsigned int rst_gpio;
	unsigned int spi_cs;
	unsigned int spi_scl;
	unsigned int spi_sdi;
	unsigned int spi_sdo;
};

static inline unsigned chan_to_field(unsigned chan, struct fb_bitfield *bf)
{
	chan &= 0xffff;
	chan >>= 16 - bf->length;
	return chan << bf->offset;
}

static int vdmafb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
		u_int transp, struct fb_info *fb_info)
{
	unsigned int val = 0x0;
	int ret = -EINVAL;

	/*
	 * If greyscale is true, then we convert the RGB value
	 * to greyscale no matter what visual we are using.
	 */
	if (fb_info->var.grayscale)
		red = green = blue = (19595 * red + 38470 * green +
					7471 * blue) >> 16;

	switch (fb_info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
		/*
		 * 12 or 16-bit True Colour.  We encode the RGB value
		 * according to the RGB bitfield information.
		 */
		if (regno < 16) {
			u32 *pal = fb_info->pseudo_palette;

			val  = chan_to_field(red, &fb_info->var.red);
			val |= chan_to_field(green, &fb_info->var.green);
			val |= chan_to_field(blue, &fb_info->var.blue);

			if (fb_info->var.transp.length > 0) {
				u32 mask = (1 << fb_info->var.transp.length) - 1;
				mask <<= fb_info->var.transp.offset;
				val |= mask;
			}

			pal[regno] = val;
			ret = 0;
		}
		break;

	case FB_VISUAL_STATIC_PSEUDOCOLOR:
	case FB_VISUAL_PSEUDOCOLOR:
		break;
	}

	return ret;
}

static int vdmafb_check_var(struct fb_var_screeninfo *var,
			struct fb_info *fb_info)
{
	struct fb_var_screeninfo *fb_var = &fb_info->var;
	memcpy(var, fb_var, sizeof(struct fb_var_screeninfo));

	return 0;
}

static struct fb_ops vdmafb_ops = {
	.owner        = THIS_MODULE,
	.fb_setcolreg = vdmafb_setcolreg,
	.fb_check_var = vdmafb_check_var,
	.fb_fillrect  = sys_fillrect,
	.fb_copyarea  = sys_copyarea,
	.fb_imageblit = sys_imageblit,
};

static int vdmafb_get_fbinfo_dt(struct xilinx_vdmafb_dev *fbdev,
			struct xilinx_vdmafb_videomode *mode)
{
	struct device *dev = &fbdev->pdev->dev;
	struct device_node *node = NULL;
	const char *name = NULL;
	const struct vdma_video_format_desc *desc = NULL;
	u32 val = 0;
	u32 flags = 0;
	int ret = -1;
	int i   = 0;
	int display_timing_num;

	ret = of_property_read_string(dev->of_node, "xlnx,pixel-format", &name);
	if (0 > ret) {
		dev_err(dev, "Failed to get pixel format from DT\n");
		return ret;
	}

	/* Check LCD or HDMI */
	ret = of_property_read_u32(dev->of_node, "is-hdmi", &fbdev->is_hdmi);
	if (!ret && fbdev->is_hdmi) {
		display_timing_num = 0;
		goto check_done;
	}

	fbdev->is_hdmi = 0;
	display_timing_num = 0;

check_done:
	node = of_parse_phandle(dev->of_node, "display-timings", display_timing_num);
	if (!node) {
		dev_err(dev, "Failed to find display timing phandle\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(node, "clock-frequency", (u32 *)(&mode->vmode.pixelclock));
	if (0 > ret) {
		dev_err(dev, "Failed to get property clock frequency\n");
		goto err;
	}

	ret = of_property_read_u32(node, "hactive", &mode->vmode.hactive);
	if (0 > ret) {
		dev_err(dev, "Failed to get property hactive\n");
		goto err;
	}

	ret = of_property_read_u32(node, "vactive", &mode->vmode.vactive);
	if (0 > ret) {
		dev_err(dev, "Failed to get property vactive\n");
		goto err;
	}

	ret = of_property_read_u32(node, "hback-porch", &mode->vmode.hback_porch);
	if (0 > ret) {
		dev_err(dev, "Failed to get property hback porch\n");
		goto err;
	}

	ret = of_property_read_u32(node, "hsync-len", &mode->vmode.hsync_len);
	if (0 > ret) {
		dev_err(dev, "Failed to get property hsync len\n");
		goto err;
	}

	ret = of_property_read_u32(node, "hfront-porch", &mode->vmode.hfront_porch);
	if (0 > ret) {
		dev_err(dev, "Failed to get property hfront porch\n");
		goto err;
	}

	ret = of_property_read_u32(node, "vback-porch", &mode->vmode.vback_porch);
	if (0 > ret) {
		dev_err(dev, "Failed to get property vback porch\n");
		goto err;
        }

	ret = of_property_read_u32(node, "vsync-len", &mode->vmode.vsync_len);
	if (0 > ret) {
		dev_err(dev, "Failed to get property vsync len\n");
		goto err;
	}

	ret = of_property_read_u32(node, "vfront-porch", &mode->vmode.vfront_porch);
	if (0 > ret) {
		dev_err(dev, "Failed to get property vfront porch\n");
		goto err;
	}

	if (!of_property_read_u32(node, "vsync-active", &val))
		flags |= val ? DISPLAY_FLAGS_VSYNC_HIGH : DISPLAY_FLAGS_VSYNC_LOW;
	if (!of_property_read_u32(node, "hsync-active", &val))
		flags |= val ? DISPLAY_FLAGS_HSYNC_HIGH : DISPLAY_FLAGS_HSYNC_LOW;
	if (!of_property_read_u32(node, "de-active", &val))
		flags |= val ? DISPLAY_FLAGS_DE_HIGH : DISPLAY_FLAGS_DE_LOW;
	if (!of_property_read_u32(node, "pixelclk-active", &val))
		flags |= val ? DISPLAY_FLAGS_PIXDATA_POSEDGE : DISPLAY_FLAGS_PIXDATA_NEGEDGE;
	mode->vmode.flags = flags;
	of_node_put(node);

	mode->desc = NULL;
	for (i = 0; i < ARRAY_SIZE(video_formats_table); i++) {

		desc = &video_formats_table[i];
		if (0 == strcmp(desc->name, name)) {
			mode->desc = desc;
			break;
		}
	}

	if (NULL == mode->desc) {
		dev_err(dev, "Does not support %s format\n", name);
		return -EINVAL;
	}

	return fb_videomode_from_videomode(&mode->vmode, &mode->fb_vmode);

err:
	of_node_put(node);
	return ret;
}

static void vdmafb_init_fbinfo_var(struct xilinx_vdmafb_dev *fbdev,
			struct xilinx_vdmafb_videomode *mode)
{
	struct fb_var_screeninfo *var = &fbdev->info->var;
	unsigned int format = mode->desc->pixel_format;

	switch (format) {
	case VDMA_PIXEL_FORMAT_RGB565:
		var->red.offset = 0;
		var->green.offset = 5;
		var->blue.offset = 11;

		var->red.length = 5;
		var->green.length = 6;
		var->blue.length = 5;

		var->bits_per_pixel = 16;
		break;

	case VDMA_PIXEL_FORMAT_BGR565:
		var->red.offset = 11;
		var->green.offset = 5;
		var->blue.offset = 0;

		var->red.length = 5;
		var->green.length = 6;
		var->blue.length = 5;

		var->bits_per_pixel = 16;
		break;

	case VDMA_PIXEL_FORMAT_RGB888:
		var->red.offset = 0;
		var->green.offset = 8;
		var->blue.offset = 16;

		var->red.length = 8;
		var->green.length = 8;
		var->blue.length = 8;

		var->bits_per_pixel = 24;
		break;

	case VDMA_PIXEL_FORMAT_BGR888:
		var->red.offset = 16;
		var->green.offset = 8;
		var->blue.offset = 0;

		var->red.length = 8;
		var->green.length = 8;
		var->blue.length = 8;

		var->bits_per_pixel = 24;
		break;

	default:    /* This is unlikely to happen!!!!  So What */
		break;
	}

	var->transp.offset = 0;
	var->transp.length = 0;

	var->grayscale   = 0;
	var->nonstd      = 0;
	var->activate    = FB_ACTIVATE_NOW;
	var->height      = -1;
	var->width       = -1;
	var->accel_flags = FB_ACCEL_NONE;

	fb_videomode_to_var(var, &mode->fb_vmode);

        if (var->xres < ATK_LCD_MIN_XRES)
                var->xres = ATK_LCD_MIN_XRES;
        if (var->yres < ATK_LCD_MIN_YRES)
                var->yres = ATK_LCD_MIN_YRES;
        if (var->xres > ATK_LCD_MAX_XRES)
                var->xres = ATK_LCD_MAX_XRES;
        if (var->yres > ATK_LCD_MAX_YRES)
                var->yres = ATK_LCD_MAX_YRES;

	var->xres_virtual = var->xres;
	var->yres_virtual = var->yres;
}

static void vdmafb_init_fbinfo_fix(struct xilinx_vdmafb_dev *fbdev)
{
	struct fb_fix_screeninfo *fix = &fbdev->info->fix;

	fix->type   = FB_TYPE_PACKED_PIXELS;
	fix->visual = FB_VISUAL_TRUECOLOR;
	fix->accel  = FB_ACCEL_NONE;
	strcpy(fix->id, "vdmafb");
	//fix->xpanstep = 1;  // Do not set 1, I do not know why
	//fix->ypanstep = 1;
	fix->ywrapstep  = 0;
	fix->mmio_start = 0;
	fix->mmio_len   = 0;
	fix->type_aux   = 0;
}

static int vdmafb_init_vtc(struct xilinx_vdmafb_dev *fbdev)
{
	struct device_node *node = NULL;
	struct device *dev = &fbdev->pdev->dev;

	node = of_parse_phandle(dev->of_node, "xlnx,vtc", 0);
	if (!node) {
		dev_err(dev, "Failed to find vtc phandle\n");
		return -EINVAL;
	}

	fbdev->vtc = xilinx_vtc_probe(dev, node);
	of_node_put(node);
	if (IS_ERR(fbdev->vtc)) {
		dev_err(dev, "Failed to probe video timing controller\n");
		return PTR_ERR(fbdev->vtc);
	}

	xilinx_vtc_reset(fbdev->vtc);
	xilinx_vtc_disable(fbdev->vtc);

	return 0;
}

static int vdmafb_init_vdma(struct xilinx_vdmafb_dev *fbdev)
{
	struct device *dev = &fbdev->pdev->dev;
	struct dma_interleaved_template *dma_template = fbdev->dma_template;
	struct fb_info *info = fbdev->info;
	struct dma_async_tx_descriptor *tx_desc = NULL;
	struct xilinx_vdma_config vdma_config = {0};
	const char *name = NULL;
	int ret = -1;

	ret = of_property_read_string(dev->of_node, "dma-names", &name);
	if (0 > ret) {
		dev_err(dev, "Failed to get vdma name from DT\n");
		return ret;
	}

	fbdev->dma = of_dma_request_slave_channel(dev->of_node, name);
	if (IS_ERR(fbdev->dma)) {
		dev_err(dev, "Failed to request vdma channel\n");
		return PTR_ERR(fbdev->dma);
	}

	dmaengine_terminate_all(fbdev->dma);

	dma_template->dir         = DMA_MEM_TO_DEV;
	dma_template->numf        = info->var.yres;
	dma_template->sgl[0].size = info->fix.line_length;
	dma_template->frame_size  = 1;
	dma_template->sgl[0].icg  = 0;
	dma_template->src_start   = info->fix.smem_start;
	dma_template->src_sgl     = 1;
	dma_template->src_inc     = 1;
	dma_template->dst_inc     = 0;
	dma_template->dst_sgl     = 0;

	tx_desc = dmaengine_prep_interleaved_dma(fbdev->dma, dma_template,
			DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!tx_desc) {
		dev_err(dev, "Failed to prepare DMA descriptor\n");
		dma_release_channel(fbdev->dma);
		return -1;
	}

	vdma_config.park = 1;

	ret = xilinx_vdma_channel_set_config(fbdev->dma, &vdma_config);
	if (0 > ret) {
		dev_err(dev, "Failed to set DMA channel config\n");
		dma_release_channel(fbdev->dma);
		return ret;
	}

	dmaengine_submit(tx_desc);
	dma_async_issue_pending(fbdev->dma);

	return 0;
}

static void write_command(struct xilinx_vdmafb_dev *fbdev, unsigned char y) // (uchar y,uchar x)
{
  unsigned char i;
  //csb=0;
  gpio_set_value(fbdev->spi_cs, 0);
  //sclb=0;
  gpio_set_value(fbdev->spi_scl, 0);
  //sdi=0;
  gpio_set_value(fbdev->spi_sdi, 0);
  //sclb=1;
  gpio_set_value(fbdev->spi_scl, 1);
  for(i=0;i<8;i++)
	{
	 //sclb=0;
	 gpio_set_value(fbdev->spi_scl, 0);
	  if (y&0x80)
	   {
		 // sdi=1;
		  gpio_set_value(fbdev->spi_sdi, 1);
		}
		  else
		 {
		  //sdi=0;
		  gpio_set_value(fbdev->spi_sdi, 0);
		 }
	  // sclb=1;
	   gpio_set_value(fbdev->spi_scl, 1);
	  y=y<<1;
	}
  // csb=1;
   gpio_set_value(fbdev->spi_cs, 1);
}
//***************************************************************
static void write_data(struct xilinx_vdmafb_dev *fbdev, unsigned char w) // (uchar w, uchar v)
{
  unsigned char i;
//csb=0;
gpio_set_value(fbdev->spi_cs, 0);
//sclb=0;
gpio_set_value(fbdev->spi_scl, 0);
//sdi=1;
gpio_set_value(fbdev->spi_sdi, 1);
//sclb=1;
gpio_set_value(fbdev->spi_scl, 1);
for(i=0;i<8;i++)
 {
  //sclb=0;
  gpio_set_value(fbdev->spi_scl, 0);
	 if (w&0x80)
	 {
		//sdi=1;
		gpio_set_value(fbdev->spi_sdi, 1);
	   }
		  else
		{
		 //sdi=0;
		 gpio_set_value(fbdev->spi_sdi, 0);
		}
  // sclb=1;
   gpio_set_value(fbdev->spi_scl, 1);
   w=w<<1;
	}
   //csb=1;
   gpio_set_value(fbdev->spi_cs, 1);
}

static void vdmafb_init_drm(struct xilinx_vdmafb_dev *fbdev)
{

	//res=1;
	gpio_set_value_cansleep(fbdev->rst_gpio, 1);
	msleep(1);
	//res=0;
	gpio_set_value_cansleep(fbdev->rst_gpio, 0);
	msleep(10);
	//res=1;
	gpio_set_value_cansleep(fbdev->rst_gpio, 1);
	msleep(200);
	//***************************************************************//LCD SETING
	write_command(fbdev, 0xFF);        // Change to Page 1 CMD
	write_data(fbdev, 0xff);
	write_data(fbdev, 0x98);
	write_data(fbdev, 0x06);
	write_data(fbdev, 0x04);
	write_data(fbdev, 0x01);

	write_command(fbdev, 0x08); //Output    SDA
	write_data(fbdev, 0x00); //10

	write_command(fbdev, 0x20);//set DE/VSYNC mode
	write_data(fbdev, 0x00); 	  //01 VSYNC MODE

	write_command(fbdev, 0x21); //DE = 1 Active
	write_data(fbdev, 0x01);//01

	//write_command(0x22);//
	//write_data(0x01);

	write_command(fbdev, 0x30);//Resolution setting 480 X 800
	write_data(fbdev, 0x02); //02

	write_command(fbdev, 0x31); //Inversion setting
	write_data(fbdev, 0x00); //02-2dot

	write_command(fbdev, 0x40); //BT DDVDH DDVDL
	write_data(fbdev, 0x00); //10,14,18 00	2XVCI

	write_command(fbdev, 0x41);
	write_data(fbdev, 0x44);  //avdd +5.2v,avee-5.2v 33

	write_command(fbdev, 0x42);
	write_data(fbdev, 0x00); //VGL=DDVDH+VCIP -DDVDL,VGH=2DDVDL-VCIP

	write_command(fbdev, 0x43);
	write_data(fbdev, 0x80); //SET VGH clamp level +10v

	write_command(fbdev, 0x44);
	write_data(fbdev, 0x86); //SET VGL clamp level -10v

	write_command(fbdev, 0x46);
	write_data(fbdev, 0x34);

	write_command(fbdev, 0x50);//VREG1 for positive Gamma
	write_data(fbdev, 0x94); //A8

	write_command(fbdev, 0x51);//VREG2 for negative Gamma
	write_data(fbdev, 0x94); //A8

	write_command(fbdev, 0x52);//VCOM
	write_data(fbdev, 0x00);

	write_command(fbdev, 0x53); //Forward Flicker
	write_data(fbdev, 0x67); //VCOM

	write_command(fbdev, 0x54); //VCOM
	write_data(fbdev, 0x00);

	write_command(fbdev, 0x55); //Backward Flicker
	write_data(fbdev, 0x67); //VCOM

	write_command(fbdev, 0x60);
	write_data(fbdev, 0x07);

	write_command(fbdev, 0x61);
	write_data(fbdev, 0x04);

	write_command(fbdev, 0x62);
	write_data(fbdev, 0x08);

	write_command(fbdev, 0x63);
	write_data(fbdev, 0x04);

	write_command(fbdev, 0xA0);  //Positive Gamma
	write_data(fbdev, 0x00);
	write_command(fbdev, 0xA1);        //
	write_data(fbdev, 0x0B);
	write_command(fbdev, 0xA2);        //
	write_data(fbdev, 0x13);
	write_command(fbdev, 0xA3);        //
	write_data(fbdev, 0x0C);
	write_command(fbdev, 0xA4);        //
	write_data(fbdev, 0x05);
	write_command(fbdev, 0xA5);        //
	write_data(fbdev, 0x0C);
	write_command(fbdev, 0xA6);        //
	write_data(fbdev, 0x08);
	write_command(fbdev, 0xA7);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0xA8);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0xA9);        //
	write_data(fbdev, 0x0A);
	write_command(fbdev, 0xAA);        //
	write_data(fbdev, 0x0F);
	write_command(fbdev, 0xAB);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0xAC);        //
	write_data(fbdev, 0x12);
	write_command(fbdev, 0xAD);        //
	write_data(fbdev, 0x18);
	write_command(fbdev, 0xAE);        //
	write_data(fbdev, 0x12);
	write_command(fbdev, 0xAF);        //
	write_data(fbdev, 0x0B);

	write_command(fbdev, 0xC0);        //Negative Gamma
	write_data(fbdev, 0x00);
	write_command(fbdev, 0xC1);        //
	write_data(fbdev, 0x0B);
	write_command(fbdev, 0xC2);        //
	write_data(fbdev, 0x13);
	write_command(fbdev, 0xC3);        //
	write_data(fbdev, 0x0C);
	write_command(fbdev, 0xC4);        //
	write_data(fbdev, 0x05);
	write_command(fbdev, 0xC5);        //
	write_data(fbdev, 0x0C);
	write_command(fbdev, 0xC6);        //
	write_data(fbdev, 0x08);
	write_command(fbdev, 0xC7);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0xC8);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0xC9);        //
	write_data(fbdev, 0x0A);
	write_command(fbdev, 0xCA);        //
	write_data(fbdev, 0x0F);
	write_command(fbdev, 0xCB);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0xCC);        //
	write_data(fbdev, 0x12);
	write_command(fbdev, 0xCD);        //
	write_data(fbdev, 0x18);
	write_command(fbdev, 0xCE);        //
	write_data(fbdev, 0x12);
	write_command(fbdev, 0xCF);        //
	write_data(fbdev, 0x0B);

	write_command(fbdev, 0xFF); // Change to Page 6 CMD for GIP timing
	write_data(fbdev, 0xFF);
	write_data(fbdev, 0x98);
	write_data(fbdev, 0x06);
	write_data(fbdev, 0x04);
	write_data(fbdev, 0x06);

	write_command(fbdev, 0x00);        //
	write_data(fbdev, 0x21);
	write_command(fbdev, 0x01);        //
	write_data(fbdev, 0x0A);
	write_command(fbdev, 0x02);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x03);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x04);        //
	write_data(fbdev, 0x32);
	write_command(fbdev, 0x05);        //
	write_data(fbdev, 0x32);
	write_command(fbdev, 0x06);        //
	write_data(fbdev, 0x98);
	write_command(fbdev, 0x07);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0x08);        //
	write_data(fbdev, 0x05);
	write_command(fbdev, 0x09);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x0A);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x0B);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x0C);        //
	write_data(fbdev, 0x32);
	write_command(fbdev, 0x0D);        //
	write_data(fbdev, 0x32);
	write_command(fbdev, 0x0E);        //
	write_data(fbdev, 0x01);
	write_command(fbdev, 0x0F);        //
	write_data(fbdev, 0x01);

	write_command(fbdev, 0x10);        //
	write_data(fbdev, 0xF0);
	write_command(fbdev, 0x11);        //
	write_data(fbdev, 0xF0);
	write_command(fbdev, 0x12);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x13);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x14);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x15);        //
	write_data(fbdev, 0x43);
	write_command(fbdev, 0x16);        //
	write_data(fbdev, 0x0B);
	write_command(fbdev, 0x17);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x18);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x19);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x1A);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x1B);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x1C);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x1D);        //
	write_data(fbdev, 0x00);

	write_command(fbdev, 0x20);        //
	write_data(fbdev, 0x01);
	write_command(fbdev, 0x21);        //
	write_data(fbdev, 0x23);
	write_command(fbdev, 0x22);        //
	write_data(fbdev, 0x45);
	write_command(fbdev, 0x23);        //
	write_data(fbdev, 0x67);
	write_command(fbdev, 0x24);        //
	write_data(fbdev, 0x01);
	write_command(fbdev, 0x25);        //
	write_data(fbdev, 0x23);
	write_command(fbdev, 0x26);        //
	write_data(fbdev, 0x45);
	write_command(fbdev, 0x27);        //
	write_data(fbdev, 0x67);

	write_command(fbdev, 0x30);        //
	write_data(fbdev, 0x01);
	write_command(fbdev, 0x31);        //
	write_data(fbdev, 0x11);
	write_command(fbdev, 0x32);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x33);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x34);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x35);        //
	write_data(fbdev, 0xcb);
	write_command(fbdev, 0x36);        //
	write_data(fbdev, 0xda);
	write_command(fbdev, 0x37);        //
	write_data(fbdev, 0xAD);
	write_command(fbdev, 0x38);        //
	write_data(fbdev, 0xbc);
	write_command(fbdev, 0x39);        //
	write_data(fbdev, 0x66);
	write_command(fbdev, 0x3A);        //
	write_data(fbdev, 0x77);
	write_command(fbdev, 0x3B);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x3C);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x3D);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x3E);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x3F);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x40);        //
	write_data(fbdev, 0x22);

	write_command(fbdev, 0x52);        //
	write_data(fbdev, 0x10);

	write_command(fbdev, 0xFF);        // Change to Page 7 CMD for GIP timing
	write_data(fbdev, 0xFF);
	write_data(fbdev, 0x98);
	write_data(fbdev, 0x06);
	write_data(fbdev, 0x04);
	write_data(fbdev, 0x07);

	write_command(fbdev, 0x18);        //
	write_data(fbdev, 0x1d);

	write_command(fbdev, 0x02);        //
	write_data(fbdev, 0x77);

	write_command(fbdev, 0xE1);        //
	write_data(fbdev, 0x79);

	write_command(fbdev, 0xFF);        // Change to Page 0 CMD for Normal command
	write_data(fbdev, 0xFF);
	write_data(fbdev, 0x98);
	write_data(fbdev, 0x06);
	write_data(fbdev, 0x04);
	write_data(fbdev, 0x00);

	write_command(fbdev, 0x36);
	write_data(fbdev, 0x01);

	write_command(fbdev, 0x3A);
	write_data(fbdev, 0x70); //16BIT

	write_command(fbdev, 0x11);
	msleep(120);
	write_command(fbdev, 0x29);
	msleep(25);
}

static int vdmafb_probe(struct platform_device *pdev)
{
	struct xilinx_vdmafb_dev *fbdev = NULL;
	struct fb_info *info = NULL;
	dma_addr_t fb_phys;
	void *fb_virt = NULL;
	size_t fb_size;
	struct xilinx_vdmafb_videomode mode = {0};
	unsigned int byte_per_pixel;
	const char *name = NULL;
	int ret = -1;

	/* fb_info alloc */
	info = framebuffer_alloc(sizeof(struct xilinx_vdmafb_dev), &pdev->dev);
	if (NULL == info) {
		dev_err(&pdev->dev, "Failed to allocate fbdev\n");
		return -ENOMEM;
	}

	fbdev = info->par;
	fbdev->info = info;
	fbdev->pdev = pdev;
	platform_set_drvdata(pdev, fbdev);

	fbdev->dma_template = devm_kzalloc(&pdev->dev,
		sizeof(struct dma_interleaved_template) +
		sizeof(struct data_chunk), GFP_KERNEL);
	if (!fbdev->dma_template)
		return -ENOMEM;
	/* get pclk */
	ret = of_property_read_string(pdev->dev.of_node, "clock-names", &name);
	if (0 > ret) {
		dev_err(&pdev->dev, "Failed to get clock name from DT\n");
		goto fb_release;
	}

	fbdev->pclk = devm_clk_get(&pdev->dev, name);
	if (IS_ERR(fbdev->pclk)) {
		dev_err(&pdev->dev, "Failed to get pixel clock\n");
		ret = PTR_ERR(fbdev->pclk);
		goto fb_release;
	}

//	clk_disable_unprepare(fbdev->pclk);

	/* Parsing the device tree node Get LCD timing parameters */
	ret = vdmafb_get_fbinfo_dt(fbdev, &mode);
	if (0 > ret)
		goto clk_put;

	/* alloc framebuffer */
	byte_per_pixel = mode.desc->bpp >> 3;
	fb_size = PAGE_ALIGN(mode.fb_vmode.xres * mode.fb_vmode.yres * 4);
	fb_virt = dma_alloc_wc(&pdev->dev, fb_size, &fb_phys, GFP_KERNEL);
	if (!fb_virt) {
		dev_err(&pdev->dev, "Failed to allocate framebuffer\n");
		ret = -ENOMEM;
		goto clk_put;
	}

	memset(fb_virt, 0, fb_size);

	/* Initialize fb_info variable and fixed parameters */
	vdmafb_init_fbinfo_var(fbdev, &mode);
	vdmafb_init_fbinfo_fix(fbdev);

	info->fix.line_length = info->var.xres * byte_per_pixel;
	info->fix.smem_start  = fb_phys;
	info->fix.smem_len    = fb_size;

	/* Enable, set the pixel clock */
	ret = clk_set_rate(fbdev->pclk, PICOS2KHZ(info->var.pixclock) * 1000U);
	if (0 > ret) {
		dev_err(&pdev->dev, "Failed to set pclk rate\n");
		goto dma_free;
	}

	ret = clk_prepare_enable(fbdev->pclk);
	if (0 > ret) {
		dev_err(&pdev->dev, "Failed to enable pclk\n");
		goto dma_free;
	}

	msleep(50);  // delay

	/* Get LCD timing controller */
	ret = vdmafb_init_vtc(fbdev);
	if (0 > ret)
		goto dma_free;

	xilinx_vtc_config_sig(fbdev->vtc, &mode.vmode);
	xilinx_vtc_enable(fbdev->vtc);

	msleep(50);

	/* vdma setup */
	ret = vdmafb_init_vdma(fbdev);
	if (0 > ret)
		goto vtc_disable;

	/* set fb_info struct */
	info->flags  = FBINFO_FLAG_DEFAULT;
	info->fbops  = &vdmafb_ops;

	ret = fb_alloc_cmap(&info->cmap, 256, 0);
	if (0 > ret) {
		dev_err(&pdev->dev, "Failed to allocate color map\n");
		goto dma_channel_release;
	}

	info->screen_base = fb_virt;
	info->screen_size = fb_size;

	info->pseudo_palette = devm_kzalloc(&pdev->dev, sizeof(u32) * 16, GFP_KERNEL);
	if (NULL == info->pseudo_palette) {
		ret = -ENOMEM;
		goto cap_dealloc;
	}

	/* reset LCD hardware */
	if (fbdev->is_hdmi)
		goto reset_done;

	fbdev->rst_gpio = of_get_named_gpio(pdev->dev.of_node, "rst-gpios", 0);
	if (!gpio_is_valid(fbdev->rst_gpio)) {
		dev_err(&pdev->dev, "Failed to get lcd reset gpio\n");
		ret = -ENODEV;
		goto cap_dealloc;
	}

	ret = devm_gpio_request_one(&pdev->dev, fbdev->rst_gpio, GPIOF_OUT_INIT_HIGH, "lcd_rst");
	if (ret < 0)
		goto cap_dealloc;

	fbdev->spi_cs = of_get_named_gpio(pdev->dev.of_node, "spi_cs", 0);
	if (!gpio_is_valid(fbdev->spi_cs)) {
		dev_err(&pdev->dev, "Failed to get spi_cs gpio\n");
		ret = -ENODEV;
		goto cap_dealloc;
	}

	ret = devm_gpio_request_one(&pdev->dev, fbdev->spi_cs, GPIOF_OUT_INIT_HIGH, "lcd_spi_cs");
	if (ret < 0)
		goto cap_dealloc;

	fbdev->spi_scl = of_get_named_gpio(pdev->dev.of_node, "spi_scl", 0);
	if (!gpio_is_valid(fbdev->spi_scl)) {
		dev_err(&pdev->dev, "Failed to get spi_scl gpio\n");
		ret = -ENODEV;
		goto cap_dealloc;
	}

	ret = devm_gpio_request_one(&pdev->dev, fbdev->spi_scl, GPIOF_OUT_INIT_HIGH, "lcd_spi_scl");
	if (ret < 0)
		goto cap_dealloc;

	fbdev->spi_sdi = of_get_named_gpio(pdev->dev.of_node, "spi_sdi", 0);
	if (!gpio_is_valid(fbdev->spi_sdi)) {
		dev_err(&pdev->dev, "Failed to get spi_sdi gpio\n");
		ret = -ENODEV;
		goto cap_dealloc;
	}

	ret = devm_gpio_request_one(&pdev->dev, fbdev->spi_sdi, GPIOF_OUT_INIT_HIGH, "lcd_spi_sdi");
	if (ret < 0)
		goto cap_dealloc;
	/*
	msleep(50);
	gpio_set_value_cansleep(rst_gpio, 0);
	msleep(20);
	gpio_set_value_cansleep(rst_gpio, 1);
	msleep(20);
	*/
	vdmafb_init_drm(fbdev);
	/* turn on the backlight */
/*
	fbdev->bl_gpio = of_get_named_gpio(pdev->dev.of_node, "bl-gpios", 0);
	if (!gpio_is_valid(fbdev->bl_gpio)) {
		dev_err(&pdev->dev, "Failed to get lcd backlight gpio\n");
		ret = -ENODEV;
		goto cap_dealloc;
	}

	ret = devm_gpio_request_one(&pdev->dev, fbdev->bl_gpio, GPIOF_OUT_INIT_LOW, "lcd_bl");
	if (ret < 0) 
		goto cap_dealloc;
*/

reset_done:
	/* register framebuffer */
	ret = register_framebuffer(info);
	if (0 > ret) {
		dev_err(&pdev->dev,"Failed to register framebuffer\n");
		goto cap_dealloc;
	}

	//if (!fbdev->is_hdmi)
	//	gpio_set_value_cansleep(fbdev->bl_gpio, 1);  // turn on the backlight

	dev_info(&pdev->dev, "Initialized successful.\n");

	return 0;

cap_dealloc:
	fb_dealloc_cmap(&info->cmap);

dma_channel_release:
	dma_release_channel(fbdev->dma);

vtc_disable:
	xilinx_vtc_disable(fbdev->vtc);

dma_free:
	dma_free_wc(&pdev->dev, fb_size, fb_virt, fb_phys);

clk_put:
	clk_disable_unprepare(fbdev->pclk);
	devm_clk_put(&pdev->dev, fbdev->pclk);

fb_release:
	framebuffer_release(info);
	return ret;
}

static int vdmafb_remove(struct platform_device *pdev)
{
	struct xilinx_vdmafb_dev *fbdev = platform_get_drvdata(pdev);
	struct fb_info *info = fbdev->info;

	fb_dealloc_cmap(&info->cmap);
	dma_release_channel(fbdev->dma);
	dma_free_wc(&pdev->dev, info->fix.smem_len, info->screen_base, info->fix.smem_start);

	xilinx_vtc_disable(fbdev->vtc);
	clk_disable_unprepare(fbdev->pclk);

	devm_clk_put(&pdev->dev, fbdev->pclk);

	unregister_framebuffer(fbdev->info);
	framebuffer_release(fbdev->info);

	//if (!fbdev->is_hdmi)
	//	gpio_set_value_cansleep(fbdev->bl_gpio, 0);  // turn off the backlight

	return 0;
}

static void vdmafb_shutdown(struct platform_device *pdev)
{
	struct xilinx_vdmafb_dev *fbdev = platform_get_drvdata(pdev);

	/* Turn off the clock, turn off the timing controller */
	xilinx_vtc_disable(fbdev->vtc);
	clk_disable_unprepare(fbdev->pclk);
}

static const struct of_device_id vdmafb_of_match_table[] = {
	{ .compatible = "xilinx,vdmafb", },
	{ /* end of table */ },
};
MODULE_DEVICE_TABLE(of, vdmafb_of_match_table);

static struct platform_driver xilinx_vdmafb_driver = {
	.probe    = vdmafb_probe,
	.remove   = vdmafb_remove,
	.shutdown = vdmafb_shutdown,
	.driver = {
		.name           = DRIVER_NAME,
		.of_match_table = vdmafb_of_match_table,
	},
};

module_platform_driver(xilinx_vdmafb_driver);

MODULE_DESCRIPTION("Framebuffer driver based on Xilinx VDMA IP Core.");
MODULE_AUTHOR("bill gui <gsj0791@126.com> GridRF.Com.");
MODULE_LICENSE("GPL v2");
