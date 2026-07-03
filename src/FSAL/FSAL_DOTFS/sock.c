// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Author: Sachin S <sarodhesachin96@gmail.com>
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * -------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <pthread.h>
#include <libgen.h>

#include "dotfs.h"
#include "messages.h"

#define RECONNECT_RETRY_DELAY_MS	500  	// 0.5 second
#define INCOMING_SOCKET_BACKLOG 	5
#define DEFAULT_IO_TIMEOUT_MS		2000	// 2 seconds

dfs_status_t reconnect_outbound_socket(socket_context_t *ctx);
dfs_status_t dial_socket(int *sock_fd_out, const char *socket_path);

dfs_status_t initialize_socket_ctx(socket_context_t *ctx)
{
	LogInfoMsg("Inbound socket file: %s", ctx->inbound_socket_path);
	LogInfoMsg("Outbound socket file: %s", ctx->outbound_socket_path);

	if (!ctx || strlen(ctx->inbound_socket_path) == 0 ||
			strlen(ctx->inbound_socket_path) >= SOCK_PATH_MAX) {
		return (DFS_FAIL);
	}

	if (!ctx || strlen(ctx->outbound_socket_path) == 0 ||
			strlen(ctx->outbound_socket_path) >= SOCK_PATH_MAX) {
		return (DFS_FAIL);
	}

	if (pthread_mutex_init(&ctx->outbound_sock_fd_lock, NULL) != 0) {
		return (DFS_FAIL);
	}

	char path_copy[SOCK_PATH_MAX];
    strncpy(path_copy, ctx->inbound_socket_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

	char *dir_name = dirname(path_copy);
	if (mkdir(dir_name, 0755) == -1) {
        if (errno != EEXIST) {
			LogSysError("Failed to create directory for socket file.", errno);
			return (DFS_FAIL);
		}
	}

	unlink(ctx->inbound_socket_path);

	return (DFS_PASS);
}

dfs_status_t init_inbound_server(socket_context_t *ctx)
{
	struct sockaddr_un addr;
	int listen_fd = -1;

	unlink(ctx->inbound_socket_path);

	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		LogSysError("Failed to create inbound listen socket.", errno);
		return (DFS_FAIL);
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, ctx->inbound_socket_path, sizeof(addr.sun_path) - 1);
	addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LogSysError("Failed to bind inbound socket path.", errno);
		close(listen_fd);
		return (DFS_FAIL);
	}

	if (listen(listen_fd, 10) < 0) {
		LogSysError("Failed to listen on inbound socket.", errno);
		close(listen_fd);
		unlink(ctx->inbound_socket_path);
		return (DFS_FAIL);
	}

	ctx->inbound_sock_fd = listen_fd;
	LogInfoMsg("Inbound UDS Server started and listening successfully with socket path: %s",
		ctx->inbound_socket_path);
	return (DFS_PASS);
}

void* inbound_reader_thread(void* arg)
{
	socket_context_t *ctx = (socket_context_t *)arg;
	int listen_fd = ctx->inbound_sock_fd;

	LogInfoMsg("Inbound reader thread worker spawned successfully.");

	// Master Accept Loop
	while (1) {
		int client_fd = -1;

		do {
			client_fd = accept(listen_fd, NULL, NULL);
		} while (client_fd < 0 && errno == EINTR);

		if (client_fd < 0) {
			LogSysError("Accept failed on inbound socket.", errno);
			sleep(RECONNECT_RETRY_DELAY_MS);	// 0.5s
			continue;
		}

		LogInfoMsg("Accepted a new incoming client connection.");

		while (1) {
			uint8_t frame[WIRE_FRAME_HEADER_SIZE];
			dfs_status_t status = socket_read_message(client_fd, frame, WIRE_FRAME_HEADER_SIZE);
			if (status != DFS_PASS) {
				break;
			}

			uint16_t type_id = get_message_type(frame);
			uint16_t fixed_len = get_message_fixed_length(frame);
			if (fixed_len == 0) {
				LogErrorMsg("Protocol Violation, Received 0-length message for type %u. Dropping client.", type_id);
				break;
			}

			uint8_t* fixed_buf = malloc(fixed_len);
			if (!fixed_buf) {
				LogErrorMsg("Memory allocation failed for incoming message body.");
				break; 
			}

			status = socket_read_message(client_fd, fixed_buf, fixed_len);
			if (status != DFS_PASS) {
				LogErrorMsg("Failed to read complete message body from inbound client.");
				free(fixed_buf);
				break;
			}

			int drop_client = 0;
			switch (type_id) {
				// TODO: Add your valid message cases here
				
				default: {
					LogErrorMsg("Unknown message type received: %u. Dropping connection.", type_id);
					drop_client = 1;
					break;
				}
			}

			free(fixed_buf);
			if (drop_client) {
				break;
			}
		}

		close(client_fd);
	}

	return (NULL);
}

