/*
 * Copyright (c) 2019 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Sample echo app for CDC ACM class
 *
 * Sample app for USB CDC ACM class driver. The received data is echoed back
 * to the serial port.
 */

#include <sample_usbd.h>

#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(cdc_acm_echo, LOG_LEVEL_INF);

const struct device *const uart_dev = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);

#define STRIP_NODE		DT_ALIAS(led_strip)
static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
#if DT_NODE_HAS_PROP(DT_ALIAS(led_strip), chain_length)
#define STRIP_NUM_PIXELS	DT_PROP(DT_ALIAS(led_strip), chain_length)
#else
#error Unable to determine length of LED strip
#endif
static struct led_rgb pixels[STRIP_NUM_PIXELS];
#define RGB(_r, _g, _b) { .r = (_r), .g = (_g), .b = (_b) }
static const struct led_rgb colors[] = {
	RGB(0x0f, 0x00, 0x00), /* red */
	RGB(0x00, 0x0f, 0x00), /* green */
	RGB(0x00, 0x00, 0x0f), /* blue */
};


#define UART1_DEV

static const struct device *const uart1 = DEVICE_DT_GET(DT_ALIAS(uart1));
static const struct device *const uart2 = DEVICE_DT_GET(DT_ALIAS(uart2));


#define RING_BUF_SIZE 1024
uint8_t ring_buffer[RING_BUF_SIZE];


#define RX_RING_BUF_SIZE 1024
uint8_t rx_ring_buffer1[RX_RING_BUF_SIZE];
uint8_t rx_ring_buffer2[RX_RING_BUF_SIZE];

struct rx_data {
	uint8_t idx;
	uint8_t *buffer;
	uint16_t size;
	uint16_t pos;
};

struct rx_data rx_data1 = {
	.idx = 1,
	.buffer = rx_ring_buffer1,
	.size = RX_RING_BUF_SIZE,
	.pos = 0,
};

struct rx_data rx_data2 = {
	.idx = 2,
	.buffer = rx_ring_buffer2,
	.size = RX_RING_BUF_SIZE,
	.pos = 0,
};


struct ring_buf ringbuf;

static bool rx_throttled;

static inline void print_baudrate(const struct device *dev)
{
	uint32_t baudrate;
	int ret;

	ret = uart_line_ctrl_get(dev, UART_LINE_CTRL_BAUD_RATE, &baudrate);
	if (ret) {
		LOG_WRN("Failed to get baudrate, ret code %d", ret);
	} else {
		LOG_INF("Baudrate %u", baudrate);
	}
}

#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
static struct usbd_context *sample_usbd;
K_SEM_DEFINE(dtr_sem, 0, 1);

static void sample_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	LOG_INF("USBD message: %s", usbd_msg_type_string(msg->type));

	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			if (usbd_enable(ctx)) {
				LOG_ERR("Failed to enable device support");
			}
		}

		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			if (usbd_disable(ctx)) {
				LOG_ERR("Failed to disable device support");
			}
		}
	}

	if (msg->type == USBD_MSG_CDC_ACM_CONTROL_LINE_STATE) {
		uint32_t dtr = 0U;

		uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_DTR, &dtr);
		if (dtr) {
			k_sem_give(&dtr_sem);
		}
	}

	if (msg->type == USBD_MSG_CDC_ACM_LINE_CODING) {
		print_baudrate(msg->dev);
	}
}

static int enable_usb_device_next(void)
{
	int err;

	sample_usbd = sample_usbd_init_device(sample_msg_cb);
	if (sample_usbd == NULL) {
		LOG_ERR("Failed to initialize USB device");
		return -ENODEV;
	}

	if (!usbd_can_detect_vbus(sample_usbd)) {
		err = usbd_enable(sample_usbd);
		if (err) {
			LOG_ERR("Failed to enable device support");
			return err;
		}
	}

	LOG_INF("USB device support enabled");

	return 0;
}
#endif /* defined(CONFIG_USB_DEVICE_STACK_NEXT) */

static void interrupt_handler(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (!rx_throttled && uart_irq_rx_ready(dev)) {
			int recv_len, rb_len;
			uint8_t buffer[64];
			size_t len = MIN(ring_buf_space_get(&ringbuf),
					 sizeof(buffer));

			if (len == 0) {
				/* Throttle because ring buffer is full */
				uart_irq_rx_disable(dev);
				rx_throttled = true;
				continue;
			}

			recv_len = uart_fifo_read(dev, buffer, len);
			if (recv_len < 0) {
				LOG_ERR("Failed to read UART FIFO");
				recv_len = 0;
			};

			rb_len = ring_buf_put(&ringbuf, buffer, recv_len);
			if (rb_len < recv_len) {
				LOG_ERR("Drop %u bytes", recv_len - rb_len);
			}

			LOG_DBG("tty fifo -> ringbuf %d bytes", rb_len);
			if (rb_len) {
				uart_irq_tx_enable(dev);
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t buffer[64];
			int rb_len, send_len;

			rb_len = ring_buf_get(&ringbuf, buffer, sizeof(buffer));
			if (!rb_len) {
				LOG_DBG("Ring buffer empty, disable TX IRQ");
				uart_irq_tx_disable(dev);
				continue;
			}

			if (rx_throttled) {
				uart_irq_rx_enable(dev);
				rx_throttled = false;
			}

			send_len = uart_fifo_fill(dev, buffer, rb_len);
			if (send_len < rb_len) {
				LOG_ERR("Drop %d bytes", rb_len - send_len);
			}

			LOG_DBG("ringbuf -> tty fifo %d bytes", send_len);
		}
	}
}


