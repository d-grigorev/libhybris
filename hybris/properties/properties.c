/*
 * Copyright (c) 2012 Carsten Munk <carsten.munk@gmail.com>
 *               2008 The Android Open Source Project
 *               2013 Simon Busch <morphis@gravedo.de>
 *               2013 Canonical Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#define __USE_GNU
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <poll.h>

#include <hybris/properties/properties.h>

static const char property_service_socket[] = "/dev/socket/" PROP_SERVICE_NAME;

/* Find a key value from a static /system/build.prop file */
static char *find_key(const char *key)
{
	FILE *f = fopen("/system/build.prop", "r");
	char buf[1024];
	char *mkey, *value;

	if (!f)
		return NULL;

	while (fgets(buf, 1024, f) != NULL) {
		if (strchr(buf, '\r'))
			*(strchr(buf, '\r')) = '\0';
		if (strchr(buf, '\n'))
			*(strchr(buf, '\n')) = '\0';

		mkey = strtok(buf, "=");

		if (!mkey)
			continue;

		value = strtok(NULL, "=");
		if (!value)
			continue;

		if (strcmp(key, mkey) == 0) {
			fclose(f);
			return strdup(value);
		}
	}

	fclose(f);
	return NULL;
}

/* Find a key value from the kernel command line, which is parsed
 * by Android at init (on an Android working system) */
static char *find_key_kernel_cmdline(const char *key)
{
	char cmdline[1024];
	char *ptr;
	int fd;

	fd = open("/proc/cmdline", O_RDONLY);
	if (fd >= 0) {
		int n = read(fd, cmdline, 1023);
		if (n < 0) n = 0;

		/* get rid of trailing newline, it happens */
		if (n > 0 && cmdline[n-1] == '\n') n--;

		cmdline[n] = 0;
		close(fd);
	} else {
		cmdline[0] = 0;
	}

	ptr = cmdline;

	while (ptr && *ptr) {
		char *x = strchr(ptr, ' ');
		if (x != 0) *x++ = 0;

		char *name = ptr;
		ptr = x;

		char *value = strchr(name, '=');
		int name_len = strlen(name);

		if (value == 0) continue;
		*value++ = 0;
		if (name_len == 0) continue;

		if (!strncmp(name, "androidboot.", 12) && name_len > 12) {
			const char *boot_prop_name = name + 12;
			char prop[PROP_NAME_MAX];
			snprintf(prop, sizeof(prop), "ro.%s", boot_prop_name);
			if (strcmp(prop, key) == 0)
				return strdup(value);
		}
	}

	return NULL;
}

/* Get/Set a property from the Android Init property socket */
static int send_prop_msg(prop_msg_t *msg,
		void (*propfn)(const char *, const char *, void *),
		void *cookie)
{
	struct pollfd pollfds[1];
	union {
		struct sockaddr_un addr;
		struct sockaddr addr_g;
	} addr;
	socklen_t alen;
	size_t namelen;
	int s;
	int r;
	int result = -1;
	int patched_init = 0;

	s = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (s < 0) {
		return result;
	}

	memset(&addr, 0, sizeof(addr));
	namelen = strlen(property_service_socket);
	strncpy(addr.addr.sun_path, property_service_socket,
			sizeof(addr.addr.sun_path));
	addr.addr.sun_family = AF_LOCAL;
	alen = namelen + offsetof(struct sockaddr_un, sun_path) + 1;

	if (TEMP_FAILURE_RETRY(connect(s, &addr.addr_g, alen) < 0)) {
		close(s);
		return result;
	}

	r = TEMP_FAILURE_RETRY(send(s, msg, sizeof(prop_msg_t), 0));

	if (r == sizeof(prop_msg_t)) {
		pollfds[0].fd = s;
		pollfds[0].events = 0;
		// We successfully wrote to the property server, so use recv
		// in case we need to get a property. Once the other side is
		// finished, the socket is closed.
		while ((r = recv(s, msg, sizeof(prop_msg_t), 0)) > 0) {
			if (r != sizeof(prop_msg_t)) {
				close(s);
				return result;
			}

			/* If we got a reply, this is a patched init */
			if (!patched_init)
				patched_init = 1;

			if (propfn)
				propfn(msg->name, msg->value, cookie);
		}

		/* We also just get a close in case of setprop */
		if ((r >= 0) && (patched_init ||
				(msg->cmd == PROP_MSG_SETPROP))) {
			result = 0;
		}
	}

	close(s);
	return result;
}

int property_list(void (*propfn)(const char *key, const char *value, void *cookie),
		void *cookie)
{
	int err;
	prop_msg_t msg;

	memset(&msg, 0, sizeof(msg));
	msg.cmd = PROP_MSG_LISTPROP;

	err = send_prop_msg(&msg, propfn, cookie);
	if (err < 0) {
		return err;
	}

	return 0;
}

static int property_get_socket(const char *key, char *value, const char *default_value)
{
	int err;
	int len;
	prop_msg_t msg;

	memset(&msg, 0, sizeof(msg));
	msg.cmd = PROP_MSG_GETPROP;

	if (key) {
		strncpy(msg.name, key, sizeof(msg.name));
		err = send_prop_msg(&msg, NULL, NULL);
		if (err < 0)
			return err;
	}

	/* In case it's null, just use the default */
	if ((strlen(msg.value) == 0) && (default_value)) {
		if (strlen(default_value) >= PROP_VALUE_MAX) return -1;
		len = strlen(default_value);
		memcpy(msg.value, default_value, len + 1);
	}

	strncpy(value, msg.value, sizeof(msg.value));

	return 0;
}

int property_get(const char *key, char *value, const char *default_value)
{
	char *ret = NULL;

	if ((key) && (strlen(key) >= PROP_NAME_MAX)) return -1;

	if (property_get_socket(key, value, default_value) == 0) {
		if (value)
			return strlen(value);
		else
			return 0;
	}

	/* In case the socket is not available, parse the file by hand */
	if ((ret = find_key(key)) == NULL) {
		/* Property might be available via /proc/cmdline */
		ret = find_key_kernel_cmdline(key);
	}

	if (ret) {
		strcpy(value, ret);
		free(ret);
		return strlen(value);
	} else if (default_value != NULL) {
		strcpy(value, default_value);
		return strlen(value);
	} else {
		value = '\0';
	}

	return 0;
}

int property_set(const char *key, const char *value)
{
	int err;
	prop_msg_t msg;

	if (key == 0) return -1;
	if (value == 0) value = "";
	if (strlen(key) >= PROP_NAME_MAX) return -1;
	if (strlen(value) >= PROP_VALUE_MAX) return -1;

	memset(&msg, 0, sizeof(msg));
	msg.cmd = PROP_MSG_SETPROP;
	strncpy(msg.name, key, sizeof(msg.name));
	strncpy(msg.value, value, sizeof(msg.value));

	err = send_prop_msg(&msg, NULL, NULL);
	if (err < 0) {
		return err;
	}

	return 0;
}

// vim:ts=4:sw=4:noexpandtab