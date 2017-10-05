/*
 * Copyright (c) 2017, Cascoda
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

#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

#include "ca821x-generic-exchange.h"
#include "ca821x-queue.h"
#include "ca821x_api.h"

int s_worker_run_flag = 0;
int s_generic_initialised = 0;

pthread_mutex_t flag_mutex = PTHREAD_MUTEX_INITIALIZER;

struct buffer_queue *downstream_dispatch_queue = NULL;
pthread_mutex_t downstream_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t dd_thread;
pthread_cond_t dd_cond = PTHREAD_COND_INITIALIZER;

static void *ca821x_downstream_dispatch_worker(void *arg)
{
	struct ca821x_dev *pDeviceRef;
	struct ca821x_exchange_base *priv;

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

int init_generic_statics()
{
	int rval, error = 0;

	if (s_generic_initialised) goto exit;

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

	s_generic_initialised = 1;

exit:
	return error;
}

int deinit_generic_statics()
{
	if (!s_generic_initialised) goto exit;

	s_generic_initialised = 0;
	pthread_mutex_lock(&flag_mutex);
	s_worker_run_flag = 0;
	pthread_mutex_unlock(&flag_mutex);

	//Wake the downstream dispatch thread up so that it dies cleanly
	add_to_waiting_queue(&downstream_dispatch_queue, &downstream_queue_mutex,
	                     &dd_cond,
	                     NULL, 0, NULL);

exit:
	return 0;
}

int exchange_register_user_callback(exchange_user_callback callback,
                                    struct ca821x_dev *pDeviceRef)
{
	struct ca821x_exchange_base *priv = pDeviceRef->exchange_context;

	if (priv->user_callback) return -1;

	priv->user_callback = callback;

	return 0;
}

int exchange_handle_error(int error, struct ca821x_exchange_base *priv)
{
	if (priv->error_callback)
	{
		return priv->error_callback(error);
	}
	else
	{
		abort();
	}
	return 0;
}

void *ca8210_io_worker(void *arg)
{
	struct ca821x_dev *pDeviceRef = arg;
	struct ca821x_exchange_base *priv = pDeviceRef->exchange_context;
	uint8_t buffer[MAX_BUF_SIZE];
	ssize_t len;
	int error = 0;

	priv->flush_func(pDeviceRef);

	pthread_mutex_lock(&flag_mutex);
	while (s_worker_run_flag && priv->io_thread_runflag)
	{
		pthread_mutex_unlock(&flag_mutex);

		len = priv->read_func(pDeviceRef, buffer);
		if (len > 0)
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
				                     &dd_cond,
				                     buffer, len, pDeviceRef);
			}
		}
		else if (len < 0)
		{
			exchange_handle_error(len, priv);
		}

		//Send any queued messages
		len = pop_from_queue(&(priv->out_buffer_queue),
		                     &(priv->out_queue_mutex),
		                     buffer,
		                     MAX_BUF_SIZE, &pDeviceRef);

		if (len > 0)
		{
			error = priv->write_func(buffer, len, pDeviceRef);
			if (error < 0)
			{
				exchange_handle_error(error, priv);
			}
		}

		pthread_mutex_lock(&flag_mutex);
	}

	pthread_mutex_unlock(&flag_mutex);
	return 0;
}
