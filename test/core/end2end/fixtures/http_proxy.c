/*
 *
 * Copyright 2016, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "test/core/end2end/fixtures/http_proxy.h"

#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/log.h>
#include <grpc/support/slice_buffer.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include <grpc/support/thd.h>
#include <grpc/support/useful.h>

#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/http/parser.h"
#include "src/core/lib/iomgr/closure.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/pollset.h"
#include "src/core/lib/iomgr/pollset_set.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/iomgr/tcp_client.h"
#include "src/core/lib/iomgr/tcp_server.h"
#include "test/core/util/port.h"

struct grpc_end2end_http_proxy {
  char* proxy_name;
  gpr_thd_id thd;
  grpc_tcp_server* server;
  grpc_channel_args* channel_args;
  gpr_mu* mu;
  grpc_pollset* pollset;
  bool shutdown;
};

//
// Connection handling
//

typedef struct proxy_connection {
  grpc_endpoint* client_endpoint;
  grpc_endpoint* server_endpoint;

  gpr_refcount refcount;
  bool client_shutdown;
  bool server_shutdown;
  bool client_write_pending;
  bool server_write_pending;

size_t client_bytes_read;
size_t client_bytes_written;
size_t server_bytes_read;
size_t server_bytes_written;

  grpc_pollset_set* pollset_set;

  grpc_closure on_read_request_done;
  grpc_closure on_server_connect_done;
  grpc_closure on_write_response_done;
  grpc_closure on_client_read_done;
  grpc_closure on_client_write_done;
  grpc_closure on_server_read_done;
  grpc_closure on_server_write_done;

  gpr_slice_buffer client_read_buffer;
  gpr_slice_buffer client_write_buffer;
  gpr_slice_buffer server_read_buffer;
  gpr_slice_buffer server_write_buffer;

  grpc_http_parser http_parser;
  grpc_http_request http_request;

  grpc_end2end_http_proxy* proxy;  // Does not own.
} proxy_connection;

// Helper function to destroy the proxy connection.
static void proxy_connection_destroy(grpc_exec_ctx* exec_ctx,
                                     proxy_connection* conn) {
gpr_log(GPR_INFO, "==> %s()", __func__);
gpr_log(GPR_INFO, "client_bytes_read=%lu", conn->client_bytes_read);
gpr_log(GPR_INFO, "server_bytes_written=%lu", conn->server_bytes_written);
gpr_log(GPR_INFO, "server_bytes_read=%lu", conn->server_bytes_read);
gpr_log(GPR_INFO, "client_bytes_written=%lu", conn->client_bytes_written);
  // Tell the server to shut down when this connection is closed.
  conn->proxy->shutdown = true;
  grpc_endpoint_destroy(exec_ctx, conn->client_endpoint);
  if (conn->server_endpoint != NULL)
    grpc_endpoint_destroy(exec_ctx, conn->server_endpoint);
  grpc_pollset_set_destroy(conn->pollset_set);
  gpr_slice_buffer_destroy(&conn->client_read_buffer);
  gpr_slice_buffer_destroy(&conn->client_write_buffer);
  gpr_slice_buffer_destroy(&conn->server_read_buffer);
  gpr_slice_buffer_destroy(&conn->server_write_buffer);
  grpc_http_parser_destroy(&conn->http_parser);
  grpc_http_request_destroy(&conn->http_request);
  gpr_free(conn);
}

// Helper function to shut down the proxy connection.
// Does NOT take ownership of a reference to error.
static void proxy_connection_failed(grpc_exec_ctx* exec_ctx,
                                    proxy_connection* conn, bool is_client,
                                    const char* prefix, grpc_error* error) {
gpr_log(GPR_INFO, "==> %s()", __func__);
  const char* msg = grpc_error_string(error);
  gpr_log(GPR_ERROR, "%s: %s", prefix, msg);
  grpc_error_free_string(msg);
  if (is_client || !conn->client_write_pending) {
    grpc_endpoint_shutdown(exec_ctx, conn->client_endpoint);
    conn->client_shutdown = true;
  }
  if (!is_client || !conn->server_write_pending) {
    grpc_endpoint_shutdown(exec_ctx, conn->server_endpoint);
    conn->server_shutdown = true;
  }
  if (gpr_unref(&conn->refcount))
    proxy_connection_destroy(exec_ctx, conn);
}

// Forward declarations.
static void do_client_read(grpc_exec_ctx* exec_ctx, proxy_connection* conn);
static void do_server_read(grpc_exec_ctx* exec_ctx, proxy_connection* conn);

// Callback for writing proxy data to the client.
static void on_client_write_done(grpc_exec_ctx* exec_ctx, void* arg,
                                 grpc_error* error) {
gpr_log(GPR_INFO, "==> %s()", __func__);
  proxy_connection* conn = arg;
  conn->client_write_pending = false;
  if (error != GRPC_ERROR_NONE) {
    proxy_connection_failed(exec_ctx, conn, true /* is_client */,
                            "HTTP proxy client write", error);
    return;
  }
  gpr_unref(&conn->refcount);
  // Clear write buffer.
