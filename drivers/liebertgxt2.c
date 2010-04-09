/* liebertgxt2.c - driver for Liebert GXT2, using the ESP2 protocol
 *
 *  Copyright (C)
 *  2009	Richard Gregory <r.gregory liverpool ac uk>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "main.h"
#include "serial.h"
#include "timehead.h"
#include "nut_stdint.h"

#define DRIVER_NAME	"Liebert GXT2 serial UPS driver"
#define DRIVER_VERSION	"0.02"

static int instcmd(const char *cmdname, const char *extra);
static int setvar(const char *varname, const char *val);

/* driver description structure */
upsdrv_info_t upsdrv_info = {
	DRIVER_NAME,
	DRIVER_VERSION,
	"Richard Gregory <r.gregory liv ac uk>",
	DRV_EXPERIMENTAL,
	{ NULL }
};

static const unsigned char
	/* Bit field information provided by Spiros Ioannou */
	/* Ordered on MSB to LSB. Shown as DESCRIPTION (bit number), starting at 0. */
	cmd_bitfield1[]		= { 1,148,2,1,1,153 },	/* ON_BATTERY(8), INPUT_OVERVOLTAGE(7), BATTERY_TEST_STATE(6), OVERTEMP_WARNING(5), INRUSH_LIMIT_ON(4), UTILITY_STATE(3), ON_INVERTER(2), DC_DC_CONVERTER_STATE(1), PFC_ON(0) */
	cmd_bitfield2[]		= { 1,148,2,1,2,154 },	/* BUCK_ON (9), DIAG_LINK_SET(7), BOOST_ON(6), REPLACE_BATTERY(5), BATTERY_LIFE_ENHANCER_ON(4), BATTERY_CHARGED (1), ON_BYPASS (0) */
	cmd_bitfield3[]		= { 1,148,2,1,3,155 },	/* CHECK_AIR_FILTER (10), BAD_BYPASS_PWR (8), OUTPUT_OVERVOLTAGE (7), OUTPUT_UNDERVOLTAGE (6), LOW_BATTERY (5), CHARGER_FAIL (3), SHUTDOWN_PENDING (2), BAD_INPUT_FREQ (1), UPS_OVERLOAD (0) */
	cmd_bitfield7[]		= { 1,148,2,1,7,159 },	/* AMBIENT_OVERTEMP (2) */
	cmd_battestres[]	= { 1,148,2,1,12,164 },	/* BATTERY_TEST_RESULT */
	cmd_selftestres[]	= { 1,148,2,1,13,165 };	/* SELF_TEST_RESULT */

static char cksum(const char *buf, const size_t len)
 {
	char	sum = 0;
	size_t	i;

	for (i = 0; i < len; i++) {
		sum += buf[i];
	}

	return sum;
}

static int do_command(const unsigned char *command, char *reply)
{
	int	ret;

	ret = ser_send_buf(upsfd, command, 6);
	if (ret < 0) {
		upsdebug_with_errno(2, "send");
		return -1;
	} else if (ret < 6) {
		upsdebug_hex(2, "send: truncated", command, ret);
		return -1;
	}

	upsdebug_hex(2, "send", command, ret);

	ret = ser_get_buf(upsfd, reply, 8, 1, 0);
	if (ret < 0) {
		upsdebug_with_errno(2, "read");
		return -1;
	} else if (ret < 6) {
		upsdebug_hex(2, "read: truncated", reply, ret);
		return -1;
	} else if (reply[7] != cksum(reply, 7)) {
		upsdebug_hex(2, "read: checksum error", reply, ret);
		return -1;
	}

	upsdebug_hex(2, "read", reply, ret);
	return ret;
}

void upsdrv_initinfo(void)
{
	struct {
		const char	*var;
	} vartab[] = {
		{ "ups.model" },
		{ "ups.firmware" },
		{ "ups.serial" },
		{ "ups.mfr.date" },
		{ NULL }
	};

	char	buf[LARGEBUF], *s;
	int	i;

	dstate_setinfo("ups.mfr", "%s", "Liebert");

	for (i = 0; i < 37; i++) {
		char	command[6], reply[8];
		int	ret;

		snprintf(command, sizeof(command), "\x01\x88\x02\x01%c", i+4);
		command[5] = cksum(command, 5);

		ret = do_command((unsigned char *)command, reply);
		if (ret < 8) {
			break;
		}

		buf[i<<1] = reply[6];
		buf[(i<<1)+1] = reply[5];
	}

	buf[i<<1] = 0;

	for (s = strtok(buf, " "), i = 0; s && vartab[i].var; s = strtok(NULL, " "), i++) {
		dstate_setinfo(vartab[i].var, "%s", s);
	}

	if (i == 0) {
		fatalx(EXIT_FAILURE, "GTX2 capable UPS not detected");
	}

	upsh.instcmd = instcmd;
	upsh.setvar = setvar;
}

