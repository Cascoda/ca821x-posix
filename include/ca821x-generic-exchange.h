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

#ifndef CA821X_GENERIC_EXCHANGE_H
#define CA821X_GENERIC_EXCHANGE_H

#include "ca821x-types.h"

#define MAX_BUF_SIZE 189

extern int s_worker_run_flag;
extern int s_generic_initialised;

extern pthread_mutex_t flag_mutex;

extern struct buffer_queue *downstream_dispatch_queue;
extern pthread_mutex_t downstream_queue_mutex;
extern pthread_cond_t dd_cond;

extern void (*wake_hw_worker)(void);

int init_generic_statics();
int deinit_generic_statics();
int exchange_register_user_callback(exchange_user_callback callback,
                                    struct ca821x_dev *pDeviceRef);
int exchange_handle_error(int error, struct ca821x_exchange_base *priv);
void *ca8210_io_worker(void *arg);
int ca8210_exchange_commands(const uint8_t *buf,
                             size_t len,
                             uint8_t *response,
                             struct ca821x_dev *pDeviceRef);

#endif