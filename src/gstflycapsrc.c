/* GStreamer Flycap Plugin
 * Copyright (C) 2015-2016 Gray Cancer Institute
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 *
 * Author: P Barber
 *
 */
/**
 * SECTION:element-gstflycap_src
 *
 * The flycapsrc element is a source for a USB 3 camera supported by the Point Grey Fly Capture SDK.
 * A live source, operating in push mode.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 flycapsrc ! videoconvert ! autovideosink
 * ]|
 * </refsect2>
 */

// Which functions of the base class to override. Create must alloc and fill the buffer. Fill just needs to fill it
//#define OVERRIDE_FILL  !!! NOT IMPLEMENTED !!!
#define OVERRIDE_CREATE

#include <unistd.h> // for usleep
#include <string.h> // for memcpy
#include <math.h>  // for pow
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>

#include "FlyCapture2_C.h"

#include "gstflycapsrc.h"

GST_DEBUG_CATEGORY_STATIC (gst_flycap_src_debug);
#define GST_CAT_DEFAULT gst_flycap_src_debug

/* prototypes */
static void gst_flycap_src_set_property (GObject * object,
		guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_flycap_src_get_property (GObject * object,
		guint property_id, GValue * value, GParamSpec * pspec);
static void gst_flycap_src_dispose (GObject * object);
static void gst_flycap_src_finalize (GObject * object);

static gboolean gst_flycap_src_start (GstBaseSrc * src);
static gboolean gst_flycap_src_stop (GstBaseSrc * src);
static GstCaps *gst_flycap_src_get_caps (GstBaseSrc * src, GstCaps * filter);
static gboolean gst_flycap_src_set_caps (GstBaseSrc * src, GstCaps * caps);

#ifdef OVERRIDE_CREATE
	static GstFlowReturn gst_flycap_src_create (GstPushSrc * src, GstBuffer ** buf);
#endif
#ifdef OVERRIDE_FILL
	static GstFlowReturn gst_flycap_src_fill (GstPushSrc * src, GstBuffer * buf);
#endif

//static GstCaps *gst_flycap_src_create_caps (GstFlycapSrc * src);
static void gst_flycap_src_reset (GstFlycapSrc * src);
enum
{
	PROP_0,
	PROP_CAMERAPRESENT,
	PROP_EXPOSURE,
	PROP_PIXELCLOCK,
	PROP_GAIN,
	PROP_BLACKLEVEL,
	PROP_RGAIN,
	PROP_GGAIN,
	PROP_BGAIN,
	PROP_BINNING,
	PROP_SHARPNESS,
	PROP_SATURATION,
	PROP_WHITEBALANCE,
	PROP_WB_ONEPUSHINPROGRESS,
	PROP_LUT,
	PROP_LUT1_OFFSET_R,
	PROP_LUT1_OFFSET_G,
	PROP_LUT1_OFFSET_B,
	PROP_LUT1_GAMMA,
	PROP_LUT1_GAIN,
	PROP_LUT2_OFFSET_R,
	PROP_LUT2_OFFSET_G,
	PROP_LUT2_OFFSET_B,
	PROP_LUT2_GAMMA,
	PROP_LUT2_GAIN,
	PROP_MAXFRAMERATE
};


#define	FLYCAP_UPDATE_LOCAL  FALSE
#define	FLYCAP_UPDATE_CAMERA TRUE

#define DEFAULT_PROP_EXPOSURE           40.0
#define DEFAULT_PROP_GAIN               1
#define DEFAULT_PROP_BLACKLEVEL         15
#define DEFAULT_PROP_RGAIN              425
#define DEFAULT_PROP_BGAIN              727
#define DEFAULT_PROP_BINNING            1
#define DEFAULT_PROP_SHARPNESS			2    // this is 'normal'
#define DEFAULT_PROP_SATURATION			25   // this is 100 on the camera scale 0-400
#define DEFAULT_PROP_HORIZ_FLIP         0
#define DEFAULT_PROP_VERT_FLIP          0
#define DEFAULT_PROP_WHITEBALANCE       GST_WB_MANUAL
#define DEFAULT_PROP_LUT		        GST_LUT_1
#define DEFAULT_PROP_LUT1_OFFSET		0    
#define DEFAULT_PROP_LUT1_GAMMA		    0.45
#define DEFAULT_PROP_LUT1_GAIN		    1.099
#define DEFAULT_PROP_LUT2_OFFSET		10    
#define DEFAULT_PROP_LUT2_GAMMA		    0.45
#define DEFAULT_PROP_LUT2_GAIN		    1.501   
#define DEFAULT_PROP_MAXFRAMERATE       25
#define DEFAULT_PROP_GAMMA			    1.5

#define DEFAULT_GST_VIDEO_FORMAT GST_VIDEO_FORMAT_RGB
#define DEFAULT_FLYCAP_VIDEO_FORMAT FC2_PIXEL_FORMAT_RGB8
// Put matching type text in the pad template below

// pad template
static GstStaticPadTemplate gst_flycap_src_template =
		GST_STATIC_PAD_TEMPLATE ("src",
				GST_PAD_SRC,
				GST_PAD_ALWAYS,
				GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE
						("{ RGB }"))
		);

// error check, use in functions where 'src' is declared and initialised
#define FLYCAPEXECANDCHECK(function) \
{\
	fc2Error Ret = function;\
	if (FC2_ERROR_OK != Ret){\
		GST_ERROR_OBJECT(src, "FlyCapture call failed: %s", fc2ErrorToDescription(Ret));\
		goto fail;\
	}\
}

#define TYPE_WHITEBALANCE (whitebalance_get_type ())
static GType
whitebalance_get_type (void)
{
  static GType whitebalance_type = 0;

  if (!whitebalance_type) {
    static GEnumValue wb_types[] = {
	  { GST_WB_MANUAL, "Auto white balance disabled.",    "disabled" },
	  { GST_WB_ONEPUSH,  "One push white balance.", "onepush"  },
	  { GST_WB_AUTO, "Auto white balance.", "auto" },
      { 0, NULL, NULL },
    };

    whitebalance_type =
	g_enum_register_static ("WhiteBalanceType", wb_types);
  }

  return whitebalance_type;
}

#define TYPE_LUT (lut_get_type ())
static GType
lut_get_type (void)
{
  static GType lut_type = 0;

  if (!lut_type) {
    static GEnumValue lut_types[] = {
    		  { GST_LUT_OFF, "Intensity look up table and gamma off.",    "off" },
    		  { GST_LUT_1, "Apply intensity look up table 1.",    "1" },
    		  { GST_LUT_2, "Apply intensity look up table 2.",    "2" },
    		  { GST_LUT_GAMMA, "Gamma intensity look up table.",    "gamma" },
    		  { 0, NULL, NULL },
    };

    lut_type =
	g_enum_register_static ("LUTType", lut_types);
  }

  return lut_type;
}

static void gst_flycap_Write_ROI_Register(GstFlycapSrc * src)
{
	unsigned int pValue, presence, on_off, base, left, top, width, height;
//	unsigned int left2, top2, width2, height2;

	if (!src->deviceContext)
		return;

	// Read main flags
	FLYCAPEXECANDCHECK(fc2ReadRegister(src->deviceContext,  0x1A70, &pValue));

	//	presence = (pValue & 0x80000000)>>31;
//	on_off   = (pValue & 0x02000000)>>25;
//	GST_DEBUG_OBJECT(src, "gst_flycap_Read_ROI_Register presence %d on_off %d", presence, on_off);

	// Make registers writable and turn on ROI
	pValue = pValue | 0x02000000;
	FLYCAPEXECANDCHECK(fc2WriteRegister(src->deviceContext, 0x1A70, pValue));

	// Read main flags to check
	FLYCAPEXECANDCHECK(fc2ReadRegister(src->deviceContext,  0x1A70, &pValue));
	presence = (pValue & 0x80000000)>>31;
	on_off   = (pValue & 0x02000000)>>25;
	GST_DEBUG_OBJECT(src, "gst_flycap_Read_ROI_Register presence %d on_off %d", presence, on_off);

	// Get offset to ROI data
	FLYCAPEXECANDCHECK(fc2ReadRegister(src->deviceContext,  0x1A74, &pValue));
	base = (pValue*4) - 0xF00000;
	GST_DEBUG_OBJECT(src, "gst_flycap_Read_ROI_Register BASE 0x%x", base);

	left = 2*src->nWidth/5;
	top  = 2*src->nHeight/5;
	width = src->nWidth/5;
	height = src->nHeight/5;

	GST_DEBUG_OBJECT(src, "gst_flycap_Set_ROI_Register left %d top %d width %d height %d", left, top, width, height);

	// Set left and top
	pValue = left*0x10000 + top;

	// check
//	left2 = (pValue & 0xFFFF0000)>>16;   // take MS 16 bits
//	top2  = (pValue & 0x0000FFFF);       // take LS 16 bits

	FLYCAPEXECANDCHECK(fc2WriteRegister(src->deviceContext, base + 0x8, pValue));

	// Set width and height
	pValue = width*0x10000 + height;

	// check
//	width2  = (pValue & 0xFFFF0000)>>16;   // take MS 16 bits
//	height2 = (pValue & 0x0000FFFF);       // take LS 16 bits

	FLYCAPEXECANDCHECK(fc2WriteRegister(src->deviceContext, base + 0xC, pValue));

//	GST_DEBUG_OBJECT(src, "gst_flycap_Set_ROI_Register left2 %d top2 %d width2 %d height2 %d", left2, top2, width2, height2);

	return;

	fail:   // required for FLYCAPEXECANDCHECK, but do no more than the debug message here.
	return;
}

