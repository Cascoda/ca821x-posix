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

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <dlfcn.h>

#include "hidapi/hidapi/hidapi.h"
#include "usb_exchange.h"
#include "ca821x_api.h"
#include "ca821x-posix.h"

#define USB_VID 0x0416
#define USB_PID 0x5020

#define MAX_FRAG_SIZE 64
#define POLL_DELAY 2

#ifndef USB_MAX_DEVICES
#define USB_MAX_DEVICES 5
#endif

#define FRAG_LEN_MASK 0x3F
#define FRAG_LAST_MASK (1 << 7)
#define FRAG_FIRST_MASK (1 << 6)

struct usb_exchange_priv
{
	struct ca821x_exchange_base base;
	hid_device *hid_dev;
	char *hid_path;
	ca821x_errorhandler error_callback;
	usb_exchange_user_callback user_callback;

	//Synchronous queue
	pthread_t io_thread;
	int io_thread_runflag;
	pthread_cond_t sync_cond;
	pthread_mutex_t sync_mutex;
	//In queue = Device to host(us)
	//Out queue = Host(us) to device
	pthread_mutex_t in_queue_mutex, out_queue_mutex;
	struct buffer_queue *in_buffer_queue, *out_buffer_queue;
};

static struct ca821x_dev *s_devs[USB_MAX_DEVICES] = { 0 };

static int s_initialised, s_worker_run_flag, s_devcount = 0;

//Dynamic hid-api library
static void *s_hid_lib_handle = NULL;
static struct hid_device_info *(*dhid_enumerate)(unsigned short, unsigned short);
static hid_device *(*dhid_open_path)(const char *);
static void (*dhid_free_enumeration)(struct hid_device_info *);
static int (*dhid_read_timeout)(hid_device *, unsigned char *, size_t, int);
static int (*dhid_write)(hid_device *, const unsigned char *, size_t);

static pthread_t dd_thread;
static pthread_mutex_t flag_mutex = PTHREAD_MUTEX_INITIALIZER,
                       devs_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dd_cond = PTHREAD_COND_INITIALIZER,
                      devs_cond = PTHREAD_COND_INITIALIZER;

static struct buffer_queue *downstream_dispatch_queue = NULL;
static pthread_mutex_t downstream_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

static int ca8210_test_int_exchange(const uint8_t *buf,
                                    size_t len,
                                    uint8_t *response,
                                    struct ca821x_dev *pDeviceRef);

//returns 1 for non-final fragment, 0 for final
static int get_next_frag(uint8_t *buf_in, uint8_t len_in, uint8_t *frag_out,
                         uint8_t *offset)
{
	int end_offset = *offset + MAX_FRAG_SIZE - 1;
	uint8_t is_first = 0, is_last = 0, frag_len = 0;
	is_first = (*offset == 0);

	if (end_offset >= len_in)
	{
		end_offset = len_in;
		is_last = 1;
	}
	frag_len = end_offset - *offset;

	assert((frag_len & FRAG_LEN_MASK) == frag_len);

	frag_out[0] = 0;
	frag_out[1] = 0;
	frag_out[1] |= frag_len;
	frag_out[1] |= is_first ? FRAG_FIRST_MASK : 0;
	frag_out[1] |= is_last ? FRAG_LAST_MASK : 0;
	memcpy(&frag_out[2], &buf_in[*offset], frag_len);

	*offset = end_offset;

	if (is_last) *offset = 0;
	return !is_last;
}

//returns 1 for non-final fragment, 0 for final
static int assemble_frags(uint8_t *frag_in, uint8_t *buf_out, uint8_t *len_out,
                          uint8_t *offset)
{
	uint8_t is_first = 0, is_last = 0, frag_len = 0;
	frag_len = frag_in[0] & FRAG_LEN_MASK;
	is_last = !!(frag_in[0] & FRAG_LAST_MASK);
	is_first = !!(frag_in[0] & FRAG_FIRST_MASK);

	assert((is_first) == (*offset == 0));

	memcpy(&buf_out[*offset], &frag_in[1], frag_len);

	*offset += frag_len;
	*len_out = *offset;

	if (is_last) *offset = 0;
	return !is_last;
}

