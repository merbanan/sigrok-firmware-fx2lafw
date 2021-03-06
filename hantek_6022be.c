/*
 * This file is part of the sigrok-firmware-fx2lafw project.
 *
 * Copyright (C) 2009 Ubixum, Inc.
 * Copyright (C) 2015 Jochen Hoenicke
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <fx2macros.h>
#include <fx2ints.h>
#include <autovector.h>
#include <delay.h>
#include <setupdat.h>

/* Change to support as many interfaces as you need. */
static BYTE altiface = 0;

static volatile WORD ledcounter = 0;

static volatile __bit dosud = FALSE;
static volatile __bit dosuspend = FALSE;

extern __code BYTE highspd_dscr;
extern __code BYTE fullspd_dscr;

void resume_isr(void) __interrupt RESUME_ISR
{
	CLEAR_RESUME();
}

void sudav_isr(void) __interrupt SUDAV_ISR
{
	dosud = TRUE;
	CLEAR_SUDAV();
}

void usbreset_isr(void) __interrupt USBRESET_ISR
{
	handle_hispeed(FALSE);
	CLEAR_USBRESET();
}

void hispeed_isr(void) __interrupt HISPEED_ISR
{
	handle_hispeed(TRUE);
	CLEAR_HISPEED();
}

void suspend_isr(void) __interrupt SUSPEND_ISR
{
	dosuspend = TRUE;
	CLEAR_SUSPEND();
}

void timer2_isr(void) __interrupt TF2_ISR
{
	PA7 = !PA7;
	if (ledcounter) {
		if (--ledcounter == 0) {
			/* Clear LED. */
			PC0 = 1;
			PC1 = 1;
		}
	}
	TF2 = 0;
}

/*
 * This sets three bits for each channel, one channel at a time.
 * For channel 0 we want to set bits 5, 6 & 7
 * For channel 1 we want to set bits 2, 3 & 4
 *
 * We convert the input values that are strange due to original
 * firmware code into the value of the three bits as follows:
 *
 * val -> bits
 * 1  -> 010b
 * 2  -> 001b
 * 5  -> 000b
 * 10 -> 011b
 *
 * The third bit is always zero since there are only four outputs connected
 * in the serial selector chip.
 *
 * The multiplication of the converted value by 0x24 sets the relevant bits in
 * both channels and then we mask it out to only affect the channel currently
 * requested.
 */
static BOOL set_voltage(BYTE channel, BYTE val)
{
	BYTE bits, mask;

	switch (val) {
	case 1:
		bits = 0x24 * 2;
		break;
	case 2:
		bits = 0x24 * 1;
		break;
	case 5:
		bits = 0x24 * 0;
		break;
	case 10:
		bits = 0x24 * 3;
		break;
	default:
		return FALSE;
	}

	mask = (channel) ? 0xe0 : 0x1c;
	IOC = (IOC & ~mask) | (bits & mask);

	return TRUE;
}

static BOOL set_numchannels(BYTE numchannels)
{
	if (numchannels == 1 || numchannels == 2) {
		BYTE fifocfg = 7 + numchannels;
		EP2FIFOCFG = fifocfg;
		EP6FIFOCFG = fifocfg;
		return TRUE;
	}

	return FALSE;
}

static void clear_fifo(void)
{
	GPIFABORT = 0xff;
	SYNCDELAY3;
	FIFORESET = 0x80;
	SYNCDELAY3;
	FIFORESET = 0x82;
	SYNCDELAY3;
	FIFORESET = 0x86;
	SYNCDELAY3;
	FIFORESET = 0;
}

static void stop_sampling(void)
{
	GPIFABORT = 0xff;
	SYNCDELAY3;
	INPKTEND = (altiface == 0) ? 6 : 2;
}