dfs_status_t socket_read_message(int fd, uint8_t *buffer, size_t total_len)
{
	if (!buffer || total_len == 0) {
		return (DFS_FAIL);
	}

	uint8_t *ptr = buffer;
	size_t bytes_left = total_len;

	// Loop until we have read EXACTLY the number of bytes requested
	while (bytes_left > 0) {
		struct pollfd pfd;
		pfd.fd = fd;
		pfd.events = POLLIN;

		int pol_rc;
		do {
			pol_rc = poll(&pfd, 1, DEFAULT_IO_TIMEOUT_MS);
		} while (pol_rc < 0 && errno == EINTR);

		if (pol_rc == 0) {
			LogWarnMsg("Receive timeout: Client failed to send data within the time limit.");
			return (DFS_FAIL);
		} else if (pol_rc < 0) {
			LogSysError("Poll failed during receive operations.", errno);
			return (DFS_FAIL);
		}

		ssize_t bytes_read;
		do {
			bytes_read = read(fd, ptr, bytes_left);
		} while (bytes_read < 0 && errno == EINTR);

		if (bytes_read > 0) {
			ptr += bytes_read;
			bytes_left -= bytes_read;
		} else if (bytes_read == 0) {
			LogEventMsg("Inbound client disconnected gracefully.");
			return (DFS_FAIL);
		} else {
			if (errno != ECONNRESET) {
				LogSysError("Error reading from inbound socket.", errno);
			} else {
				LogErrorMsg("Inbound client connection reset abruptly.");
			}
			return (DFS_FAIL);
		}
	}

	return (DFS_PASS);
}

void* outbound_dialer_thread(void* arg)
{
	socket_context_t *ctx = (socket_context_t *)arg;

	while (1) {
		pthread_mutex_lock(&ctx->outbound_sock_fd_lock);
		int need_connect = (ctx->outbound_sock_fd == -1);
		pthread_mutex_unlock(&ctx->outbound_sock_fd_lock);

		if (need_connect) {
			reconnect_outbound_socket(ctx);
		}
		sleep(RECONNECT_RETRY_DELAY_MS);	// 0.5s
	}
	return NULL;
}

dfs_status_t reconnect_outbound_socket(socket_context_t *ctx)
{
	int new_fd = -1;
	dfs_status_t status = dial_socket(&new_fd, ctx->outbound_socket_path);
	if (status == DFS_PASS) {
		pthread_mutex_lock(&ctx->outbound_sock_fd_lock);
		ctx->outbound_sock_fd = new_fd;
		LogEventMsg("Outbound socket reconnected successfully.");
		pthread_mutex_unlock(&ctx->outbound_sock_fd_lock);
	} else {
		LogErrorMsg("Outbound socket reconnection failed.");
	}

	return status;
}

dfs_status_t dial_socket(int *sock_fd_out, const char *socket_path)
{
	struct sockaddr_un addr;
	long flags;
	int rc;
	int sock_fd = -1;
	dfs_status_t status = DFS_FAIL;

	// Create raw Unix Stream Socket
	sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		LogSysError("Failed to create socket.", errno);
		return (status);
	}

	// Force non-blocking mode to apply connect timeout
	if ((flags = fcntl(sock_fd, F_GETFL, NULL)) < 0) {
		LogSysError("Failed to get socket flags.", errno);
		goto exit;
	}

	if (fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		LogSysError("Failed to set socket to non-blocking mode.", errno);
		goto exit;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
	addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

#if defined(SO_NOSIGPIPE)
    int set = 1;
    setsockopt(sock_fd, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int));
#endif

	// Connect
	rc = connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr));
	if (rc < 0) {
		if (errno != EINPROGRESS) {
			LogSysError("Failed to initiate connection.", errno);
			goto exit;
		}

		struct pollfd pfd;
		pfd.fd = sock_fd;
		pfd.events = POLLOUT;

		do {
			rc = poll(&pfd, 1, DEFAULT_IO_TIMEOUT_MS);
		} while (rc < 0 && errno == EINTR);

		if (rc <= 0) {
			if (rc == 0) {
				LogWarnMsg("Connection attempt timed out after %d ms.",
					DEFAULT_IO_TIMEOUT_MS);
				errno = ETIMEDOUT;
			} else {
				LogSysError("Poll failed during connection.", errno);
			}
			goto exit;
		}

		int valopt;
		socklen_t lon = sizeof(int);
		if (getsockopt(sock_fd, SOL_SOCKET, SO_ERROR,
					(void *)(&valopt), &lon) < 0) {
			LogSysError("Failed to get socket options after poll.", errno);
			goto exit;
		}

		if (valopt) {
			errno = valopt;
			LogSysError("Socket error after poll.", errno);
			goto exit;
		}
	}

	// Restore original socket flags
	if (fcntl(sock_fd, F_SETFL, flags) < 0) {
		LogSysError("Failed to restore socket flags blocking mode.", errno);
		goto exit;
	}

	*sock_fd_out = sock_fd;
	status = DFS_PASS;

