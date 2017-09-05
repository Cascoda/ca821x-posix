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

#include "hidapi/hidapi/hidapi.h"
#include "usb_exchange.h"
#include "ca821x_api.h"

#define USB_VID 0x0416
#define USB_PID 0x5020

#define MAX_BUF_SIZE 189
#define MAX_FRAG_SIZE 64
#define POLL_DELAY 2

#ifndef USB_MAX_DEVICES
#define USB_MAX_DEVICES 5
#endif

#define FRAG_LEN_MASK 0x3F
#define FRAG_LAST_MASK (1 << 7)
#define FRAG_FIRST_MASK (1 << 6)

struct usb_exchange_priv {
	hid_device *hid_dev;
	usb_exchange_errorhandler error_callback;
	usb_exchange_user_callback user_callback;

	//Synchronous queue
	pthread_cond_t sync_cond;
	pthread_mutex_t sync_mutex;
	pthread_mutex_t in_queue_mutex;
	struct buffer_queue *in_buffer_queue;
};

struct ca821x_dev *s_devs[USB_MAX_DEVICES] = {0};

int s_initialised, s_worker_run_flag, s_devcount = 0;

static pthread_t usb_io_thread, dd_thread;
static pthread_mutex_t flag_mutex = PTHREAD_MUTEX_INITIALIZER,
                       devs_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dd_cond = PTHREAD_COND_INITIALIZER,
                      devs_cond = PTHREAD_COND_INITIALIZER;

struct buffer_queue{
	size_t len;
	uint8_t * buf;
	struct ca821x_dev *pDeviceRef;
	struct buffer_queue * next;
};

//In queue = Device to host(us)
//Out queue = Host(us) to device
static struct buffer_queue *out_buffer_queue = NULL,
                           *downstream_dispatch_queue = NULL;
static pthread_mutex_t out_queue_mutex = PTHREAD_MUTEX_INITIALIZER,
                       downstream_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

static void add_to_queue(struct buffer_queue **head_buffer_queue,
                         pthread_mutex_t *buf_queue_mutex,
                         const uint8_t *buf,
                         size_t len,
                         struct ca821x_dev *pDeviceRef);

static void add_to_waiting_queue(struct buffer_queue **head_buffer_queue,
                                 pthread_mutex_t *buf_queue_mutex,
                                 pthread_cond_t *queue_cond,
                                 const uint8_t *buf,
                                 size_t len,
                                 struct ca821x_dev *pDeviceRef);

static size_t pop_from_queue(struct buffer_queue **head_buffer_queue,
                             pthread_mutex_t *buf_queue_mutex,
                             uint8_t * destBuf,
                             size_t maxlen,
                             struct ca821x_dev **pDeviceRef_out);

static size_t peek_queue(struct buffer_queue *head_buffer_queue,
                         pthread_mutex_t *buf_queue_mutex);

static size_t wait_on_queue(struct buffer_queue ** head_buffer_queue,
                            pthread_mutex_t *buf_queue_mutex,
                            pthread_cond_t *queue_cond);

static int ca8210_test_int_exchange(const uint8_t *buf,
                                    size_t len,
                                    uint8_t *response,
                                    struct ca821x_dev *pDeviceRef);

//returns 1 for non-final fragment, 0 for final
static int get_next_frag(uint8_t *buf_in, uint8_t len_in, uint8_t *frag_out)
{
	static uint8_t offset = 0;
	int end_offset = offset + MAX_FRAG_SIZE - 1;
	uint8_t is_first = 0, is_last = 0, frag_len = 0;
	is_first = (offset == 0);

	if(end_offset >= len_in)
	{
		end_offset = len_in;
		is_last = 1;
	}
	frag_len = end_offset - offset;

	assert((frag_len & FRAG_LEN_MASK) == frag_len);

	frag_out[0] = 0;
	frag_out[1] = 0;
	frag_out[1] |= frag_len;
	frag_out[1] |= is_first ? FRAG_FIRST_MASK : 0;
	frag_out[1] |= is_last ? FRAG_LAST_MASK : 0;
	memcpy(&frag_out[2], &buf_in[offset], frag_len);

	offset = end_offset;

	if(is_last) offset = 0;
	return !is_last;
}