#ifdef TEST_ENABLE
void test_frag_loopback()
{
	uint8_t data_in[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	                      0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
	                      0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a,
	                      0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23,
	                      0xca, 0x5c, 0x0d, 0xaa, 0xca, 0x5c, 0x0d, 0xaa, 0x11,
	                      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
	                      0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x45, 0x56, 0x67,
	                      0xe9, 0xdc, 0x7c, 0x64, 0x56, 0x9a, 0x68, 0xe9, 0x86,
	                      0xe8, 0xe2, 0xf1, 0x92, 0x9e, 0xc5, 0x92, 0x67, 0x5f,
	                      0x91, 0x65, 0xae, 0x9f, 0x01, 0x45, 0x12, 0xe5, 0xdb,
	                      0xfb, 0x07, 0xf2, 0xe8, 0xfd, 0xb2, 0x54, 0x26, 0x1d,
	                      0xe8, 0xec, 0x3e, 0xf8, 0x25, 0xaa, 0xe6, 0x7e, 0xba,
	                      0x5b, 0xa0, 0x6e, 0xfc, 0xa3, 0xdf, 0x6d, 0x97, 0xbe,
	                      0x7c, 0xf6, 0x51, 0x77, 0x7f, 0x28, 0x44, 0xda, 0x48,
	                      0x4f, 0x2e, 0x57, 0xc3, 0x81, 0x8e, 0x76, 0x22, 0x3d,
	                      0x40, 0x5a, 0x69, 0x62, 0x91, 0x10, 0x87, 0x1d, 0x11,
	                      0x11, 0x11, 0xca, 0x5c, 0x0d, 0xaa, 0x11, 0x11 };

	int data_in_size = sizeof(data_in) / sizeof(data_in[0]);
	uint8_t data_out[MAX_BUF_SIZE];
	uint8_t frag_buf[MAX_FRAG_SIZE + 1];
	uint8_t len, rval, offset1 = 0, offset2 = 0;

	do
	{
		rval = get_next_frag(data_in, data_in_size, frag_buf, &offset1);
	} while (assemble_frags(frag_buf + 1, data_out, &len, &offset2));

	assert(!rval); //make sure both assembly and deconstruction thought this was last frag
	assert(len == data_in_size);

	rval = memcmp(data_in, data_out, len);
	assert(rval == 0);
}
#endif

static void *ca821x_downstream_dispatch_worker(void *arg)
{
	struct ca821x_dev *pDeviceRef;
	struct usb_exchange_priv *priv;

	uint8_t buffer[MAX_BUF_SIZE];
	uint8_t len;
	int rval;

	pthread_mutex_lock(&flag_mutex);
	while (s_worker_run_flag)
	{
		pthread_mutex_unlock(&flag_mutex);

		wait_on_queue(&downstream_dispatch_queue, &downstream_queue_mutex,
		              &dd_cond);

		len = pop_from_queue(&downstream_dispatch_queue,
		                     &downstream_queue_mutex,
		                     buffer,
		                     MAX_BUF_SIZE, &pDeviceRef);

		if (len > 0)
		{
			priv = pDeviceRef->exchange_context;
			rval = ca821x_downstream_dispatch(buffer, len, pDeviceRef);

			if (rval < 0 && priv->user_callback)
			{
				priv->user_callback(buffer, len, pDeviceRef);
			}
		}

		pthread_mutex_lock(&flag_mutex);
	}

	pthread_mutex_unlock(&flag_mutex);
	return 0;
}