void upsdrv_updateinfo(void)
{
	struct {
		const unsigned char	cmd[6];
		const char	*var;
		const char	*fmt;
		const double	mult;
	} vartab[] = {
		{ { 1,149,2,1,4,157 },	"battery.charge", "%.0f", 1.0 },
		{ { 1,149,2,1,1,154 },	"battery.runtime", "%.0f", 60 },
		{ { 1,149,2,1,2,155 },	"battery.voltage", "%.1f", 0.1 },
		{ { 1,149,2,1,3,156 },	"battery.current", "%.2f", 0.01 },
		{ { 1,161,2,1,13,178 },	"battery.voltage.nominal", "%.1f", 0.1 },
		{ { 1,149,2,1,7,160 },	"ups.load", "%.0f", 1.0 },
		{ { 1,149,2,1,6,159 },	"ups.power", "%.0f", 1.0 },
		{ { 1,161,2,1,8,173 },	"ups.power.nominal", "%.0f", 1.0 },
		{ { 1,149,2,1,5,158 },	"ups.realpower", "%.0f", 1.0 },
		{ { 1,149,2,1,14,167 },	"ups.temperature", "%.1f", 0.1 },
		{ { 1,144,2,1,1,149 },	"input.voltage", "%.1f", 0.1 },
		{ { 1,149,2,1,8,161 },	"input.frequency", "%.1f", 0.1 },
		{ { 1,149,2,1,10,163 },	"input.frequency.nominal", "%.1f", 0.1 },
		{ { 1,144,2,1,5,153 },	"input.bypass.voltage", "%.1f", 0.1 },
		{ { 1,144,2,1,3,151 },	"output.voltage", "%.1f", 0.1 },
		{ { 1,149,2,1,9,162 },	"output.frequency", "%.1f", 0.1 },
		{ { 1,144,2,1,4,152 },	"output.current", "%.1f", 0.1 },
		{ { 0 }, NULL, NULL, 0 }
	};

	const char	*val;
	char	reply[8];
	int	ret, i;

	for (i = 0; vartab[i].var; i++) {
		int16_t	val;

		ret = do_command(vartab[i].cmd, reply);
		if (ret < 8) {
			continue;
		}

		val = (unsigned char)reply[5];
		val <<= 8;
		val += (unsigned char)reply[6];

		dstate_setinfo(vartab[i].var, vartab[i].fmt, val * vartab[i].mult);
	}

	status_init();

	ret = do_command(cmd_bitfield1, reply);
	if (ret < 8) {
		upslogx(LOG_ERR, "Failed reading bitfield #1");
		dstate_datastale();
		return;
	}

	if (reply[5] & (1<<0)) {	/* ON_BATTERY */
		status_set("OB");
	} else {
		status_set("OL");
	}

	val = dstate_getinfo("battery.current");
	if (val) {
		if (atof(val) > 0.05) {
			status_set("CHRG");
		}
		if (atof(val) < -0.05) {
			status_set("DISCHRG");
		}
	}

	ret = do_command(cmd_bitfield2, reply);
	if (ret < 8) {
		upslogx(LOG_ERR, "Failed reading bitfield #2");
		dstate_datastale();
		return;
	}

	if (reply[6] & (1<<0)) {	/* ON_BYPASS */
		status_set("BYPASS");
	}

	if (reply[6] & (1<<5)) {	/* REPLACE_BATTERY */
		status_set("RB");
	}

	if (reply[6] & (1<<6)) {	/* BOOST_ON */
		status_set("BOOST");
	}

	if (reply[5] & (1<<1)) {	/* BUCK_ON */
		status_set("TRIM");
	}

	ret = do_command(cmd_bitfield3, reply);
	if (ret < 8) {
		upslogx(LOG_ERR, "Failed reading bitfield #3");
		dstate_datastale();
		return;
	}

	if (reply[6] & (1<<0) ) {	/* UPS_OVERLOAD */
		status_set("OVER");
	}

	if (reply[6] & (1<<5) ) {	/* LOW_BATTERY */
		status_set("LB");
	}

	status_commit();

	dstate_dataok();
}

void upsdrv_shutdown(void)
{
	/* replace with a proper shutdown function */
	fatalx(EXIT_FAILURE, "shutdown not supported");
}

static int instcmd(const char *cmdname, const char *extra)
{
/*
	if (!strcasecmp(cmdname, "test.battery.stop")) {
		ser_send_buf(upsfd, ...);
		return STAT_INSTCMD_HANDLED;
	}
 */
	upslogx(LOG_NOTICE, "instcmd: unknown command [%s]", cmdname);
	return STAT_INSTCMD_UNKNOWN;
}

static int setvar(const char *varname, const char *val)
{
/*
	if (!strcasecmp(varname, "ups.test.interval")) {

	ser_send_buf(upsfd, ...);
		return STAT_SET_HANDLED;
	}
 */
	upslogx(LOG_NOTICE, "setvar: unknown variable [%s]", varname);
	return STAT_SET_UNKNOWN;
}

void upsdrv_help(void)
{
}

/* list flags and values that you want to receive via -x */
void upsdrv_makevartable(void)
{
	addvar (VAR_VALUE, "baudrate", "serial line speed");
}

void upsdrv_initups(void)
{
	const char *val = getval("baudrate");
	speed_t baudrate = B2400;

	if (val) {
		switch (atoi(val))
		{
		case 1200:
			baudrate = B1200;
			break;
		case 2400:
			baudrate = B2400;
			break;
		case 4800:
			baudrate = B4800;
			break;
		case 9600:
			baudrate = B9600;
			break;
		case 19200:
			baudrate = B19200;
			break;
		default:
			fatalx(EXIT_FAILURE, "Baudrate [%s] unsupported", val);
		}
	}

	upsfd = ser_open(device_path);
	ser_set_speed(upsfd, device_path, baudrate);
}

void upsdrv_cleanup(void)
{
	ser_close(upsfd, device_path);
}