static void gst_flycap_Read_ROI_Register(GstFlycapSrc * src)
{
	unsigned int pValue, presence, on_off, base, left, top, width, height, left_unit, top_unit, width_unit, height_unit;

	if (!src->deviceContext)
		return;

	// Read main flags
	FLYCAPEXECANDCHECK(fc2ReadRegister(src->deviceContext,  0x1A70, &pValue));
	presence = (pValue & 0x80000000)>>31;
	on_off   = (pValue & 0x02000000)>>25;

	GST_DEBUG_OBJECT(src, "gst_flycap_Read_ROI_Register presence %d on_off %d", presence, on_off);

	// Get offset to ROI data
	FLYCAPEXECANDCHECK(fc2ReadRegister(src->deviceContext,  0x1A74, &pValue));
	base = (pValue*4) - 0xF00000;

	// Read left and top
	FLYCAPEXECANDCHECK(fc2ReadRegister(src->deviceContext, base + 0x8, &pValue));
	left = (pValue & 0xFFFF0000)>>16;   // take MS 16 bits
	top  = (pValue & 0x0000FFFF);       // take LS 16 bits

	// Read width and height
	FLYCAPEXECANDCHECK(fc2ReadRegister(src->deviceContext, base + 0xC, &pValue));
	width  = (pValue & 0xFFFF0000)>>16;   // take MS 16 bits
	height = (pValue & 0x0000FFFF);       // take LS 16 bits

	GST_DEBUG_OBJECT(src, "gst_flycap_Read_ROI_Register left %d top %d width %d height %d", left, top, width, height);

	// Read left and top UNITS
	FLYCAPEXECANDCHECK(fc2ReadRegister(src->deviceContext, base, &pValue));
	left_unit = (pValue & 0xFFFF0000)>>16;   // take MS 16 bits
	top_unit  = (pValue & 0x0000FFFF);       // take LS 16 bits

	// Read width and height UNITS
	FLYCAPEXECANDCHECK(fc2ReadRegister(src->deviceContext, base + 0x4, &pValue));
	width_unit  = (pValue & 0xFFFF0000)>>16;   // take MS 16 bits
	height_unit = (pValue & 0x0000FFFF);       // take LS 16 bits

	GST_DEBUG_OBJECT(src, "gst_flycap_Read_ROI_Register left_unit %d top_unit %d width_unit %d height_unit %d", left_unit, top_unit, width_unit, height_unit);

	return;

	fail:   // required for FLYCAPEXECANDCHECK, but do no more than the debug message here.
	return;
}

static void gst_flycap_Read_WB_Register(GstFlycapSrc * src)
{
	unsigned int pValue, presence, one_push, on_off, auto_man, Bval, Rval;

	if (!src->deviceContext)
		return;

	// Decode the 32 bit register, remember bit 0 is the MSB
	FLYCAPEXECANDCHECK(fc2ReadRegister(src->deviceContext, 0x80C, &pValue));
	presence = (pValue & 0x80000000)>>31;
	one_push = (pValue & 0x04000000)>>26;
	on_off   = (pValue & 0x02000000)>>25;
	auto_man = (pValue & 0x01000000)>>24;
	Bval     = (pValue & 0x00FFF000)>>12;
	Rval     = (pValue & 0x00000FFF);

	GST_DEBUG_OBJECT(src, "gst_flycap_Read_WB_Register %d one_push %d on_off %d auto_man %d Bval %d Rval %d", presence, one_push, on_off, auto_man, Bval, Rval);

	return;

	fail:   // required for FLYCAPEXECANDCHECK, but do no more than the debug message here.
	return;
}


static void gst_flycap_set_property_val(GstFlycapSrc * src, fc2PropertyType type, gint val)
{
	fc2Property prop;

	if (!src->deviceContext)
		return;

	prop.type = type;
	fc2GetProperty(src->deviceContext, &prop);

	prop.onOff = TRUE;  // turn this property on
	prop.autoManualMode = FALSE; // Ensure auto-adjust mode is off, not all properties have auto mode
	prop.absControl = FALSE; // Ensure the property is not set up to use absolute value control.
	prop.valueA = (unsigned int)val; // Set the absolute value of the property
	GST_DEBUG_OBJECT(src, "gst_flycap_set_property_val %d %d", type, val);
	FLYCAPEXECANDCHECK(fc2SetProperty(src->deviceContext, &prop));
	return;

	fail:   // required for FLYCAPEXECANDCHECK, but do no more than the debug message here.
	return;
}

static void gst_flycap_get_property_val(GstFlycapSrc * src, fc2PropertyType type, gint *val)
{
	fc2Property prop;

	if (!src->deviceContext)
		return;

	prop.type = type;
	FLYCAPEXECANDCHECK(fc2GetProperty(src->deviceContext, &prop));
	*val = (gint)prop.valueA;
	return;

	fail:   // required for FLYCAPEXECANDCHECK, but do no more than the debug message here.
	return;
}

static void gst_flycap_set_property_absVal(GstFlycapSrc * src, fc2PropertyType type, float val)
{
	fc2Property prop;

	if (!src->deviceContext)
		return;

	prop.type = type;
	fc2GetProperty(src->deviceContext, &prop);

	prop.onOff = TRUE;  // turn this property on
	prop.autoManualMode = FALSE; // Ensure auto-adjust mode is off, not all properties have auto mode
	prop.absControl = TRUE; // Ensure the property is set up to use absolute value control.
	prop.absValue = val; // Set the absolute value of the property
	GST_DEBUG_OBJECT(src, "gst_flycap_set_property_absVal %d %f", type, val);
	FLYCAPEXECANDCHECK(fc2SetProperty(src->deviceContext, &prop));
	return;

	fail:   // required for FLYCAPEXECANDCHECK, but do no more than the debug message here.
	return;
}

static void gst_flycap_get_property_absVal(GstFlycapSrc * src, fc2PropertyType type, float *val)
{
	fc2Property prop;

	if (!src->deviceContext)
		return;

	prop.type = type;
	prop.absControl = TRUE;
	FLYCAPEXECANDCHECK(fc2GetProperty(src->deviceContext, &prop));
	*val = prop.absValue;
	return;

	fail:   // required for FLYCAPEXECANDCHECK, but do no more than the debug message here.
	return;
}

static void gst_flycap_set_property_auto(GstFlycapSrc * src, fc2PropertyType type)
{
	fc2Property prop;

	if (!src->deviceContext)
		return;

	prop.type = type;
	fc2GetProperty(src->deviceContext, &prop);

	prop.onOff = TRUE;  // turn this property on
	prop.autoManualMode = TRUE; // Ensure auto-adjust mode is on
	GST_DEBUG_OBJECT(src, "gst_flycap_set_property_auto %d", type);
	FLYCAPEXECANDCHECK(fc2SetProperty(src->deviceContext, &prop));
	return;

	fail:   // required for FLYCAPEXECANDCHECK, but do no more than the debug message here.
	return;
}

static void gst_flycap_set_property_off(GstFlycapSrc * src, fc2PropertyType type)
{
	fc2Property prop;

	if (!src->deviceContext)
		return;

	prop.type = type;
	fc2GetProperty(src->deviceContext, &prop);

	prop.onOff = FALSE;  // turn this property off
	prop.autoManualMode = FALSE; // Ensure auto-adjust mode is off
	GST_DEBUG_OBJECT(src, "gst_flycap_set_property_off %d", type);
	FLYCAPEXECANDCHECK(fc2SetProperty(src->deviceContext, &prop));
	return;

	fail:   // required for FLYCAPEXECANDCHECK, but do no more than the debug message here.
	return;
}

static void gst_flycap_set_property_on(GstFlycapSrc * src, fc2PropertyType type)
{
	fc2Property prop;

	if (!src->deviceContext)
		return;

	prop.type = type;
	fc2GetProperty(src->deviceContext, &prop);

	prop.onOff = TRUE;  // turn this property on
	prop.autoManualMode = FALSE; // Ensure auto-adjust mode is off
	GST_DEBUG_OBJECT(src, "gst_flycap_set_property_off %d", type);
	FLYCAPEXECANDCHECK(fc2SetProperty(src->deviceContext, &prop));
	return;

	fail:   // required for FLYCAPEXECANDCHECK, but do no more than the debug message here.
	return;
}

static void setupStrobe(GstFlycapSrc * src)
{
	fc2StrobeControl mStrobe;
	mStrobe.source = 1;     // GPIO1
	mStrobe.onOff = TRUE;   // Enable strobe output
	mStrobe.polarity = 1;   // active high (rising edge) strobe signal
	mStrobe.delay = 0.0f;   // ms
	mStrobe.duration = 0;  // 0=exposure time or else specify in ms
	GST_DEBUG_OBJECT (src, "Setting flash trigger.");
	FLYCAPEXECANDCHECK(fc2SetStrobe(src->deviceContext, &mStrobe));

	fail:
		// prints error
	return;
}

static void
gst_flycap_set_camera_exposure (GstFlycapSrc * src, gboolean send)
{  // How should the pipeline be told/respond to a change in frame rate - seems to be ok with a push source
	src->framerate = 1000.0/(src->exposure); // set a suitable frame rate for the exposure, if too fast for usb camera it will slow down.
	src->duration = 1000000000.0/src->framerate;  // frame duration in ns

	if (src->framerate <= src->maxframerate){
		if (send){
			gst_flycap_set_property_off(src, FC2_FRAME_RATE);
			GST_DEBUG_OBJECT(src, "Request duration %d us, and exposure to %.1f ms", (int)GST_TIME_AS_USECONDS(src->duration), src->exposure);
			gst_flycap_set_property_absVal(src, FC2_SHUTTER, src->exposure);
			// Get the exposure value actually set back from the camera
			gst_flycap_get_property_absVal(src, FC2_SHUTTER, &src->exposure);
			// Update the duration to the actual value
			src->duration = 1000000000.0/src->framerate;  // frame duration in ns
			GST_DEBUG_OBJECT(src, "Set duration %d us, and exposure to %.1f ms", (int)GST_TIME_AS_USECONDS(src->duration), src->exposure);
		}
	}
	else{ // limit at max framerate, and turn on camera framerate feature
		src->framerate = src->maxframerate;
		src->duration = 1000000000.0/src->framerate;  // frame duration in ns
		if (send){
			GST_DEBUG_OBJECT(src, "Request frame rate to %.1f, duration %d us, and exposure to %.1f ms", src->framerate, (int)GST_TIME_AS_USECONDS(src->duration), src->exposure);
			gst_flycap_set_property_absVal(src, FC2_FRAME_RATE, src->framerate);
			gst_flycap_set_property_absVal(src, FC2_SHUTTER, src->exposure);
			// Get the exposure value actually set back from the camera
			gst_flycap_get_property_absVal(src, FC2_FRAME_RATE, &src->framerate);
			gst_flycap_get_property_absVal(src, FC2_SHUTTER, &src->exposure);
			// Update the duration to the actual value
			src->duration = 1000000000.0/src->framerate;  // frame duration in ns
			GST_DEBUG_OBJECT(src, "Set frame rate to %.1f, duration %d us, and exposure to %.1f ms", src->framerate, (int)GST_TIME_AS_USECONDS(src->duration), src->exposure);
		}
	}

	// adjust and turn on the output strobe 'flash' sync pulse direct from the camera
//	setupStrobe(src);

	src->exposure_just_changed = TRUE;
}