gpr_log(GPR_INFO, "wrote %lu bytes to client", conn->client_write_buffer.length);
conn->client_bytes_written += conn->client_write_buffer.length;
  gpr_slice_buffer_reset_and_unref(&conn->client_write_buffer);
  // If the server has been shut down, shut down the client now.
  if (conn->server_shutdown)
    grpc_endpoint_shutdown(exec_ctx, conn->client_endpoint);
}

// Callback for writing proxy data to the backend server.
static void on_server_write_done(grpc_exec_ctx* exec_ctx, void* arg,
                                 grpc_error* error) {
gpr_log(GPR_INFO, "==> %s()", __func__);
  proxy_connection* conn = arg;
  conn->server_write_pending = false;
  if (error != GRPC_ERROR_NONE) {
    proxy_connection_failed(exec_ctx, conn, false /* is_client */,
                            "HTTP proxy server write", error);
    return;
  }
  gpr_unref(&conn->refcount);
  // Clear write buffer.
gpr_log(GPR_INFO, "wrote %lu bytes to server", conn->server_write_buffer.length);
conn->server_bytes_written += conn->server_write_buffer.length;
  gpr_slice_buffer_reset_and_unref(&conn->server_write_buffer);
  // If the client has been shut down, shut down the server now.
  if (conn->client_shutdown)
    grpc_endpoint_shutdown(exec_ctx, conn->server_endpoint);
}

// Callback for reading data from the client, which will be proxied to
// the backend server.
static void on_client_read_done(grpc_exec_ctx* exec_ctx, void* arg,
                                grpc_error* error) {
gpr_log(GPR_INFO, "==> %s()", __func__);
  proxy_connection* conn = arg;
  if (error != GRPC_ERROR_NONE) {
    proxy_connection_failed(exec_ctx, conn, true /* is_client */,
                            "HTTP proxy client read", error);
    return;
  }
  // Move read data into write buffer and write it.
  // The write operation inherits our reference to conn.
gpr_log(GPR_INFO, "read %lu bytes from client", conn->client_read_buffer.length);
conn->client_bytes_read += conn->client_read_buffer.length;
  gpr_slice_buffer_move_into(&conn->client_read_buffer,
                             &conn->server_write_buffer);
  conn->server_write_pending = true;
  grpc_endpoint_write(exec_ctx, conn->server_endpoint,
                      &conn->server_write_buffer, &conn->on_server_write_done);
  // Read more data.
  do_client_read(exec_ctx, conn);
}

