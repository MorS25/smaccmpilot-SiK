// -*- Mode: C; c-basic-offset: 8; -*-
//
// Copyright (c) 2011 Michael Smith, All Rights Reserved
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//  o Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  o Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in
//    the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.
//

///
/// @file	at.c
///
/// A simple AT command parser.
///

#include "radio.h"
#include "tdm.h"
#include "hxstream.h"

// canary data for ram wrap. It is in at.c as the compiler
// assigns addresses in alphabetial order and we want this at a low
// address
__pdata uint8_t pdata_canary = 0x41;

// AT command buffer
__xdata volatile char at_cmd[AT_CMD_MAXLEN + 1];
__xdata volatile uint8_t at_cmd_len;

// mode flags
volatile bool at_mode_active; ///< if true, incoming bytes are for AT command
volatile bool at_cmd_ready; ///< if true, at_cmd / at_cmd_len contain valid data
volatile bool at_resp_noframing = false; ///< if true, don't use hx framing for output

// test bits
__pdata uint8_t		at_testmode;    ///< test modes enabled (AT_TEST_*)

// command handlers
static void	at_ok(void);
static void	at_error(void);
static void	at_i(void);
static void	at_s(void);
static void	at_ampersand(void);
static void	at_plus(void);
static void	at_bin_status(void);
static void	at_bin_debug(void);
static void	at_bin_common(void);

#pragma save
#pragma nooverlay
void
at_input(register uint8_t c)
{
    at_input_aux(c);
}
#pragma restore

void
at_input_aux(register uint8_t c)
{
	// Ignore input if a command is pending.
	if (at_cmd_ready == true) return;
	// AT mode is active and waiting for a command
	switch (c) {
		// CR - submits command for processing
	case '\r':
		at_cmd[at_cmd_len] = 0;
		at_cmd_ready = true;
		break;

		// character - add to buffer if valid
	default:
		if (at_cmd_len < AT_CMD_MAXLEN) {
			if (isprint(c)) {
				c = toupper(c);
				at_cmd[at_cmd_len++] = c;
			}
			break;
		}

		// If the AT command buffer overflows we abandon
		// AT mode and return to passthrough mode; this is
		// to minimise the risk of locking up on reception
		// of an accidental escape sequence.

		at_mode_active = 0;
		at_resp_noframing = 0;
		at_cmd_len = 0;
		break;
	}
}

// +++ detector state machine
//
// states:
//
// wait_for_idle:	-> wait_for_plus1, count = 0 after 1s
//
// wait_for_plus:	-> wait_for_idle if char != +
//			-> wait_for_plus, count++ if count < 3
// wait_for_enable:	-> enabled after 1s
//			-> wait_for_idle if any char
//

#define	ATP_WAIT_FOR_IDLE	0
#define ATP_WAIT_FOR_PLUS1	1
#define ATP_WAIT_FOR_PLUS2	2
#define ATP_WAIT_FOR_PLUS3	3
#define ATP_WAIT_FOR_ENABLE	4

#define ATP_COUNT_1S		100	// 100 ticks of the 100Hz timer

static __pdata uint8_t	at_plus_state;
static __pdata uint8_t	at_plus_counter = ATP_COUNT_1S;

#pragma save
#pragma nooverlay
void
at_plus_detector(register uint8_t c)
{
	// If we get a character that's not '+', unconditionally
	// reset the state machine to wait-for-idle; this will
	// restart the 1S timer.
	//
	if (c != (uint8_t)'+')
		at_plus_state = ATP_WAIT_FOR_IDLE;

	// We got a plus; handle it based on our current state.
	//
	switch (at_plus_state) {

	case ATP_WAIT_FOR_PLUS1:
	case ATP_WAIT_FOR_PLUS2:
		at_plus_state++;
		break;

	case ATP_WAIT_FOR_PLUS3:
		at_plus_state = ATP_WAIT_FOR_ENABLE;
		at_plus_counter = ATP_COUNT_1S;
		break;

	default:
		at_plus_state = ATP_WAIT_FOR_IDLE;
		// FALLTHROUGH
	case ATP_WAIT_FOR_IDLE:
	case ATP_WAIT_FOR_ENABLE:
		at_plus_counter = ATP_COUNT_1S;
		break;
	}
}
#pragma restore

