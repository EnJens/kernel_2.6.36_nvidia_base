#ifndef _INCLUDE_SOUND_ALC5623_H
#define _INCLUDE_SOUND_ALC5623_H

struct alc5623_platform_data {
	char*		mclk;
	/* configure :                              */
	/* Lineout/Speaker Amps Vmid ratio control  */
	/* enable/disable adc/dac high pass filters */
	unsigned int add_ctrl;
	/* configure :                              */
	/* output to enable when jack is low        */
	/* output to enable when jack is high       */
	/* jack detect (gpio/nc/jack detect [12]    */
	unsigned int jack_det_ctrl;
	unsigned int    linevdd_mv;              /* Line Vdd in millivolts */
	unsigned int    linevol_scale;
	unsigned int    gpio_base;
};

#endif
