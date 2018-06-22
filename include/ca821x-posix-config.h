/*
 * ca821x-posix-config.h
 *
 *  Created on: 22 Jun 2018
 *      Author: ciaran
 */

#ifndef CA821X_POSIX_CA821X_POSIX_CONFIG_H_
#define CA821X_POSIX_CA821X_POSIX_CONFIG_H_

/*
 * POSIX_ASYNC_DISPATCH enables the asynchronous, automatic calling of the
 * callback functions from a secondary thread. If synchronous polling of
 * indications is required, then ca821x_util_dispatch_poll should
 * be regularly called from a polling loop.
 */
#ifndef POSIX_ASYNC_DISPATCH
#define POSIX_ASYNC_DISPATCH 1
#endif

#endif /* CA821X_POSIX_CA821X_POSIX_CONFIG_H_ */