// Callback for reading data from the backend server, which will be
// proxied to the client.
static void on_server_read_done(grpc_exec_ctx* exec_ctx, void* arg,
                                grpc_error* error) {
gpr_log(GPR_INFO, "==> %s()", __func__);
  proxy_connection* conn = arg;
  if (error != GRPC_ERROR_NONE) {
    proxy_connection_failed(exec_ctx, conn, false /* is_client */,
                            "HTTP proxy server read", error);
    return;
  }
  // Move read data into write buffer and write it.
  // The write operation inherits our reference to conn.
gpr_log(GPR_INFO, "read %lu bytes from server", conn->server_read_buffer.length);
conn->server_bytes_read += conn->server_read_buffer.length;
  gpr_slice_buffer_move_into(&conn->server_read_buffer,
                             &conn->client_write_buffer);
  conn->client_write_pending = true;
  grpc_endpoint_write(exec_ctx, conn->client_endpoint,
                      &conn->client_write_buffer, &conn->on_client_write_done);
  // Read more data.
  do_server_read(exec_ctx, conn);
}

// Callback to write the HTTP response for the CONNECT request.
static void on_write_response_done(grpc_exec_ctx* exec_ctx, void* arg,
                                   grpc_error* error) {
gpr_log(GPR_INFO, "==> %s()", __func__);
  proxy_connection* conn = arg;
  if (error != GRPC_ERROR_NONE) {
    proxy_connection_failed(exec_ctx, conn, true /* is_client */,
                            "HTTP proxy write response", error);
    return;
  }
  gpr_unref(&conn->refcount);
  // Clear write buffer.
  gpr_slice_buffer_reset_and_unref(&conn->client_write_buffer);
  // Start reading from both client and server.
  do_client_read(exec_ctx, conn);
  do_server_read(exec_ctx, conn);
}

// Start a read from the client.
static void do_client_read(grpc_exec_ctx* exec_ctx, proxy_connection* conn) {
  gpr_ref(&conn->refcount);
  grpc_endpoint_read(exec_ctx, conn->client_endpoint, &conn->client_read_buffer,
                     &conn->on_client_read_done);
}

// Start a read from the server.
static void do_server_read(grpc_exec_ctx* exec_ctx, proxy_connection* conn) {
  gpr_ref(&conn->refcount);
  grpc_endpoint_read(exec_ctx, conn->server_endpoint, &conn->server_read_buffer,
                     &conn->on_server_read_done);
}

// Callback to connect to the backend server specified by the HTTP
// CONNECT request.
static void on_server_connect_done(grpc_exec_ctx* exec_ctx, void* arg,
                                   grpc_error* error) {
gpr_log(GPR_INFO, "==> %s()", __func__);
  proxy_connection* conn = arg;
  if (error != GRPC_ERROR_NONE) {
    // TODO(roth): Technically, in this case, we should handle the error
    // by returning an HTTP response to the client indicating that the
    // connection failed.  However, for the purposes of this test code,
    // it's fine to pretend this is a client-side error, which will
    // cause the client connection to be dropped.
    proxy_connection_failed(exec_ctx, conn, true /* is_client */,
                            "HTTP proxy server connect", error);
    return;
  }
  // We've established a connection, so send back a 200 response code to
  // the client.
  // The write callback inherits our reference to conn.
  gpr_slice slice =
      gpr_slice_from_copied_string("HTTP/1.0 200 connected\r\n\r\n");
  gpr_slice_buffer_add(&conn->client_write_buffer, slice);
  grpc_endpoint_write(exec_ctx, conn->client_endpoint,
                      &conn->client_write_buffer,
                      &conn->on_write_response_done);
}