static void start_sampling(void)
{
	int i;

	clear_fifo();

	for (i = 0; i < 1000; i++);

	while (!(GPIFTRIG & 0x80))
		;

	SYNCDELAY3;
	GPIFTCB1 = 0x28;
	SYNCDELAY3;
	GPIFTCB0 = 0;
	GPIFTRIG = (altiface == 0) ? 6 : 4;

	/* Set green LED, don't clear LED. */
	ledcounter = 0;
	PC0 = 1;
	PC1 = 0;
}

static void select_interface(BYTE alt)
{
	const BYTE *pPacketSize = \
		((USBCS & bmHSM) ? &highspd_dscr : &fullspd_dscr)
		+ (9 + (16 * alt) + 9 + 4);

	altiface = alt;

	if (alt == 0) {
		/* Bulk on EP6. */
		EP2CFG = 0x00;
		EP6CFG = 0xe0;
		EP6GPIFFLGSEL = 1;
		EP6AUTOINLENL = pPacketSize[0];
		EP6AUTOINLENH = pPacketSize[1];
	} else {
		/* Iso on EP2. */
		EP2CFG = 0xd8;
		EP6CFG = 0x00;
		EP2GPIFFLGSEL = 1;
		EP2AUTOINLENL = pPacketSize[0];
		EP2AUTOINLENH = pPacketSize[1] & 0x7;
		EP2ISOINPKTS = (pPacketSize[1] >> 3) + 1;
	}
}

static const struct samplerate_info {
	BYTE rate;
	BYTE wait0;
	BYTE wait1;
	BYTE opc0;
	BYTE opc1;
	BYTE out0;
	BYTE ifcfg;
} samplerates[] = {
	{ 48, 0x80,   0, 3, 0, 0x00, 0xea },
	{ 30, 0x80,   0, 3, 0, 0x00, 0xaa },
	{ 24,    1,   0, 2, 1, 0x40, 0xca },
	{ 16,    1,   1, 2, 0, 0x40, 0xca },
	{ 12,    2,   1, 2, 0, 0x40, 0xca },
	{  8,    3,   2, 2, 0, 0x40, 0xca },
	{  4,    6,   5, 2, 0, 0x40, 0xca },
	{  2,   12,  11, 2, 0, 0x40, 0xca },
	{  1,   24,  23, 2, 0, 0x40, 0xca },
	{ 50,   48,  47, 2, 0, 0x40, 0xca },
	{ 20,  120, 119, 2, 0, 0x40, 0xca },
	{ 10,  240, 239, 2, 0, 0x40, 0xca },
};

static BOOL set_samplerate(BYTE rate)
{
	BYTE i = 0;

	while (samplerates[i].rate != rate) {
		i++;
		if (i == sizeof(samplerates) / sizeof(samplerates[0]))
			return FALSE;
	}

	IFCONFIG = samplerates[i].ifcfg;

	AUTOPTRSETUP = 7;
	AUTOPTRH2 = 0xE4;
	AUTOPTRL2 = 0x00;

	/*
	 * The program for low-speed, e.g. 1 MHz, is:
	 * wait 24, CTL2=0, FIFO
	 * wait 23, CTL2=1
	 * jump 0, CTL2=1
	 *
	 * The program for 24 MHz is:
	 * wait 1, CTL2=0, FIFO
	 * jump 0, CTL2=1
	 *
	 * The program for 30/48 MHz is:
	 * jump 0, CTL2=Z, FIFO, LOOP
	 */

	EXTAUTODAT2 = samplerates[i].wait0;
	EXTAUTODAT2 = samplerates[i].wait1;
	EXTAUTODAT2 = 1;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;

	EXTAUTODAT2 = samplerates[i].opc0;
	EXTAUTODAT2 = samplerates[i].opc1;
	EXTAUTODAT2 = 1;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;

	EXTAUTODAT2 = samplerates[i].out0;
	EXTAUTODAT2 = 0x44;
	EXTAUTODAT2 = 0x44;
	EXTAUTODAT2 = 0x00;
	EXTAUTODAT2 = 0x00;
	EXTAUTODAT2 = 0x00;
	EXTAUTODAT2 = 0x00;
	EXTAUTODAT2 = 0x00;

	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;
	EXTAUTODAT2 = 0;

	for (i = 0; i < 96; i++)
		EXTAUTODAT2 = 0;

	return TRUE;
}

