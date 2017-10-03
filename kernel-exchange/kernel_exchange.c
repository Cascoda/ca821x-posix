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

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/ioctl.h>

#include "kernel_exchange.h"
#include "ca821x-posix.h"

/******************************************************************************/

#define DebugFSMount            "/sys/kernel/debug"
#define DriverNode              "/ca8210"
#define DriverFilePath 			(DebugFSMount DriverNode)

#define CA8210_IOCTL_HARD_RESET (0)

/******************************************************************************/

static int ca8210_test_int_exchange(
	const uint8_t *buf,
	size_t len,
	uint8_t *response,
	struct ca821x_dev *pDeviceRef
);

/******************************************************************************/

static int DriverFileDescriptor, DriverFDPipe[2];
static pthread_mutex_t flag_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rx_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t buf_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t unhandled_sync_cond = PTHREAD_COND_INITIALIZER;
static int unhandled_sync_count = 0;
static int s_worker_run_flag = 0;
static int s_initialised = 0;
static fd_set rx_block_fd_set;

static struct buffer_queue *downstream_dispatch_queue;
static pthread_mutex_t downstream_queue_mutex;
static pthread_t dd_thread;
static pthread_cond_t dd_cond = PTHREAD_COND_INITIALIZER;

/******************************************************************************/