//returns 1 for non-final fragment, 0 for final
static int assemble_frags(uint8_t *frag_in, uint8_t *buf_out, uint8_t *len_out)
{
	static uint8_t offset = 0;
	uint8_t is_first = 0, is_last = 0, frag_len = 0;
	frag_len = frag_in[0] & FRAG_LEN_MASK;
	is_last = !!(frag_in[0] & FRAG_LAST_MASK);
	is_first = !!(frag_in[0] & FRAG_FIRST_MASK);

	assert((is_first) == (offset == 0));

	memcpy(&buf_out[offset], &frag_in[1], frag_len);

	offset += frag_len;
	*len_out = offset;

	if(is_last) offset = 0;
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
	                      0x11, 0x11, 0xca, 0x5c, 0x0d, 0xaa, 0x11, 0x11       };

	int data_in_size = sizeof(data_in)/sizeof(data_in[0]);
	uint8_t data_out[MAX_BUF_SIZE];
	uint8_t frag_buf[MAX_FRAG_SIZE+1];
	uint8_t len, rval;

	do{
		rval = get_next_frag(data_in, data_in_size, frag_buf);
	} while(assemble_frags(frag_buf+1, data_out, &len));

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
	while(s_worker_run_flag)
	{
		pthread_mutex_unlock(&flag_mutex);

		wait_on_queue(&downstream_dispatch_queue, &downstream_queue_mutex, &dd_cond);

		len = pop_from_queue(&downstream_dispatch_queue, &downstream_queue_mutex,
		                     buffer, MAX_BUF_SIZE, &pDeviceRef);
		priv = pDeviceRef->exchange_context;

		if(len > 0) rval = ca821x_downstream_dispatch(buffer, len, pDeviceRef);

		if(rval < 0 && priv->user_callback)
		{
			priv->user_callback(buffer, len, pDeviceRef);
		}

		pthread_mutex_lock(&flag_mutex);
	}

	pthread_mutex_unlock(&flag_mutex);
	return 0;
}

static struct ca821x_dev *get_next_io_dev()
{
	static size_t i = 0, ni = 0;
	static pthread_mutex_t fmut = PTHREAD_MUTEX_INITIALIZER;

	struct ca821x_dev *pDeviceRef;

	pthread_mutex_lock(&fmut);
	pthread_mutex_lock(&devs_mutex);
	ni = i;
	do{
		ni = (ni + 1) % USB_MAX_DEVICES;
	} while(s_devs[ni] == NULL && ni != i);
	i = ni;
	pDeviceRef = s_devs[i];
	pthread_mutex_unlock(&devs_mutex);
	pthread_mutex_unlock(&fmut);

	return pDeviceRef;
}

