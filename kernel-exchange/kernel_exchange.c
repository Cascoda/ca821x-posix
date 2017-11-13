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
#include "ca821x-queue.h"
#include "ca821x-generic-exchange.h"

/******************************************************************************/

#define DebugFSMount            "/sys/kernel/debug"
#define DriverNode              "/ca8210"
#define DriverFilePath          (DebugFSMount DriverNode)

#define CA8210_IOCTL_HARD_RESET (0)

/** Max time to wait on rx data in seconds */
#define POLL_DELAY 1

/******************************************************************************/

static int DriverFileDescriptor, DriverFDPipe[2];
static pthread_mutex_t tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_initialised = 0;
static fd_set rx_block_fd_set;

/******************************************************************************/

struct kernel_exchange_priv
{
	struct ca821x_exchange_base base;
};

/******************************************************************************/

static int ca8210_test_int_write(const uint8_t *buf,
                                 size_t len,
                                 struct ca821x_dev *pDeviceRef)
{
	int remaining = len;
	int attempts = 0;

	pthread_mutex_lock(&tx_mutex);
	do {
		int returnvalue;

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

ssize_t kernel_exchange_try_read(struct ca821x_dev *pDeviceRef,
                                 uint8_t *buf)
{
	struct kernel_exchange_priv *priv = pDeviceRef->exchange_context;
	struct timeval timeout;

	if (!peek_queue(priv->base.out_buffer_queue, &(priv->base.out_queue_mutex)))
	{
		uint8_t dummybyte = 0;

		timeout.tv_sec = POLL_DELAY;
		timeout.tv_usec = 0;
		select(DriverFDPipe[0] + 1, &rx_block_fd_set, NULL, NULL, &timeout);
		read(DriverFDPipe[0], &dummybyte, 1);
	}

	//Read from the device if possible
	return read(DriverFileDescriptor, buf, 0);
}

void flush_unread_ke(struct ca821x_dev *pDeviceRef)
{
	uint8_t buffer[MAX_BUF_SIZE];
	ssize_t rval;
	do
	{
		rval = read(DriverFileDescriptor, buffer, 0);
	} while (rval > 0);
}

void unblock_read(struct ca821x_dev *pDeviceRef)
{
	const uint8_t dummybyte = 0;
	write(DriverFDPipe[1], &dummybyte, 1);
}

static int init_statics()
{
	int error = 0;

	DriverFileDescriptor = -1;

	error = init_generic_statics();
	if (error) goto exit;

	s_initialised = 1;

exit:
	return error;
}

static int deinit_statics()
{
	s_initialised = 0;
	return deinit_generic_statics();
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
	fcntl(DriverFDPipe[0], F_SETFL, O_NONBLOCK);
	FD_ZERO(&rx_block_fd_set);
	FD_SET(DriverFileDescriptor, &rx_block_fd_set);
	FD_SET(DriverFDPipe[0], &rx_block_fd_set);

	pDeviceRef->exchange_context = calloc(1, sizeof(struct kernel_exchange_priv));
	priv = pDeviceRef->exchange_context;
	priv->base.exchange_type = ca821x_exchange_kernel;
	priv->base.error_callback = callback;
	priv->base.write_func = ca8210_test_int_write;
	priv->base.signal_func = unblock_read;
	priv->base.read_func = kernel_exchange_try_read;
	priv->base.flush_func = flush_unread_ke;

	pthread_mutex_init(&(priv->base.sync_mutex), NULL);
	pthread_mutex_init(&(priv->base.in_queue_mutex), NULL);
	pthread_mutex_init(&(priv->base.out_queue_mutex), NULL);
	pthread_cond_init(&(priv->base.sync_cond), NULL);

	pthread_mutex_lock(&flag_mutex);
	priv->base.io_thread_runflag = 1;
	pthread_mutex_unlock(&flag_mutex);

	error = pthread_create(&(priv->base.io_thread),
	                       NULL,
	                       &ca8210_io_worker,
	                       pDeviceRef);

	if (error != 0)
	{
		error = -1;
		goto exit;
	}

	pDeviceRef->ca821x_api_downstream = ca8210_exchange_commands;

exit:
	if (error && pDeviceRef->exchange_context)
	{
		free(pDeviceRef->exchange_context);
		pDeviceRef->exchange_context = NULL;
	}
	return error;
}

void kernel_exchange_deinit(struct ca821x_dev *pDeviceRef){
	int ret;

	deinit_statics();

	//Lock all mutexes
	pthread_mutex_lock(&tx_mutex);

	//close the driver file
	do{
		ret = close(DriverFileDescriptor);
	} while(ret < 0 && errno == EINTR);
	s_initialised = 0;
	free(pDeviceRef->exchange_context);
	pDeviceRef->exchange_context = NULL;

	//unlock all mutexes
	pthread_mutex_unlock(&tx_mutex);
}

int kernel_exchange_reset(unsigned long resettime, struct ca821x_dev *pDeviceRef)
{
	return ioctl(DriverFileDescriptor, CA8210_IOCTL_HARD_RESET, resettime);
}
