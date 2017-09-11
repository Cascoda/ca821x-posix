/*
 * ca821x-types.h
 *
 *  Created on: 11 Sep 2017
 *      Author: ciaran
 */

#ifndef CA821X_TYPES_H_
#define CA821X_TYPES_H_

/* Optional callback for the application layer
 * to handle any chip errors which would otherwise
 * cause a crash.
 */
typedef int (*ca821x_errorhandler)(
	int error_number
);

enum ca821x_exchange_type {
	ca821x_exchange_kernel = 1,
	ca821x_exchange_usb
};

struct ca821x_exchange_base {
	enum ca821x_exchange_type exchange_type;
};

#endif /* CA821X_TYPES_H_ */
