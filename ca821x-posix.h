#ifndef CA821X_POSIX_H
#define CA821X_POSIX_H 1

#include "ca821x-api/include/ca821x_api.h"
#include "usb-exchange/usb_exchange.h"
#include "kernel-exchange/kernel_exchange.h"
#include "ca821x-types.h"

/**
 * Generic function to initialise an available ca821x device. This includes
 * initialisation of the api and an exchange. Use of these generic functions
 * over using a specific exchange allows more flexibility.
 *
 * Calling twice on the same pDeviceRef without a deinit produces undefined
 * behaviour.
 *
 * @param[in]   pDeviceRef   Device reference to be initialised. Must point to
 *                           allocated memory, but does not have to be
 *                           initialised. The memory is cleared and initialised
 *                           internally.
 *
 * @param[in]   errorHandler A function pointer to an error handling function.
 *                           This callback will be triggered in the event of an
 *                           unrecoverable error.
 *
 * @returns 0 for success, -1 for error
 *
 */
int ca821x_util_init(struct ca821x_dev *pDeviceRef,
                         ca821x_errorhandler errorHandler);

/**
 * Generic function to deinitialise an initialised ca821x device. This will
 * free any resources that were allocated by ca821x_util_init.
 *
 * Calling on an uninitialised pDeviceRef produces undefined behaviour.
 *
 * @param[in]   pDeviceRef   Device reference to be deinitialised.
 *
 * @returns 0 for success, -1 for error
 *
 */
void ca821x_util_deinit(struct ca821x_dev *pDeviceRef);

/**
 * Generic function to attempt a hard reset of the ca821x chip.
 *
 * Calling on an uninitialised pDeviceRef produces undefined behaviour.
 *
 * @param[in]   pDeviceRef   Device reference for device to be reset.
 *
 * @returns 0 for success, -1 for error
 *
 */
int ca821x_util_reset(struct ca821x_dev *pDeviceRef);

#endif //CA821X_POSIX_H
