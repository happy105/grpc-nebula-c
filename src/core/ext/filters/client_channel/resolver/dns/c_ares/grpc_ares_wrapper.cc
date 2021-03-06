/*
 *
 * Copyright 2016 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <grpc/support/port_platform.h>

#if GRPC_ARES == 1 && !defined(GRPC_UV)

#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/lib/iomgr/sockaddr.h"

#include <string.h>
#include <sys/types.h>

#include <ares.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>

#include <address_sorting/address_sorting.h>
#include "src/core/ext/filters/client_channel/parse_address.h"
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_ev_driver.h"
#include "src/core/lib/gpr/host_port.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/executor.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/nameser.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"

static gpr_once g_basic_init = GPR_ONCE_INIT;
static gpr_mu g_init_mu;

grpc_core::TraceFlag grpc_trace_cares_address_sorting(false,
                                                      "cares_address_sorting");

grpc_core::TraceFlag grpc_trace_cares_resolver(false, "cares_resolver");

struct grpc_ares_request {
  /** indicates the DNS server to use, if specified */
  struct ares_addr_port_node dns_server_addr;
  /** following members are set in grpc_resolve_address_ares_impl */
  /** closure to call when the request completes */
  grpc_closure* on_done;
  /** the pointer to receive the resolved addresses */
  grpc_lb_addresses** lb_addrs_out;
  /** the pointer to receive the service config in JSON */
  char** service_config_json_out;
  /** the evernt driver used by this request */
  grpc_ares_ev_driver* ev_driver;
  /** number of ongoing queries */
  size_t pending_queries;

  /** is there at least one successful query, set in on_done_cb */
  bool success;
  /** the errors explaining the request failure, set in on_done_cb */
  grpc_error* error;
};

typedef struct grpc_ares_hostbyname_request {
  /** following members are set in create_hostbyname_request_locked
   */
  /** the top-level request instance */
  grpc_ares_request* parent_request;
  /** host to resolve, parsed from the name to resolve */
  char* host;
  /** port to fill in sockaddr_in, parsed from the name to resolve */
  uint16_t port;
  /** is it a grpclb address */
  bool is_balancer;
} grpc_ares_hostbyname_request;

static void do_basic_init(void) { gpr_mu_init(&g_init_mu); }

static void log_address_sorting_list(grpc_lb_addresses* lb_addrs,
                                     const char* input_output_str) {
  for (size_t i = 0; i < lb_addrs->num_addresses; i++) {
    char* addr_str;
    if (grpc_sockaddr_to_string(&addr_str, &lb_addrs->addresses[i].address,
                                true)) {
      gpr_log(GPR_DEBUG, "c-ares address sorting: %s[%" PRIuPTR "]=%s",
              input_output_str, i, addr_str);
      gpr_free(addr_str);
    } else {
      gpr_log(GPR_DEBUG,
              "c-ares address sorting: %s[%" PRIuPTR "]=<unprintable>",
              input_output_str, i);
    }
  }
}

void grpc_cares_wrapper_address_sorting_sort(grpc_lb_addresses* lb_addrs) {
  if (grpc_trace_cares_address_sorting.enabled()) {
    log_address_sorting_list(lb_addrs, "input");
  }
  address_sorting_sortable* sortables = (address_sorting_sortable*)gpr_zalloc(
      sizeof(address_sorting_sortable) * lb_addrs->num_addresses);
  for (size_t i = 0; i < lb_addrs->num_addresses; i++) {
    sortables[i].user_data = &lb_addrs->addresses[i];
    memcpy(&sortables[i].dest_addr.addr, &lb_addrs->addresses[i].address.addr,
           lb_addrs->addresses[i].address.len);
    sortables[i].dest_addr.len = lb_addrs->addresses[i].address.len;
  }
  address_sorting_rfc_6724_sort(sortables, lb_addrs->num_addresses);
  grpc_lb_address* sorted_lb_addrs = (grpc_lb_address*)gpr_zalloc(
      sizeof(grpc_lb_address) * lb_addrs->num_addresses);
  for (size_t i = 0; i < lb_addrs->num_addresses; i++) {
    sorted_lb_addrs[i] = *(grpc_lb_address*)sortables[i].user_data;
  }
  gpr_free(sortables);
  gpr_free(lb_addrs->addresses);
  lb_addrs->addresses = sorted_lb_addrs;
  if (grpc_trace_cares_address_sorting.enabled()) {
    log_address_sorting_list(lb_addrs, "output");
  }
}