static void
gst_flycap_set_camera_gain (GstFlycapSrc * src)
{   // This converts the src->gain 0-100 value to a value the camera needs
//	gst_flycap_set_property_absVal(src, FC2_GAIN, src->cam_min_gain + src->gain/100.0*(src->cam_max_gain-src->cam_min_gain));  // gain comes 0-100%
//	gst_flycap_set_property_absVal(src, FC2_GAIN, src->gain);  // gain comes in db
	// TODO Change to receive linear gain value
	float val;
	val = (float)(20.0*log10(src->gain));
	gst_flycap_set_property_absVal(src, FC2_GAIN, val);   // gain comes as linear factor
//	GST_DEBUG_OBJECT (src, "gst_flycap_set_camera_gain src->gain: %d val: %f db", src->gain, val);

	src->gain_just_changed = TRUE;
}

static void
gst_flycap_get_camera_gain (GstFlycapSrc * src)
{   // This converts the camera value to src->gain 0-100 value
	float val;
	gst_flycap_get_property_absVal(src, FC2_GAIN, &val);
	src->gain = val;  // gain is on db
//	src->gain = 100.0*(val - src->cam_min_gain)/(src->cam_max_gain-src->cam_min_gain);  // gain is 0-100%
	// TODO Change to receive linear gain value
	src->gain = (int)(pow(10.0, val/20.0)+0.5);  // gain is linear factor
//	GST_DEBUG_OBJECT (src, "gst_flycap_get_camera_gain val: %f db src->gain: %d", val, src->gain);
}

static void
get_image_size_for_camera_and_mode(GstFlycapSrc * src, fc2Mode mode, int *sensor_w, int *sensor_h, int *w, int *h)
{
	// init some value.
	*w=0;
	*h=0;
	*sensor_w=0;
	*sensor_h=0;

	if (strcmp(src->camInfo.sensorResolution, "1288x964")==0){
		*sensor_w = 1288;
		*sensor_h = 964;
		switch (mode){
		case FC2_MODE_5:   // NB These values are NOT exactly 1/4 sensor
			*w = 320;
			*h = 240;
			break;
		case FC2_MODE_1:   // NB These values are exactly 1/2 sensor
			*w = 644;
			*h = 482;
			break;
		case FC2_MODE_0:
		default:
			*w = 1288;
			*h = 964;
			break;
		}
	}
	else if (strcmp(src->camInfo.sensorResolution, "808x608")==0){
		*sensor_w = 808;
		*sensor_h = 608;
		switch (mode){
		case FC2_MODE_5:   // NB These values are NOT exactly 1/4 sensor
			*w = 200;
			*h = 122;
			break;
		case FC2_MODE_1:   // NB These values are exactly 1/2 sensor
			*w = 404;
			*h = 304;
			break;
		case FC2_MODE_0:
		default:
			*w = 808;
			*h = 608;
			break;
		}
	}
}

static int
gst_flycap_set_video_mode (GstFlycapSrc * src, fc2Mode mode)
{
    fc2Format7ImageSettings imageSettings;
	fc2Format7PacketInfo packetInfo;
	gboolean ok;
	unsigned int packetSize;
	float packetSizeAsPercentage;
	int sensor_w, sensor_h, w, h;

    // We will use camera binning mode but interpolate up to full sensor resolution so image size does not change for the rest of the pipeline.

    if(src->acq_started == TRUE)
		FLYCAPEXECANDCHECK(fc2StopCapture(src->deviceContext));

    get_image_size_for_camera_and_mode(src, mode, &sensor_w, &sensor_h, &w, &h);

    // Use the correct image size etc to set the mode of the camera
    imageSettings.mode = mode;
	imageSettings.offsetX = 0;
	imageSettings.offsetY = 0;
	imageSettings.width = w;
	imageSettings.height = h;
	imageSettings.pixelFormat = DEFAULT_FLYCAP_VIDEO_FORMAT;
	//imageSettings.reserved = ???;

	GST_DEBUG_OBJECT (src, "1 fc2GetFormat7Configuration: mode %d offset %d %d size %d %d format %x packet size %d %f",
			imageSettings.mode, imageSettings.offsetX, imageSettings.offsetY, imageSettings.width, imageSettings.height,
			imageSettings.pixelFormat, packetSize, packetSizeAsPercentage);

	fc2ValidateFormat7Settings(src->deviceContext, &imageSettings, &ok, &packetInfo);
	GST_DEBUG_OBJECT (src, "fc2ValidateFormat7Settings for mode: %d - %s", imageSettings.mode, (ok?"OK":"BAD SETTINGS!"));

	FLYCAPEXECANDCHECK(fc2SetFormat7ConfigurationPacket(src->deviceContext, &imageSettings, packetInfo.recommendedBytesPerPacket));

	fc2GetFormat7Configuration(src->deviceContext, &imageSettings, &packetSize, &packetSizeAsPercentage);
	GST_DEBUG_OBJECT (src, "2 fc2GetFormat7Configuration: mode %d offset %d %d size %d %d format %x packet size %d %f",
			imageSettings.mode, imageSettings.offsetX, imageSettings.offsetY, imageSettings.width, imageSettings.height,
			imageSettings.pixelFormat, packetSize, packetSizeAsPercentage);


	// Record width and height etc. of the full sensor, and the image we expect from the camera
	src->nWidth = sensor_w;
	src->nHeight = sensor_h;
	src->nRawWidth = imageSettings.width;
	src->nRawHeight = imageSettings.height;

	// Colour format
	// We support just colour of one type, RGB 24-bit, I am not attempting to support all camera types
	fc2DetermineBitsPerPixel(imageSettings.pixelFormat, &src->nBitsPerPixel);

	src->nBytesPerPixel = (src->nBitsPerPixel+1)/8;
	src->nImageSize = src->nWidth * src->nHeight * src->nBytesPerPixel;
	src->nPitch = src->nWidth * src->nBytesPerPixel;
	src->nRawPitch = src->nRawWidth * src->nBytesPerPixel;
	GST_DEBUG_OBJECT (src, "Image is %d x %d, pitch %d, bpp %d, Bpp %d", src->nWidth, src->nHeight, src->nPitch, src->nBitsPerPixel, src->nBytesPerPixel);

	if(src->acq_started == TRUE)
		FLYCAPEXECANDCHECK(fc2StartCapture(src->deviceContext));

	return 0;

	fail:
		return -1;
}

static void
gst_flycap_set_camera_binning (GstFlycapSrc * src)
{

	if(src->binning==2){
		// Bin 2x2
		gst_flycap_set_video_mode (src, FC2_MODE_1);
	}
	else if(src->binning==4){
		// Bin 4x4
		gst_flycap_set_video_mode (src, FC2_MODE_5);
	}
	else{
		// Bin 1x1
		gst_flycap_set_video_mode (src, FC2_MODE_0);
	}

	src->binning_just_changed = TRUE;
}

static void
gst_flycap_set_camera_saturation (GstFlycapSrc * src)
{
	// Input will be 0-100 from CSI system, convert to 0-400
	float val = src->saturation * 4.0;

	gst_flycap_set_property_absVal(src, FC2_SATURATION, val);

}

static void
gst_flycap_get_camera_saturation (GstFlycapSrc * src)
{
	// Input will be 0-400 from camera, convert to 0-100
	float val;
	gst_flycap_get_property_absVal(src, FC2_SATURATION, &val);
	src->saturation = (int)(val / 4.0);
}

static void
gst_flycap_set_camera_sharpness (GstFlycapSrc * src)
{
	// Input will be 0-10 from CSI system, convert to 0-4095
	gint val = (src->sharpness * 4095) / 10;

	gst_flycap_set_property_val(src, FC2_SHARPNESS, val);

}

static void
gst_flycap_get_camera_sharpness (GstFlycapSrc * src)
{
	// Input will be 0-4095 from camera, convert to 0-10 for CSI system
	gint val;
	gst_flycap_get_property_val(src, FC2_SHARPNESS, &val);
	src->sharpness = val*10/4095;
}

static void gst_flycap_set_property_WB_manual(GstFlycapSrc * src)
{
	fc2Property prop;
	prop.type = FC2_WHITE_BALANCE;

	FLYCAPEXECANDCHECK(fc2GetProperty(src->deviceContext, &prop));

	GST_DEBUG ("fc2GetProperty p %d op %d oo %d am %d", prop.present, prop.onePush, prop.onOff, prop.autoManualMode);

	prop.onOff = TRUE;
	prop.autoManualMode = FALSE;
	prop.onePush = FALSE;
	prop.valueA = src->rgain; // Set the white balance red channel
	prop.valueB = src->bgain; // Set the white balance blue channel
	GST_DEBUG ("Using gst_flycap_set_property_WB_manual.");
	FLYCAPEXECANDCHECK(fc2SetProperty(src->deviceContext, &prop));
	return;

	fail:
	return;
}

static void gst_flycap_set_property_WB_auto(GstFlycapSrc * src)
{
	fc2Property prop;
	prop.type = FC2_WHITE_BALANCE;

	FLYCAPEXECANDCHECK(fc2GetProperty(src->deviceContext, &prop));

	GST_DEBUG ("fc2GetProperty p %d op %d oo %d am %d", prop.present, prop.onePush, prop.onOff, prop.autoManualMode);

	prop.onOff = TRUE;
	prop.autoManualMode = TRUE;
	prop.onePush = FALSE;
	GST_DEBUG ("Using gst_flycap_set_property_WB_auto.");
	FLYCAPEXECANDCHECK(fc2SetProperty(src->deviceContext, &prop));
	return;

	fail:
	return;
}