/* Set *alt_ifc to the current alt interface for ifc. */
BOOL handle_get_interface(BYTE ifc, BYTE *alt_ifc)
{
	(void)ifc;

	*alt_ifc = altiface;

	return TRUE;
}

/*
 * Return TRUE if you set the interface requested.
 *
 * Note: This function should reconfigure and reset the endpoints
 * according to the interface descriptors you provided.
 */
BOOL handle_set_interface(BYTE ifc,BYTE alt_ifc)
{
	if (ifc == 0)
		select_interface(alt_ifc);

	return TRUE;
}

BYTE handle_get_configuration(void)
{
	/* We only support configuration 0. */
	return 0;
}

BOOL handle_set_configuration(BYTE cfg)
{
	/* We only support configuration 0. */
	(void)cfg;

	return TRUE;
}

BOOL handle_vendorcommand(BYTE cmd)
{
	stop_sampling();

	/* Set red LED. */
	PC0 = 0;
	PC1 = 1;
	ledcounter = 1000;

	/* Clear EP0BCH/L for each valid command. */
	if (cmd >= 0xe0 && cmd <= 0xe4) {
		EP0BCH = 0;
		EP0BCL = 0;
		while (EP0CS & bmEPBUSY);
	}

	switch (cmd) {
	case 0xe0:
	case 0xe1:
		set_voltage(cmd - 0xe0, EP0BUF[0]);
		return TRUE;
	case 0xe2:
		set_samplerate(EP0BUF[0]);
		return TRUE;
	case 0xe3:
		if (EP0BUF[0] == 1)
			start_sampling();
		return TRUE;
	case 0xe4:
		set_numchannels(EP0BUF[0]);
		return TRUE;
	}

	return FALSE; /* Not handled by handlers. */
}

static void init(void)
{
	EP4CFG = 0;
	EP8CFG = 0;

	/* In idle mode tristate all outputs. */
	GPIFIDLECTL = 0x00;
	GPIFCTLCFG = 0x80;
	GPIFWFSELECT = 0x00;
	GPIFREADYSTAT = 0x00;

	stop_sampling();

	set_voltage(0, 1);
	set_voltage(1, 1);
	set_samplerate(1);
	set_numchannels(2);
	select_interface(0);
}

static void main(void)
{
	/* Save energy. */
	SETCPUFREQ(CLK_12M);

	init();

	/* Set up interrupts. */
	USE_USB_INTS();

	ENABLE_SUDAV();
	ENABLE_USBRESET();
	ENABLE_HISPEED(); 
	ENABLE_SUSPEND();
	ENABLE_RESUME();

	/* Global (8051) interrupt enable. */
	EA = 1;

	/* Init timer2. */
	RCAP2L = -500 & 0xff;
	RCAP2H = (-500 & 0xff00) >> 8;
	T2CON = 0;
	ET2 = 1;
	TR2 = 1;

	RENUMERATE();

	PORTCCFG = 0;
	PORTACFG = 0;
	OEC = 0xff;
	OEA = 0x80;

	while (TRUE) {
		if (dosud) {
			dosud = FALSE;
			handle_setupdata();
		}

		if (dosuspend) {
			dosuspend = FALSE;
			do {
				/* Make sure ext wakeups are cleared. */
				WAKEUPCS |= bmWU|bmWU2;
				SUSPEND = 1;
				PCON |= 1;
				__asm
				nop
				nop
				nop
				nop
				nop
				nop
				nop
				__endasm;
			} while (!remote_wakeup_allowed && REMOTE_WAKEUP());

			/* Resume (TRM 6.4). */
			if (REMOTE_WAKEUP()) {
				delay(5);
				USBCS |= bmSIGRESUME;
				delay(15);
				USBCS &= ~bmSIGRESUME;
			}
		}
	}
}