// Callback to read the HTTP CONNECT request.
// TODO(roth): Technically, for any of the failure modes handled by this
// function, we should handle the error by returning an HTTP response to
// the client indicating that the request failed.  However, for the purposes
// of this test code, it's fine to pretend this is a client-side error,
// which will cause the client connection to be dropped.
static void on_read_request_done(grpc_exec_ctx* exec_ctx, void* arg,
                                 grpc_error* error) {
gpr_log(GPR_INFO, "==> %s()", __func__);
  proxy_connection* conn = arg;
  if (error != GRPC_ERROR_NONE) {
    proxy_connection_failed(exec_ctx, conn, true /* is_client */,
                            "HTTP proxy read request", error);
    return;
  }
  // Read request and feed it to the parser.
  for (size_t i = 0; i < conn->client_read_buffer.count; ++i) {
    if (GPR_SLICE_LENGTH(conn->client_read_buffer.slices[i]) > 0) {
      error = grpc_http_parser_parse(
          &conn->http_parser, conn->client_read_buffer.slices[i], NULL);
      if (error != GRPC_ERROR_NONE) {
        proxy_connection_failed(exec_ctx, conn, true /* is_client */,
                                "HTTP proxy request parse", error);
        GRPC_ERROR_UNREF(error);
        return;
      }
    }
  }
  gpr_slice_buffer_reset_and_unref(&conn->client_read_buffer);
  // If we're not done reading the request, read more data.
  if (conn->http_parser.state != GRPC_HTTP_BODY) {
    grpc_endpoint_read(exec_ctx, conn->client_endpoint,
                       &conn->client_read_buffer, &conn->on_read_request_done);
    return;
  }
  // Make sure we got a CONNECT request.
  if (strcmp(conn->http_request.method, "CONNECT") != 0) {
    char* msg;
    gpr_asprintf(&msg, "HTTP proxy got request method %s",
                 conn->http_request.method);
    error = GRPC_ERROR_CREATE(msg);
    gpr_free(msg);
    proxy_connection_failed(exec_ctx, conn, true /* is_client */,
                            "HTTP proxy read request", error);
    GRPC_ERROR_UNREF(error);
    return;
  }
  // Resolve address.
  grpc_resolved_addresses* resolved_addresses = NULL;
  error = grpc_blocking_resolve_address(conn->http_request.path, "80",
                                        &resolved_addresses);
  if (error != GRPC_ERROR_NONE) {
    proxy_connection_failed(exec_ctx, conn, true /* is_client */,
                            "HTTP proxy DNS lookup", error);
    GRPC_ERROR_UNREF(error);
    return;
  }
  GPR_ASSERT(resolved_addresses->naddrs >= 1);
  // Connect to requested address.
  // The connection callback inherits our reference to conn.
  const gpr_timespec deadline = gpr_time_add(
      gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_seconds(10, GPR_TIMESPAN));
  grpc_tcp_client_connect(exec_ctx, &conn->on_server_connect_done,
                          &conn->server_endpoint, conn->pollset_set,
                          (struct sockaddr*)&resolved_addresses->addrs[0].addr,
                          resolved_addresses->addrs[0].len, deadline);
  grpc_resolved_addresses_destroy(resolved_addresses);
}

static void on_accept(grpc_exec_ctx* exec_ctx, void* arg,
                      grpc_endpoint* endpoint, grpc_pollset* accepting_pollset,
                      grpc_tcp_server_acceptor* acceptor) {
gpr_log(GPR_INFO, "==> %s()", __func__);
  grpc_end2end_http_proxy* proxy = arg;
  // Instantiate proxy_connection.
  proxy_connection* conn = gpr_malloc(sizeof(*conn));
  memset(conn, 0, sizeof(*conn));
  conn->client_endpoint = endpoint;
  gpr_ref_init(&conn->refcount, 1);
  conn->pollset_set = grpc_pollset_set_create();
  grpc_pollset_set_add_pollset(exec_ctx, conn->pollset_set, proxy->pollset);
  grpc_closure_init(&conn->on_read_request_done, on_read_request_done, conn);
  grpc_closure_init(&conn->on_server_connect_done, on_server_connect_done,
                    conn);
  grpc_closure_init(&conn->on_write_response_done, on_write_response_done,
                    conn);
  grpc_closure_init(&conn->on_client_read_done, on_client_read_done, conn);
  grpc_closure_init(&conn->on_client_write_done, on_client_write_done, conn);
  grpc_closure_init(&conn->on_server_read_done, on_server_read_done, conn);
  grpc_closure_init(&conn->on_server_write_done, on_server_write_done, conn);
  gpr_slice_buffer_init(&conn->client_read_buffer);
  gpr_slice_buffer_init(&conn->client_write_buffer);
  gpr_slice_buffer_init(&conn->server_read_buffer);
  gpr_slice_buffer_init(&conn->server_write_buffer);
  grpc_http_parser_init(&conn->http_parser, GRPC_HTTP_REQUEST,
                        &conn->http_request);
  conn->proxy = proxy;
  grpc_endpoint_read(exec_ctx, conn->client_endpoint, &conn->client_read_buffer,
                     &conn->on_read_request_done);
}

