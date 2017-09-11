#ifndef CA821X_POSIX_H
#define CA821X_POSIX_H 1

#include "ca821x-api/include/ca821x_api.h"
#include "usb-exchange/usb_exchange.h"
#include "kernel-exchange/kernel_exchange.h"

typedef int (*ca821x_errorhandler)(
	int error_number
);

int ca821x_initialise_and_open(struct ca821x_dev *pDeviceRef,
                               ca821x_errorhandler errorHandler);

#endif //CA821X_POSIX_H