static void grpc_ares_request_ref_locked(grpc_ares_request* r) {
  r->pending_queries++;
}

static void grpc_ares_request_unref_locked(grpc_ares_request* r) {
  r->pending_queries--;
  if (r->pending_queries == 0u) {
    grpc_ares_ev_driver_on_queries_complete_locked(r->ev_driver);
  }
}

void grpc_ares_complete_request_locked(grpc_ares_request* r) {
  /* Invoke on_done callback and destroy the
     request */
  r->ev_driver = nullptr;
  grpc_lb_addresses* lb_addrs = *(r->lb_addrs_out);
  if (lb_addrs != nullptr) {
    grpc_cares_wrapper_address_sorting_sort(lb_addrs);
  }
  GRPC_CLOSURE_SCHED(r->on_done, r->error);
}

static grpc_ares_hostbyname_request* create_hostbyname_request_locked(
    grpc_ares_request* parent_request, char* host, uint16_t port,
    bool is_balancer) {
  grpc_ares_hostbyname_request* hr = static_cast<grpc_ares_hostbyname_request*>(
      gpr_zalloc(sizeof(grpc_ares_hostbyname_request)));
  hr->parent_request = parent_request;
  hr->host = gpr_strdup(host);
  hr->port = port;
  hr->is_balancer = is_balancer;
  grpc_ares_request_ref_locked(parent_request);
  return hr;
}

static void destroy_hostbyname_request_locked(
    grpc_ares_hostbyname_request* hr) {
  grpc_ares_request_unref_locked(hr->parent_request);
  gpr_free(hr->host);
  gpr_free(hr);
}

static void on_hostbyname_done_locked(void* arg, int status, int timeouts,
                                      struct hostent* hostent) {
  grpc_ares_hostbyname_request* hr =
      static_cast<grpc_ares_hostbyname_request*>(arg);
  grpc_ares_request* r = hr->parent_request;
  if (status == ARES_SUCCESS) {
    GRPC_ERROR_UNREF(r->error);
    r->error = GRPC_ERROR_NONE;
    r->success = true;
    grpc_lb_addresses** lb_addresses = r->lb_addrs_out;
    if (*lb_addresses == nullptr) {
      *lb_addresses = grpc_lb_addresses_create(0, nullptr);
    }
    size_t prev_naddr = (*lb_addresses)->num_addresses;
    size_t i;
    for (i = 0; hostent->h_addr_list[i] != nullptr; i++) {
    }
    (*lb_addresses)->num_addresses += i;
    (*lb_addresses)->addresses = static_cast<grpc_lb_address*>(
        gpr_realloc((*lb_addresses)->addresses,
                    sizeof(grpc_lb_address) * (*lb_addresses)->num_addresses));
    for (i = prev_naddr; i < (*lb_addresses)->num_addresses; i++) {
      switch (hostent->h_addrtype) {
        case AF_INET6: {
          size_t addr_len = sizeof(struct sockaddr_in6);
          struct sockaddr_in6 addr;
          memset(&addr, 0, addr_len);
          memcpy(&addr.sin6_addr, hostent->h_addr_list[i - prev_naddr],
                 sizeof(struct in6_addr));
          addr.sin6_family = static_cast<unsigned char>(hostent->h_addrtype);
          addr.sin6_port = hr->port;
          grpc_lb_addresses_set_address(
              *lb_addresses, i, &addr, addr_len,
              hr->is_balancer /* is_balancer */,
              hr->is_balancer ? hr->host : nullptr /* balancer_name */,
              nullptr /* user_data */);
          char output[INET6_ADDRSTRLEN];
          ares_inet_ntop(AF_INET6, &addr.sin6_addr, output, INET6_ADDRSTRLEN);
          gpr_log(GPR_DEBUG,
                  "c-ares resolver gets a AF_INET6 result: \n"
                  "  addr: %s\n  port: %d\n  sin6_scope_id: %d\n",
                  output, ntohs(hr->port), addr.sin6_scope_id);
          break;
        }
        case AF_INET: {
          size_t addr_len = sizeof(struct sockaddr_in);
          struct sockaddr_in addr;
          memset(&addr, 0, addr_len);
          memcpy(&addr.sin_addr, hostent->h_addr_list[i - prev_naddr],
                 sizeof(struct in_addr));
          addr.sin_family = static_cast<unsigned char>(hostent->h_addrtype);
          addr.sin_port = hr->port;
          grpc_lb_addresses_set_address(
              *lb_addresses, i, &addr, addr_len,
              hr->is_balancer /* is_balancer */,
              hr->is_balancer ? hr->host : nullptr /* balancer_name */,
              nullptr /* user_data */);
          char output[INET_ADDRSTRLEN];
          ares_inet_ntop(AF_INET, &addr.sin_addr, output, INET_ADDRSTRLEN);
          gpr_log(GPR_DEBUG,
                  "c-ares resolver gets a AF_INET result: \n"
                  "  addr: %s\n  port: %d\n",
                  output, ntohs(hr->port));
          break;
        }
      }
    }
  } else if (!r->success) {
    char* error_msg;
    gpr_asprintf(&error_msg, "C-ares status is not ARES_SUCCESS: %s",
                 ares_strerror(status));
    grpc_error* error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(error_msg);
    gpr_free(error_msg);
    if (r->error == GRPC_ERROR_NONE) {
      r->error = error;
    } else {
      r->error = grpc_error_add_child(error, r->error);
    }
  }
  destroy_hostbyname_request_locked(hr);
}

