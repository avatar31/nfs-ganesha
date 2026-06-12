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

#include "dotfs.h"

const int DEFAULT_CONN_TIMEOUT_MS = 250; // 250 milliseconds
const int DEFAULT_IO_TIMEOUT_MS = 2000;  // 2 seconds
const int DEFAULT_MAX_RETRIES = 3;
const int DEFAULT_RETRY_DELAY_SEC = 1; // 1 second

static dfs_status_t socket_connect_internal_locked(socket_context_t *ctx);

/**
 * @brief Initializes the socket context structure and mutex.
 */
dfs_status_t initialize_socket_ctx(socket_context_t *ctx)
{
	if (!ctx || strlen(ctx->socket_path) == 0 ||
			strlen(ctx->socket_path) >= SOCK_PATH_MAX) {
		return (DFS_FAIL);
	}

	if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
		return (DFS_FAIL);
	}

	ctx->sock_fd = -1;
	ctx->connect_timeout_ms = DEFAULT_CONN_TIMEOUT_MS;
	ctx->io_timeout_ms = DEFAULT_IO_TIMEOUT_MS;
	ctx->max_retries = DEFAULT_MAX_RETRIES;
	ctx->retry_delay_sec = DEFAULT_RETRY_DELAY_SEC;
	
	return (DFS_PASS);
}

/**
 * @brief Connect Function wrapping retry loop logic.
 */
dfs_status_t socket_connect(socket_context_t *ctx)
{
	dfs_status_t status = DFS_FAIL;

	for (int attempt = 0; attempt <= ctx->max_retries; attempt++) {
		pthread_mutex_lock(&ctx->lock);
		status = socket_connect_internal_locked(ctx);
		pthread_mutex_unlock(&ctx->lock);

		if (status == DFS_PASS) {
			LogEventMsg("Connected successfully to backend via socket.");
			return (DFS_PASS);
		}

		LogWarnMsg("Connection attempt %d failed.", attempt + 1);

		if (attempt == ctx->max_retries) {
			break;
		}

		LogInfoMsg("Retrying connection (%d of %d)...", attempt + 1,
			   ctx->max_retries);
		sleep(ctx->retry_delay_sec);
	}

	LogErrorMsg("All %d connection attempts failed.", ctx->max_retries + 1);
	return (status);
}

/**
 * @brief Internal connect helper. 
 * CRITICAL: Expects ctx->lock to be held by the calling thread.
 */
