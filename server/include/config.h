#ifndef RIFT_CONFIG_H
#define RIFT_CONFIG_H

#include <stdint.h>

#define CONFIG_TUNNEL_PORT 7000
#define CONFIG_PUBLIC_PORT 9000
#define CONFIG_HEALTH_PORT 8080
#define CONFIG_LISTEN_BACKLOG 128

#define CONFIG_MAX_CONNECTIONS 102400
#define CONFIG_TUNNEL_ID_MAX 64
#define CONFIG_SERVICE_ID_MAX 64

#define CONFIG_FRAME_MAX_PAYLOAD 16384
#define CONFIG_FRAME_HEADER_SIZE 12
#define CONFIG_FRAME_VERSION 1
#define CONFIG_FRAME_MAGIC 0x52494654

#define CONFIG_EPOLL_MAX_EVENTS 64
#define CONFIG_EPOLL_TIMEOUT_NORMAL -1
#define CONFIG_EPOLL_TIMEOUT_SHUTDOWN 1000

#define CONFIG_TUNNEL_IDLE_TIMEOUT 0
#define CONFIG_PUBLIC_REQUEST_TIMEOUT 0

#define CONFIG_FRAME_READ_MAX_RETRIES 5
#define CONFIG_FRAME_READ_RETRY_SLEEP_US 10

/**
 * Maximum retries for writes when kernel buffer full.
 * write() might return EAGAIN if kernel buffer is full.
 * We retry with short sleeps before giving up.
 */
#define CONFIG_FRAME_WRITE_MAX_RETRIES 10

/** Sleep duration between write retries (microseconds) */
#define CONFIG_FRAME_WRITE_RETRY_SLEEP_US 10

/* ===== Logging Configuration ===== */

/**
 * Log level for server.
 * 0 = ERROR, 1 = INFO, 2 = DEBUG
 * Can be overridden via environment variable RIFT_LOG_LEVEL.
 */
#define CONFIG_LOG_LEVEL 1  // INFO

/**
 * Whether to include timestamps in logs.
 * Format: [2026-01-23 14:32:15] [LEVEL] message
 */
#define CONFIG_LOG_TIMESTAMP 1

/**
 * Whether to log each successful connection.
 * Helpful for debugging, but verbose at high connection counts.
 */
#define CONFIG_LOG_CONNECTIONS 1

/* ===== Feature Flags ===== */

/**
 * Enable SO_REUSEADDR on listening sockets.
 * Allows quick restart without TIME_WAIT.
 * Recommended for development/testing.
 */
#define CONFIG_SO_REUSEADDR 1

/**
 * Enable TCP_NODELAY on tunnel connections.
 * Reduces latency by disabling Nagle's algorithm.
 * Not yet implemented, but planned.
 */
#define CONFIG_TCP_NODELAY 0

/**
 * Enable TCP keepalive on tunnel connections.
 * Detects dead clients, prevents indefinite hangs.
 * Not yet implemented.
 */
#define CONFIG_TCP_KEEPALIVE 0

/* ===== Performance Tuning ===== */

/**
 * Size of frame read buffer on client.
 * Accumulates partial frames until complete.
 * = FRAME_MAX_PAYLOAD + FRAME_HEADER_SIZE
 */
#define CONFIG_CLIENT_FRAME_BUFFER_SIZE (CONFIG_FRAME_MAX_PAYLOAD + CONFIG_FRAME_HEADER_SIZE)

/**
 * Size of temporary data buffers.
 * Used for HTTP header peeking, frame payload, etc.
 */
#define CONFIG_TEMP_BUFFER_SIZE 4096

/**
 * Initial HTTP header buffer size (for Host header parsing).
 * Most HTTP requests have headers < 2KB.
 */
#define CONFIG_HTTP_HEADER_BUFFER_SIZE 2048

/* ===== Compile-Time Assertions ===== */

/* Ensure frame header size matches struct definition */
#if CONFIG_FRAME_HEADER_SIZE != 12
#error "CONFIG_FRAME_HEADER_SIZE must be 12 (sizeof(frame_header_t))"
#endif

/* Ensure frame size is reasonable */
#if CONFIG_FRAME_MAX_PAYLOAD < 1024 || CONFIG_FRAME_MAX_PAYLOAD > 65536
#error "CONFIG_FRAME_MAX_PAYLOAD must be between 1KB and 64KB"
#endif

/* Ensure max connections is reasonable */
#if CONFIG_MAX_CONNECTIONS < 100 || CONFIG_MAX_CONNECTIONS > 1000000
#error "CONFIG_MAX_CONNECTIONS must be between 100 and 1M"
#endif

#endif /* RIFT_CONFIG_H */