static void on_srv_query_done_locked(void* arg, int status, int timeouts,
                                     unsigned char* abuf, int alen) {
  grpc_ares_request* r = static_cast<grpc_ares_request*>(arg);
  gpr_log(GPR_DEBUG, "on_query_srv_done_locked");
  if (status == ARES_SUCCESS) {
    gpr_log(GPR_DEBUG, "on_query_srv_done_locked ARES_SUCCESS");
    struct ares_srv_reply* reply;
    const int parse_status = ares_parse_srv_reply(abuf, alen, &reply);
    if (parse_status == ARES_SUCCESS) {
      ares_channel* channel =
          grpc_ares_ev_driver_get_channel_locked(r->ev_driver);
      for (struct ares_srv_reply* srv_it = reply; srv_it != nullptr;
           srv_it = srv_it->next) {
        if (grpc_ares_query_ipv6()) {
          grpc_ares_hostbyname_request* hr = create_hostbyname_request_locked(
              r, srv_it->host, htons(srv_it->port), true /* is_balancer */);
          ares_gethostbyname(*channel, hr->host, AF_INET6,
                             on_hostbyname_done_locked, hr);
        }
        grpc_ares_hostbyname_request* hr = create_hostbyname_request_locked(
            r, srv_it->host, htons(srv_it->port), true /* is_balancer */);
        ares_gethostbyname(*channel, hr->host, AF_INET,
                           on_hostbyname_done_locked, hr);
        grpc_ares_ev_driver_start_locked(r->ev_driver);
      }
    }
    if (reply != nullptr) {
      ares_free_data(reply);
    }
  } else if (!r->success) {
    char* error_msg;
    gpr_asprintf(&error_msg, "C-ares status is not ARES_SUCCESS: %s",
                 ares_strerror(status));
    grpc_error* error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(error_msg);
    gpr_free(error_msg);
    if (r->error == GRPC_ERROR_NONE) {
      r->error = error;
    } else {
      r->error = grpc_error_add_child(error, r->error);
    }
  }
  grpc_ares_request_unref_locked(r);
}

static const char g_service_config_attribute_prefix[] = "grpc_config=";

