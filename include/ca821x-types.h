/*
 * ca821x-types.h
 *
 *  Created on: 11 Sep 2017
 *      Author: ciaran
 */

#ifndef CA821X_TYPES_H_
#define CA821X_TYPES_H_

#include <pthread.h>
#include <unistd.h>

struct ca821x_dev;

/**
 * \brief Error callback
 *
 * Optional callback for the application layer
 * to handle any chip errors which would otherwise
 * cause a crash.
 */
typedef int (*ca821x_errorhandler)(
	int error_number
);

/* Optional callback for the application layer
 * to handle any non-ca821x communication with
 * a device over the same protocol. Any
 * command IDs which are not recognised as
 * a valid ca821x SPI command will be passed
 * to this callback.
 */
typedef int (*exchange_user_callback)(
	const uint8_t *buf, size_t len, void *pDeviceRef
);

typedef int (*exchange_write)(
	const uint8_t *buf, size_t len, struct ca821x_dev *pDeviceRef
);

typedef ssize_t (*exchange_read)(
	struct ca821x_dev *pDeviceRef, uint8_t *buf
);

typedef void (*exchange_flush_unread)(
	struct ca821x_dev *pDeviceRef
);

/** Enumeration for identifying the underlying exchange interface type */
enum ca821x_exchange_type {
	ca821x_exchange_kernel = 1, //!< kernel driver's debugfs node
	ca821x_exchange_usb //!< USB HID device
};

/** Base structure for exchange private data collections */
struct ca821x_exchange_base {
	enum ca821x_exchange_type exchange_type;

	ca821x_errorhandler error_callback;
	exchange_user_callback user_callback;
	exchange_write write_func;
	exchange_read read_func;
	exchange_flush_unread flush_func;

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

/** Single index in a singly-linked list of data buffers */
struct buffer_queue
{
	size_t len; //!< Length of buffer
	uint8_t * buf; //!< Buffer pointer
	struct ca821x_dev *pDeviceRef; //!< Data's target/originating device
	struct buffer_queue * next; //!< Next queue item
};

#endif /* CA821X_TYPES_H_ */