/*
 * Read characters from UART until line end is detected. Afterwards push the
 * data to the message queue.
 */
void serial_cb(const struct device *dev, void *user_data)
{
	uint8_t c;

	struct rx_data *rx_data = (struct rx_data *)user_data;

	if (!uart_irq_update(dev)) {
		return;
	}
	
	if (!uart_irq_rx_ready(dev)) {
		return;
	}

	/* read until FIFO empty */
	while (uart_fifo_read(dev, &c, 1) == 1) {
		// LOG_ERR("%d/%d/0x%02x", rx_data->idx, rx_data->pos, c);
		if( (c == '\n') || (c == '\r') ) {
			if(rx_data->pos > 0) {
				rx_data->buffer[rx_data->pos] = '\0';
				if( rx_data->idx == 1 ) {
					LOG_INF("%d:%s", rx_data->idx, rx_data->buffer);
				} else {
					LOG_ERR("%d:%s", rx_data->idx, rx_data->buffer);
				}
				rx_data->pos = 0;
			}
		} else if( rx_data->pos < (RX_RING_BUF_SIZE - 2) ) {
			rx_data->buffer[rx_data->pos++] = c;
			// rx_buf[rx_buf_pos++] = c;
		}
		// char buffer[64];
		// buffer[0] = c;
		// /*rb_len = */ring_buf_put(&ringbuf, buffer, 1);
	}
}

int main(void)
{
	int ret;


	/* hardware UARTs */
	if (!device_is_ready(uart1)) {
		LOG_ERR("UART1 device not ready");
		return 0;
	}
	ret = uart_irq_callback_user_data_set(uart1, serial_cb, (void *)&rx_data1);
	if (ret) {
		LOG_ERR("Failed to set IRQ callback");
		return 0;
	}
	uart_irq_rx_enable(uart1);


	if (!device_is_ready(uart2)) {
		LOG_ERR("UART2 device not ready");
		return 0;
	}
	ret = uart_irq_callback_user_data_set(uart2, serial_cb, (void *)&rx_data2);
	if (ret) {
		LOG_ERR("Failed to set IRQ callback");
		return 0;
	}
	uart_irq_rx_enable(uart2);


	/* USB UART */
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("CDC ACM device not ready");
		return 0;
	}

#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
		ret = enable_usb_device_next();
#else
		ret = usb_enable(NULL);
#endif

	if (ret != 0) {
		LOG_ERR("Failed to enable USB");
		return 0;
	}

	ring_buf_init(&ringbuf, sizeof(ring_buffer), ring_buffer);


	if (device_is_ready(strip)) {
		LOG_INF("Found LED strip device %s. %d pixels", strip->name, STRIP_NUM_PIXELS);
	} else {
		LOG_ERR("LED strip device %s is not ready", strip->name);
		return 0;
	}

	memset(&pixels, 0x00, sizeof(pixels));
	memcpy(&pixels[0], &colors[0], sizeof(struct led_rgb));
	led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);

	LOG_INF("Wait for DTR");

#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
	k_sem_take(&dtr_sem, K_FOREVER);
#else
	while (true) {
		uint32_t dtr = 0U;

		uart_line_ctrl_get(uart_dev, UART_LINE_CTRL_DTR, &dtr);
		if (dtr) {
			break;
		} else {
			/* Give CPU resources to low priority threads. */
			k_sleep(K_MSEC(100));
		}
	}
#endif

	LOG_INF("DTR set");

	/* They are optional, we use them to test the interrupt endpoint */
	ret = uart_line_ctrl_set(uart_dev, UART_LINE_CTRL_DCD, 1);
	if (ret) {
		LOG_WRN("Failed to set DCD, ret code %d", ret);
	}

	ret = uart_line_ctrl_set(uart_dev, UART_LINE_CTRL_DSR, 1);
	if (ret) {
		LOG_WRN("Failed to set DSR, ret code %d", ret);
	}

	/* Wait 100ms for the host to do all settings */
	k_msleep(100);

#ifndef CONFIG_USB_DEVICE_STACK_NEXT
	print_baudrate(uart_dev);
#endif
	uart_irq_callback_set(uart_dev, interrupt_handler);

	/* Enable rx interrupts */
	uart_irq_rx_enable(uart_dev);

	return 0;
}