static void *ca8210_io_worker(void *arg)
{
	struct ca821x_dev *pDeviceRef = arg;
	struct usb_exchange_priv *priv = pDeviceRef->exchange_context;
	uint8_t buffer[MAX_BUF_SIZE];
	uint8_t frag_buf[MAX_FRAG_SIZE + 1]; //+1 for report ID
	uint8_t delay, len, offset;
	int rval, error = 0;

	do
	{
		rval = dhid_read_timeout(priv->hid_dev, frag_buf, MAX_FRAG_SIZE, 10);
	} while (rval > 0);

	pthread_mutex_lock(&flag_mutex);
	while (s_worker_run_flag && priv->io_thread_runflag)
	{
		pthread_mutex_unlock(&flag_mutex);

		if (peek_queue(priv->out_buffer_queue, &(priv->out_queue_mutex)))
		{ //Use a nonblocking read if we are waiting to send messages
			delay = 0;
		}
		else
		{
			delay = POLL_DELAY;
		}

		//Read from the device if possible
		offset = 0;
		do
		{
			error = dhid_read_timeout(priv->hid_dev, frag_buf, MAX_FRAG_SIZE,
			                          delay);
			if (error <= 0) break;
			delay = -1;
		} while (assemble_frags(frag_buf, buffer, &len, &offset));

		if (error > 0)
		{
			if (buffer[0] & SPI_SYN)
			{
				//Add to queue for synchronous processing
				add_to_waiting_queue(&(priv->in_buffer_queue),
				                     &(priv->in_queue_mutex),
				                     &(priv->sync_cond),
				                     buffer, len, pDeviceRef);
			}
			else
			{
				//Add to queue for dispatching downstream
				add_to_waiting_queue(&downstream_dispatch_queue,
				                     &downstream_queue_mutex,
				                     &dd_cond, buffer, len, pDeviceRef);
			}
		}

		//Send any queued messages
		len = pop_from_queue(&(priv->out_buffer_queue),
		                     &(priv->out_queue_mutex),
		                     buffer,
		                     MAX_BUF_SIZE, &pDeviceRef);

		if (len > 0)
		{
			offset = 0;
			priv = pDeviceRef->exchange_context;
			do
			{
				rval = get_next_frag(buffer, len, frag_buf, &offset);
				error = dhid_write(priv->hid_dev, frag_buf, MAX_FRAG_SIZE + 1);
			} while (rval);
		}

		if (error < 0)
		{
			if (priv->error_callback)
			{
				priv->error_callback(usb_exchange_err_usb);
			}
			else
			{
				abort();
			}
		}

		pthread_mutex_lock(&flag_mutex);
	}

	pthread_mutex_unlock(&flag_mutex);
	return 0;
}

static int load_dlibs()
{
	int error = 0;

	//Load the dynamic library
	s_hid_lib_handle = dlopen("libhidapi-libusb.so", RTLD_NOW);
	if (s_hid_lib_handle == NULL)
	{
		s_hid_lib_handle = dlopen("libhidapi-hidraw.so", RTLD_NOW);
	}

	if (s_hid_lib_handle == NULL)
	{
		error = -1;
		goto exit;
	}

	dhid_enumerate = dlsym(s_hid_lib_handle, "hid_enumerate");
	dhid_open_path = dlsym(s_hid_lib_handle, "hid_open_path");
	dhid_free_enumeration = dlsym(s_hid_lib_handle, "hid_free_enumeration");
	dhid_read_timeout = dlsym(s_hid_lib_handle, "hid_read_timeout");
	dhid_write = dlsym(s_hid_lib_handle, "hid_write");

	if (dlerror() != NULL)
	{
		error = -1;
		goto exit;
	}

exit:
	if (error && (s_hid_lib_handle != NULL))
	{
		dlclose(s_hid_lib_handle);
	}
	return error;
}

static int init_statics()
{
	int rval, error = 0;

	error = load_dlibs();
	if(error) goto exit;

	s_worker_run_flag = 1;
	rval = pthread_create(&dd_thread, NULL, &ca821x_downstream_dispatch_worker,
	                      NULL);
	if (rval != 0)
	{
		//The io thread is successfully running but dd is not
		pthread_mutex_lock(&flag_mutex);
		s_worker_run_flag = 0;
		pthread_mutex_unlock(&flag_mutex);
		error = -1;
		goto exit;
	}

	s_initialised = 1;

exit:
	return error;
}