#pragma save
#pragma nooverlay
void
at_timer(void)
{
	// if the counter is running
	if (at_plus_counter > 0) {

		// if it reaches zero, the timeout has expired
		if (--at_plus_counter == 0) {

			// make the relevant state change
			switch (at_plus_state) {
			case ATP_WAIT_FOR_IDLE:
				at_plus_state = ATP_WAIT_FOR_PLUS1;
				break;

			case ATP_WAIT_FOR_ENABLE:
				at_mode_active = true;
				at_plus_state = ATP_WAIT_FOR_IDLE;

				// stuff an empty 'AT' command to get the OK prompt
				at_cmd[0] = 'A';
				at_cmd[1] = 'T';
				at_cmd[2] = '\0';
				at_cmd_len = 2;
				at_resp_noframing = true;
				at_cmd_ready = true;
				break;
			default:
				// should never happen, but otherwise harmless
			}
		}
	}
}
#pragma restore

void
at_command(void)
{
	// require a command with the AT prefix
	if (at_cmd_ready) {
		if ((at_cmd_len >= 2) && (at_cmd[0] == 'A') && (at_cmd[1] == 'T')) {
			hxstream_term_begin_frame();
			printf("AT");
			// look at the next byte to determine what to do
			switch (at_cmd[2]) {
			case '\0':		// no command -> OK
				putchar('\n');
				at_ok();
				break;
			case '&':
				putchar('&');
				at_ampersand();
				break;
			case '+':
				putchar('+');
				at_plus();
				break;
			case 'I':
				putchar('I');
				at_i();
				break;
			case 'O':		// O -> go online (exit command mode)
				printf("O\n");
				at_plus_counter = ATP_COUNT_1S;
				at_mode_active = 0;
				at_resp_noframing = 0;
				break;
			case 'S':
				putchar('S');
				at_s();
				break;

			case 'Z':
				// generate a software reset
				RSTSRC |= (1 << 4);
				for (;;)
					;

			default:
				at_error();
			}
		} else if ((at_cmd_len == 1) && (at_cmd[0] == 'B')) {
			hxstream_term_begin_frame();
			at_bin_status();
		} else if ((at_cmd_len == 1) && (at_cmd[0] == 'D')) {
			hxstream_term_begin_frame();
			at_bin_debug();
		}

		hxstream_term_end_frame();

		// unlock the command buffer
		at_cmd_len = 0;
		at_cmd_ready = false;
	}
}

static void
at_ok(void)
{
	printf("%s\n", "OK");
}

static void
at_error(void)
{
	printf("%s\n", "ERROR");
}

__pdata uint8_t		idx;

static uint32_t
at_parse_number() __reentrant
{
	uint32_t	reg;
	uint8_t		c;

	reg = 0;
	for (;;) {
		c = at_cmd[idx];
		putchar(c);
		if (!isdigit(c))
			break;
		reg = (reg * 10) + (c - '0');
		idx++;
	}
	return reg;
}

static void
at_i(void)
{
	putchar(at_cmd[3]);
	putchar('\n');
	switch (at_cmd[3]) {
	case '\0':
	case '0':
		printf("%s\n", g_banner_string);
		return;
	case '1':
		printf("%s\n", g_version_string);
		return;
	case '2':
		printf("%u\n", BOARD_ID);
		break;
	case '3':
		printf("%u\n", g_board_frequency);
		break;
	case '4':
		printf("%u\n", g_board_bl_version);
		return;
	case '5': {
		enum ParamID id;
		// convenient way of showing all parameters
		for (id = 0; id < PARAM_MAX; id++) {
			printf("S%u: %s=%lu\n", 
			       (unsigned)id, 
			       param_name(id), 
			       (unsigned long)param_get(id));
		}
		return;
	}
	case '6':
		tdm_report_timing();
		return;
	case '7':
		tdm_show_rssi();
		return;
	default:
		at_error();
		return;
	}
}