//
// Proxy class
//

static void thread_main(void* arg) {
  grpc_end2end_http_proxy *proxy = arg;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  do {
    const gpr_timespec now = gpr_now(GPR_CLOCK_MONOTONIC);
    const gpr_timespec deadline =
        gpr_time_add(now, gpr_time_from_seconds(1, GPR_TIMESPAN));
    grpc_pollset_worker *worker = NULL;
    gpr_mu_lock(proxy->mu);
    GRPC_LOG_IF_ERROR("grpc_pollset_work",
                      grpc_pollset_work(&exec_ctx, proxy->pollset, &worker,
                                        now, deadline));
    gpr_mu_unlock(proxy->mu);
    grpc_exec_ctx_flush(&exec_ctx);
  } while (!proxy->shutdown);
  grpc_exec_ctx_finish(&exec_ctx);
}

grpc_end2end_http_proxy* grpc_end2end_http_proxy_create() {
  grpc_end2end_http_proxy* proxy = gpr_malloc(sizeof(*proxy));
  memset(proxy, 0, sizeof(*proxy));
  // Construct proxy address.
  const int proxy_port = grpc_pick_unused_port_or_die();
  gpr_join_host_port(&proxy->proxy_name, "localhost", proxy_port);
  gpr_log(GPR_INFO, "Proxy address: %s", proxy->proxy_name);
  // Create TCP server.
  proxy->channel_args = grpc_channel_args_copy(NULL);
  grpc_error* error = grpc_tcp_server_create(
      NULL, proxy->channel_args, &proxy->server);
  GPR_ASSERT(error == GRPC_ERROR_NONE);
  // Bind to port.
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  grpc_sockaddr_set_port((struct sockaddr*)&addr, proxy_port);
  int port;
  error = grpc_tcp_server_add_port(
      proxy->server, (struct sockaddr*)&addr, sizeof(addr), &port);
  GPR_ASSERT(error == GRPC_ERROR_NONE);
  GPR_ASSERT(port == proxy_port);
  // Start server.
  proxy->pollset = gpr_malloc(grpc_pollset_size());
  grpc_pollset_init(proxy->pollset, &proxy->mu);
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_tcp_server_start(&exec_ctx, proxy->server, &proxy->pollset, 1,
                        on_accept, proxy);
  grpc_exec_ctx_finish(&exec_ctx);
  // Start proxy thread.
  gpr_thd_options opt = gpr_thd_options_default();
  gpr_thd_options_set_joinable(&opt);
  GPR_ASSERT(gpr_thd_new(&proxy->thd, thread_main, proxy, &opt));
  return proxy;
}

static void destroy_pollset(grpc_exec_ctx *exec_ctx, void *arg,
                            grpc_error *error) {
  grpc_pollset* pollset = arg;
  grpc_pollset_destroy(pollset);
  gpr_free(pollset);
}

void grpc_end2end_http_proxy_destroy(grpc_end2end_http_proxy* proxy) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  gpr_thd_join(proxy->thd);
  grpc_tcp_server_shutdown_listeners(&exec_ctx, proxy->server);
  grpc_tcp_server_unref(&exec_ctx, proxy->server);
  gpr_free(proxy->proxy_name);
  grpc_channel_args_destroy(proxy->channel_args);
  grpc_closure destroyed;
  grpc_closure_init(&destroyed, destroy_pollset, proxy->pollset);
  grpc_pollset_shutdown(&exec_ctx, proxy->pollset, &destroyed);
  gpr_free(proxy);
  grpc_exec_ctx_finish(&exec_ctx);
}

const char *grpc_end2end_http_proxy_get_proxy_name(
    grpc_end2end_http_proxy *proxy) {
  return proxy->proxy_name;
}
