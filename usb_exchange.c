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

#include "hidapi/hidapi/hidapi.h"
#include "usb_exchange.h"
#include "ca821x_api.h"

#define USB_VID 0x0416
#define USB_PID 0x5020

#define MAX_BUF_SIZE 256
#define MAX_FRAG_SIZE 64
#define POLL_DELAY 2

#define FRAG_LEN_MASK 0x3F
#define FRAG_LAST_MASK (1 << 7)
#define FRAG_FIRST_MASK (1 << 6)

int initialised, worker_run_flag = 0;

static pthread_t work_thread;
static pthread_mutex_t flag_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sync_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cond = PTHREAD_COND_INITIALIZER;

struct buffer_queue{
	size_t len;
	uint8_t * buf;
	struct buffer_queue * next;
};

//In queue = Device to host(us)
//Out queue = Host(us) to device
static struct buffer_queue *in_buffer_queue, *out_buffer_queue = NULL;
static pthread_mutex_t in_queue_mutex, out_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

static void add_to_queue(struct buffer_queue **head_buffer_queue,
                         pthread_mutex_t *buf_queue_mutex,
                         const uint8_t *buf, size_t len);

static size_t pop_from_queue(struct buffer_queue **head_buffer_queue,
                             pthread_mutex_t *buf_queue_mutex,
                             uint8_t * destBuf, size_t maxlen);

static size_t peek_queue(struct buffer_queue *head_buffer_queue,
                             pthread_mutex_t *buf_queue_mutex);

static int ca8210_test_int_exchange(
	const uint8_t *buf,
	size_t len,
	uint8_t *response,
	void *pDeviceRef);

//returns 1 for non-final fragment, 0 for final
static int get_next_frag(uint8_t *buf_in, uint8_t len_in, uint8_t *frag_out){
	static uint8_t offset = 0;
	int end_offset = offset + MAX_FRAG_SIZE - 1;
	uint8_t is_first = 0, is_last = 0, frag_len = 0;
	is_first = (offset == 0);

	if(end_offset >= len_in){
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
static int assemble_frags(uint8_t *frag_in, uint8_t *buf_out, uint8_t *len_out){
	static uint8_t offset = 0;
	uint8_t is_first = 0, is_last = 0, frag_len = 0;
	if(frag_in[0] == 0) frag_in = &frag_in[1]; //Fix for questionable report number existence
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
void test_frag_loopback(){
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
	}while(assemble_frags(frag_buf, data_out, &len));

	assert(!rval); //make sure both assembly and deconstruction thought this was last frag
	assert(len == data_in_size);

	rval = memcmp(data_in, data_out, len);
	assert(rval == 0);
}
#endif

static void *ca8210_test_int_worker(void *arg)
{
	hid_device *hid_dev = (hid_device *) arg;
	uint8_t buffer[MAX_BUF_SIZE];
	uint8_t frag_buf[MAX_FRAG_SIZE+1]; //+1 for report ID
	uint8_t delay;
	uint8_t len;
	int rval;

	pthread_mutex_lock(&flag_mutex);
	while(worker_run_flag)
	{
		pthread_mutex_unlock(&flag_mutex);

		//Use a nonblocking read if we are waiting to send messages
		delay = POLL_DELAY;
		if(peek_queue(out_buffer_queue, &out_queue_mutex)) delay = 0;

		//Read from the device if possible
		do{
			rval = hid_read_timeout(hid_dev, frag_buf, MAX_FRAG_SIZE, delay); //TODO: Need +1 size here for report number?
			if(rval <= 0) break;
			delay = -1;
		} while(assemble_frags(frag_buf, buffer, &len));

		if(rval > 0)
		{
			if(buffer[0] & SPI_SYN) //TODO: Take the filter byte into account
			{
				//Add to queue for synchronous processing
				add_to_queue(&in_buffer_queue, &in_queue_mutex, buffer, len);
				pthread_cond_signal(&sync_cond);
			}
			else
			{
				ca821x_downstream_dispatch(buffer, len);
			}
		}

		//Send any queued messages
		len = pop_from_queue(&out_buffer_queue, &out_queue_mutex, buffer, MAX_BUF_SIZE);
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
	struct hid_device_info *hid_ll, *hid_cur;
	hid_device *hid_dev;
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

	ca821x_api_downstream = ca8210_test_int_exchange;

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

	ca821x_api_downstream = NULL;
}


int ca8210_test_int_reset(unsigned long resettime){
	return -1;
}

static int ca8210_test_int_exchange(
	const uint8_t *buf,
	size_t len,
	uint8_t *response,
	void *pDeviceRef
)
{
	const uint8_t isSynchronous = ((buf[0] & SPI_SYN) && response);

	//Synchronous must execute synchronously
	//Get sync responses from the in queue
	//Send messages by adding them to the out queue

	if(isSynchronous) pthread_mutex_lock(&sync_mutex);

	add_to_queue(&out_buffer_queue, &out_queue_mutex, buf, len);

	if(!isSynchronous) return 0;

	while(!peek_queue(in_buffer_queue, &in_queue_mutex))
	{
		pthread_cond_wait(&sync_cond, &sync_mutex);
	}

	pop_from_queue(&in_buffer_queue, &in_queue_mutex, response, sizeof(struct MAC_Message));

	pthread_mutex_unlock(&sync_mutex);

	return 0;
}

static void add_to_queue(struct buffer_queue **head_buffer_queue,
                         pthread_mutex_t *buf_queue_mutex,
                         const uint8_t *buf, size_t len){

	pthread_mutex_lock(buf_queue_mutex);
	{
		struct buffer_queue *nextbuf = *head_buffer_queue;
		if(nextbuf == NULL){
			//queue empty -> start new queue
			*head_buffer_queue = malloc(sizeof(struct buffer_queue));
			memset(*head_buffer_queue, 0, sizeof(struct buffer_queue));
			nextbuf = *head_buffer_queue;
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

static size_t pop_from_queue(struct buffer_queue **head_buffer_queue,
                             pthread_mutex_t *buf_queue_mutex,
                             uint8_t * destBuf, size_t maxlen){

	if(pthread_mutex_lock(buf_queue_mutex) == 0){

		struct buffer_queue * current = *head_buffer_queue;
		size_t len = 0;

		if(*head_buffer_queue != NULL){
			*head_buffer_queue = current->next;
			len = current->len;

			if(len > maxlen) len = 0; //Invalid

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