static void gst_flycap_set_property_WB_onepush(GstFlycapSrc * src)
{   // Not sure why but this sequence of events gives a 1 shot white balance.
	fc2Property prop;

	prop.type = FC2_WHITE_BALANCE;

	GST_DEBUG ("Using gst_flycap_set_property_WB_oneshot.");

	// Read to populate prop, and check current vales
	FLYCAPEXECANDCHECK(fc2GetProperty(src->deviceContext, &prop));

	GST_DEBUG ("fc2GetProperty p %d op %d oo %d am %d", prop.present, prop.onePush, prop.onOff, prop.autoManualMode);

	// Write to make sure auto is off
	prop.type = FC2_WHITE_BALANCE;
	prop.onOff = TRUE;
	prop.autoManualMode = FALSE;
	prop.onePush = FALSE;
	FLYCAPEXECANDCHECK(fc2SetProperty(src->deviceContext, &prop));

	// write to turn on one push
	prop.type = FC2_WHITE_BALANCE;
	prop.onOff = TRUE;
	prop.autoManualMode = FALSE;
	prop.onePush = TRUE;
	FLYCAPEXECANDCHECK(fc2SetProperty(src->deviceContext, &prop));

	// Set globals to say WB is in progress and allow a number of frames before aborting it
	src->WB_in_progress = TRUE;
	src->WB_progress = 100;

	return;

	fail:
	return;
}

static gboolean gst_flycap_check_WB_onepush(GstFlycapSrc * src, unsigned int *rgain, unsigned int *bgain)
{   // This is called when we think one push is operating to look for its end.
	fc2Property prop;

	prop.type = FC2_WHITE_BALANCE;

	FLYCAPEXECANDCHECK(fc2GetProperty(src->deviceContext, &prop));

	if (prop.onePush==FALSE)  // onepush has ended
		GST_DEBUG ("gst_flycap_check_WB_oneshot ended %d %d %d", prop.onePush, prop.valueA, prop.valueB);

	// If required return the final r and b gain values
	if (rgain)
		*rgain = prop.valueA;
	if (bgain)
		*bgain = prop.valueB;

	return (prop.onePush);

	fail:
	return FALSE;
}

static void
gst_flycap_set_camera_whitebalance (GstFlycapSrc * src)
{
	if (!src->deviceContext)
		return;

	switch (src->whitebalance){
	case GST_WB_AUTO:
		gst_flycap_set_property_WB_auto(src);
		break;
	case GST_WB_ONEPUSH:
		gst_flycap_set_property_WB_onepush(src);
		break;
	case GST_WB_MANUAL:
		gst_flycap_set_property_WB_manual(src);
		break;
	default:
		gst_flycap_set_property_WB_manual(src);
		break;
	}
}

static void
gst_flycap_calculate_luts (GstFlycapSrc * src, gint lut_bank, gint channel)
{
	// Setup an the luts, should be 9-bit input and output
	// channel 0=red, 1=green, 2=blue

	unsigned int i, lut[512];
	int    a, e;
	double b, c, d, f;

	// Just make sure lut_bank is in limits
	if (lut_bank<0) lut_bank=0;
	else if (lut_bank>1) lut_bank=1;

	// basic gamma curve y=c.(x-a)^b with a linear portion
	a = src->lut_offset[lut_bank][channel];  // NB a is 0-511
	b = src->lut_gamma[lut_bank];
	c = src->lut_gain[lut_bank];
	d = src->lut_slope[lut_bank];
	e = src->lut_linearcutoff[lut_bank];
	f = src->lut_outputoffset[lut_bank];

	GST_DEBUG_OBJECT (src, "LUT bank %d gamma a=%d b=%f c=%f d=%f e=%d", lut_bank, a, b, c, d, e);

	for (i=0;i<512;i++){

		double x = (double)(i-a) / 511.0      ;  // value along 0-1 input axis

		if (i<a)
			lut[i]=0;
		else if ((i-a) <= e)   // e linear section according to Rec. 709 standard
			lut[i] = (unsigned int)MIN(d*(i-a), 511);
		else
			lut[i] = (unsigned int)MIN((c*(pow(x, b))-f)*511, 511);
//		GST_DEBUG_OBJECT (src, "bank %d %d %d", lut_bank, i, lut[i]);
	}

	// Set bank's RGB channels
	FLYCAPEXECANDCHECK(fc2SetLUTChannel(src->deviceContext, lut_bank, channel, 512, lut));

	fail:
	return;

}

static void
gst_flycap_set_camera_lut (GstFlycapSrc * src)
{
	if (!src->deviceContext)
		return;

	switch (src->lut){
	case GST_LUT_OFF:
		GST_DEBUG_OBJECT (src, "GST_LUT_OFF");
		FLYCAPEXECANDCHECK(fc2EnableLUT(src->deviceContext, FALSE));
		gst_flycap_set_property_off(src->deviceContext, FC2_GAMMA);
		break;
	case GST_LUT_1:
		GST_DEBUG_OBJECT (src, "GST_LUT_1");
		FLYCAPEXECANDCHECK(fc2EnableLUT(src->deviceContext, TRUE));
		FLYCAPEXECANDCHECK(fc2SetActiveLUTBank(src->deviceContext, 0));  // use lut bank 0
	break;
	case GST_LUT_2:
		GST_DEBUG_OBJECT (src, "GST_LUT_2");
		FLYCAPEXECANDCHECK(fc2EnableLUT(src->deviceContext, TRUE));
		FLYCAPEXECANDCHECK(fc2SetActiveLUTBank(src->deviceContext, 1));  // use lut bank 1
		break;
	case GST_LUT_GAMMA:
		GST_DEBUG_OBJECT (src, "GST_LUT_GAMMA %f", src->gamma);
//		FLYCAPEXECANDCHECK(fc2EnableLUT(src->deviceContext, FALSE));
		gst_flycap_set_property_absVal(src->deviceContext, FC2_GAMMA, src->gamma);
		GST_DEBUG_OBJECT (src, "GST_LUT_GAMMA ok");
		break;
	default:
		GST_DEBUG_OBJECT (src, "GST_LUT_???");
		FLYCAPEXECANDCHECK(fc2EnableLUT(src->deviceContext, FALSE));
		gst_flycap_set_property_off(src->deviceContext, FC2_GAMMA);
		break;
	}

	fail:
	return;
}

static void
gst_flycap_get_camera_lut (GstFlycapSrc * src)
{
	unsigned int val;
	fc2GetActiveLUTBank(src->deviceContext, &val);

	// Decode the value, really not sure how we can determine if LUT is off or in gamma mode.
	if (val==0)
		src->lut = GST_LUT_1;
	else if (val==1)
		src->lut = GST_LUT_2;
	else
		src->lut = GST_LUT_OFF;
}

/* class initialisation */

G_DEFINE_TYPE (GstFlycapSrc, gst_flycap_src, GST_TYPE_PUSH_SRC);

static void
gst_flycap_src_class_init (GstFlycapSrcClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
	GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
	GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);

	GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "flycapsrc", 0,
			"FlyCapture Camera source");

	gobject_class->set_property = gst_flycap_src_set_property;
	gobject_class->get_property = gst_flycap_src_get_property;
	gobject_class->dispose = gst_flycap_src_dispose;
	gobject_class->finalize = gst_flycap_src_finalize;

	gst_element_class_add_pad_template (gstelement_class,
			gst_static_pad_template_get (&gst_flycap_src_template));

	gst_element_class_set_static_metadata (gstelement_class,
			"FlyCapture Video Source", "Source/Video",
			"FlyCapture Camera video source", "Paul R. Barber <paul.barber@oncology.ox.ac.uk>");

	gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_flycap_src_start);
	gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_flycap_src_stop);
	gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_flycap_src_get_caps);
	gstbasesrc_class->set_caps = GST_DEBUG_FUNCPTR (gst_flycap_src_set_caps);

#ifdef OVERRIDE_CREATE
	gstpushsrc_class->create = GST_DEBUG_FUNCPTR (gst_flycap_src_create);
	GST_DEBUG ("Using gst_flycap_src_create.");
#endif
#ifdef OVERRIDE_FILL
	gstpushsrc_class->fill   = GST_DEBUG_FUNCPTR (gst_flycap_src_fill);
	GST_DEBUG ("Using gst_flycap_src_fill.");
