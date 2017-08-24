/*
 * Copyright (c) 2016, Cascoda
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef USB_EXCHANGE_H
#define USB_EXCHANGE_H

#define TEST_ENABLE 1

enum usb_exchange_errors {
	usb_exchange_err_usb,		//Usb error - probably device removed and going to have to crash safely
	usb_exchange_err_ca821x,	//ca821x error - can probably trigger a hard reset and recover
	usb_exchange_err_generic
};

/* Optional callback for the application layer
 * to handle any errors which would otherwise
 * cause a crash.
 */
typedef int (*usb_exchange_errorhandler)(
	int error_number
);

/* Optional callback for the application layer
 * to handle any non-ca821x communication with
 * a usb device over the same protocol. Any
 * command IDs which are not recognised as
 * a valid ca821x SPI command will be passed
 * to this callback.
 */
typedef int (*usb_exchange_user_callback)(
	const uint8_t *buf, size_t len, void *pDeviceRef
);

/*
 * Must call ONE of the following functions in order to initialize driver communications
 *
 * Using usb_exchange_init will cause the program to crash if there is an error
 *
 * Using usb_exchange_init_withhandler and passing a callback function will cause
 * that callback function to execute in the case of an error. Passing a callback of NULL causes
 * the same behaviour as usb_exchange_init.
 */

/**
 * Initialise the usb exchange, with no callback for errors (program will
 * crash in the case of an error.
 *
 * @warning It is recommended to use the usb_exchange_init_withandler function
 * instead, so that any errors can be handled by your application.
 *
 * @returns 0 for success, -1 for error, 1 if already initialised
 *
 */
int usb_exchange_init(void);

/**
 * Initialise the usb exchange, using the supplied errorhandling callback to
 * report any errors back to the application, which can react as required
 * (i.e. crash gracefully or attempt to reset the ca8210)
 *
 * @param[in]  callback   Function pointer to an error-handling callback
 *
 * @returns 0 for success, -1 for error
 *
 */
int usb_exchange_init_withhandler(usb_exchange_errorhandler callback);

/**
 * Registers the callback to call for any non-ca821x commands that are sent over
 * the usb interface. Commands are still limited to the ca821x format, and must
 * use a command ID that is not currently used by the ca821x-spi protocol.
 * Currently, 0xA8 is used for openthread commands.
 *
 * @param[in]  callback   Function pointer to an user-command-handling callback
 *
 * @returns 0 for success, -1 for error
 *
 */
int usb_exchange_register_user_callback(usb_exchange_user_callback callback);

/**
 * Sends a USB command over the USB interface using the TLV format from ca821x-spi.
 *
 * Requirements:
 *  -The command byte is not already used by the ca821x-spi protocol
 *  -The SPI_SYNC (0x40) bit is not set
 *  -The command length (not including command and length) is less than 189 bytes
 *
 * @param[in]   buf   Buffer containing the message to be sent over usb. First
 *                    byte is the command ID, second byte is the length of the
 *                    command not including the first 2 bytes.
 *
 * @param[in]   len   Length of the buffer (including first 2 bytes)
 *
 * @param[in]   pDeviceRef   Device reference for sending
 *
 * @returns 0 for success, -1 for error
 *
 */
int usb_exchange_user_send(const uint8_t *buf, size_t len, void *pDeviceRef);

/**
 * Deinitialise the usb exchange, so that it can be reinitialised by another
 * process, or reopened later.
 *
 */
void usb_exchange_deinit(void);

/**
 * Send a hard reset to the ca8210. This should not be necessary, but is provided
 * in case the ca8210 becomes unresponsive to spi.
 *
 * @param[in]  resettime   The length of time (in ms) to hold the reset pin
 *                         active for. 1ms is usually a suitable value for this.
 *
 *
 */
int ca8210_test_int_reset(unsigned long resettime);

#ifdef TEST_ENABLE
	//Run to test fragmentation. Crashes upon fail.
	void test_frag_loopback();
#endif

#endif