static int deinit_statics()
{

	s_initialised = 0;
	pthread_mutex_lock(&flag_mutex);
	s_worker_run_flag = 0;
	pthread_mutex_unlock(&flag_mutex);

	//Wake the downstream dispatch thread up so that it dies cleanly
	add_to_waiting_queue(&downstream_dispatch_queue, &downstream_queue_mutex,
	                     &dd_cond,
	                     NULL, 0, NULL);

	//TODO: Should probably wait for the workers to actually complete here

	dlclose(s_hid_lib_handle);
	return 0;
}

int usb_exchange_init(struct ca821x_dev *pDeviceRef)
{
	return usb_exchange_init_withhandler(NULL, pDeviceRef);
}

static int is_hidpath_in_use(char *path)
{
	int rval;
	for (int i = 0; i < USB_MAX_DEVICES; i++)
	{
		if (!s_devs[i]) continue;
		struct usb_exchange_priv *cur = s_devs[i]->exchange_context;
		rval = strcmp(path, cur->hid_path);
		if (rval == 0) return 1;
	}
	return 0;
}

static struct hid_device_info *get_next_hid(struct hid_device_info *hid_cur)
{
	while (hid_cur != NULL)
	{
		if (!is_hidpath_in_use(hid_cur->path)) break;
		hid_cur = hid_cur->next;
	}

	return hid_cur;
}

int usb_exchange_init_withhandler(ca821x_errorhandler callback,
                                  struct ca821x_dev *pDeviceRef)
{
	struct hid_device_info *hid_ll = NULL, *hid_cur = NULL;
	hid_device *dev = NULL;
	struct usb_exchange_priv *priv = NULL;
	int error = 0;
	size_t len = 0;

	if (!s_initialised)
	{
		error = init_statics();
		if (error) return error;
	}

	if (pDeviceRef->exchange_context) return 1;

	pthread_mutex_lock(&devs_mutex);
	if (s_devcount >= USB_MAX_DEVICES)
	{
		error = -1;
		goto exit;
	}

	//Iterate through compatible HIDs until one is found that hasn't already
	//been opened.
	hid_ll = dhid_enumerate(USB_VID, USB_PID);
	hid_cur = get_next_hid(hid_ll);
	while (dev == NULL && hid_cur != NULL)
	{
		dev = dhid_open_path(hid_cur->path);
		if (dev == NULL)
		{
			hid_cur = get_next_hid(hid_cur->next);
		}
	}
	if (hid_cur == NULL)
	{ //Device not found
		error = -1;
		goto exit;
	}

	pDeviceRef->exchange_context = calloc(1, sizeof(struct usb_exchange_priv));
	priv = pDeviceRef->exchange_context;
	priv->base.exchange_type = ca821x_exchange_usb;
	priv->error_callback = callback;

	len = strlen(hid_cur->path);
	priv->hid_path = calloc(1, len + 1);
	strncpy(priv->hid_path, hid_cur->path, len);
	priv->hid_dev = dev;

	pthread_mutex_init(&(priv->sync_mutex), NULL);
	pthread_mutex_init(&(priv->in_queue_mutex), NULL);
	pthread_mutex_init(&(priv->out_queue_mutex), NULL);
	pthread_cond_init(&(priv->sync_cond), NULL);

	pthread_mutex_lock(&flag_mutex);
	priv->io_thread_runflag = 1;
	pthread_mutex_unlock(&flag_mutex);

	error = pthread_create(&(priv->io_thread),
	                       NULL,
	                       &ca8210_io_worker,
	                       pDeviceRef);
	if (error != 0)
	{
		error = -1;
		goto exit;
	}

	pDeviceRef->ca821x_api_downstream = ca8210_test_int_exchange;