static void on_txt_done_locked(void* arg, int status, int timeouts,
                               unsigned char* buf, int len) {
  gpr_log(GPR_DEBUG, "on_txt_done_locked");
  char* error_msg;
  grpc_ares_request* r = static_cast<grpc_ares_request*>(arg);
  const size_t prefix_len = sizeof(g_service_config_attribute_prefix) - 1;
  struct ares_txt_ext* result = nullptr;
  struct ares_txt_ext* reply = nullptr;
  grpc_error* error = GRPC_ERROR_NONE;
  if (status != ARES_SUCCESS) goto fail;
  status = ares_parse_txt_reply_ext(buf, len, &reply);
  if (status != ARES_SUCCESS) goto fail;
  // Find service config in TXT record.
  for (result = reply; result != nullptr; result = result->next) {
    if (result->record_start &&
        memcmp(result->txt, g_service_config_attribute_prefix, prefix_len) ==
            0) {
      break;
    }
  }
  // Found a service config record.
  if (result != nullptr) {
    size_t service_config_len = result->length - prefix_len;
    *r->service_config_json_out =
        static_cast<char*>(gpr_malloc(service_config_len + 1));
    memcpy(*r->service_config_json_out, result->txt + prefix_len,
           service_config_len);
    for (result = result->next; result != nullptr && !result->record_start;
         result = result->next) {
      *r->service_config_json_out = static_cast<char*>(
          gpr_realloc(*r->service_config_json_out,
                      service_config_len + result->length + 1));
      memcpy(*r->service_config_json_out + service_config_len, result->txt,
             result->length);
      service_config_len += result->length;
    }
    (*r->service_config_json_out)[service_config_len] = '\0';
    gpr_log(GPR_INFO, "found service config: %s", *r->service_config_json_out);
  }
  // Clean up.
  ares_free_data(reply);
  goto done;
fail:
  gpr_asprintf(&error_msg, "C-ares TXT lookup status is not ARES_SUCCESS: %s",
               ares_strerror(status));
  error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(error_msg);
  gpr_free(error_msg);
  if (r->error == GRPC_ERROR_NONE) {
    r->error = error;
  } else {
    r->error = grpc_error_add_child(error, r->error);
  }
done:
  grpc_ares_request_unref_locked(r);
}

