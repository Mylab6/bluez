/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2013  Intel Corporation. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stddef.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/socket.h>

#include "hal-msg.h"
#include "ipc.h"
#include "log.h"

struct service_handler {
	const struct ipc_handler *handler;
	uint8_t size;
};

static struct service_handler services[HAL_SERVICE_ID_MAX + 1];

static int cmd_sk = -1;
static int notif_sk = -1;

void ipc_init(int command_sk, int notification_sk)
{
	cmd_sk = command_sk;
	notif_sk = notification_sk;
}

void ipc_cleanup(void)
{
	cmd_sk = -1;
	notif_sk = -1;
}

static void ipc_send(int sk, uint8_t service_id, uint8_t opcode, uint16_t len,
							void *param, int fd)
{
	struct msghdr msg;
	struct iovec iv[2];
	struct hal_hdr m;
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;

	memset(&msg, 0, sizeof(msg));
	memset(&m, 0, sizeof(m));
	memset(cmsgbuf, 0, sizeof(cmsgbuf));

	m.service_id = service_id;
	m.opcode = opcode;
	m.len = len;

	iv[0].iov_base = &m;
	iv[0].iov_len = sizeof(m);

	iv[1].iov_base = param;
	iv[1].iov_len = len;

	msg.msg_iov = iv;
	msg.msg_iovlen = 2;

	if (fd >= 0) {
		msg.msg_control = cmsgbuf;
		msg.msg_controllen = sizeof(cmsgbuf);

		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));

		/* Initialize the payload */
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	}

	if (sendmsg(sk, &msg, 0) < 0) {
		error("IPC send failed, terminating :%s", strerror(errno));
		raise(SIGTERM);
	}
}

void ipc_send_rsp(uint8_t service_id, uint8_t opcode, uint8_t status)
{
	struct hal_status s;

	if (status == HAL_STATUS_SUCCESS) {
		ipc_send(cmd_sk, service_id, opcode, 0, NULL, -1);
		return;
	}

	s.code = status;

	ipc_send(cmd_sk, service_id, HAL_OP_STATUS, sizeof(s), &s, -1);
}

void ipc_send_rsp_full(uint8_t service_id, uint8_t opcode, uint16_t len,
							void *param, int fd)
{
	ipc_send(cmd_sk, service_id, opcode, len, param, fd);
}

void ipc_send_notif(uint8_t service_id, uint8_t opcode,  uint16_t len,
								void *param)
{
	if (notif_sk < 0)
		return;

	ipc_send(notif_sk, service_id, opcode, len, param, -1);
}

void ipc_register(uint8_t service, const struct ipc_handler *handlers,
								uint8_t size)
{
	services[service].handler = handlers;
	services[service].size = size;
}

void ipc_unregister(uint8_t service)
{
	services[service].handler = NULL;
	services[service].size = 0;
}

void ipc_handle_msg(const void *buf, ssize_t len)
{
	const struct hal_hdr *msg = buf;
	const struct ipc_handler *handler;

	if (len < (ssize_t) sizeof(*msg)) {
		error("IPC: message too small (%zd bytes), terminating", len);
		raise(SIGTERM);
		return;
	}

	if (len != (ssize_t) (sizeof(*msg) + msg->len)) {
		error("IPC: message malformed (%zd bytes), terminating", len);
		raise(SIGTERM);
		return;
	}

	/* if service is valid */
	if (msg->service_id > HAL_SERVICE_ID_MAX) {
		error("IPC: unknown service (0x%x), terminating",
							msg->service_id);
		raise(SIGTERM);
		return;
	}

	/* if service is registered */
	if (!services[msg->service_id].handler) {
		error("IPC: unregistered service (0x%x), terminating",
							msg->service_id);
		raise(SIGTERM);
		return;
	}

	/* if opcode is valid */
	if (msg->opcode == HAL_OP_STATUS ||
			msg->opcode > services[msg->service_id].size) {
		error("IPC: invalid opcode 0x%x for service 0x%x, terminating",
						msg->opcode, msg->service_id);
		raise(SIGTERM);
		return;
	}

	/* opcode is table offset + 1 */
	handler = &services[msg->service_id].handler[msg->opcode - 1];

	/* if payload size is valid */
	if ((handler->var_len && handler->data_len > msg->len) ||
			(!handler->var_len && handler->data_len != msg->len)) {
		error("IPC: size invalid opcode 0x%x service 0x%x, terminating",
						msg->service_id, msg->opcode);
		raise(SIGTERM);
		return;
	}

	handler->handler(msg->payload, msg->len);
}