	//Add the new device to the device list for io
	s_devcount++;
	pthread_cond_signal(&devs_cond);
	for (int i = 0; i < USB_MAX_DEVICES; i++)
	{
		if (s_devs[i] == NULL)
		{
			s_devs[i] = pDeviceRef;
			break;
		}
	}

exit:
	if (hid_ll) dhid_free_enumeration(hid_ll);
	if (error && pDeviceRef->exchange_context)
	{
		free(priv->hid_path);
		free(pDeviceRef->exchange_context);
		pDeviceRef->exchange_context = NULL;
	}
	pthread_mutex_unlock(&devs_mutex);
	return error;
}

int usb_exchange_register_user_callback(usb_exchange_user_callback callback,
                                        struct ca821x_dev *pDeviceRef)
{
	struct usb_exchange_priv *priv = pDeviceRef->exchange_context;

	if (priv->user_callback) return -1;

	priv->user_callback = callback;

	return 0;
}

void usb_exchange_deinit(struct ca821x_dev *pDeviceRef)
{
	struct usb_exchange_priv *priv = pDeviceRef->exchange_context;

	pthread_mutex_lock(&devs_mutex);
	s_devcount--;
	if (s_devcount == 0) deinit_statics();
	pthread_cond_signal(&devs_cond);
	for (int i = 0; i < USB_MAX_DEVICES; i++)
	{
		if (s_devs[i] == pDeviceRef)
		{
			s_devs[i] = NULL;
			break;
		}
	}
	pthread_mutex_unlock(&devs_mutex);

	pthread_mutex_lock(&flag_mutex);
	priv->io_thread_runflag = 0;
	pthread_mutex_unlock(&flag_mutex);

	//TODO: Wait for worker thread completion

	pthread_mutex_destroy(&(priv->sync_mutex));
	pthread_mutex_destroy(&(priv->in_queue_mutex));
	pthread_cond_destroy(&(priv->sync_cond));
	priv->error_callback = NULL;
	free(priv->hid_path);
	free(priv);
	pDeviceRef->exchange_context = NULL;
}

int usb_exchange_reset(unsigned long resettime, struct ca821x_dev *pDeviceRef)
{
	return -1;
}

int usb_exchange_user_send(const uint8_t *buf, size_t len,
                           struct ca821x_dev *pDeviceRef)
{
	struct usb_exchange_priv *priv = pDeviceRef->exchange_context;
	assert(!(buf[0] & SPI_SYN));
	assert(len < MAX_BUF_SIZE);
	if (!s_initialised) return -1;
	add_to_queue(&(priv->out_buffer_queue),
	             &(priv->out_queue_mutex),
	             buf,
	             len,
	             pDeviceRef);
	return 0;
}

static int ca8210_test_int_exchange(
                                    const uint8_t *buf,
                                    size_t len,
                                    uint8_t *response,
                                    struct ca821x_dev *pDeviceRef)
{
	const uint8_t isSynchronous = ((buf[0] & SPI_SYN) && response);
	struct usb_exchange_priv *priv = pDeviceRef->exchange_context;
	struct ca821x_dev *ref_out;

	if (!s_initialised) return -1;
	//Synchronous must execute synchronously
	//Get sync responses from the in queue
	//Send messages by adding them to the out queue

	if (isSynchronous) pthread_mutex_lock(&(priv->sync_mutex));

	add_to_queue(&(priv->out_buffer_queue),
	             &(priv->out_queue_mutex),
	             buf,
	             len,
	             pDeviceRef);

	if (!isSynchronous) return 0;

	wait_on_queue(&(priv->in_buffer_queue), &(priv->in_queue_mutex),
	              &(priv->sync_cond));

	pop_from_queue(&(priv->in_buffer_queue), &(priv->in_queue_mutex), response,
	               sizeof(struct MAC_Message),
	               &ref_out);

	assert(ref_out == pDeviceRef);
	pthread_mutex_unlock(&(priv->sync_mutex));

	return 0;
}