void grpc_dns_lookup_ares_continue_after_check_localhost_and_ip_literals_locked(
    grpc_ares_request* r, const char* dns_server, const char* name,
    const char* default_port, grpc_pollset_set* interested_parties,
    bool check_grpclb, int query_timeout_ms, grpc_combiner* combiner) {
  grpc_error* error = GRPC_ERROR_NONE;
  grpc_ares_hostbyname_request* hr = nullptr;
  ares_channel* channel = nullptr;
  /* TODO(zyc): Enable tracing after #9603 is checked in */
  /* if (grpc_dns_trace) {
      gpr_log(GPR_DEBUG, "resolve_address (blocking): name=%s, default_port=%s",
              name, default_port);
     } */

  /* parse name, splitting it into host and port parts */
  char* host;
  char* port;
  gpr_split_host_port(name, &host, &port);
  if (host == nullptr) {
    error = grpc_error_set_str(
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("unparseable host:port"),
        GRPC_ERROR_STR_TARGET_ADDRESS, grpc_slice_from_copied_string(name));
    goto error_cleanup;
  } else if (port == nullptr) {
    if (default_port == nullptr) {
      error = grpc_error_set_str(
          GRPC_ERROR_CREATE_FROM_STATIC_STRING("no port in name"),
          GRPC_ERROR_STR_TARGET_ADDRESS, grpc_slice_from_copied_string(name));
      goto error_cleanup;
    }
    port = gpr_strdup(default_port);
  }
  error = grpc_ares_ev_driver_create_locked(&r->ev_driver, interested_parties,
                                            query_timeout_ms, combiner, r);
  if (error != GRPC_ERROR_NONE) goto error_cleanup;
  channel = grpc_ares_ev_driver_get_channel_locked(r->ev_driver);
  // If dns_server is specified, use it.
  if (dns_server != nullptr) {
    gpr_log(GPR_INFO, "Using DNS server %s", dns_server);
    grpc_resolved_address addr;
    if (grpc_parse_ipv4_hostport(dns_server, &addr, false /* log_errors */)) {
      r->dns_server_addr.family = AF_INET;
      struct sockaddr_in* in = reinterpret_cast<struct sockaddr_in*>(addr.addr);
      memcpy(&r->dns_server_addr.addr.addr4, &in->sin_addr,
             sizeof(struct in_addr));
      r->dns_server_addr.tcp_port = grpc_sockaddr_get_port(&addr);
      r->dns_server_addr.udp_port = grpc_sockaddr_get_port(&addr);
    } else if (grpc_parse_ipv6_hostport(dns_server, &addr,
                                        false /* log_errors */)) {
      r->dns_server_addr.family = AF_INET6;
      struct sockaddr_in6* in6 =
          reinterpret_cast<struct sockaddr_in6*>(addr.addr);
      memcpy(&r->dns_server_addr.addr.addr6, &in6->sin6_addr,
             sizeof(struct in6_addr));
      r->dns_server_addr.tcp_port = grpc_sockaddr_get_port(&addr);
      r->dns_server_addr.udp_port = grpc_sockaddr_get_port(&addr);
    } else {
      error = grpc_error_set_str(
          GRPC_ERROR_CREATE_FROM_STATIC_STRING("cannot parse authority"),
          GRPC_ERROR_STR_TARGET_ADDRESS, grpc_slice_from_copied_string(name));
      goto error_cleanup;
    }
    int status = ares_set_servers_ports(*channel, &r->dns_server_addr);
    if (status != ARES_SUCCESS) {
      char* error_msg;
      gpr_asprintf(&error_msg, "C-ares status is not ARES_SUCCESS: %s",
                   ares_strerror(status));
      error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(error_msg);
      gpr_free(error_msg);
      goto error_cleanup;
    }
  }
  r->pending_queries = 1;
  if (grpc_ares_query_ipv6()) {
    hr = create_hostbyname_request_locked(r, host, grpc_strhtons(port),
                                          false /* is_balancer */);
    ares_gethostbyname(*channel, hr->host, AF_INET6, on_hostbyname_done_locked,
                       hr);
  }
  hr = create_hostbyname_request_locked(r, host, grpc_strhtons(port),
                                        false /* is_balancer */);
  ares_gethostbyname(*channel, hr->host, AF_INET, on_hostbyname_done_locked,
                     hr);
  if (check_grpclb) {
    /* Query the SRV record */
    grpc_ares_request_ref_locked(r);
    char* service_name;
    gpr_asprintf(&service_name, "_grpclb._tcp.%s", host);
    ares_query(*channel, service_name, ns_c_in, ns_t_srv,
               on_srv_query_done_locked, r);
    gpr_free(service_name);
  }
  if (r->service_config_json_out != nullptr) {
    grpc_ares_request_ref_locked(r);
    char* config_name;
    gpr_asprintf(&config_name, "_grpc_config.%s", host);
    ares_search(*channel, config_name, ns_c_in, ns_t_txt, on_txt_done_locked,
                r);
    gpr_free(config_name);
  }
  grpc_ares_ev_driver_start_locked(r->ev_driver);
  grpc_ares_request_unref_locked(r);
  gpr_free(host);
  gpr_free(port);
  return;

error_cleanup:
  GRPC_CLOSURE_SCHED(r->on_done, error);
  gpr_free(host);
  gpr_free(port);
}

static bool inner_resolve_as_ip_literal_locked(const char* name,
                                               const char* default_port,
                                               grpc_lb_addresses** addrs,
                                               char** host, char** port,
                                               char** hostport) {
  gpr_split_host_port(name, host, port);
  if (*host == nullptr) {
    gpr_log(GPR_ERROR,
            "Failed to parse %s to host:port while attempting to resolve as ip "
            "literal.",
            name);
    return false;
  }
  if (*port == nullptr) {
    if (default_port == nullptr) {
      gpr_log(GPR_ERROR,
              "No port or default port for %s while attempting to resolve as "
              "ip literal.",
              name);
      return false;
    }
    *port = gpr_strdup(default_port);
  }
  grpc_resolved_address addr;
  GPR_ASSERT(gpr_join_host_port(hostport, *host, atoi(*port)));
  if (grpc_parse_ipv4_hostport(*hostport, &addr, false /* log errors */) ||
      grpc_parse_ipv6_hostport(*hostport, &addr, false /* log errors */)) {
    GPR_ASSERT(*addrs == nullptr);
    *addrs = grpc_lb_addresses_create(1, nullptr);
    grpc_lb_addresses_set_address(
        *addrs, 0, addr.addr, addr.len, false /* is_balancer */,
        nullptr /* balancer_name */, nullptr /* user_data */);
    return true;
  }
  return false;
}

