/*
 * Copyright (C) 2017 John Crispin <john@phrozen.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include <libubus.h>

#include "led.h"
#include "log.h"
#include "ubus.h"

static struct ubus_auto_conn conn;

enum {
	COLOUR_LEDS,
	COLOUR_BLINK,
	COLOUR_FADE,
	COLOUR_ON,
	COLOUR_OFF,
	__COLOUR_MAX,
};

static const struct blobmsg_policy colour_policy[] = {
	[COLOUR_LEDS]	= { "leds", BLOBMSG_TYPE_TABLE },
	[COLOUR_BLINK]	= { "blink", BLOBMSG_TYPE_INT32 },
	[COLOUR_FADE]	= { "fade", BLOBMSG_TYPE_INT32 },
	[COLOUR_ON]	= { "on", BLOBMSG_TYPE_INT32 },
	[COLOUR_OFF]	= { "off", BLOBMSG_TYPE_INT32 },
};

static int
set_colour(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method,
		    struct blob_attr *msg)
{
	size_t rem;
	struct blob_attr *tb[__COLOUR_MAX], *cur;
	int blink = 0, fade = 0, on = 0, off = 0;

	blobmsg_parse(colour_policy, __COLOUR_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[COLOUR_LEDS])
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (tb[COLOUR_BLINK])
		blink = blobmsg_get_u32(tb[COLOUR_BLINK]);

	if (tb[COLOUR_FADE])
		fade = blobmsg_get_u32(tb[COLOUR_FADE]);

	if (tb[COLOUR_ON])
		on = blobmsg_get_u32(tb[COLOUR_ON]);

	if (tb[COLOUR_OFF])
		off = blobmsg_get_u32(tb[COLOUR_OFF]);

	blobmsg_for_each_attr(cur, tb[COLOUR_LEDS], rem) {
		static struct blobmsg_policy brightness_policy[2] = {
			{ .type = BLOBMSG_TYPE_INT32 },
			{ .type = BLOBMSG_TYPE_INT32 },
		};
		struct blob_attr *brightness[2];

		if (blobmsg_type(cur) == BLOBMSG_TYPE_INT32)
			led_add(blobmsg_name(cur), blobmsg_get_u32(cur), -1, blink, fade, on, off);
		else if (blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY) {
			blobmsg_parse_array(brightness_policy, 2, brightness, blobmsg_data(cur),
					    blobmsg_data_len(cur));
			if (!brightness[0] || !brightness[1])
				continue;
			led_add(blobmsg_name(cur), blobmsg_get_u32(brightness[1]), blobmsg_get_u32(brightness[0]),
				blink, fade, on, off);
		}
	}
	return 0;
}

static const struct ubus_method led_methods[] = {
	UBUS_METHOD("set", set_colour, colour_policy),
};

static struct ubus_object_type led_object_type =
	UBUS_OBJECT_TYPE("led", led_methods);

static struct ubus_object led_object = {
	.name = "led",
	.type = &led_object_type,
	.methods = led_methods,
	.n_methods = ARRAY_SIZE(led_methods),
};

static void
ubus_connect_handler(struct ubus_context *ctx)
{
	int ret;

	ret = ubus_add_object(ctx, &led_object);
	if (ret)
		ERROR("Failed to add object: %s\n", ubus_strerror(ret));
}

void
ubus_init()
{
	conn.cb = ubus_connect_handler;
	ubus_auto_connect(&conn);
}