#endif

	// Install GObject properties
	// Camera Present property
	g_object_class_install_property (gobject_class, PROP_CAMERAPRESENT,
			g_param_spec_boolean ("devicepresent", "Camera Device Present", "Is the camera present and connected OK?",
					FALSE, G_PARAM_READABLE));
	// Exposure property
	g_object_class_install_property (gobject_class, PROP_EXPOSURE,
	  g_param_spec_float("exposure", "Exposure", "Camera sensor exposure time (ms).", 0.01, 31900, DEFAULT_PROP_EXPOSURE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// Gain property
	g_object_class_install_property (gobject_class, PROP_GAIN,
			  g_param_spec_int("gain", "Gain", "Camera sensor master gain (linear factor).", 1, 16, DEFAULT_PROP_GAIN,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// Black Level property
	g_object_class_install_property (gobject_class, PROP_BLACKLEVEL,
		g_param_spec_int("blacklevel", "Black Level", "Camera sensor black level offset.", 0, 31, DEFAULT_PROP_BLACKLEVEL,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// R gain property
	g_object_class_install_property (gobject_class, PROP_RGAIN,
		g_param_spec_int("rgain", "Red Gain", "Camera sensor red channel gain.", 0, 1023, DEFAULT_PROP_RGAIN,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

	// B gain property
	g_object_class_install_property (gobject_class, PROP_BGAIN,
		g_param_spec_int("bgain", "Blue Gain", "Camera sensor blue channel gain.", 0, 1023, DEFAULT_PROP_BGAIN,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// binning property
	g_object_class_install_property (gobject_class, PROP_BINNING,
	  g_param_spec_int("binning", "Binning", "Camera sensor binning.", 1, 4, DEFAULT_PROP_BINNING,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));
	// White balance property
	g_object_class_install_property (gobject_class, PROP_WHITEBALANCE,
	  g_param_spec_enum("whitebalance", "White Balance", "White Balance mode. Disabled, One Shot or Auto.", TYPE_WHITEBALANCE, DEFAULT_PROP_WHITEBALANCE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	g_object_class_install_property (gobject_class, PROP_WB_ONEPUSHINPROGRESS,
			g_param_spec_boolean ("onepushwbinprogress", "One-Push White Balance in Progress", "Is the camera performing one-push white balance now?",
					FALSE, G_PARAM_READABLE));
	// Max Frame Rate property
	g_object_class_install_property (gobject_class, PROP_MAXFRAMERATE,
	  g_param_spec_float("maxframerate", "Maximum Frame Rate", "Camera sensor maximum allowed frame rate (fps)."
			  "The frame rate will be determined from the exposure time, up to this maximum value when short exposures are used", 10, 200, DEFAULT_PROP_MAXFRAMERATE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	// LUT property
	g_object_class_install_property (gobject_class, PROP_LUT,
	  g_param_spec_enum("lut", "Intensity look up table.", "Look up table, off, lut1, lut2 or gamma.", TYPE_LUT, DEFAULT_PROP_LUT,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	g_object_class_install_property (gobject_class, PROP_LUT1_OFFSET_R,
	  g_param_spec_int("lut1offsetred", "LUT1 Red Offset", "Intensity look up table 1 offset value for the Red channel.", 0, 511, DEFAULT_PROP_LUT1_OFFSET,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	g_object_class_install_property (gobject_class, PROP_LUT1_OFFSET_G,
	  g_param_spec_int("lut1offsetgreen", "LUT1 Green Offset", "Intensity look up table 1 offset value for the Green channel.", 0, 511, DEFAULT_PROP_LUT1_OFFSET,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	g_object_class_install_property (gobject_class, PROP_LUT1_OFFSET_B,
	  g_param_spec_int("lut1offsetblue", "LUT1 Blue Offset", "Intensity look up table 1 offset value for the Blue channel.", 0, 511, DEFAULT_PROP_LUT1_OFFSET,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	g_object_class_install_property (gobject_class, PROP_LUT1_GAMMA,
	  g_param_spec_double("lut1gamma", "LUT1 Gamma", "Intensity look up table 1 gamma value.", 0.0, 4.0, DEFAULT_PROP_LUT1_GAMMA,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	g_object_class_install_property (gobject_class, PROP_LUT1_GAIN,
	  g_param_spec_double("lut1gain", "LUT1 Gain", "Intensity look up table 1 gain value.", 0.0, 1000.0, DEFAULT_PROP_LUT1_GAIN,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	g_object_class_install_property (gobject_class, PROP_LUT2_OFFSET_R,
	  g_param_spec_int("lut2offsetred", "LUT2 Red Offset", "Intensity look up table 2 offset value for the Red channel.", 0, 511, DEFAULT_PROP_LUT2_OFFSET,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	g_object_class_install_property (gobject_class, PROP_LUT2_OFFSET_G,
	  g_param_spec_int("lut2offsetgreen", "LUT2 Green Offset", "Intensity look up table 2 offset value for the Green channel.", 0, 511, DEFAULT_PROP_LUT2_OFFSET,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	g_object_class_install_property (gobject_class, PROP_LUT2_OFFSET_B,
	  g_param_spec_int("lut2offsetblue", "LUT2 Blue Offset", "Intensity look up table 2 offset value for the Blue channel.", 0, 511, DEFAULT_PROP_LUT2_OFFSET,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	g_object_class_install_property (gobject_class, PROP_LUT2_GAMMA,
	  g_param_spec_double("lut2gamma", "LUT2 Gamma", "Intensity look up table 2 gamma value.", 0.0, 4.0, DEFAULT_PROP_LUT2_GAMMA,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	g_object_class_install_property (gobject_class, PROP_LUT2_GAIN,
	  g_param_spec_double("lut2gain", "LUT2 Gain", "Intensity look up table 2 gain value.", 0.0, 1000.0, DEFAULT_PROP_LUT2_GAIN,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	g_object_class_install_property (gobject_class, PROP_SATURATION,
	  g_param_spec_int("saturation", "Saturation", "Camera colour saturation.", 0, 100, DEFAULT_PROP_SATURATION,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
	g_object_class_install_property (gobject_class, PROP_SHARPNESS,
	  g_param_spec_int("sharpness", "Sharpness/Detail", "Camera sharpness/detail setting. Value <2 will blur.", 0, 10, DEFAULT_PROP_SHARPNESS,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
}

static void
init_properties(GstFlycapSrc * src)
{
	src->exposure = DEFAULT_PROP_EXPOSURE;
	gst_flycap_set_camera_exposure(src, FLYCAP_UPDATE_LOCAL);
	src->gain = DEFAULT_PROP_GAIN;
	src->blacklevel = DEFAULT_PROP_BLACKLEVEL;
	src->rgain = DEFAULT_PROP_RGAIN;
	src->bgain = DEFAULT_PROP_BGAIN;
	src->binning = DEFAULT_PROP_BINNING;
	src->saturation = DEFAULT_PROP_SATURATION;
	src->sharpness = DEFAULT_PROP_SHARPNESS;
	src->vflip = DEFAULT_PROP_VERT_FLIP;
	src->hflip = DEFAULT_PROP_HORIZ_FLIP;
	src->whitebalance = DEFAULT_PROP_WHITEBALANCE;
	src->maxframerate = DEFAULT_PROP_MAXFRAMERATE;
	src->lut = DEFAULT_PROP_LUT;
	src->gamma = DEFAULT_PROP_GAMMA;

	// Settings for lut bank 0 from lut1 settings
	src->lut_offset[0][0] = DEFAULT_PROP_LUT1_OFFSET;
	src->lut_offset[0][1] = DEFAULT_PROP_LUT1_OFFSET;
	src->lut_offset[0][2] = DEFAULT_PROP_LUT1_OFFSET;
	src->lut_gamma[0] = DEFAULT_PROP_LUT1_GAMMA;
	src->lut_gain[0] = DEFAULT_PROP_LUT1_GAIN;
	// The following do not have settable properties. Is this needed?
	src->lut_slope[0] = 4.5; // slope of linear portion should be 4.5
	src->lut_linearcutoff[0] = 9 ; // 0.018*511  for i-a<=e curve is linear with slope d
	src->lut_outputoffset[0] = 0.099;  // output offset according to Rec. 709 standard, for gamma portion

	// Settings for lut bank 1 from lut2 settings
	src->lut_offset[1][0] = DEFAULT_PROP_LUT2_OFFSET;
	src->lut_offset[1][1] = DEFAULT_PROP_LUT2_OFFSET;
	src->lut_offset[1][2] = DEFAULT_PROP_LUT2_OFFSET;
	src->lut_gamma[1] = DEFAULT_PROP_LUT2_GAMMA;
	src->lut_gain[1] = DEFAULT_PROP_LUT2_GAIN;
	// The following do not have settable properties. Is this needed?
	src->lut_slope[1] = 9.0; // slope of linear portion should be 2x4.5 (was 8.0 from Davide)
	src->lut_linearcutoff[1] = 5 ; // 0.009*511  for i-a<=e curve is linear with slope d (was 9 from Davide)
	src->lut_outputoffset[1] = 0.099;  // output offset according to Rec. 709 standard, for gamma portion (was 0.1 from Davide)

//	src->cam_min_gain = 0.0;
//	src->cam_max_gain = 24.0;
	src->WB_in_progress = 0;
}

static void
gst_flycap_src_init (GstFlycapSrc * src)
{
	/* set source as live (no preroll) */
	gst_base_src_set_live (GST_BASE_SRC (src), TRUE);

	/* override default of BYTES to operate in time mode */
	gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);

	init_properties(src);

	gst_flycap_src_reset (src);
}

static void
gst_flycap_src_reset (GstFlycapSrc * src)
{
	src->deviceContext = NULL;
	src->cameraPresent = FALSE;
	src->n_frames = 0;
	src->total_timeouts = 0;
	src->last_frame_time = 0;
}

void
gst_flycap_src_set_property (GObject * object, guint property_id,
		const GValue * value, GParamSpec * pspec)
{
	GstFlycapSrc *src;

	src = GST_FLYCAP_SRC (object);

	switch (property_id) {
	case PROP_EXPOSURE:
		src->exposure = g_value_get_float(value);
		GST_DEBUG_OBJECT (src, "set exposure %f", src->exposure);
		gst_flycap_set_camera_exposure(src, FLYCAP_UPDATE_CAMERA);
		break;
	case PROP_GAIN:
		src->gain = g_value_get_int (value);
		GST_DEBUG_OBJECT (src, "set gain %d", src->gain);
		gst_flycap_set_camera_gain(src);
		break;
	case PROP_BLACKLEVEL:
		src->blacklevel = g_value_get_int (value);
		GST_DEBUG_OBJECT (src, "set blacklevel %d", src->blacklevel);
		gst_flycap_set_property_val(src, FC2_BRIGHTNESS, src->blacklevel);
		break;
	case PROP_RGAIN:
		src->rgain = g_value_get_int (value);
		src->whitebalance = GST_WB_MANUAL;
		gst_flycap_set_camera_whitebalance(src);
		break;
	case PROP_BGAIN:
		src->bgain = g_value_get_int (value);
		src->whitebalance = GST_WB_MANUAL;
		gst_flycap_set_camera_whitebalance(src);
		break;
	case PROP_BINNING:
		src->binning = g_value_get_int (value);
		gst_flycap_set_camera_binning(src);
		break;
	case PROP_SATURATION:
		src->saturation = g_value_get_int (value);
		gst_flycap_set_camera_saturation(src);
		break;
	case PROP_SHARPNESS:
		src->sharpness = g_value_get_int (value);
		gst_flycap_set_camera_sharpness(src);
		break;
	case PROP_WHITEBALANCE:
		src->whitebalance = g_value_get_enum (value);
		gst_flycap_set_camera_whitebalance(src);
		break;
	case PROP_LUT:
		src->lut = g_value_get_enum (value);
		gst_flycap_set_camera_lut(src);
		break;
	case PROP_LUT1_OFFSET_R:
		src->lut_offset[0][0] = g_value_get_int (value);
		gst_flycap_calculate_luts(src, 0, 0);
		break;
	case PROP_LUT1_OFFSET_G:
		src->lut_offset[0][1] = g_value_get_int (value);
		gst_flycap_calculate_luts(src, 0, 1);
		break;
	case PROP_LUT1_OFFSET_B:
		src->lut_offset[0][2] = g_value_get_int (value);
		gst_flycap_calculate_luts(src, 0, 2);
		break;
	case PROP_LUT1_GAMMA:
		src->lut_gamma[0] = g_value_get_double (value);
		gst_flycap_calculate_luts(src, 0, 0);
		gst_flycap_calculate_luts(src, 0, 1);
		gst_flycap_calculate_luts(src, 0, 2);
		break;
	case PROP_LUT1_GAIN:
		src->lut_gain[0] = g_value_get_double (value);
		gst_flycap_calculate_luts(src, 0, 0);
		gst_flycap_calculate_luts(src, 0, 1);
		gst_flycap_calculate_luts(src, 0, 2);
		break;
	case PROP_LUT2_OFFSET_R:
		src->lut_offset[1][0] = g_value_get_int (value);
		gst_flycap_calculate_luts(src, 1, 0);
		break;
	case PROP_LUT2_OFFSET_G:
		src->lut_offset[1][1] = g_value_get_int (value);
		gst_flycap_calculate_luts(src, 1, 1);
		break;
	case PROP_LUT2_OFFSET_B:
		src->lut_offset[1][2] = g_value_get_int (value);
		gst_flycap_calculate_luts(src, 1, 2);
		break;
	case PROP_LUT2_GAMMA:
		src->lut_gamma[1] = g_value_get_double (value);
		gst_flycap_calculate_luts(src, 1, 0);
		gst_flycap_calculate_luts(src, 1, 1);
		gst_flycap_calculate_luts(src, 1, 2);
		break;
	case PROP_LUT2_GAIN:
		src->lut_gain[1] = g_value_get_double (value);
		gst_flycap_calculate_luts(src, 1, 0);
		gst_flycap_calculate_luts(src, 1, 1);
		gst_flycap_calculate_luts(src, 1, 2);
		break;
	case PROP_MAXFRAMERATE:
		src->maxframerate = g_value_get_float(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

void
gst_flycap_src_get_property (GObject * object, guint property_id,
		GValue * value, GParamSpec * pspec)
{
	GstFlycapSrc *src;

	g_return_if_fail (GST_IS_FLYCAP_SRC (object));
	src = GST_FLYCAP_SRC (object);

	switch (property_id) {
	case PROP_CAMERAPRESENT:
		g_value_set_boolean (value, src->cameraPresent);
		break;
	case PROP_EXPOSURE:
		gst_flycap_get_property_absVal(src, FC2_SHUTTER, &src->exposure);
		GST_DEBUG_OBJECT (src, "get exposure %f", src->exposure);
		g_value_set_float (value, src->exposure);
		break;
	case PROP_GAIN:
		gst_flycap_get_camera_gain(src);
		GST_DEBUG_OBJECT (src, "get gain %d", src->gain);
		g_value_set_int (value, (int)src->gain);
		break;
	case PROP_BLACKLEVEL:
		gst_flycap_get_property_val(src, FC2_BRIGHTNESS, &src->blacklevel);
		g_value_set_int (value, src->blacklevel);
		break;
	case PROP_RGAIN:
		gst_flycap_check_WB_onepush(src, &src->rgain, NULL);
		g_value_set_int (value, src->rgain);
		break;
	case PROP_BGAIN:
		gst_flycap_check_WB_onepush(src, NULL, &src->bgain);
		g_value_set_int (value, src->bgain);
		break;
	case PROP_BINNING:
		// Binning check is elaborate process, if binning fails errors will be produced and will be visually wrong
		// so, just report cached value rather than querying camera
		g_value_set_int (value, src->binning);
		break;
	case PROP_SATURATION:
		gst_flycap_get_camera_saturation(src);
		g_value_set_int (value, src->saturation);
		break;
	case PROP_SHARPNESS:
		gst_flycap_get_camera_sharpness(src);
		g_value_set_int (value, src->sharpness);
		break;
	case PROP_WHITEBALANCE:
		// WB is monitored, do not get from camera
		g_value_set_enum (value, src->whitebalance);
		break;
	case PROP_WB_ONEPUSHINPROGRESS:
		// This is used to check the progress of wb
		g_value_set_boolean (value, src->WB_in_progress);
		break;
	case PROP_LUT:
		gst_flycap_get_camera_lut(src);
		g_value_set_enum (value, src->lut);
		break;
	case PROP_LUT1_OFFSET_R:
		g_value_set_int (value, src->lut_offset[0][0]);
		break;
	case PROP_LUT1_OFFSET_G:
		g_value_set_int (value, src->lut_offset[0][1]);
		break;
	case PROP_LUT1_OFFSET_B:
		g_value_set_int (value, src->lut_offset[0][2]);
		break;
	case PROP_LUT1_GAMMA:
		g_value_set_double (value, src->lut_gamma[0]);
		break;
	case PROP_LUT1_GAIN:
		g_value_set_double (value, src->lut_gain[0]);
		break;
	case PROP_LUT2_OFFSET_R:
		g_value_set_int (value, src->lut_offset[1][0]);
		break;
	case PROP_LUT2_OFFSET_G:
		g_value_set_int (value, src->lut_offset[1][1]);
		break;
	case PROP_LUT2_OFFSET_B:
		g_value_set_int (value, src->lut_offset[1][2]);
		break;
	case PROP_LUT2_GAMMA:
		g_value_set_double (value, src->lut_gamma[1]);
		break;
	case PROP_LUT2_GAIN:
		g_value_set_double (value, src->lut_gain[1]);
		break;
	case PROP_MAXFRAMERATE:
		g_value_set_float (value, src->maxframerate);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

void
gst_flycap_src_dispose (GObject * object)
{
	GstFlycapSrc *src;

	g_return_if_fail (GST_IS_FLYCAP_SRC (object));
	src = GST_FLYCAP_SRC (object);

	GST_DEBUG_OBJECT (src, "dispose");

	// clean up as possible.  may be called multiple times

	G_OBJECT_CLASS (gst_flycap_src_parent_class)->dispose (object);
}

void
gst_flycap_src_finalize (GObject * object)
{
	GstFlycapSrc *src;

	g_return_if_fail (GST_IS_FLYCAP_SRC (object));
	src = GST_FLYCAP_SRC (object);

	GST_DEBUG_OBJECT (src, "finalize");

	/* clean up object here */
	G_OBJECT_CLASS (gst_flycap_src_parent_class)->finalize (object);
}

static gboolean
gst_flycap_src_start (GstBaseSrc * bsrc)
{
	// Start will open the device but not start it, set_caps starts it, stop should stop and close it (as v4l2src)

	GstFlycapSrc *src = GST_FLYCAP_SRC (bsrc);
    fc2PGRGuid guid;
    unsigned int numCameras = 0;

	GST_DEBUG_OBJECT (src, "start");

	// Turn on automatic timestamping, if so we do not need to do it manually, BUT there is some evidence that automatic timestamping is laggy
//	gst_base_src_set_do_timestamp(bsrc, TRUE);

	GST_DEBUG_OBJECT (src, "fc2CreateContext");
	src->deviceContext = NULL;
	FLYCAPEXECANDCHECK(fc2CreateContext(&src->deviceContext));
	FLYCAPEXECANDCHECK(fc2GetNumOfCameras(src->deviceContext, &numCameras));
	// display error when no camera has been found
	if (numCameras<1){
		GST_ERROR_OBJECT(src, "No Flycapture device found.");
		goto fail;
	}

	GST_INFO_OBJECT (src, "FlyCapture Library: Context created and camera(s) found.");

	// open first usable device
	GST_DEBUG_OBJECT (src, "fc2GetCameraFromIndex");
	FLYCAPEXECANDCHECK(fc2GetCameraFromIndex(src->deviceContext, 0, &guid));
	GST_DEBUG_OBJECT (src, "fc2Connect");
	FLYCAPEXECANDCHECK(fc2Connect(src->deviceContext, &guid));

	// NOTE:
	// from now on, the "deviceContext" handle can be used to access the camera board.
	// use fc2DestroyContext to end the usage
	src->cameraPresent = TRUE;

	// Get information about the camera sensor
	FLYCAPEXECANDCHECK(fc2GetCameraInfo(src->deviceContext, &src->camInfo));
	GST_DEBUG_OBJECT (src, "fc2GetCameraInfo: %s, %s", src->camInfo.sensorInfo, src->camInfo.sensorResolution);

	// Set binning first which determines the video mode and image size etc.
	gst_flycap_set_camera_binning(src);

	// Alloc a buffer for the converted image, for use later
	GST_DEBUG_OBJECT (src, "fc2CreateImage");
	FLYCAPEXECANDCHECK(fc2CreateImage(&src->rawImage));
	FLYCAPEXECANDCHECK(fc2CreateImage(&src->convertedImage));
//	FLYCAPEXECANDCHECK(fc2CreateImage(&src->tempImage));

//	GST_DEBUG_OBJECT (src, "gain %d, min %f, max %f", src->gain, src->cam_min_gain, src->cam_max_gain);
	GST_DEBUG_OBJECT (src, "gain %d", src->gain);
	GST_DEBUG_OBJECT (src, "exposure %f", src->exposure);
	gst_flycap_set_camera_exposure(src, FLYCAP_UPDATE_CAMERA);
	gst_flycap_set_camera_gain(src);
	GST_DEBUG_OBJECT (src, "set blacklevel %d", src->blacklevel);
	gst_flycap_set_property_val(src, FC2_BRIGHTNESS, src->blacklevel);
	//gst_flycap_set_camera_whitebalance(src);  // moved to later as got property not present error here

	gst_flycap_set_camera_saturation(src);

	//is_SetRopEffect(src->hCam, IS_SET_ROP_MIRROR_LEFTRIGHT, src->hflip, 0);
	//is_SetRopEffect(src->hCam, IS_SET_ROP_MIRROR_UPDOWN, src->vflip, 0);

	setupStrobe(src);

	// Calculate and preset both luts, and all channels on the camera
	gst_flycap_calculate_luts(src, 0, 0);
	gst_flycap_calculate_luts(src, 0, 1);
	gst_flycap_calculate_luts(src, 0, 2);
	gst_flycap_calculate_luts(src, 1, 0);
	gst_flycap_calculate_luts(src, 1, 1);
	gst_flycap_calculate_luts(src, 1, 2);
	// Set default lut or gamma
    gst_flycap_set_camera_lut(src);

	// Set sharpness (think this has to come after setting LUT)
	gst_flycap_set_property_on(src, FC2_SHARPNESS);
	gst_flycap_set_camera_sharpness(src);

	// Set default white balance
	gst_flycap_set_camera_whitebalance(src);

	// For debugging read these registers
//	gst_flycap_Read_WB_Register(src);
//	gst_flycap_Read_ROI_Register(src);

	// Change the AE and WB ROI
	gst_flycap_Write_ROI_Register(src);
//	gst_flycap_Read_ROI_Register(src); // for debug only

	return TRUE;

	fail:

	if (src->deviceContext) {
		fc2DestroyContext(src->deviceContext);
		src->deviceContext = NULL;
	}

	fc2DestroyImage(&src->rawImage);
	fc2DestroyImage(&src->convertedImage);

	return FALSE;
}

static gboolean
gst_flycap_src_stop (GstBaseSrc * bsrc)
{
	// Start will open the device but not start it, set_caps starts it, stop should stop and close it (as v4l2src)

	GstFlycapSrc *src = GST_FLYCAP_SRC (bsrc);

	GST_DEBUG_OBJECT (src, "stop");
	GST_DEBUG_OBJECT (src, "fc2StopCapture");
	FLYCAPEXECANDCHECK(fc2StopCapture(src->deviceContext));
	GST_DEBUG_OBJECT (src, "fc2Disconnect");
	FLYCAPEXECANDCHECK(fc2Disconnect(src->deviceContext));
	FLYCAPEXECANDCHECK(fc2DestroyContext(src->deviceContext));

	fc2DestroyImage(&src->rawImage);
	fc2DestroyImage(&src->convertedImage);

	gst_flycap_src_reset (src);

	fail:   // Needed for FLYCAPEXECANDCHECK, does nothing in this case
	return TRUE;
}

static GstCaps *
gst_flycap_src_get_caps (GstBaseSrc * bsrc, GstCaps * filter)
{
	GstFlycapSrc *src = GST_FLYCAP_SRC (bsrc);
	GstCaps *caps;

  if (!src->deviceContext) {
    caps = gst_pad_get_pad_template_caps (GST_BASE_SRC_PAD (src));
  } else {
    GstVideoInfo vinfo;

    // Create video info 
    gst_video_info_init (&vinfo);

    vinfo.width = src->nWidth;
    vinfo.height = src->nHeight;

   	vinfo.fps_n = 0;  vinfo.fps_d = 1;  // Frames per second fraction n/d, 0/1 indicates a frame rate may vary
    vinfo.interlace_mode = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;

    vinfo.finfo = gst_video_format_get_info (DEFAULT_GST_VIDEO_FORMAT);

    // cannot do this for variable frame rate
    //src->duration = gst_util_uint64_scale_int (GST_SECOND, vinfo.fps_d, vinfo.fps_n); // NB n and d are wrong way round to invert the fps into a duration.

    caps = gst_video_info_to_caps (&vinfo);

    // We can supply our max frame rate, but not sure how to do it or what effect it will have
    // 1st attempt to set max-framerate in the caps
//    GstStructure *structure = gst_caps_get_structure (caps, 0);
//    GValue *val=NULL;
//    g_value_set_double(val, src->maxframerate);
//    gst_structure_set_value (structure, "max-framerate", val);
  }

	GST_DEBUG_OBJECT (src, "The caps are %" GST_PTR_FORMAT, caps);

	if (filter) {
		GstCaps *tmp = gst_caps_intersect (caps, filter);
		gst_caps_unref (caps);
		caps = tmp;

		GST_DEBUG_OBJECT (src, "The caps after filtering are %" GST_PTR_FORMAT, caps);
	}

	return caps;
}

static gboolean
gst_flycap_src_set_caps (GstBaseSrc * bsrc, GstCaps * caps)
{
	// Start will open the device but not start it, set_caps starts it, stop should stop and close it (as v4l2src)

	GstFlycapSrc *src = GST_FLYCAP_SRC (bsrc);
	GstVideoInfo vinfo;
	//GstStructure *s = gst_caps_get_structure (caps, 0);

    if(src->acq_started == TRUE){
		FLYCAPEXECANDCHECK(fc2StopCapture(src->deviceContext));
		src->acq_started = FALSE;
    }

	GST_DEBUG_OBJECT (src, "The caps being set are %" GST_PTR_FORMAT, caps);

	gst_video_info_from_caps (&vinfo, caps);

	if (GST_VIDEO_INFO_FORMAT (&vinfo) != GST_VIDEO_FORMAT_UNKNOWN) {
		g_assert (src->deviceContext != NULL);
		//  src->vrm_stride = get_pitch (src->device);  // wait for image to arrive for this
		src->gst_stride = GST_VIDEO_INFO_COMP_STRIDE (&vinfo, 0);
		src->nHeight = vinfo.height;
	} else {
		goto unsupported_caps;
	}

	// start freerun/continuous capture
	GST_DEBUG_OBJECT (src, "fc2StartCapture");
	FLYCAPEXECANDCHECK(fc2StartCapture(src->deviceContext));
	GST_DEBUG_OBJECT (src, "fc2StartCapture COMPLETED");
	src->acq_started = TRUE;

	return TRUE;

	unsupported_caps:
	GST_ERROR_OBJECT (src, "Unsupported caps: %" GST_PTR_FORMAT, caps);
	return FALSE;

	fail:
	return FALSE;
}

// Expect a raw16 image, before Bayer conversion, reduce this to a raw8 image
// I have not worked out how this should do this conversion !!!!!!!!!!!!!
// To try this, acquire in RAW16, retrieve into rawImage, use this fn to convert into tempImage, then Bayer convert into convertedImage
// The line should be commented below, and the acquisition line above.
//static int
//reduce_raw16_bitdepth(fc2Image *image_in, fc2Image *image_out)
//{
//	guint i, j;
//
//	// create a temp image with the correct size and format RAW8
//	fc2SetImageDimensions(image_out, image_in->rows, image_in->cols, image_in->stride, FC2_PIXEL_FORMAT_RAW8, image_in->bayerFormat);
//
//	// copy some bits from the raw16 image to the temp image
//	for (i = 0; i < image_in->rows; i++) {
//		for (j = 0; j < image_in->cols; j++) {
//
//			image_out->pData[i*image_out->stride + j] = (unsigned char)image_in->pData[i*image_in->stride + j];
//
//		}
//	}
//
//	return 0;
//}

/* Copy and duplicate data from the possibly binned image
 *  into the full size image
 */
void
copy_duplicate_data(GstFlycapSrc *src, GstMapInfo *minfo)
{
	guint i, j, ii;

	// From the grabber source we get 1 progressive frame
	// We expect src->nPitch = src->gst_stride but use separate vars for safety

//	GST_DEBUG_OBJECT (src, "copy_duplicate_data: binning %d src->nRawWidth %d src->nRawHeight %d src->nRawPitch %d", src->binning, src->nRawWidth, src->nRawHeight, src->nRawPitch);

	if (src->binning == 1){   // just copy the data into the buffer
		for (i = 0; i < src->nHeight; i++) {
			memcpy (minfo->data + i * src->gst_stride, src->convertedImage.pData + i * src->nPitch, src->nPitch);
		}
	}
	else if (src->binning == 2){   // duplicate to expand by 2x
		for (i = 0, ii = 0; i < src->nRawHeight; i++, ii+=2) {

			// For every source row we need to fill 2 rows of the destination
//			GST_DEBUG_OBJECT (src, "copy_duplicate_data: row %d,%d nBytesPerPixel %d", i, ii, src->nBytesPerPixel);

			guint8 *s_ptr = src->convertedImage.pData + i * src->nRawPitch; // source ptr
			guint8 *d_ptr  = minfo->data + ii * src->gst_stride; // destination ptr

			for (j = 0; j < src->nRawWidth; j++){   // Work in source space (smaller), increment destination in the loop

				// For every source pixel we must fill 2 destination pixels on each of 2 rows
				memcpy(d_ptr, s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->nBytesPerPixel, s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->gst_stride, s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->gst_stride+src->nBytesPerPixel, s_ptr, src->nBytesPerPixel);

				s_ptr += src->nBytesPerPixel;
				d_ptr += src->nBytesPerPixel*2;
			}

		}
	}
	else if (src->binning == 4){   // interpolate to expand by 4x, take care of image size is it is not exactly 4x, do that by iterating over the source, not destination

		// Set ii here in case centring loop becomes commented
		ii = 0;

		// With 4x4 bin image will be small, try to centre it by filling rows with black
		// Because of the part of the sensor used, this may displace the image wrt other binning modes
		// To uncentre comment this loop
		int nlines = (src->nHeight - (4*src->nRawHeight))/2;
		for (ii = 0; ii < nlines; ii++) {
			memset (minfo->data + ii * src->gst_stride, 0, src->nPitch);
		}

		for (i = 0; i < src->nRawHeight; i++, ii+=4) {   // NB starting ii set by above loop

			// For every source row we need to fill 4 rows of the destination
//			GST_DEBUG_OBJECT (src, "copy_duplicate_data: row %d,%d nBytesPerPixel %d", i, ii, src->nBytesPerPixel);

			guint8 *s_ptr = src->convertedImage.pData + i * src->nRawPitch; // source ptr
			guint8 *d_ptr = minfo->data + ii * src->gst_stride; // destination ptr

			for (j = 0; j < src->nRawWidth; j++){   // Work in source space (smaller), increment destination in the loop

				// For every source pixel we must fill 4 destination pixels on each of 4 rows

				memcpy(d_ptr, s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->nBytesPerPixel,   s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->nBytesPerPixel*2, s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->nBytesPerPixel*3, s_ptr, src->nBytesPerPixel);

				memcpy(d_ptr+src->gst_stride,                       s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->gst_stride+src->nBytesPerPixel,   s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->gst_stride+src->nBytesPerPixel*2, s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->gst_stride+src->nBytesPerPixel*3, s_ptr, src->nBytesPerPixel);

				memcpy(d_ptr+src->gst_stride*2,                       s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->gst_stride*2+src->nBytesPerPixel,   s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->gst_stride*2+src->nBytesPerPixel*2, s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->gst_stride*2+src->nBytesPerPixel*3, s_ptr, src->nBytesPerPixel);

				memcpy(d_ptr+src->gst_stride*3,                       s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->gst_stride*3+src->nBytesPerPixel,   s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->gst_stride*3+src->nBytesPerPixel*2, s_ptr, src->nBytesPerPixel);
				memcpy(d_ptr+src->gst_stride*3+src->nBytesPerPixel*3, s_ptr, src->nBytesPerPixel);

				s_ptr += src->nBytesPerPixel;
				d_ptr += src->nBytesPerPixel*4;
			}

			// With 4x4 bin there will be some pixels at end of row to fill, always 8 pixels
			memset(d_ptr,                     0, src->nBytesPerPixel*8);
			memset(d_ptr+src->gst_stride*1,   0, src->nBytesPerPixel*8);
			memset(d_ptr+src->gst_stride*2,   0, src->nBytesPerPixel*8);
			memset(d_ptr+src->gst_stride*3,   0, src->nBytesPerPixel*8);

		}

		// With 4x4 bin there will be some pixels at end of cols to fill, keep generic for different cameras
		if(ii < src->nHeight){
			for (; ii < src->nHeight; ii++) {
				memset (minfo->data + ii * src->gst_stride, 0, src->nPitch);
			}
		}

	}
}

/* Copy and interpolate data from the possibly binned image
 *  into the full size image.
 *  I believe for 2x2 this does work out to be bi-linear interpolation.
 *  Calculate 'easy' rows by adding an averaged pixel between each pair of source pixels.
 *  Extra 'difficult' rows must be added by averaging 2 easy rows
 *  TODO THIS DOES NOT WORK SO FAR
 */
void
copy_interpolate_data(GstFlycapSrc *src, GstMapInfo *minfo)
{
	guint i, j, ii;
	guint8 last;

	// From the grabber source we get 1 progressive frame
	// We expect src->nPitch = src->gst_stride but use separate vars for safety

	if (src->binning == 1){   // just copy the data into the buffer
		for (i = 0; i < src->nHeight; i++) {
			memcpy (minfo->data + i * src->gst_stride, src->convertedImage.pData + i * src->nPitch, src->nPitch);
		}
	}
	else if (src->binning == 2){   // interpolate to expand by 2x
		for (i = 0, ii = 0; i < src->nRawHeight; i++, ii+=2) {

			// For every source row we need to fill 2 rows of the destination
			// one is a 'difficult' row in between source pixel rows
			// one is an 'easy' row which has source pixels on it
			// on the first source row, only fill an easy row in dest
			// on every other row do a difficult row, based on the last and next easy rows, and then an easy row

			// Row pointers
			guint8 *s_ptr = src->convertedImage.pData + i * src->nRawPitch; // source ptr
			guint8 *d_ptr = minfo->data + ii * src->gst_stride; // destination ptr

			// Always do an 'easy' row
			for (j = 0; j < src->nRawWidth; j++){   // Work in source space (smaller), increment destination in the loop

				if(j>0){ // if past the first pixel interpolate one pixel back, and fill 2 pixels for every source pixel
					*d_ptr = (last + *s_ptr)/2;  // linear interpolate
					d_ptr += src->nBytesPerPixel;
				}

				*d_ptr = *s_ptr;
				last = *s_ptr;

				d_ptr += src->nBytesPerPixel;
				s_ptr += src->nBytesPerPixel;
			}

			// if past the first row, do a 'difficult' row
			if(i>0){

				guint8 *d_ptr = minfo->data + (ii-1) * src->gst_stride; // destination ptr, one row back

				for (j = 0; j < src->nRawWidth; j++){   // Work in source space (smaller), increment destination in the loop

					if(j>0){ // if past the first pixel interpolate one pixel back, and fill 2 pixels for every source pixel
						// we interpolate from the dest, which has one easy row ahead and behind filled already
						*d_ptr = ((*d_ptr + src->gst_stride) + *(d_ptr - src->gst_stride))/2;
						d_ptr += src->nBytesPerPixel;
					}

					// Here we can interpolate from the source, could also use dest as above
					*d_ptr = (*s_ptr + *(s_ptr - src->gst_stride))/2;

					d_ptr += src->nBytesPerPixel;
					s_ptr += src->nBytesPerPixel;
				}
			}
		}
	}
	else if (src->binning == 4){   // interpolate to expand by 4x, take care of image size is it is not exactly 4x
		// ??? OMG
	}
}

/*
 * Make some changes to the image if we have just had a call for a parameter change.
 * Useful for investigating the timing of parameter changes.
 */
void
overlay_param_changed(GstFlycapSrc *src, GstMapInfo *minfo)
{
	guint i;
	const guint size=100, offset=150;

	if (src->exposure_just_changed){
		for (i = src->nHeight-size; i < src->nHeight; i++) {
			memset (minfo->data + i * src->gst_stride, 255, size*src->nBytesPerPixel);
		}
		src->exposure_just_changed = FALSE;
	}

	if (src->gain_just_changed){
		for (i = src->nHeight-size; i < src->nHeight; i++) {
			memset (minfo->data + i * src->gst_stride + offset*src->nBytesPerPixel, 255, size*src->nBytesPerPixel);
		}
		src->gain_just_changed = FALSE;
	}

	if (src->binning_just_changed){
		for (i = src->nHeight-size; i < src->nHeight; i++) {
			memset (minfo->data + i * src->gst_stride + 2*offset*src->nBytesPerPixel, 255, size*src->nBytesPerPixel);
		}
		src->binning_just_changed = FALSE;
	}
}


//  This can override the push class create fn, it is the same as fill above but it forces the creation of a buffer here to copy into.
#ifdef OVERRIDE_CREATE
static GstFlowReturn
gst_flycap_src_create (GstPushSrc * psrc, GstBuffer ** buf)
{
	GstFlycapSrc *src = GST_FLYCAP_SRC (psrc);
	GstMapInfo minfo;
	fc2Error error;

	// Get image
//	GST_DEBUG_OBJECT (src, "fc2RetrieveBuffer");
//	error = fc2RetrieveBuffer(src->deviceContext, &src->rawImage);
	error = fc2RetrieveBuffer(src->deviceContext, &src->convertedImage);

	if(G_LIKELY(error == FC2_ERROR_OK))
	{
		//  successfully returned an image
		// ----------------------------------------------------------

//		reduce_raw16_bitdepth(&src->rawImage,  &src->tempImage);

		// Copy image to buffer in the right way
		//GST_DEBUG_OBJECT (src, "fc2ConvertImageTo");
//        error = fc2ConvertImageTo(FC2_PIXEL_FORMAT_BGR, &src->rawImage, &src->convertedImage);
//        error = fc2ConvertImageTo(FC2_PIXEL_FORMAT_RGB, &src->tempImage, &src->convertedImage);
        if ( error != FC2_ERROR_OK )
        {
    		GST_ERROR_OBJECT(src, "fc2ConvertImageTo() failed with a error: %d", error);
    		return GST_FLOW_ERROR;
        }

        //GST_DEBUG_OBJECT (src, "rawImage format %x bayer %d", src->rawImage.format, src->rawImage.bayerFormat);
        //GST_DEBUG_OBJECT (src, "convertedImage format %x bayer %d", src->convertedImage.format, src->convertedImage.bayerFormat);

		// Create a new buffer for the image
		*buf = gst_buffer_new_and_alloc (src->nHeight * src->gst_stride);

		gst_buffer_map (*buf, &minfo, GST_MAP_WRITE);

		copy_duplicate_data(src, &minfo);
		//copy_interpolate_data(src, &minfo);  // NOT WORKING, SEE ABOVE

		// Normally this is commented out, useful for timing investigation
		//overlay_param_changed(src, &minfo);

		gst_buffer_unmap (*buf, &minfo);

		// If we do not use gst_base_src_set_do_timestamp() we need to add timestamps manually
		src->last_frame_time += src->duration;   // Get the timestamp for this frame
		if(!gst_base_src_get_do_timestamp(GST_BASE_SRC(psrc))){
			GST_BUFFER_PTS(*buf) = src->last_frame_time;  // convert ms to ns
			GST_BUFFER_DTS(*buf) = src->last_frame_time;  // convert ms to ns
		}
		GST_BUFFER_DURATION(*buf) = src->duration;
		//GST_DEBUG_OBJECT(src, "pts, dts: %" GST_TIME_FORMAT ", duration: %d ms", GST_TIME_ARGS (src->last_frame_time), GST_TIME_AS_MSECONDS(src->duration));

		// count frames, and send EOS when required frame number is reached
		GST_BUFFER_OFFSET(*buf) = src->n_frames;  // from videotestsrc
		src->n_frames++;
		GST_BUFFER_OFFSET_END(*buf) = src->n_frames;  // from videotestsrc
		if (psrc->parent.num_buffers>0)  // If we were asked for a specific number of buffers, stop when complete
			if (G_UNLIKELY(src->n_frames >= psrc->parent.num_buffers))
				return GST_FLOW_EOS;
	}
	else
	{
		// did not return an image. why?
		// ----------------------------------------------------------
		GST_ERROR_OBJECT(src, "fc2RetrieveBuffer() failed with a error: %d", error);
		return GST_FLOW_ERROR;
	}

	if (G_UNLIKELY(src->WB_in_progress)){
		if (gst_flycap_check_WB_onepush(src, NULL, NULL)==FALSE){
			src->WB_in_progress = FALSE;
			src->WB_progress=0;
		}
		else
			src->WB_progress--;

		if (src->WB_progress<0){
			// Try to abort the WB, next gst_flycap_check_WB_onepush should return FALSE
			// This will reset the WB to the default
			GST_DEBUG_OBJECT (src, "Aborting WB");
			gst_flycap_set_property_WB_manual(src);
		}
	}

	return GST_FLOW_OK;
}
#endif // OVERRIDE_CREATE