static bool resolve_as_ip_literal_locked(const char* name,
                                         const char* default_port,
                                         grpc_lb_addresses** addrs) {
  char* host = nullptr;
  char* port = nullptr;
  char* hostport = nullptr;
  bool out = inner_resolve_as_ip_literal_locked(name, default_port, addrs,
                                                &host, &port, &hostport);
  gpr_free(host);
  gpr_free(port);
  gpr_free(hostport);
  return out;
}

static grpc_ares_request* grpc_dns_lookup_ares_locked_impl(
    const char* dns_server, const char* name, const char* default_port,
    grpc_pollset_set* interested_parties, grpc_closure* on_done,
    grpc_lb_addresses** addrs, bool check_grpclb, char** service_config_json,
    int query_timeout_ms, grpc_combiner* combiner) {
  grpc_ares_request* r =
      static_cast<grpc_ares_request*>(gpr_zalloc(sizeof(grpc_ares_request)));
  r->ev_driver = nullptr;
  r->on_done = on_done;
  r->lb_addrs_out = addrs;
  r->service_config_json_out = service_config_json;
  r->success = false;
  r->error = GRPC_ERROR_NONE;
  r->pending_queries = 0;
  // Early out if the target is an ipv4 or ipv6 literal.
  if (resolve_as_ip_literal_locked(name, default_port, addrs)) {
    GRPC_CLOSURE_SCHED(on_done, GRPC_ERROR_NONE);
    return r;
  }
  // Early out if the target is localhost and we're on Windows.
  if (grpc_ares_maybe_resolve_localhost_manually_locked(name, default_port,
                                                        addrs)) {
    GRPC_CLOSURE_SCHED(on_done, GRPC_ERROR_NONE);
    return r;
  }
  // Look up name using c-ares lib.
  grpc_dns_lookup_ares_continue_after_check_localhost_and_ip_literals_locked(
      r, dns_server, name, default_port, interested_parties, check_grpclb,
      query_timeout_ms, combiner);
  return r;
}

grpc_ares_request* (*grpc_dns_lookup_ares_locked)(
    const char* dns_server, const char* name, const char* default_port,
    grpc_pollset_set* interested_parties, grpc_closure* on_done,
    grpc_lb_addresses** addrs, bool check_grpclb, char** service_config_json,
    int query_timeout_ms,
    grpc_combiner* combiner) = grpc_dns_lookup_ares_locked_impl;

static void grpc_cancel_ares_request_locked_impl(grpc_ares_request* r) {
  GPR_ASSERT(r != nullptr);
  if (r->ev_driver != nullptr) {
    grpc_ares_ev_driver_shutdown_locked(r->ev_driver);
  }
}

void (*grpc_cancel_ares_request_locked)(grpc_ares_request* r) =
    grpc_cancel_ares_request_locked_impl;

grpc_error* grpc_ares_init(void) {
  gpr_once_init(&g_basic_init, do_basic_init);
  gpr_mu_lock(&g_init_mu);
  int status = ares_library_init(ARES_LIB_INIT_ALL);
  gpr_mu_unlock(&g_init_mu);

  if (status != ARES_SUCCESS) {
    char* error_msg;
    gpr_asprintf(&error_msg, "ares_library_init failed: %s",
                 ares_strerror(status));
    grpc_error* error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(error_msg);
    gpr_free(error_msg);
    return error;
  }
  return GRPC_ERROR_NONE;
}

void grpc_ares_cleanup(void) {
  gpr_mu_lock(&g_init_mu);
  ares_library_cleanup();
  gpr_mu_unlock(&g_init_mu);
}

/*
 * grpc_resolve_address_ares related structs and functions
 */

