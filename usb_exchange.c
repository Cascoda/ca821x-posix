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

#include <stdio.h>
#include <pthread.h>
#include <stdint.h>

#include "hidapi/hidapi/hidapi.h"
#include "usb_exchange.h"
#include "ca821x_api.h"

#define USB_VID 0x0416
#define USB_PID 0x5020

#define MAX_BUF_SIZE 256
#define MAX_FRAG_SIZE 64
#define POLL_DELAY 2

int initialised, worker_run_flag = 0;

static pthread_t work_thread;
static pthread_mutex_t flag_mutex = PTHREAD_MUTEX_INITIALIZER;

struct buffer_queue{
	size_t len;
	uint8_t * buf;
	struct buffer_queue * next;
};

//In queue = Device to host(us)
//Out queue = Host(us) to device
static struct buffer_queue *in_buffer_queue, *out_buffer_queue = NULL;
static pthread_mutex_t in_queue_mutex, out_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

static void add_to_queue(struct buffer_queue * head_buffer_queue,
                         pthread_mutex_t *buf_queue_mutex,
                         const uint8_t *buf, size_t len);

static size_t pop_from_queue(struct buffer_queue * head_buffer_queue,
                             pthread_mutex_t *buf_queue_mutex,
                             uint8_t * destBuf, size_t maxlen);

static size_t peek_queue(struct buffer_queue * head_buffer_queue,
                             pthread_mutex_t *buf_queue_mutex)

//returns 1 for non-final fragment, 0 for final
static int get_next_frag(uint8_t *buf_in, uint8_t len_in, uint8_t *frag_out){
	static uint8_t offset = 0;
	int end_offset = offset + MAX_FRAG_SIZE;
	if(end_offset > len_in) end_offset = len_in;

	//TODO: Actually format and copy the fragment

	return !(end_offset == len_in);
}

//returns 1 for non-final fragment, 0 for final
static int assemble_frags(uint8_t *frag_in, uint8_t *buf_out, uint8_t *len_out){
	static uint8_t offset = 0;

	//TODO: Some assembly required

	return 0; //TODO: Base off the frag bits
}

static void *ca8210_test_int_worker(void *arg)
{
	hid_device *hid_dev = (hid_device *) arg;
	uint8_t buffer[MAX_BUF_SIZE];
	uint8_t frag_buf[MAX_FRAG_SIZE];
	uint8_t delay;
	uint8_t len;
	int rval;

	pthread_mutex_lock(&flag_mutex);
	while(worker_run_flag)
	{
		pthread_mutex_unlock(&flag_mutex);

		//Use a nonblocking read if we are waiting to send messages
		delay = POLL_DELAY;
		if(peek_queue(out_buffer_queue, out_queue_mutex)) delay = 0;

		//Read from the device if possible
		do{
			rval = hid_read_timeout(hid_dev, frag_buf, MAX_FRAG_SIZE, delay); //TODO: Need +1 size here for report number?
			if(rval <= 0) break;
			delay = -1;
		} while(assemble_frags(frag_buf, buffer, &len));

		if(buffer[0] & SPI_SYN) //TODO: Take the filter byte into account
		{
			//Add to queue for synchronous processing
			add_to_queue(in_buffer_queue, in_queue_mutex, buffer, len);
		}
		else
		{
			ca821x_downstream_dispatch(buffer, len);
		}

		//Send any queued messages
		len = pop_from_queue(out_buffer_queue, out_queue_mutex, buffer, MAX_BUF_SIZE);
		if (len <= 0) continue;

		do{
			rval = get_next_frag(buffer, len, frag_buf);
			hid_write(hid_dev, frag_buf, MAX_FRAG_SIZE);	//TODO: Catch error
		} while(rval);

		pthread_mutex_lock(&flag_mutex);
	}

	hid_close(hid_dev);
	return 0;
}

int usb_exchange_init(void){
	return usb_exchange_init_withhandler(NULL);
}

int usb_exchange_init_withhandler(usb_exchange_errorhandler callback){
	struct hid_device_info *hid_ll, *hid_cur = NULL;
	hid_device *hid_dev = NULL;
	int rval, error = 0;
	int count = 0;

	if(initialised) return 1;

	hid_ll = hid_enumerate(USB_VID, USB_PID);
	if(!hid_ll){
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

	//For now, just use the first
	hid_dev = hid_open_path(hid_ll->path);
	if(!hid_dev){
		error = -1;
		goto exit;
	}

	worker_run_flag = 1;
	rval = pthread_create(&work_thread, NULL, &ca8210_test_int_worker, (void *) hid_dev);
	if(rval != 0)
	{
		worker_run_flag = 0;
		hid_close(hid_dev);
		error = -1;
		goto exit;
	}

	initialised = 1;

exit:
	hid_free_enumeration(hid_ll);
	return error;
}


void usb_exchange_deinit(void){
	pthread_mutex_lock(&flag_mutex);
	worker_run_flag = 0;
	pthread_mutex_unlock(&flag_mutex);
	//TODO: Should probably wait for the worker to actually complete here
}


int ca8210_test_int_reset(unsigned long resettime){
	return -1;
}

static void add_to_queue(struct buffer_queue * head_buffer_queue,
                         pthread_mutex_t *buf_queue_mutex,
                         const uint8_t *buf, size_t len){

	pthread_mutex_lock(buf_queue_mutex);
	{
		struct buffer_queue * nextbuf = head_buffer_queue;
		if(nextbuf == NULL){
			//queue empty -> start new queue
			head_buffer_queue = malloc(sizeof(struct buffer_queue));
			memset(head_buffer_queue, 0, sizeof(struct buffer_queue));
			nextbuf = head_buffer_queue;
		}
		else{
			while(nextbuf->next != NULL){
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

	}
	pthread_mutex_unlock(buf_queue_mutex);
}

static size_t pop_from_queue(struct buffer_queue * head_buffer_queue,
                             pthread_mutex_t *buf_queue_mutex,
                             uint8_t * destBuf, size_t maxlen){

	if(pthread_mutex_lock(buf_queue_mutex) == 0){

		struct buffer_queue * current = head_buffer_queue;
		size_t len = 0;

		if(head_buffer_queue != NULL){
			head_buffer_queue = current->next;
			len = current->len;

			if(len > maxlen) len = 0;

			memcpy(destBuf, current->buf, len);

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
                             pthread_mutex_t *buf_queue_mutex){
	size_t in_queue = 0;

	if(pthread_mutex_lock(buf_queue_mutex) == 0){
		if(head_buffer_queue != NULL){
			in_queue = head_buffer_queue->len;
		}
		pthread_mutex_unlock(buf_queue_mutex);
	}
	return in_queue;

}