struct kernel_exchange_priv
{
	struct ca821x_exchange_base base;
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

/******************************************************************************/

static int ca8210_test_int_write(const uint8_t *, size_t);

static void *ca821x_downstream_dispatch_worker(void *arg)
{
	struct ca821x_dev *pDeviceRef;
	struct kernel_exchange_priv *priv;

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
	struct kernel_exchange_priv *priv = pDeviceRef->exchange_context;
	uint8_t buffer[MAX_BUF_SIZE];
	ssize_t rx_len;
	int error = 0;
	struct timeval timeout;

	do
	{
		rx_len = read(DriverFileDescriptor, buffer, 0);
	} while (rx_len > 0);

	pthread_mutex_lock(&flag_mutex);
	while (s_worker_run_flag && priv->io_thread_runflag)
	{
		pthread_mutex_unlock(&flag_mutex);

		if (!peek_queue(priv->out_buffer_queue, &(priv->out_queue_mutex)))
		{
			timeout.tv_sec = 1;
			timeout.tv_usec = 0;
			select(DriverFDPipe[0] + 1, &rx_block_fd_set, NULL, NULL, &timeout);
		}

		//Read from the device if possible
		rx_len = read(DriverFileDescriptor, buffer, 0);

		if (rx_len > 0)
		{
			if (buffer[0] & SPI_SYN)
			{
				//Add to queue for synchronous processing
				add_to_waiting_queue(&(priv->in_buffer_queue),
				                     &(priv->in_queue_mutex),
				                     &(priv->sync_cond),
				                     buffer, rx_len, pDeviceRef);
			}
			else
			{
				//Add to queue for dispatching downstream
				add_to_waiting_queue(&downstream_dispatch_queue,
				                     &downstream_queue_mutex,
				                     &dd_cond, buffer, rx_len, pDeviceRef);
			}
		}

		//Send any queued messages
		rx_len = pop_from_queue(&(priv->out_buffer_queue),
		                     &(priv->out_queue_mutex),
		                     buffer,
		                     MAX_BUF_SIZE, &pDeviceRef);

		if (rx_len > 0)
		{
			priv = pDeviceRef->exchange_context;
			error = ca8210_test_int_write(buffer, rx_len);
		}

		if (error < 0)
		{
			if (priv->error_callback)
			{
				priv->error_callback(error);
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

static int init_statics()
{
	int rval, error = 0;

	s_worker_run_flag = 1;
	DriverFileDescriptor = -1;
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

	return 0;
}

int kernel_exchange_init(struct ca821x_dev *pDeviceRef){
	return kernel_exchange_init_withhandler(NULL, pDeviceRef);
}

int kernel_exchange_init_withhandler(ca821x_errorhandler callback,
                                     struct ca821x_dev *pDeviceRef)
{
	int error;
	struct kernel_exchange_priv *priv = NULL;

	if (!s_initialised)
	{
		error = init_statics();
		if (error) return error;
	}

	if(pDeviceRef->exchange_context) return 1;

	if (DriverFileDescriptor != -1) return 1;

	DriverFileDescriptor = open(DriverFilePath, O_RDWR | O_NONBLOCK);

	if (DriverFileDescriptor == -1) {
		return -1;
	}

	pipe(DriverFDPipe);
	FD_ZERO(&rx_block_fd_set);
	FD_SET(DriverFileDescriptor, &rx_block_fd_set);
	FD_SET(DriverFDPipe[0], &rx_block_fd_set);

	pDeviceRef->exchange_context = calloc(1, sizeof(struct kernel_exchange_priv));
	priv = pDeviceRef->exchange_context;
	priv->base.exchange_type = ca821x_exchange_kernel;
	priv->error_callback = callback;

	pthread_mutex_init(&(priv->sync_mutex), NULL);
	pthread_mutex_init(&(priv->in_queue_mutex), NULL);
	pthread_mutex_init(&(priv->out_queue_mutex), NULL);
	pthread_cond_init(&(priv->sync_cond), NULL);

	unhandled_sync_count = 0;

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

exit:
	if (error && pDeviceRef->exchange_context)
	{
		free(pDeviceRef->exchange_context);
		pDeviceRef->exchange_context = NULL;
	}
	return error;
}

int kernel_exchange_register_user_callback(kernel_exchange_user_callback callback,
                                           struct ca821x_dev *pDeviceRef)
{
	struct kernel_exchange_priv *priv = pDeviceRef->exchange_context;

	if (priv->user_callback) return -1;

	priv->user_callback = callback;

	return 0;
}

void kernel_exchange_deinit(struct ca821x_dev *pDeviceRef){
	int ret;

	deinit_statics();

	//Lock all mutexes
	pthread_mutex_lock(&tx_mutex);
	pthread_mutex_lock(&rx_mutex);
	pthread_mutex_lock(&buf_queue_mutex);

	//close the driver file
	do{
		ret = close(DriverFileDescriptor);
	} while(ret < 0 && errno == EINTR);
	s_initialised = 0;
	free(pDeviceRef->exchange_context);
	pDeviceRef->exchange_context = NULL;

	//unlock all mutexes
	pthread_mutex_unlock(&buf_queue_mutex);
	pthread_mutex_unlock(&rx_mutex);
	pthread_mutex_unlock(&tx_mutex);
}

int kernel_exchange_reset(unsigned long resettime, struct ca821x_dev *pDeviceRef)
{
	return ioctl(DriverFileDescriptor, CA8210_IOCTL_HARD_RESET, resettime);
}

static int ca8210_test_int_write(const uint8_t *buf, size_t len)
{
	int returnvalue, remaining = len;
	int attempts = 0;

	pthread_mutex_lock(&tx_mutex);
	do {
		returnvalue = write(DriverFileDescriptor, buf+len-remaining, remaining);
		if (returnvalue > 0)
			remaining -= returnvalue;

		if(returnvalue == -1){
			int error = errno;

			if(errno == EAGAIN){	//If the error is that the device is busy, try again after a short wait
				if(attempts++ < 5){
					struct timespec toSleep;
					toSleep.tv_sec = 0;
					toSleep.tv_nsec = 50*1000000;
					nanosleep(&toSleep, NULL);	//Sleep for ~50ms
					continue;
				}
			}

			pthread_mutex_unlock(&tx_mutex);
			return error;
		}

	} while (remaining > 0);

	pthread_mutex_unlock(&tx_mutex);
	return 0;
}

static int ca8210_test_int_exchange(
	const uint8_t *buf,
	size_t len,
	uint8_t *response,
	struct ca821x_dev *pDeviceRef
)
{
	const uint8_t isSynchronous = ((buf[0] & SPI_SYN) && response);
	struct kernel_exchange_priv *priv = pDeviceRef->exchange_context;
	struct ca821x_dev *ref_out;
	const uint8_t dummybyte = 0;

	if (!s_initialised) return -1;

	if(isSynchronous){
		pthread_mutex_lock(&(priv->sync_mutex));	//Enforce synchronous write then read
		while(unhandled_sync_count != 0) {pthread_cond_wait(&unhandled_sync_cond, &(priv->sync_mutex));}
	}
	else if(buf[0] & SPI_SYN){
		pthread_mutex_lock(&(priv->sync_mutex));
		unhandled_sync_count++;
		pthread_mutex_unlock(&(priv->sync_mutex));
	}

	add_to_queue(&(priv->out_buffer_queue),
	             &(priv->out_queue_mutex),
	             buf,
	             len,
	             pDeviceRef);
	write(DriverFDPipe[1], &dummybyte, 1);

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