typedef struct grpc_resolve_address_ares_request {
  /* combiner that queries and related callbacks run under */
  grpc_combiner* combiner;
  /** the pointer to receive the resolved addresses */
  grpc_resolved_addresses** addrs_out;
  /** currently resolving lb addresses */
  grpc_lb_addresses* lb_addrs;
  /** closure to call when the resolve_address_ares request completes */
  grpc_closure* on_resolve_address_done;
  /** a closure wrapping on_resolve_address_done, which should be invoked when
     the grpc_dns_lookup_ares_locked operation is done. */
  grpc_closure on_dns_lookup_done_locked;
  /* target name */
  const char* name;
  /* default port to use if none is specified */
  const char* default_port;
  /* pollset_set to be driven by */
  grpc_pollset_set* interested_parties;
  /* underlying ares_request that the query is performed on */
  grpc_ares_request* ares_request;
} grpc_resolve_address_ares_request;

static void on_dns_lookup_done_locked(void* arg, grpc_error* error) {
  grpc_resolve_address_ares_request* r =
      static_cast<grpc_resolve_address_ares_request*>(arg);
  gpr_free(r->ares_request);
  grpc_resolved_addresses** resolved_addresses = r->addrs_out;
  if (r->lb_addrs == nullptr || r->lb_addrs->num_addresses == 0) {
    *resolved_addresses = nullptr;
  } else {
    *resolved_addresses = static_cast<grpc_resolved_addresses*>(
        gpr_zalloc(sizeof(grpc_resolved_addresses)));
    (*resolved_addresses)->naddrs = r->lb_addrs->num_addresses;
    (*resolved_addresses)->addrs =
        static_cast<grpc_resolved_address*>(gpr_zalloc(
            sizeof(grpc_resolved_address) * (*resolved_addresses)->naddrs));
    for (size_t i = 0; i < (*resolved_addresses)->naddrs; i++) {
      GPR_ASSERT(!r->lb_addrs->addresses[i].is_balancer);
      memcpy(&(*resolved_addresses)->addrs[i],
             &r->lb_addrs->addresses[i].address, sizeof(grpc_resolved_address));
    }
  }
  GRPC_CLOSURE_SCHED(r->on_resolve_address_done, GRPC_ERROR_REF(error));
  if (r->lb_addrs != nullptr) grpc_lb_addresses_destroy(r->lb_addrs);
  GRPC_COMBINER_UNREF(r->combiner, "on_dns_lookup_done_cb");
  gpr_free(r);
}

static void grpc_resolve_address_invoke_dns_lookup_ares_locked(
    void* arg, grpc_error* unused_error) {
  grpc_resolve_address_ares_request* r =
      static_cast<grpc_resolve_address_ares_request*>(arg);
  r->ares_request = grpc_dns_lookup_ares_locked(
      nullptr /* dns_server */, r->name, r->default_port, r->interested_parties,
      &r->on_dns_lookup_done_locked, &r->lb_addrs, false /* check_grpclb */,
      nullptr /* service_config_json */, GRPC_DNS_ARES_DEFAULT_QUERY_TIMEOUT_MS,
      r->combiner);
}

static void grpc_resolve_address_ares_impl(const char* name,
                                           const char* default_port,
                                           grpc_pollset_set* interested_parties,
                                           grpc_closure* on_done,
                                           grpc_resolved_addresses** addrs) {
  grpc_resolve_address_ares_request* r =
      static_cast<grpc_resolve_address_ares_request*>(
          gpr_zalloc(sizeof(grpc_resolve_address_ares_request)));
  r->combiner = grpc_combiner_create();
  r->addrs_out = addrs;
  r->on_resolve_address_done = on_done;
  GRPC_CLOSURE_INIT(&r->on_dns_lookup_done_locked, on_dns_lookup_done_locked, r,
                    grpc_combiner_scheduler(r->combiner));
  r->name = name;
  r->default_port = default_port;
  r->interested_parties = interested_parties;
  GRPC_CLOSURE_SCHED(
      GRPC_CLOSURE_CREATE(grpc_resolve_address_invoke_dns_lookup_ares_locked, r,
                          grpc_combiner_scheduler(r->combiner)),
      GRPC_ERROR_NONE);
}

void (*grpc_resolve_address_ares)(
    const char* name, const char* default_port,
    grpc_pollset_set* interested_parties, grpc_closure* on_done,
    grpc_resolved_addresses** addrs) = grpc_resolve_address_ares_impl;

#endif /* GRPC_ARES == 1 && !defined(GRPC_UV) */