static dfs_status_t socket_connect_internal_locked(socket_context_t *ctx)
{
	struct sockaddr_un addr;
	long flags;
	int rc;
	dfs_status_t status = DFS_FAIL;

	// If an old fd is floating around, purge it cleanly
	if (ctx->sock_fd != -1) {
		close(ctx->sock_fd);
		ctx->sock_fd = -1;
	}

	// Create raw Unix Stream Socket
	ctx->sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ctx->sock_fd < 0) {
		LogSysError("Failed to create socket.", errno);
		return (status);
	}

	// Force non-blocking mode to apply connect timeout
	if ((flags = fcntl(ctx->sock_fd, F_GETFL, NULL)) < 0) {
		LogSysError("Failed to get socket flags.", errno);
		goto exit;
	}

	if (fcntl(ctx->sock_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		LogSysError("Failed to set socket to non-blocking mode.", errno);
		goto exit;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;

	// Secure string bounds checking for UNIX socket paths
	strncpy(addr.sun_path, ctx->socket_path, sizeof(addr.sun_path) - 1);
	addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

	// Connect
	rc = connect(ctx->sock_fd, (struct sockaddr *)&addr, sizeof(addr));
	if (rc < 0) {
		if (errno != EINPROGRESS) {
			LogSysError("Failed to initiate connection.", errno);
			goto exit;
		}

		struct pollfd pfd;
		pfd.fd = ctx->sock_fd;
		pfd.events = POLLOUT;

		// Handle signal interruption loop safely
		do {
			rc = poll(&pfd, 1, ctx->connect_timeout_ms);
		} while (rc < 0 && errno == EINTR);

		if (rc <= 0) {
			if (rc == 0) {
				LogWarnMsg("Connection attempt timed out after %d ms.",
					ctx->connect_timeout_ms);
				errno = ETIMEDOUT;
			} else {
				LogSysError("Poll failed during connection.", errno);
			}
			goto exit;
		}

		int valopt;
		socklen_t lon = sizeof(int);
		if (getsockopt(ctx->sock_fd, SOL_SOCKET, SO_ERROR,
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

	if (fcntl(ctx->sock_fd, F_SETFL, flags) < 0) {
		LogSysError("Failed to restore socket flags blocking mode.", errno);
		goto exit;
	}

	status = DFS_PASS;

exit:
	if (status != DFS_PASS) {
		int saved_errno = errno;
		if (ctx->sock_fd != -1) {
			close(ctx->sock_fd);
			ctx->sock_fd = -1;
		}
		errno = saved_errno;
	}
	return (status);
}

/**
 * @brief Thread-safe Reconnection routine.
 */
dfs_status_t socket_reconnect(socket_context_t *ctx)
{
	pthread_mutex_lock(&ctx->lock);

	/* Double-Check Lock Pattern: Verify if another thread recovered link */
	if (ctx->sock_fd != -1) {
		struct pollfd pfd;
		pfd.fd = ctx->sock_fd;
		pfd.events = POLLOUT;

		// Zero timeout poll check to instantly peek status
		if (poll(&pfd, 1, 0) >= 0) {
			int valopt = 0;
			socklen_t lon = sizeof(int);
			if (getsockopt(ctx->sock_fd, SOL_SOCKET, SO_ERROR,
				       (void *)(&valopt), &lon) == 0) {
				if (valopt == 0) {
					pthread_mutex_unlock(&ctx->lock);
					LogInfoMsg(
						"Connection already recovered by another thread.");
					return (DFS_PASS);
				}
			}
		}
		close(ctx->sock_fd);
		ctx->sock_fd = -1;
	}

	LogWarnMsg(
		"Broken link confirmed. Triggering runtime reconnect sequence...");

	dfs_status_t status = DFS_FAIL;
	for (int attempt = 0; attempt <= ctx->max_retries; attempt++) {
		status = socket_connect_internal_locked(ctx);
		if (status == DFS_PASS) {
			break;
		}
		if (attempt < ctx->max_retries) {
			pthread_mutex_unlock(&ctx->lock);
			sleep(ctx->retry_delay_sec);
			pthread_mutex_lock(&ctx->lock);
		}
	}

	pthread_mutex_unlock(&ctx->lock);

	if (status == DFS_PASS) {
		LogInfoMsg("Runtime reconnect successful");
		return (DFS_PASS);
	}

	LogErrorMsg("Runtime reconnection failed completely.");
	return (DFS_FAIL);
}

/**
 * @brief Send message routine featuring full tracking for stream partial writes.
 */
ssize_t socket_send_message(socket_context_t *ctx, const void *buffer,
			size_t len)
{
	struct pollfd pfd;
	int retry_count = 0;
	size_t total_sent = 0;

	if (!ctx || !buffer || len == 0) {
		return (-EINVAL);
	}

	while (retry_count <= 1) {
		pthread_mutex_lock(&ctx->lock);
		pfd.fd = ctx->sock_fd;
		pthread_mutex_unlock(&ctx->lock);

		// Disconnected Socket Check & Reconnection
		if (pfd.fd < 0) {
			if (socket_reconnect(ctx) != DFS_PASS) {
				return (-ENOTCONN);
			}
			retry_count++;
			continue;
		}

		// monitor the socket for POLLOUT
		pfd.events = POLLOUT;
		int pol_rc = poll(&pfd, 1, ctx->io_timeout_ms);
		if (pol_rc == 0) {
			LogErrorMsg("Send timeout: Backend is frozen or clogged.");
			return (-ETIMEDOUT);
		} else if (pol_rc < 0) {
			if (errno == EINTR) {
				continue;
			}
			LogSysError("Poll failed during send.", errno);
			return (-errno);
		}

		// Handle partial streaming sends cleanly
		while (total_sent < len) {
			ssize_t sent = send(pfd.fd, buffer + total_sent,
					    len - total_sent, MSG_NOSIGNAL);
			if (sent < 0) {
				if (errno == EINTR) {
					continue;
				}
				if (errno == EPIPE || errno == ECONNRESET ||
				    errno == EBADF) {
					LogSysError(
						"Send failed. Attempting self-healing...",
						errno);
					if (socket_reconnect(ctx) == DFS_PASS) {
						retry_count++;
						break;
					}
					return (-ENOTCONN);
				}

				LogSysError("Send failed with unrecoverable error.", errno);
				return (-errno);
			}

			total_sent += sent;
		}

		if (total_sent == len) {
			return (ssize_t)(total_sent);
		}
	}

	return (-EIO);
}

/**
 * @brief Robust message reception matching send_message's self-healing mechanics.
 */
ssize_t socket_recv_message(socket_context_t *ctx, void *buffer, size_t max_len)
{
	struct pollfd pfd;
	int retry_count = 0;

	if (!ctx || !buffer || max_len == 0) {
		return (-EINVAL);
	}

	while (retry_count <= 1) {
		pthread_mutex_lock(&ctx->lock);
		pfd.fd = ctx->sock_fd;
		pthread_mutex_unlock(&ctx->lock);

		// Disconnected Socket Check & Reconnection
		if (pfd.fd < 0) {
			if (socket_reconnect(ctx) != DFS_PASS) {
				return (-ENOTCONN);
			}
			retry_count++;
			continue;
		}

		// Instead of blocking indefinitely on recv,
		// use poll to wait for incoming data (POLLIN)
		pfd.events = POLLIN;
		int pol_rc = poll(&pfd, 1, ctx->io_timeout_ms);
		if (pol_rc == 0) {
			LogWarnMsg("Receive timeout: Backend failed to respond.");
			return (-ETIMEDOUT);
		} else if (pol_rc < 0) {
			if (errno == EINTR) {
				continue;
			}

			LogSysError("Poll failed during receive.", errno);
			return (-errno);
		}

		// Data is ready
		ssize_t bytes_read = recv(pfd.fd, buffer, max_len, 0);
		if (bytes_read == 0) {
			LogWarnMsg("Backend closed connection gracefully (EOF).");
			if (socket_reconnect(ctx) == DFS_PASS) {
				retry_count++;
				continue;
			}
			return (-ENOTCONN);
		} else if (bytes_read < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == ECONNRESET || errno == EBADF) {
				LogSysError("Backend abruptly reset connection during read.", errno);
				if (socket_reconnect(ctx) == DFS_PASS) {
					retry_count++;
					continue;
				}
			}
			LogSysError("Receive failed with unrecoverable error.", errno);
			return (-errno);
		}

		return (bytes_read);
	}

	return (-EIO);
}

/**
 * @brief Closes the socket context and destroys the lifecycle mutex.
 */
void socket_close(socket_context_t *ctx)
{
	if (!ctx) {
		return;
	}

	pthread_mutex_lock(&ctx->lock);
	if (ctx->sock_fd != -1) {
		close(ctx->sock_fd);
		ctx->sock_fd = -1;
		LogEventMsg("Socket disconnected and resource closed.");
	}
	pthread_mutex_unlock(&ctx->lock);
	pthread_mutex_destroy(&ctx->lock);
}