static void
at_s(void)
{
	__pdata uint8_t		sreg;
	__pdata uint32_t	val;

	// get the register number first
	idx = 3;
	sreg = at_parse_number();
	// validate the selected sreg
	if (sreg >= PARAM_MAX) {
		at_error();
		return;
	}
	putchar(at_cmd[idx]);

	switch (at_cmd[idx]) {
	case '?':
		val = param_get(sreg);
		putchar('\n');
		printf("%lu\n", val);
		return;

	case '=':
		if (sreg > 0) {
			idx++;
			val = at_parse_number();
			if (param_set(sreg, val)) {
				putchar('\n');
				at_ok();
				return;
			}
		}
		break;
	}
	at_error();
}

static void
at_ampersand(void)
{
	putchar(at_cmd[3]);
	putchar('\n');
	switch (at_cmd[3]) {
	case 'F':
		param_default();
		at_ok();
		break;
	case 'W':
		param_save();
		at_ok();
		break;

	case 'U':
		if (!strcmp(at_cmd + 4, "PDATE")) {
			// force a flash error
			volatile char x = *(__code volatile char *)0xfc00;
			for (;;)
				;
		}
		at_error();
		break;

	case 'P':
		tdm_change_phase();
		break;

	case 'T':
		// enable test modes
		if (!strcmp(at_cmd + 4, "")) {
			// disable all tests
			at_testmode = 0;
		} else if (!strcmp(at_cmd + 4, "=RSSI")) {
			// display RSSI stats
			at_testmode ^= AT_TEST_RSSI;
		} else if (!strcmp(at_cmd + 4, "=TDM")) {
			// display TDM debug
			at_testmode ^= AT_TEST_TDM;
		} else {
			at_error();
		}
		break;
		
	default:
		at_error();
		break;
	}
}

static void
at_plus(void)
{
#ifdef BOARD_rfd900a
	__pdata uint8_t		creg;
	__pdata uint32_t	val;

	putchar(at_cmd[3]);
	putchar('\n');
	// get the register number first
	idx = 4;
	creg = at_parse_number();
	switch (at_cmd[3])
	{
	case 'P': // AT+P=x set power level pwm to x immediately
		if (at_cmd[4] != '=')
		{
			break;
		}
		idx = 5;
		val = at_parse_number();
		PCA0CPH3 = val & 0xFF;
		radio_set_diversity(false);
		at_ok();
		return;
	case 'C': // AT+Cx=y write calibration value
		switch (at_cmd[idx])
		{
		case '?':
			val = calibration_get(creg);
			printf("%lu\n",val);
			return;
		case '=':
			idx++;
			val = at_parse_number();
			if (calibration_set(creg, val&0xFF))
			{
				at_ok();
			} else {
				at_error();
			}
			return;
		}
		break;
	case 'L': // AT+L lock bootloader area if all calibrations written
		if (calibration_lock())
		{
			at_ok();
		} else {
			at_error();
		}
		return;
	}
#endif //BOARD_rfd900a
	at_error();

}

static void
at_bin_status(void)
{
	putchar('B');                             // 0
	at_bin_common();                          // 1..24
}

static void
at_bin_debug(void)
{
	putchar('D');                             // 0
	at_bin_common();                          // 1..24
	putchar(errors.max_xmit);                 // 25
	putchar(tx_insert);                       // 26
	putchar(tx_remove);                       // 27
	putchar(rx_insert);                       // 28
	putchar(rx_remove);                       // 29
	errors.max_xmit = 0;
}

#define put16(_v) do  { \
	putchar( (uint8_t) (((_v) >> 8) & 0xFF) ); \
	putchar( (uint8_t) (((_v) >> 0) & 0xFF) ); \
	} while (0);

static void
at_bin_common(void)
{
	putchar(statistics.average_rssi);         // 1
	putchar(statistics.average_noise);        // 2
	put16  (statistics.receive_count);        // 3, 4
	putchar(remote_statistics.average_rssi);  // 5
	putchar(remote_statistics.average_noise); // 6
	put16  (remote_statistics.receive_count); // 7, 8
	put16  (errors.tx_errors);                // 9, 10
	put16  (errors.rx_errors);                // 11, 12
	put16  (errors.serial_tx_overflow);       // 13, 14
	put16  (errors.serial_rx_overflow);       // 15, 16
	put16  (errors.corrected_errors);         // 17, 18
	put16  (errors.corrected_packets);        // 19, 20
	put16  (errors.serial_tx_ok);             // 21, 22
	put16  (errors.serial_rx_ok);             // 23, 24
}