exit:
	if (status != DFS_PASS) {
		int saved_errno = errno;
		if (sock_fd != -1) {
			close(sock_fd);
		}
		errno = saved_errno;
	}
	return (status);
}

ssize_t socket_send_message(int fd, const uint8_t *buffer, size_t len)
{
	if (!buffer || len == 0) {
		return (-EINVAL);
	}

	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLOUT;

	size_t total_sent = 0;
	while (total_sent < len) {
		int pol_rc;
		do {
			pol_rc = poll(&pfd, 1, DEFAULT_IO_TIMEOUT_MS);
		} while (pol_rc < 0 && errno == EINTR);

		if (pol_rc == 0) {
			LogErrorMsg("Send timeout: Backend socket buffer is frozen or clogged.");
			return (-ETIMEDOUT);
		} else if (pol_rc < 0) {
			LogSysError("Poll failed during send orchestration.", errno);
			return (-errno);
		}

		ssize_t sent;
		do {
			// MSG_NOSIGNAL keeps our process alive if the receiver dies mid-flight
			sent = send(fd, buffer + total_sent, len - total_sent,
						MSG_NOSIGNAL);
		} while (sent < 0 && errno == EINTR);

		if (sent < 0) {
			// If the buffer is temporarily full but socket is non-blocking,
			// let our loop circle back up to poll() to wait safely.
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				continue; 
			}

			LogSysError("Send system call failed with error.", errno);
			return (-errno);
		}

		total_sent += sent;
	}

	return ((ssize_t) total_sent);
}

// void* client_heartbeat_thread(void* arg) {
// 	socket_context_t *ctx = (socket_context_t *)arg;

// 	while (1) {
// 		sleep(2);

// 		uint8_t* buf = NULL;
// 		heartbeat_message_t hb = {0};
// 		heartbeat_message_set_timestamp(&hb, (int64_t)time(NULL));
		
// 		int len = heartbeat_message_marshal(&hb, &buf);
// 		if (len > 0 && buf) {			
// 			pthread_mutex_lock(&ctx->outbound_sock_fd_lock);
// 			int sock_fd = ctx->outbound_sock_fd;
// 			pthread_mutex_unlock(&ctx->outbound_sock_fd_lock);

// 			if (sock_fd == -1) {
// 				LogWarnMsg("Heartbeat skipped: Outbound socket is disconnected.");
// 				free(buf);
// 				heartbeat_message_free(&hb);
// 				continue;
// 			}

// 			ssize_t sent = socket_send_message(sock_fd, buf, len);

// 			if (sent < 0) {
// 				LogSysError("Heartbeat send failed with unrecoverable error.", errno);
				
// 				pthread_mutex_lock(&ctx->outbound_sock_fd_lock);
// 				// Double-check it hasn't changed
// 				if (ctx->outbound_sock_fd == sock_fd) {
// 					close(ctx->outbound_sock_fd);
// 					ctx->outbound_sock_fd = -1;
// 				}
// 				pthread_mutex_unlock(&ctx->outbound_sock_fd_lock);
// 			} else {
// 				LogInfoMsg("Heartbeat sent successfully.");
// 			}
// 		}
		
// 		heartbeat_message_free(&hb);
// 		free(buf);
// 	}

// 	return NULL;
// }

/**
 * @brief Closes the socket context and destroys the lifecycle mutex.
 */
void socket_close(socket_context_t *ctx)
{
	if (!ctx) {
		return;
	}

	unlink(ctx->inbound_socket_path);
	close(ctx->inbound_sock_fd);
	ctx->inbound_sock_fd = -1;

	pthread_mutex_lock(&ctx->outbound_sock_fd_lock);
	if (ctx->outbound_sock_fd != -1) {
		close(ctx->outbound_sock_fd);
		ctx->outbound_sock_fd = -1;
	}
	pthread_mutex_unlock(&ctx->outbound_sock_fd_lock);
	pthread_mutex_destroy(&ctx->outbound_sock_fd_lock);

	if (ctx->inbound_socket_path != NULL) {
		free(ctx->inbound_socket_path);
		ctx->inbound_socket_path = NULL;
	}

	if (ctx->outbound_socket_path != NULL) {
		free(ctx->outbound_socket_path);
		ctx->outbound_socket_path = NULL;
	}

	LogEventMsg("Socket disconnected and resource closed.");
}