static void *ca8210_test_int_worker(void *arg)
{
	struct ca821x_dev *pDeviceRef;
	struct usb_exchange_priv *priv;
	uint8_t buffer[MAX_BUF_SIZE];
	uint8_t frag_buf[MAX_FRAG_SIZE+1]; //+1 for report ID
	uint8_t delay, len;
	int rval, error, devi = 0;

	pthread_mutex_lock(&flag_mutex);
	while(s_worker_run_flag)
	{
		pthread_mutex_unlock(&flag_mutex);

		pthread_mutex_lock(&devs_mutex);
		if(s_devcount == 0)
		{
			pthread_cond_wait(&devs_cond, &devs_mutex);
			pthread_mutex_unlock(&devs_mutex);
			goto end_loop;
		}
		else
		{
			devi = (devi+1) % s_devcount;
		}
		pthread_mutex_unlock(&devs_mutex);

		if(devi != 0)
		{ //Don't pause for every device because this will cause slowdown
			delay = 0;
		}
		else if(peek_queue(out_buffer_queue, &out_queue_mutex))
		{ //Use a nonblocking read if we are waiting to send messages
			delay = 0;
		}
		else
		{ //We are not waiting to send, and are at the end of a full iteration
			delay = POLL_DELAY;
		}

		//Focus the next device
		pDeviceRef = get_next_io_dev();
		priv = pDeviceRef->exchange_context;

		//Read from the device if possible
		do{
			error = hid_read_timeout(priv->hid_dev, frag_buf, MAX_FRAG_SIZE, delay);
			if(error <= 0) break;
			delay = -1;
		} while(assemble_frags(frag_buf, buffer, &len));

		if(error > 0)
		{
			if(buffer[0] & SPI_SYN)
			{
				//Add to queue for synchronous processing
				add_to_waiting_queue(&(priv->in_buffer_queue), &(priv->in_queue_mutex),
				                     &(priv->sync_cond), buffer, len, pDeviceRef);
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
		len = pop_from_queue(&out_buffer_queue, &out_queue_mutex,
		                      buffer, MAX_BUF_SIZE, &pDeviceRef);

		if(len > 0)
		{
			priv = pDeviceRef->exchange_context;
			do{
				rval = get_next_frag(buffer, len, frag_buf);
				error = hid_write(priv->hid_dev, frag_buf, MAX_FRAG_SIZE + 1);
			} while(rval);
		}

		if(error < 0)
		{
			if(priv->error_callback)
			{
				priv->error_callback(usb_exchange_err_usb);
			}
			else
			{
				abort();
			}
		}

	end_loop:
		pthread_mutex_lock(&flag_mutex);
	}

	pthread_mutex_unlock(&flag_mutex);
	return 0;
}

static int init_statics(){
	int rval, error = 0;
	s_worker_run_flag = 1;
	rval = pthread_create(&usb_io_thread, NULL, &ca8210_test_int_worker, NULL);
	if(rval != 0)
	{
		s_worker_run_flag = 0;
		error = -1;
		goto exit;
	}
	rval = pthread_create(&dd_thread, NULL, &ca821x_downstream_dispatch_worker, NULL);
	if(rval != 0)
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

static int deinit_statics(){

	s_initialised = 0;
	pthread_mutex_lock(&flag_mutex);
	s_worker_run_flag = 0;
	pthread_mutex_unlock(&flag_mutex);

	//Wake the downstream dispatch thread up so that it dies cleanly
	add_to_waiting_queue(&downstream_dispatch_queue, &downstream_queue_mutex,
					     &dd_cond, NULL, 0, NULL);

	//TODO: Should probably wait for the workers to actually complete here
	return 0;
}

int usb_exchange_init(struct ca821x_dev *pDeviceRef)
{
	return usb_exchange_init_withhandler(NULL, pDeviceRef);
}

int usb_exchange_init_withhandler(usb_exchange_errorhandler callback,
                                  struct ca821x_dev *pDeviceRef)
{
	struct hid_device_info *hid_ll = NULL, *hid_cur = NULL;
	struct usb_exchange_priv *priv = NULL;
	int error = 0;
	int count = 0;

	if(!s_initialised)
	{
		error = init_statics();
		if(error) return error;
	}

	if(pDeviceRef->exchange_context) return 1;

	pthread_mutex_lock(&devs_mutex);
	if(s_devcount >= USB_MAX_DEVICES)
	{
		error = -1;
		goto exit;
	}

	//Find all compatible USB HIDs
	hid_ll = hid_enumerate(USB_VID, USB_PID);
	if(!hid_ll)
	{
		error = -1;
		goto exit;
	}
	hid_cur = hid_ll;
	do{
		count++;
		hid_cur = hid_cur->next;
	} while(hid_cur != NULL);

	//TODO: Give some capability to choose which Chili is opened.
	printf("%d Chilis found.\n", count);

	pDeviceRef->exchange_context = calloc(1, sizeof(struct usb_exchange_priv));
	priv = pDeviceRef->exchange_context;
	priv->error_callback = callback;

	//For now, just use the first compatible HID
	priv->hid_dev = hid_open_path(hid_ll->path);
	if(priv->hid_dev == NULL)
	{
		error = -1;
		goto exit;
	}

	//Add the new device to the device list for io
	s_devcount++;
	pthread_cond_signal(&devs_cond);
	for(int i = 0; i < USB_MAX_DEVICES; i++)
	{
		if(s_devs[i] == NULL)
		{
			s_devs[i] = pDeviceRef;
			break;
		}
	}

	//Set up the pthread primitives for the device
	pthread_mutex_init(&(priv->sync_mutex), NULL);
	pthread_mutex_init(&(priv->in_queue_mutex), NULL);
	pthread_cond_init(&(priv->sync_cond), NULL);

	pDeviceRef->ca821x_api_downstream = ca8210_test_int_exchange;

exit:
	if(hid_ll) hid_free_enumeration(hid_ll);
	if(error && pDeviceRef->exchange_context)
	{
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

	if(priv->user_callback) return -1;

	priv->user_callback = callback;

	return 0;
}


void usb_exchange_deinit(struct ca821x_dev *pDeviceRef)
{
	struct usb_exchange_priv *priv = pDeviceRef->exchange_context;

	pthread_mutex_lock(&devs_mutex);
	s_devcount--;
	if(s_devcount == 0) deinit_statics();
	pthread_cond_signal(&devs_cond);
	for(int i = 0; i < USB_MAX_DEVICES; i++)
	{
		if(s_devs[i] == pDeviceRef)
		{
			s_devs[i] = NULL;
			break;
		}
	}
	pthread_mutex_unlock(&devs_mutex);

	pthread_mutex_destroy(&(priv->sync_mutex));
	pthread_mutex_destroy(&(priv->in_queue_mutex));
	pthread_cond_destroy(&(priv->sync_cond));
	priv->error_callback = NULL;
	free(priv);
	pDeviceRef->exchange_context = NULL;
}


int ca8210_test_int_reset(unsigned long resettime, struct ca821x_dev *pDeviceRef)
{
	return -1;
}

int usb_exchange_user_send(const uint8_t *buf, size_t len,
                           struct ca821x_dev *pDeviceRef)
{
	assert(!(buf[0] & SPI_SYN));
	assert(len < MAX_BUF_SIZE);
	if(!s_initialised) return -1;
	add_to_queue(&out_buffer_queue, &out_queue_mutex, buf, len, pDeviceRef);
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

	if(!s_initialised) return -1;
	//Synchronous must execute synchronously
	//Get sync responses from the in queue
	//Send messages by adding them to the out queue

	if(isSynchronous) pthread_mutex_lock(&(priv->sync_mutex));

	add_to_queue(&out_buffer_queue, &out_queue_mutex, buf, len, pDeviceRef);

	if(!isSynchronous) return 0;

	wait_on_queue(&(priv->in_buffer_queue), &(priv->in_queue_mutex), &(priv->sync_cond));

	pop_from_queue(&(priv->in_buffer_queue), &(priv->in_queue_mutex), response,
	               sizeof(struct MAC_Message), &ref_out);

	assert(ref_out == pDeviceRef);
	pthread_mutex_unlock(&(priv->sync_mutex));

	return 0;
}

static void add_to_queue(struct buffer_queue **head_buffer_queue,
                         pthread_mutex_t *buf_queue_mutex,
                         const uint8_t *buf,
                         size_t len,
                         struct ca821x_dev *pDeviceRef)
{
	add_to_waiting_queue(head_buffer_queue,
						 buf_queue_mutex,
						 NULL, buf, len, pDeviceRef);
}

static void add_to_waiting_queue(struct buffer_queue **head_buffer_queue,
                                 pthread_mutex_t *buf_queue_mutex,
                                 pthread_cond_t *queue_cond,
                                 const uint8_t *buf,
                                 size_t len,
                                 struct ca821x_dev *pDeviceRef)
{
	if(pthread_mutex_lock(buf_queue_mutex) == 0)
	{
		struct buffer_queue *nextbuf = *head_buffer_queue;
		if(nextbuf == NULL)
		{
			//queue empty -> start new queue
			*head_buffer_queue = malloc(sizeof(struct buffer_queue));
			memset(*head_buffer_queue, 0, sizeof(struct buffer_queue));
			nextbuf = *head_buffer_queue;
		}
		else
		{
			while(nextbuf->next != NULL)
			{
				nextbuf = nextbuf->next;
			}
			//allocate new buffer cell
			nextbuf->next = malloc(sizeof(struct buffer_queue));
			memset(nextbuf->next, 0, sizeof(struct buffer_queue));
			nextbuf = nextbuf->next;
		}

		nextbuf->len = len;
		nextbuf->buf = malloc(len);
		memcpy(nextbuf->buf, buf, len);
		nextbuf->pDeviceRef = pDeviceRef;
		if(queue_cond) pthread_cond_broadcast(queue_cond);
		pthread_mutex_unlock(buf_queue_mutex);
	}
}

static size_t pop_from_queue(struct buffer_queue **head_buffer_queue,
                             pthread_mutex_t *buf_queue_mutex,
                             uint8_t * destBuf,
                             size_t maxlen,
                             struct ca821x_dev **pDeviceRef_out)
{
	if(pthread_mutex_lock(buf_queue_mutex) == 0)
	{
		struct buffer_queue * current = *head_buffer_queue;
		size_t len = 0;

		if(*head_buffer_queue != NULL)
		{
			*head_buffer_queue = current->next;
			len = current->len;

			if(len > maxlen) len = 0; //Invalid

			memcpy(destBuf, current->buf, len);
			*pDeviceRef_out = current->pDeviceRef;

			free(current->buf);
			free(current);
		}

		pthread_mutex_unlock(buf_queue_mutex);
		return len;
	}
	return 0;
}

//return the length of the next buffer in the queue if it exists, otherwise 0
static size_t peek_queue(struct buffer_queue * head_buffer_queue,
                             pthread_mutex_t *buf_queue_mutex)
{
	size_t in_queue = 0;

	if(pthread_mutex_lock(buf_queue_mutex) == 0)
	{
		if(head_buffer_queue != NULL)
		{
			in_queue = head_buffer_queue->len;
		}
		pthread_mutex_unlock(buf_queue_mutex);
	}
	return in_queue;
}

//return the length of the next buffer in the queue, blocking until
//it arrives. Returns length of buffer (or -1 upon error).
static size_t wait_on_queue(struct buffer_queue ** head_buffer_queue,
                            pthread_mutex_t *buf_queue_mutex,
                            pthread_cond_t *queue_cond)
{
	size_t in_queue = -1;

	if(pthread_mutex_lock(buf_queue_mutex) == 0)
	{
		do{
			if(*head_buffer_queue != NULL)
			{
				in_queue = (*head_buffer_queue)->len;
			}
			else
			{
				pthread_cond_wait(queue_cond, buf_queue_mutex);
			}
		} while(in_queue == ((size_t)-1));
		pthread_mutex_unlock(buf_queue_mutex);

	}
	return in_queue;
}
