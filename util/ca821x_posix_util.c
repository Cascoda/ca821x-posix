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

#include "../ca821x-posix.h"

int ca821x_util_init(struct ca821x_dev *pDeviceRef,
                     ca821x_errorhandler errorHandler)
{
	int error = 0;
	enum ca821x_exchange_type ex_type;
	struct ca821x_exchange_base *base;
	error = ca821x_api_init(pDeviceRef);
	if(error) goto exit;

	error = kernel_exchange_init_withhandler(errorHandler, pDeviceRef);
	ex_type = ca821x_exchange_kernel;
	if(error)
	{
		error = usb_exchange_init_withhandler(errorHandler, pDeviceRef);
		ex_type = ca821x_exchange_usb;
	}
	if(error) goto exit;

	base = pDeviceRef->exchange_context;
	base->exchange_type = ex_type;
exit:
	return error;
}

void ca821x_util_deinit(struct ca821x_dev *pDeviceRef)
{
	struct ca821x_exchange_base *base = pDeviceRef->exchange_context;

	if(base == NULL) return;

	switch (base->exchange_type)
	{
	case ca821x_exchange_kernel:
		kernel_exchange_deinit(pDeviceRef);
		break;
	case ca821x_exchange_usb:
		usb_exchange_deinit(pDeviceRef);
		break;
	}
}

int ca821x_util_reset(struct ca821x_dev *pDeviceRef)
{
	struct ca821x_exchange_base *base = pDeviceRef->exchange_context;
	int error = -1;

	if(base == NULL) return -1;

	switch (base->exchange_type)
	{
	case ca821x_exchange_kernel:
		error = kernel_exchange_reset(1, pDeviceRef);
		break;
	case ca821x_exchange_usb:
		error = usb_exchange_reset(1, pDeviceRef);
		break;
	}

	return error;
}
