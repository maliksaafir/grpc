/*
 *
 * Copyright 2015 gRPC authors.
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

#include "src/core/lib/security/security_connector/security_connector.h"

#include <stdbool.h>
#include <string.h>
#include <dirent.h>

#include <fstream>
#include <experimental/filesystem>

#include <grpc/slice_buffer.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

#include "src/core/ext/transport/chttp2/alpn/alpn.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/handshaker.h"
#include "src/core/lib/gpr/env.h"
#include "src/core/lib/gpr/host_port.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/iomgr/load_file.h"
#include "src/core/lib/security/context/security_context.h"
#include "src/core/lib/security/credentials/credentials.h"
#include "src/core/lib/security/credentials/fake/fake_credentials.h"
#include "src/core/lib/security/credentials/ssl/ssl_credentials.h"
#include "src/core/lib/security/transport/secure_endpoint.h"
#include "src/core/lib/security/transport/security_handshaker.h"
#include "src/core/lib/security/transport/target_authority_table.h"
#include "src/core/tsi/fake_transport_security.h"
#include "src/core/tsi/ssl_transport_security.h"

grpc_core::DebugOnlyTraceFlag grpc_trace_security_connector_refcount(
    false, "security_connector_refcount");

/* -- Constants. -- */

#ifndef INSTALL_PREFIX
static const char* installed_roots_path = "/usr/share/grpc/roots.pem";
#else
static const char* installed_roots_path =
    INSTALL_PREFIX "/share/grpc/roots.pem";
#endif

#ifndef GRPC_SYSTEM_SSL_ROOTS_FLAG
#define GRPC_SYSTEM_SSL_ROOTS_FLAG "GRPC_SYSTEM_SSL_ROOTS_FLAG"
#endif

/* -- System certs identification -- */
char* use_system_certs = gpr_getenv(GRPC_SYSTEM_SSL_ROOTS_FLAG);

/* -- Overridden default roots. -- */

static grpc_ssl_roots_override_callback ssl_roots_override_cb = nullptr;

void grpc_set_ssl_roots_override_callback(grpc_ssl_roots_override_callback cb) {
  ssl_roots_override_cb = cb;
}

/* -- Cipher suites. -- */

/* Defines the cipher suites that we accept by default. All these cipher suites
   are compliant with HTTP2. */
#define GRPC_SSL_CIPHER_SUITES     \
  "ECDHE-ECDSA-AES128-GCM-SHA256:" \
  "ECDHE-ECDSA-AES256-GCM-SHA384:" \
  "ECDHE-RSA-AES128-GCM-SHA256:"   \
  "ECDHE-RSA-AES256-GCM-SHA384"

static gpr_once cipher_suites_once = GPR_ONCE_INIT;
static const char* cipher_suites = nullptr;

static void init_cipher_suites(void) {
  char* overridden = gpr_getenv("GRPC_SSL_CIPHER_SUITES");
  cipher_suites = overridden != nullptr ? overridden : GRPC_SSL_CIPHER_SUITES;
}

static const char* ssl_cipher_suites(void) {
  gpr_once_init(&cipher_suites_once, init_cipher_suites);
  return cipher_suites;
}

/* -- Common methods. -- */

/* Returns the first property with that name. */
const tsi_peer_property* tsi_peer_get_property_by_name(const tsi_peer* peer,
                                                       const char* name) {
  size_t i;
  if (peer == nullptr) return nullptr;
  for (i = 0; i < peer->property_count; i++) {
    const tsi_peer_property* property = &peer->properties[i];
    if (name == nullptr && property->name == nullptr) {
      return property;
    }
    if (name != nullptr && property->name != nullptr &&
        strcmp(property->name, name) == 0) {
      return property;
    }
  }
  return nullptr;
}

void grpc_channel_security_connector_add_handshakers(
    grpc_channel_security_connector* connector,
    grpc_handshake_manager* handshake_mgr) {
  if (connector != nullptr) {
    connector->add_handshakers(connector, handshake_mgr);
  }
}

void grpc_server_security_connector_add_handshakers(
    grpc_server_security_connector* connector,
    grpc_handshake_manager* handshake_mgr) {
  if (connector != nullptr) {
    connector->add_handshakers(connector, handshake_mgr);
  }
}

void grpc_security_connector_check_peer(grpc_security_connector* sc,
                                        tsi_peer peer,
                                        grpc_auth_context** auth_context,
                                        grpc_closure* on_peer_checked) {
  if (sc == nullptr) {
    GRPC_CLOSURE_SCHED(on_peer_checked,
                       GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                           "cannot check peer -- no security connector"));
    tsi_peer_destruct(&peer);
  } else {
    sc->vtable->check_peer(sc, peer, auth_context, on_peer_checked);
  }
}

int grpc_security_connector_cmp(grpc_security_connector* sc,
                                grpc_security_connector* other) {
  if (sc == nullptr || other == nullptr) return GPR_ICMP(sc, other);
  int c = GPR_ICMP(sc->vtable, other->vtable);
  if (c != 0) return c;
  return sc->vtable->cmp(sc, other);
}

int grpc_channel_security_connector_cmp(grpc_channel_security_connector* sc1,
                                        grpc_channel_security_connector* sc2) {
  GPR_ASSERT(sc1->channel_creds != nullptr);
  GPR_ASSERT(sc2->channel_creds != nullptr);
  int c = GPR_ICMP(sc1->channel_creds, sc2->channel_creds);
  if (c != 0) return c;
  c = GPR_ICMP(sc1->request_metadata_creds, sc2->request_metadata_creds);
  if (c != 0) return c;
  c = GPR_ICMP((void*)sc1->check_call_host, (void*)sc2->check_call_host);
  if (c != 0) return c;
  c = GPR_ICMP((void*)sc1->cancel_check_call_host,
               (void*)sc2->cancel_check_call_host);
  if (c != 0) return c;
  return GPR_ICMP((void*)sc1->add_handshakers, (void*)sc2->add_handshakers);
}

int grpc_server_security_connector_cmp(grpc_server_security_connector* sc1,
                                       grpc_server_security_connector* sc2) {
  GPR_ASSERT(sc1->server_creds != nullptr);
  GPR_ASSERT(sc2->server_creds != nullptr);
  int c = GPR_ICMP(sc1->server_creds, sc2->server_creds);
  if (c != 0) return c;
  return GPR_ICMP((void*)sc1->add_handshakers, (void*)sc2->add_handshakers);
}

bool grpc_channel_security_connector_check_call_host(
    grpc_channel_security_connector* sc, const char* host,
    grpc_auth_context* auth_context, grpc_closure* on_call_host_checked,
    grpc_error** error) {
  if (sc == nullptr || sc->check_call_host == nullptr) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "cannot check call host -- no security connector");
    return true;
  }
  return sc->check_call_host(sc, host, auth_context, on_call_host_checked,
                             error);
}

void grpc_channel_security_connector_cancel_check_call_host(
    grpc_channel_security_connector* sc, grpc_closure* on_call_host_checked,
    grpc_error* error) {
  if (sc == nullptr || sc->cancel_check_call_host == nullptr) {
    GRPC_ERROR_UNREF(error);
    return;
  }
  sc->cancel_check_call_host(sc, on_call_host_checked, error);
}

#ifndef NDEBUG
grpc_security_connector* grpc_security_connector_ref(
    grpc_security_connector* sc, const char* file, int line,
    const char* reason) {
  if (sc == nullptr) return nullptr;
  if (grpc_trace_security_connector_refcount.enabled()) {
    gpr_atm val = gpr_atm_no_barrier_load(&sc->refcount.count);
    gpr_log(file, line, GPR_LOG_SEVERITY_DEBUG,
            "SECURITY_CONNECTOR:%p   ref %" PRIdPTR " -> %" PRIdPTR " %s", sc,
            val, val + 1, reason);
  }
#else
grpc_security_connector* grpc_security_connector_ref(
    grpc_security_connector* sc) {
  if (sc == nullptr) return nullptr;
#endif
  gpr_ref(&sc->refcount);
  return sc;
}

#ifndef NDEBUG
void grpc_security_connector_unref(grpc_security_connector* sc,
                                   const char* file, int line,
                                   const char* reason) {
  if (sc == nullptr) return;
  if (grpc_trace_security_connector_refcount.enabled()) {
    gpr_atm val = gpr_atm_no_barrier_load(&sc->refcount.count);
    gpr_log(file, line, GPR_LOG_SEVERITY_DEBUG,
            "SECURITY_CONNECTOR:%p unref %" PRIdPTR " -> %" PRIdPTR " %s", sc,
            val, val - 1, reason);
  }
#else
void grpc_security_connector_unref(grpc_security_connector* sc) {
  if (sc == nullptr) return;
#endif
  if (gpr_unref(&sc->refcount)) sc->vtable->destroy(sc);
}

static void connector_arg_destroy(void* p) {
  GRPC_SECURITY_CONNECTOR_UNREF((grpc_security_connector*)p,
                                "connector_arg_destroy");
}

static void* connector_arg_copy(void* p) {
  return GRPC_SECURITY_CONNECTOR_REF((grpc_security_connector*)p,
                                     "connector_arg_copy");
}

static int connector_cmp(void* a, void* b) {
  return grpc_security_connector_cmp(static_cast<grpc_security_connector*>(a),
                                     static_cast<grpc_security_connector*>(b));
}

static const grpc_arg_pointer_vtable connector_arg_vtable = {
    connector_arg_copy, connector_arg_destroy, connector_cmp};

grpc_arg grpc_security_connector_to_arg(grpc_security_connector* sc) {
  return grpc_channel_arg_pointer_create((char*)GRPC_ARG_SECURITY_CONNECTOR, sc,
                                         &connector_arg_vtable);
}

grpc_security_connector* grpc_security_connector_find_in_args(
    const grpc_channel_args* channel_args) {
  return grpc_channel_args_get_pointer<grpc_security_connector>(
      channel_args, GRPC_ARG_SECURITY_CONNECTOR);
}

static tsi_client_certificate_request_type
get_tsi_client_certificate_request_type(
    grpc_ssl_client_certificate_request_type grpc_request_type) {
  switch (grpc_request_type) {
    case GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE:
      return TSI_DONT_REQUEST_CLIENT_CERTIFICATE;

    case GRPC_SSL_REQUEST_CLIENT_CERTIFICATE_BUT_DONT_VERIFY:
      return TSI_REQUEST_CLIENT_CERTIFICATE_BUT_DONT_VERIFY;

    case GRPC_SSL_REQUEST_CLIENT_CERTIFICATE_AND_VERIFY:
      return TSI_REQUEST_CLIENT_CERTIFICATE_AND_VERIFY;

    case GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_BUT_DONT_VERIFY:
      return TSI_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_BUT_DONT_VERIFY;

    case GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY:
      return TSI_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;

    default:
      return TSI_DONT_REQUEST_CLIENT_CERTIFICATE;
  }
}

/* -- Fake implementation. -- */

typedef struct {
  grpc_channel_security_connector base;
  char* target;
  char* expected_targets;
  bool is_lb_channel;
  char* target_name_override;
} grpc_fake_channel_security_connector;

static void fake_channel_destroy(grpc_security_connector* sc) {
  grpc_fake_channel_security_connector* c =
      reinterpret_cast<grpc_fake_channel_security_connector*>(sc);
  grpc_call_credentials_unref(c->base.request_metadata_creds);
  gpr_free(c->target);
  gpr_free(c->expected_targets);
  gpr_free(c->target_name_override);
  gpr_free(c);
}

static void fake_server_destroy(grpc_security_connector* sc) { gpr_free(sc); }

static bool fake_check_target(const char* target_type, const char* target,
                              const char* set_str) {
  GPR_ASSERT(target_type != nullptr);
  GPR_ASSERT(target != nullptr);
  char** set = nullptr;
  size_t set_size = 0;
  gpr_string_split(set_str, ",", &set, &set_size);
  bool found = false;
  for (size_t i = 0; i < set_size; ++i) {
    if (set[i] != nullptr && strcmp(target, set[i]) == 0) found = true;
  }
  for (size_t i = 0; i < set_size; ++i) {
    gpr_free(set[i]);
  }
  gpr_free(set);
  return found;
}

static void fake_secure_name_check(const char* target,
                                   const char* expected_targets,
                                   bool is_lb_channel) {
  if (expected_targets == nullptr) return;
  char** lbs_and_backends = nullptr;
  size_t lbs_and_backends_size = 0;
  bool success = false;
  gpr_string_split(expected_targets, ";", &lbs_and_backends,
                   &lbs_and_backends_size);
  if (lbs_and_backends_size > 2 || lbs_and_backends_size == 0) {
    gpr_log(GPR_ERROR, "Invalid expected targets arg value: '%s'",
            expected_targets);
    goto done;
  }
  if (is_lb_channel) {
    if (lbs_and_backends_size != 2) {
      gpr_log(GPR_ERROR,
              "Invalid expected targets arg value: '%s'. Expectations for LB "
              "channels must be of the form 'be1,be2,be3,...;lb1,lb2,...",
              expected_targets);
      goto done;
    }
    if (!fake_check_target("LB", target, lbs_and_backends[1])) {
      gpr_log(GPR_ERROR, "LB target '%s' not found in expected set '%s'",
              target, lbs_and_backends[1]);
      goto done;
    }
    success = true;
  } else {
    if (!fake_check_target("Backend", target, lbs_and_backends[0])) {
      gpr_log(GPR_ERROR, "Backend target '%s' not found in expected set '%s'",
              target, lbs_and_backends[0]);
      goto done;
    }
    success = true;
  }
done:
  for (size_t i = 0; i < lbs_and_backends_size; ++i) {
    gpr_free(lbs_and_backends[i]);
  }
  gpr_free(lbs_and_backends);
  if (!success) abort();
}

static void fake_check_peer(grpc_security_connector* sc, tsi_peer peer,
                            grpc_auth_context** auth_context,
                            grpc_closure* on_peer_checked) {
  const char* prop_name;
  grpc_error* error = GRPC_ERROR_NONE;
  *auth_context = nullptr;
  if (peer.property_count != 1) {
    error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "Fake peers should only have 1 property.");
    goto end;
  }
  prop_name = peer.properties[0].name;
  if (prop_name == nullptr ||
      strcmp(prop_name, TSI_CERTIFICATE_TYPE_PEER_PROPERTY)) {
    char* msg;
    gpr_asprintf(&msg, "Unexpected property in fake peer: %s.",
                 prop_name == nullptr ? "<EMPTY>" : prop_name);
    error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(msg);
    gpr_free(msg);
    goto end;
  }
  if (strncmp(peer.properties[0].value.data, TSI_FAKE_CERTIFICATE_TYPE,
              peer.properties[0].value.length)) {
    error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "Invalid value for cert type property.");
    goto end;
  }
  *auth_context = grpc_auth_context_create(nullptr);
  grpc_auth_context_add_cstring_property(
      *auth_context, GRPC_TRANSPORT_SECURITY_TYPE_PROPERTY_NAME,
      GRPC_FAKE_TRANSPORT_SECURITY_TYPE);
end:
  GRPC_CLOSURE_SCHED(on_peer_checked, error);
  tsi_peer_destruct(&peer);
}

static void fake_channel_check_peer(grpc_security_connector* sc, tsi_peer peer,
                                    grpc_auth_context** auth_context,
                                    grpc_closure* on_peer_checked) {
  fake_check_peer(sc, peer, auth_context, on_peer_checked);
  grpc_fake_channel_security_connector* c =
      reinterpret_cast<grpc_fake_channel_security_connector*>(sc);
  fake_secure_name_check(c->target, c->expected_targets, c->is_lb_channel);
}

static void fake_server_check_peer(grpc_security_connector* sc, tsi_peer peer,
                                   grpc_auth_context** auth_context,
                                   grpc_closure* on_peer_checked) {
  fake_check_peer(sc, peer, auth_context, on_peer_checked);
}

static int fake_channel_cmp(grpc_security_connector* sc1,
                            grpc_security_connector* sc2) {
  grpc_fake_channel_security_connector* c1 =
      reinterpret_cast<grpc_fake_channel_security_connector*>(sc1);
  grpc_fake_channel_security_connector* c2 =
      reinterpret_cast<grpc_fake_channel_security_connector*>(sc2);
  int c = grpc_channel_security_connector_cmp(&c1->base, &c2->base);
  if (c != 0) return c;
  c = strcmp(c1->target, c2->target);
  if (c != 0) return c;
  if (c1->expected_targets == nullptr || c2->expected_targets == nullptr) {
    c = GPR_ICMP(c1->expected_targets, c2->expected_targets);
  } else {
    c = strcmp(c1->expected_targets, c2->expected_targets);
  }
  if (c != 0) return c;
  return GPR_ICMP(c1->is_lb_channel, c2->is_lb_channel);
}

static int fake_server_cmp(grpc_security_connector* sc1,
                           grpc_security_connector* sc2) {
  return grpc_server_security_connector_cmp(
      reinterpret_cast<grpc_server_security_connector*>(sc1),
      reinterpret_cast<grpc_server_security_connector*>(sc2));
}

static bool fake_channel_check_call_host(grpc_channel_security_connector* sc,
                                         const char* host,
                                         grpc_auth_context* auth_context,
                                         grpc_closure* on_call_host_checked,
                                         grpc_error** error) {
  grpc_fake_channel_security_connector* c =
      reinterpret_cast<grpc_fake_channel_security_connector*>(sc);
  char* authority_hostname = nullptr;
  char* authority_ignored_port = nullptr;
  char* target_hostname = nullptr;
  char* target_ignored_port = nullptr;
  gpr_split_host_port(host, &authority_hostname, &authority_ignored_port);
  gpr_split_host_port(c->target, &target_hostname, &target_ignored_port);
  if (c->target_name_override != nullptr) {
    char* fake_security_target_name_override_hostname = nullptr;
    char* fake_security_target_name_override_ignored_port = nullptr;
    gpr_split_host_port(c->target_name_override,
                        &fake_security_target_name_override_hostname,
                        &fake_security_target_name_override_ignored_port);
    if (strcmp(authority_hostname,
               fake_security_target_name_override_hostname) != 0) {
      gpr_log(GPR_ERROR,
              "Authority (host) '%s' != Fake Security Target override '%s'",
              host, fake_security_target_name_override_hostname);
      abort();
    }
    gpr_free(fake_security_target_name_override_hostname);
    gpr_free(fake_security_target_name_override_ignored_port);
  } else if (strcmp(authority_hostname, target_hostname) != 0) {
    gpr_log(GPR_ERROR, "Authority (host) '%s' != Target '%s'",
            authority_hostname, target_hostname);
    abort();
  }
  gpr_free(authority_hostname);
  gpr_free(authority_ignored_port);
  gpr_free(target_hostname);
  gpr_free(target_ignored_port);
  return true;
}

static void fake_channel_cancel_check_call_host(
    grpc_channel_security_connector* sc, grpc_closure* on_call_host_checked,
    grpc_error* error) {
  GRPC_ERROR_UNREF(error);
}

static void fake_channel_add_handshakers(
    grpc_channel_security_connector* sc,
    grpc_handshake_manager* handshake_mgr) {
  grpc_handshake_manager_add(
      handshake_mgr,
      grpc_security_handshaker_create(
          tsi_create_fake_handshaker(true /* is_client */), &sc->base));
}

static void fake_server_add_handshakers(grpc_server_security_connector* sc,
                                        grpc_handshake_manager* handshake_mgr) {
  grpc_handshake_manager_add(
      handshake_mgr,
      grpc_security_handshaker_create(
          tsi_create_fake_handshaker(false /* is_client */), &sc->base));
}

static grpc_security_connector_vtable fake_channel_vtable = {
    fake_channel_destroy, fake_channel_check_peer, fake_channel_cmp};

static grpc_security_connector_vtable fake_server_vtable = {
    fake_server_destroy, fake_server_check_peer, fake_server_cmp};

grpc_channel_security_connector* grpc_fake_channel_security_connector_create(
    grpc_channel_credentials* channel_creds,
    grpc_call_credentials* request_metadata_creds, const char* target,
    const grpc_channel_args* args) {
  grpc_fake_channel_security_connector* c =
      static_cast<grpc_fake_channel_security_connector*>(
          gpr_zalloc(sizeof(*c)));
  gpr_ref_init(&c->base.base.refcount, 1);
  c->base.base.url_scheme = GRPC_FAKE_SECURITY_URL_SCHEME;
  c->base.base.vtable = &fake_channel_vtable;
  c->base.channel_creds = channel_creds;
  c->base.request_metadata_creds =
      grpc_call_credentials_ref(request_metadata_creds);
  c->base.check_call_host = fake_channel_check_call_host;
  c->base.cancel_check_call_host = fake_channel_cancel_check_call_host;
  c->base.add_handshakers = fake_channel_add_handshakers;
  c->target = gpr_strdup(target);
  const char* expected_targets = grpc_fake_transport_get_expected_targets(args);
  c->expected_targets = gpr_strdup(expected_targets);
  c->is_lb_channel = grpc_core::FindTargetAuthorityTableInArgs(args) != nullptr;
  const grpc_arg* target_name_override_arg =
      grpc_channel_args_find(args, GRPC_SSL_TARGET_NAME_OVERRIDE_ARG);
  if (target_name_override_arg != nullptr) {
    c->target_name_override =
        gpr_strdup(grpc_channel_arg_get_string(target_name_override_arg));
  }
  return &c->base;
}

grpc_server_security_connector* grpc_fake_server_security_connector_create(
    grpc_server_credentials* server_creds) {
  grpc_server_security_connector* c =
      static_cast<grpc_server_security_connector*>(
          gpr_zalloc(sizeof(grpc_server_security_connector)));
  gpr_ref_init(&c->base.refcount, 1);
  c->base.vtable = &fake_server_vtable;
  c->base.url_scheme = GRPC_FAKE_SECURITY_URL_SCHEME;
  c->server_creds = server_creds;
  c->add_handshakers = fake_server_add_handshakers;
  return c;
}

/* --- Ssl implementation. --- */

grpc_ssl_session_cache* grpc_ssl_session_cache_create_lru(size_t capacity) {
  tsi_ssl_session_cache* cache = tsi_ssl_session_cache_create_lru(capacity);
  return reinterpret_cast<grpc_ssl_session_cache*>(cache);
}

void grpc_ssl_session_cache_destroy(grpc_ssl_session_cache* cache) {
  tsi_ssl_session_cache* tsi_cache =
      reinterpret_cast<tsi_ssl_session_cache*>(cache);
  tsi_ssl_session_cache_unref(tsi_cache);
}

static void* grpc_ssl_session_cache_arg_copy(void* p) {
  tsi_ssl_session_cache* tsi_cache =
      reinterpret_cast<tsi_ssl_session_cache*>(p);
  // destroy call below will unref the pointer.
  tsi_ssl_session_cache_ref(tsi_cache);
  return p;
}

static void grpc_ssl_session_cache_arg_destroy(void* p) {
  tsi_ssl_session_cache* tsi_cache =
      reinterpret_cast<tsi_ssl_session_cache*>(p);
  tsi_ssl_session_cache_unref(tsi_cache);
}

static int grpc_ssl_session_cache_arg_cmp(void* p, void* q) {
  return GPR_ICMP(p, q);
}

grpc_arg grpc_ssl_session_cache_create_channel_arg(
    grpc_ssl_session_cache* cache) {
  static const grpc_arg_pointer_vtable vtable = {
      grpc_ssl_session_cache_arg_copy,
      grpc_ssl_session_cache_arg_destroy,
      grpc_ssl_session_cache_arg_cmp,
  };
  return grpc_channel_arg_pointer_create(
      const_cast<char*>(GRPC_SSL_SESSION_CACHE_ARG), cache, &vtable);
}

typedef struct {
  grpc_channel_security_connector base;
  tsi_ssl_client_handshaker_factory* client_handshaker_factory;
  char* target_name;
  char* overridden_target_name;
} grpc_ssl_channel_security_connector;

typedef struct {
  grpc_server_security_connector base;
  tsi_ssl_server_handshaker_factory* server_handshaker_factory;
} grpc_ssl_server_security_connector;

static bool server_connector_has_cert_config_fetcher(
    grpc_ssl_server_security_connector* c) {
  GPR_ASSERT(c != nullptr);
  grpc_ssl_server_credentials* server_creds =
      reinterpret_cast<grpc_ssl_server_credentials*>(c->base.server_creds);
  GPR_ASSERT(server_creds != nullptr);
  return server_creds->certificate_config_fetcher.cb != nullptr;
}

static void ssl_channel_destroy(grpc_security_connector* sc) {
  grpc_ssl_channel_security_connector* c =
      reinterpret_cast<grpc_ssl_channel_security_connector*>(sc);
  grpc_channel_credentials_unref(c->base.channel_creds);
  grpc_call_credentials_unref(c->base.request_metadata_creds);
  tsi_ssl_client_handshaker_factory_unref(c->client_handshaker_factory);
  c->client_handshaker_factory = nullptr;
  if (c->target_name != nullptr) gpr_free(c->target_name);
  if (c->overridden_target_name != nullptr) gpr_free(c->overridden_target_name);
  gpr_free(sc);
}

static void ssl_server_destroy(grpc_security_connector* sc) {
  grpc_ssl_server_security_connector* c =
      reinterpret_cast<grpc_ssl_server_security_connector*>(sc);
  grpc_server_credentials_unref(c->base.server_creds);
  tsi_ssl_server_handshaker_factory_unref(c->server_handshaker_factory);
  c->server_handshaker_factory = nullptr;
  gpr_free(sc);
}

static void ssl_channel_add_handshakers(grpc_channel_security_connector* sc,
                                        grpc_handshake_manager* handshake_mgr) {
  grpc_ssl_channel_security_connector* c =
      reinterpret_cast<grpc_ssl_channel_security_connector*>(sc);
  // Instantiate TSI handshaker.
  tsi_handshaker* tsi_hs = nullptr;
  tsi_result result = tsi_ssl_client_handshaker_factory_create_handshaker(
      c->client_handshaker_factory,
      c->overridden_target_name != nullptr ? c->overridden_target_name
                                           : c->target_name,
      &tsi_hs);
  if (result != TSI_OK) {
    gpr_log(GPR_ERROR, "Handshaker creation failed with error %s.",
            tsi_result_to_string(result));
    return;
  }
  // Create handshakers.
  grpc_handshake_manager_add(
      handshake_mgr, grpc_security_handshaker_create(tsi_hs, &sc->base));
}

static const char** fill_alpn_protocol_strings(size_t* num_alpn_protocols) {
  GPR_ASSERT(num_alpn_protocols != nullptr);
  *num_alpn_protocols = grpc_chttp2_num_alpn_versions();
  const char** alpn_protocol_strings = static_cast<const char**>(
      gpr_malloc(sizeof(const char*) * (*num_alpn_protocols)));
  for (size_t i = 0; i < *num_alpn_protocols; i++) {
    alpn_protocol_strings[i] = grpc_chttp2_get_alpn_version_index(i);
  }
  return alpn_protocol_strings;
}

/* Attempts to replace the server_handshaker_factory with a new factory using
 * the provided grpc_ssl_server_certificate_config. Should new factory creation
 * fail, the existing factory will not be replaced. Returns true on success (new
 * factory created). */
static bool try_replace_server_handshaker_factory(
    grpc_ssl_server_security_connector* sc,
    const grpc_ssl_server_certificate_config* config) {
  if (config == nullptr) {
    gpr_log(GPR_ERROR,
            "Server certificate config callback returned invalid (NULL) "
            "config.");
    return false;
  }
  gpr_log(GPR_DEBUG, "Using new server certificate config (%p).", config);

  size_t num_alpn_protocols = 0;
  const char** alpn_protocol_strings =
      fill_alpn_protocol_strings(&num_alpn_protocols);
  tsi_ssl_pem_key_cert_pair* cert_pairs = grpc_convert_grpc_to_tsi_cert_pairs(
      config->pem_key_cert_pairs, config->num_key_cert_pairs);
  tsi_ssl_server_handshaker_factory* new_handshaker_factory = nullptr;
  grpc_ssl_server_credentials* server_creds =
      reinterpret_cast<grpc_ssl_server_credentials*>(sc->base.server_creds);
  tsi_result result = tsi_create_ssl_server_handshaker_factory_ex(
      cert_pairs, config->num_key_cert_pairs, config->pem_root_certs,
      get_tsi_client_certificate_request_type(
          server_creds->config.client_certificate_request),
      ssl_cipher_suites(), alpn_protocol_strings,
      static_cast<uint16_t>(num_alpn_protocols), &new_handshaker_factory);
  gpr_free(cert_pairs);
  gpr_free((void*)alpn_protocol_strings);

  if (result != TSI_OK) {
    gpr_log(GPR_ERROR, "Handshaker factory creation failed with %s.",
            tsi_result_to_string(result));
    return false;
  }
  tsi_ssl_server_handshaker_factory_unref(sc->server_handshaker_factory);
  sc->server_handshaker_factory = new_handshaker_factory;
  return true;
}

/* Attempts to fetch the server certificate config if a callback is available.
 * Current certificate config will continue to be used if the callback returns
 * an error. Returns true if new credentials were sucessfully loaded. */
static bool try_fetch_ssl_server_credentials(
    grpc_ssl_server_security_connector* sc) {
  grpc_ssl_server_certificate_config* certificate_config = nullptr;
  bool status;

  GPR_ASSERT(sc != nullptr);
  if (!server_connector_has_cert_config_fetcher(sc)) return false;

  grpc_ssl_server_credentials* server_creds =
      reinterpret_cast<grpc_ssl_server_credentials*>(sc->base.server_creds);
  grpc_ssl_certificate_config_reload_status cb_result =
      server_creds->certificate_config_fetcher.cb(
          server_creds->certificate_config_fetcher.user_data,
          &certificate_config);
  if (cb_result == GRPC_SSL_CERTIFICATE_CONFIG_RELOAD_UNCHANGED) {
    gpr_log(GPR_DEBUG, "No change in SSL server credentials.");
    status = false;
  } else if (cb_result == GRPC_SSL_CERTIFICATE_CONFIG_RELOAD_NEW) {
    status = try_replace_server_handshaker_factory(sc, certificate_config);
  } else {
    // Log error, continue using previously-loaded credentials.
    gpr_log(GPR_ERROR,
            "Failed fetching new server credentials, continuing to "
            "use previously-loaded credentials.");
    status = false;
  }

  if (certificate_config != nullptr) {
    grpc_ssl_server_certificate_config_destroy(certificate_config);
  }
  return status;
}

static void ssl_server_add_handshakers(grpc_server_security_connector* sc,
                                       grpc_handshake_manager* handshake_mgr) {
  grpc_ssl_server_security_connector* c =
      reinterpret_cast<grpc_ssl_server_security_connector*>(sc);
  // Instantiate TSI handshaker.
  try_fetch_ssl_server_credentials(c);
  tsi_handshaker* tsi_hs = nullptr;
  tsi_result result = tsi_ssl_server_handshaker_factory_create_handshaker(
      c->server_handshaker_factory, &tsi_hs);
  if (result != TSI_OK) {
    gpr_log(GPR_ERROR, "Handshaker creation failed with error %s.",
            tsi_result_to_string(result));
    return;
  }
  // Create handshakers.
  grpc_handshake_manager_add(
      handshake_mgr, grpc_security_handshaker_create(tsi_hs, &sc->base));
}

int grpc_ssl_host_matches_name(const tsi_peer* peer, const char* peer_name) {
  char* allocated_name = nullptr;
  int r;

  char* ignored_port;
  gpr_split_host_port(peer_name, &allocated_name, &ignored_port);
  gpr_free(ignored_port);
  peer_name = allocated_name;
  if (!peer_name) return 0;

  // IPv6 zone-id should not be included in comparisons.
  char* const zone_id = strchr(allocated_name, '%');
  if (zone_id != nullptr) *zone_id = '\0';

  r = tsi_ssl_peer_matches_name(peer, peer_name);
  gpr_free(allocated_name);
  return r;
}

grpc_auth_context* grpc_ssl_peer_to_auth_context(const tsi_peer* peer) {
  size_t i;
  grpc_auth_context* ctx = nullptr;
  const char* peer_identity_property_name = nullptr;

  /* The caller has checked the certificate type property. */
  GPR_ASSERT(peer->property_count >= 1);
  ctx = grpc_auth_context_create(nullptr);
  grpc_auth_context_add_cstring_property(
      ctx, GRPC_TRANSPORT_SECURITY_TYPE_PROPERTY_NAME,
      GRPC_SSL_TRANSPORT_SECURITY_TYPE);
  for (i = 0; i < peer->property_count; i++) {
    const tsi_peer_property* prop = &peer->properties[i];
    if (prop->name == nullptr) continue;
    if (strcmp(prop->name, TSI_X509_SUBJECT_COMMON_NAME_PEER_PROPERTY) == 0) {
      /* If there is no subject alt name, have the CN as the identity. */
      if (peer_identity_property_name == nullptr) {
        peer_identity_property_name = GRPC_X509_CN_PROPERTY_NAME;
      }
      grpc_auth_context_add_property(ctx, GRPC_X509_CN_PROPERTY_NAME,
                                     prop->value.data, prop->value.length);
    } else if (strcmp(prop->name,
                      TSI_X509_SUBJECT_ALTERNATIVE_NAME_PEER_PROPERTY) == 0) {
      peer_identity_property_name = GRPC_X509_SAN_PROPERTY_NAME;
      grpc_auth_context_add_property(ctx, GRPC_X509_SAN_PROPERTY_NAME,
                                     prop->value.data, prop->value.length);
    } else if (strcmp(prop->name, TSI_X509_PEM_CERT_PROPERTY) == 0) {
      grpc_auth_context_add_property(ctx, GRPC_X509_PEM_CERT_PROPERTY_NAME,
                                     prop->value.data, prop->value.length);
    } else if (strcmp(prop->name, TSI_SSL_SESSION_REUSED_PEER_PROPERTY) == 0) {
      grpc_auth_context_add_property(ctx, GRPC_SSL_SESSION_REUSED_PROPERTY,
                                     prop->value.data, prop->value.length);
    }
  }
  if (peer_identity_property_name != nullptr) {
    GPR_ASSERT(grpc_auth_context_set_peer_identity_property_name(
                   ctx, peer_identity_property_name) == 1);
  }
  return ctx;
}

static grpc_error* ssl_check_peer(grpc_security_connector* sc,
                                  const char* peer_name, const tsi_peer* peer,
                                  grpc_auth_context** auth_context) {
  /* Check the ALPN. */
  const tsi_peer_property* p =
      tsi_peer_get_property_by_name(peer, TSI_SSL_ALPN_SELECTED_PROTOCOL);
  if (p == nullptr) {
    return GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "Cannot check peer: missing selected ALPN property.");
  }
  if (!grpc_chttp2_is_alpn_version_supported(p->value.data, p->value.length)) {
    return GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "Cannot check peer: invalid ALPN value.");
  }

  /* Check the peer name if specified. */
  if (peer_name != nullptr && !grpc_ssl_host_matches_name(peer, peer_name)) {
    char* msg;
    gpr_asprintf(&msg, "Peer name %s is not in peer certificate", peer_name);
    grpc_error* error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(msg);
    gpr_free(msg);
    return error;
  }
  *auth_context = grpc_ssl_peer_to_auth_context(peer);
  return GRPC_ERROR_NONE;
}

static void ssl_channel_check_peer(grpc_security_connector* sc, tsi_peer peer,
                                   grpc_auth_context** auth_context,
                                   grpc_closure* on_peer_checked) {
  grpc_ssl_channel_security_connector* c =
      reinterpret_cast<grpc_ssl_channel_security_connector*>(sc);
  grpc_error* error = ssl_check_peer(sc,
                                     c->overridden_target_name != nullptr
                                         ? c->overridden_target_name
                                         : c->target_name,
                                     &peer, auth_context);
  GRPC_CLOSURE_SCHED(on_peer_checked, error);
  tsi_peer_destruct(&peer);
}

static void ssl_server_check_peer(grpc_security_connector* sc, tsi_peer peer,
                                  grpc_auth_context** auth_context,
                                  grpc_closure* on_peer_checked) {
  grpc_error* error = ssl_check_peer(sc, nullptr, &peer, auth_context);
  tsi_peer_destruct(&peer);
  GRPC_CLOSURE_SCHED(on_peer_checked, error);
}

static int ssl_channel_cmp(grpc_security_connector* sc1,
                           grpc_security_connector* sc2) {
  grpc_ssl_channel_security_connector* c1 =
      reinterpret_cast<grpc_ssl_channel_security_connector*>(sc1);
  grpc_ssl_channel_security_connector* c2 =
      reinterpret_cast<grpc_ssl_channel_security_connector*>(sc2);
  int c = grpc_channel_security_connector_cmp(&c1->base, &c2->base);
  if (c != 0) return c;
  c = strcmp(c1->target_name, c2->target_name);
  if (c != 0) return c;
  return (c1->overridden_target_name == nullptr ||
          c2->overridden_target_name == nullptr)
             ? GPR_ICMP(c1->overridden_target_name, c2->overridden_target_name)
             : strcmp(c1->overridden_target_name, c2->overridden_target_name);
}

static int ssl_server_cmp(grpc_security_connector* sc1,
                          grpc_security_connector* sc2) {
  return grpc_server_security_connector_cmp(
      reinterpret_cast<grpc_server_security_connector*>(sc1),
      reinterpret_cast<grpc_server_security_connector*>(sc2));
}

static void add_shallow_auth_property_to_peer(tsi_peer* peer,
                                              const grpc_auth_property* prop,
                                              const char* tsi_prop_name) {
  tsi_peer_property* tsi_prop = &peer->properties[peer->property_count++];
  tsi_prop->name = const_cast<char*>(tsi_prop_name);
  tsi_prop->value.data = prop->value;
  tsi_prop->value.length = prop->value_length;
}

tsi_peer grpc_shallow_peer_from_ssl_auth_context(
    const grpc_auth_context* auth_context) {
  size_t max_num_props = 0;
  grpc_auth_property_iterator it;
  const grpc_auth_property* prop;
  tsi_peer peer;
  memset(&peer, 0, sizeof(peer));

  it = grpc_auth_context_property_iterator(auth_context);
  while (grpc_auth_property_iterator_next(&it) != nullptr) max_num_props++;

  if (max_num_props > 0) {
    peer.properties = static_cast<tsi_peer_property*>(
        gpr_malloc(max_num_props * sizeof(tsi_peer_property)));
    it = grpc_auth_context_property_iterator(auth_context);
    while ((prop = grpc_auth_property_iterator_next(&it)) != nullptr) {
      if (strcmp(prop->name, GRPC_X509_SAN_PROPERTY_NAME) == 0) {
        add_shallow_auth_property_to_peer(
            &peer, prop, TSI_X509_SUBJECT_ALTERNATIVE_NAME_PEER_PROPERTY);
      } else if (strcmp(prop->name, GRPC_X509_CN_PROPERTY_NAME) == 0) {
        add_shallow_auth_property_to_peer(
            &peer, prop, TSI_X509_SUBJECT_COMMON_NAME_PEER_PROPERTY);
      } else if (strcmp(prop->name, GRPC_X509_PEM_CERT_PROPERTY_NAME) == 0) {
        add_shallow_auth_property_to_peer(&peer, prop,
                                          TSI_X509_PEM_CERT_PROPERTY);
      }
    }
  }
  return peer;
}

void grpc_shallow_peer_destruct(tsi_peer* peer) {
  if (peer->properties != nullptr) gpr_free(peer->properties);
}

static bool ssl_channel_check_call_host(grpc_channel_security_connector* sc,
                                        const char* host,
                                        grpc_auth_context* auth_context,
                                        grpc_closure* on_call_host_checked,
                                        grpc_error** error) {
  grpc_ssl_channel_security_connector* c =
      reinterpret_cast<grpc_ssl_channel_security_connector*>(sc);
  grpc_security_status status = GRPC_SECURITY_ERROR;
  tsi_peer peer = grpc_shallow_peer_from_ssl_auth_context(auth_context);
  if (grpc_ssl_host_matches_name(&peer, host)) status = GRPC_SECURITY_OK;
  /* If the target name was overridden, then the original target_name was
     'checked' transitively during the previous peer check at the end of the
     handshake. */
  if (c->overridden_target_name != nullptr &&
      strcmp(host, c->target_name) == 0) {
    status = GRPC_SECURITY_OK;
  }
  if (status != GRPC_SECURITY_OK) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "call host does not match SSL server name");
  }
  grpc_shallow_peer_destruct(&peer);
  return true;
}

static void ssl_channel_cancel_check_call_host(
    grpc_channel_security_connector* sc, grpc_closure* on_call_host_checked,
    grpc_error* error) {
  GRPC_ERROR_UNREF(error);
}

static grpc_security_connector_vtable ssl_channel_vtable = {
    ssl_channel_destroy, ssl_channel_check_peer, ssl_channel_cmp};

static grpc_security_connector_vtable ssl_server_vtable = {
    ssl_server_destroy, ssl_server_check_peer, ssl_server_cmp};

grpc_security_status grpc_ssl_channel_security_connector_create(
    grpc_channel_credentials* channel_creds,
    grpc_call_credentials* request_metadata_creds,
    const grpc_ssl_config* config, const char* target_name,
    const char* overridden_target_name,
    tsi_ssl_session_cache* ssl_session_cache,
    grpc_channel_security_connector** sc) {
  tsi_result result = TSI_OK;
  grpc_ssl_channel_security_connector* c;
  char* port;
  bool has_key_cert_pair;
  tsi_ssl_client_handshaker_options options;
  memset(&options, 0, sizeof(options));
  options.alpn_protocols =
      fill_alpn_protocol_strings(&options.num_alpn_protocols);

  if (config == nullptr || target_name == nullptr) {
    gpr_log(GPR_ERROR, "An ssl channel needs a config and a target name.");
    goto error;
  }
  if (config->pem_root_certs == nullptr) {
    // Use default root certificates.
    options.pem_root_certs = grpc_core::DefaultSslRootStore::GetPemRootCerts();
    options.root_store = grpc_core::DefaultSslRootStore::GetRootStore();
    if (options.pem_root_certs == nullptr) {
      gpr_log(GPR_ERROR, "Could not get default pem root certs.");
      goto error;
    }
  } else {
    options.pem_root_certs = config->pem_root_certs;
  }
  c = static_cast<grpc_ssl_channel_security_connector*>(
      gpr_zalloc(sizeof(grpc_ssl_channel_security_connector)));

  gpr_ref_init(&c->base.base.refcount, 1);
  c->base.base.vtable = &ssl_channel_vtable;
  c->base.base.url_scheme = GRPC_SSL_URL_SCHEME;
  c->base.channel_creds = grpc_channel_credentials_ref(channel_creds);
  c->base.request_metadata_creds =
      grpc_call_credentials_ref(request_metadata_creds);
  c->base.check_call_host = ssl_channel_check_call_host;
  c->base.cancel_check_call_host = ssl_channel_cancel_check_call_host;
  c->base.add_handshakers = ssl_channel_add_handshakers;
  gpr_split_host_port(target_name, &c->target_name, &port);
  gpr_free(port);
  if (overridden_target_name != nullptr) {
    c->overridden_target_name = gpr_strdup(overridden_target_name);
  }

  has_key_cert_pair = config->pem_key_cert_pair != nullptr &&
                      config->pem_key_cert_pair->private_key != nullptr &&
                      config->pem_key_cert_pair->cert_chain != nullptr;
  if (has_key_cert_pair) {
    options.pem_key_cert_pair = config->pem_key_cert_pair;
  }
  options.cipher_suites = ssl_cipher_suites();
  options.session_cache = ssl_session_cache;
  result = tsi_create_ssl_client_handshaker_factory_with_options(
      &options, &c->client_handshaker_factory);
  if (result != TSI_OK) {
    gpr_log(GPR_ERROR, "Handshaker factory creation failed with %s.",
            tsi_result_to_string(result));
    ssl_channel_destroy(&c->base.base);
    *sc = nullptr;
    goto error;
  }
  *sc = &c->base;
  gpr_free((void*)options.alpn_protocols);
  return GRPC_SECURITY_OK;

error:
  gpr_free((void*)options.alpn_protocols);
  return GRPC_SECURITY_ERROR;
}

static grpc_ssl_server_security_connector*
grpc_ssl_server_security_connector_initialize(
    grpc_server_credentials* server_creds) {
  grpc_ssl_server_security_connector* c =
      static_cast<grpc_ssl_server_security_connector*>(
          gpr_zalloc(sizeof(grpc_ssl_server_security_connector)));
  gpr_ref_init(&c->base.base.refcount, 1);
  c->base.base.url_scheme = GRPC_SSL_URL_SCHEME;
  c->base.base.vtable = &ssl_server_vtable;
  c->base.add_handshakers = ssl_server_add_handshakers;
  c->base.server_creds = grpc_server_credentials_ref(server_creds);
  return c;
}

grpc_security_status grpc_ssl_server_security_connector_create(
    grpc_server_credentials* gsc, grpc_server_security_connector** sc) {
  tsi_result result = TSI_OK;
  grpc_ssl_server_credentials* server_credentials =
      reinterpret_cast<grpc_ssl_server_credentials*>(gsc);
  grpc_security_status retval = GRPC_SECURITY_OK;

  GPR_ASSERT(server_credentials != nullptr);
  GPR_ASSERT(sc != nullptr);

  grpc_ssl_server_security_connector* c =
      grpc_ssl_server_security_connector_initialize(gsc);
  if (server_connector_has_cert_config_fetcher(c)) {
    // Load initial credentials from certificate_config_fetcher:
    if (!try_fetch_ssl_server_credentials(c)) {
      gpr_log(GPR_ERROR, "Failed loading SSL server credentials from fetcher.");
      retval = GRPC_SECURITY_ERROR;
    }
  } else {
    size_t num_alpn_protocols = 0;
    const char** alpn_protocol_strings =
        fill_alpn_protocol_strings(&num_alpn_protocols);
    result = tsi_create_ssl_server_handshaker_factory_ex(
        server_credentials->config.pem_key_cert_pairs,
        server_credentials->config.num_key_cert_pairs,
        server_credentials->config.pem_root_certs,
        get_tsi_client_certificate_request_type(
            server_credentials->config.client_certificate_request),
        ssl_cipher_suites(), alpn_protocol_strings,
        static_cast<uint16_t>(num_alpn_protocols),
        &c->server_handshaker_factory);
    gpr_free((void*)alpn_protocol_strings);
    if (result != TSI_OK) {
      gpr_log(GPR_ERROR, "Handshaker factory creation failed with %s.",
              tsi_result_to_string(result));
      retval = GRPC_SECURITY_ERROR;
    }
  }

  if (retval == GRPC_SECURITY_OK) {
    *sc = &c->base;
  } else {
    if (c != nullptr) ssl_server_destroy(&c->base.base);
    if (sc != nullptr) *sc = nullptr;
  }
  return retval;
}

namespace grpc_core {

tsi_ssl_root_certs_store* DefaultSslRootStore::default_root_store_;
grpc_slice DefaultSslRootStore::default_pem_root_certs_;
const char* DefaultSslRootStore::linux_cert_files_[] = {
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/ca-bundle.pem",
    "/etc/pki/tls/cacert.pem",
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"
};
const char* DefaultSslRootStore::linux_cert_directories_[] = {
  "/etc/ssl/certs",
  "/system/etc/security/cacerts",
  "/usr/local/share/certs",
  "/etc/pki/tls/certs",
  "/etc/openssl/certs"
};
size_t DefaultSslRootStore::num_cert_files_ = 5;
size_t DefaultSslRootStore::num_cert_dirs_ = 5;
const char* DefaultSslRootStore::platform;

const tsi_ssl_root_certs_store* DefaultSslRootStore::GetRootStore() {
  InitRootStore();
  return default_root_store_;
}

const char* DefaultSslRootStore::GetPemRootCerts() {
  InitRootStore();
  return GRPC_SLICE_IS_EMPTY(default_pem_root_certs_)
             ? nullptr
             : reinterpret_cast<const char*>
                   GRPC_SLICE_START_PTR(default_pem_root_certs_);
}

grpc_slice DefaultSslRootStore::ComputePemRootCerts() {
  grpc_slice result = grpc_empty_slice();
  // First try to load the roots from the environment.
  char* default_root_certs_path =
      gpr_getenv(GRPC_DEFAULT_SSL_ROOTS_FILE_PATH_ENV_VAR);
  if (default_root_certs_path != nullptr) {
    GRPC_LOG_IF_ERROR("load_file",
                      grpc_load_file(default_root_certs_path, 1, &result));
    gpr_free(default_root_certs_path);
  }
  // Try overridden roots if needed.
  grpc_ssl_roots_override_result ovrd_res = GRPC_SSL_ROOTS_OVERRIDE_FAIL;
  if (GRPC_SLICE_IS_EMPTY(result) && ssl_roots_override_cb != nullptr) {
    char* pem_root_certs = nullptr;
    ovrd_res = ssl_roots_override_cb(&pem_root_certs);
    if (ovrd_res == GRPC_SSL_ROOTS_OVERRIDE_OK) {
      GPR_ASSERT(pem_root_certs != nullptr);
      result = grpc_slice_from_copied_buffer(
          pem_root_certs,
          strlen(pem_root_certs) + 1);  // nullptr terminator.
    }
    gpr_free(pem_root_certs);
  }
  // Use system certs if needed.
  if (GRPC_SLICE_IS_EMPTY(result) &&
      ovrd_res != GRPC_SSL_ROOTS_OVERRIDE_FAIL_PERMANENTLY) {
      const char* system_root_certs = nullptr;
      if (use_system_certs != nullptr) {
          system_root_certs = GetSystemRootCerts();
          if (system_root_certs != nullptr) {
              GRPC_LOG_IF_ERROR("load_file",
                          grpc_load_file(system_root_certs, 1, &result));
          }
      }
      if (use_system_certs == nullptr || system_root_certs == nullptr) {
          // Fallback to certs manually shipped with gRPC
          GRPC_LOG_IF_ERROR("load_file",
                    grpc_load_file(installed_roots_path, 1, &result));
      }
  }
  return result;
}

const char* DefaultSslRootStore::GetSystemRootCerts() {
  const char* result = nullptr;
	if (strcmp(platform, "linux") == 0) {
    //TODO case in which there's no bundle, just single cert files
    FILE* cert_file;
    for (size_t i = 0; i < num_cert_files_; i++) {
        cert_file = fopen(linux_cert_files_[i], "r");
        if (cert_file != nullptr) {
          fclose(cert_file);
          result = linux_cert_files_[i];
        }
    }
    if (result == nullptr) { // If no cert file was found try directories
     	DIR* cert_dir;
    	const char* found_cert_dir = nullptr;
     	for (size_t i = 0; i < num_cert_dirs_; i++) {
   	    cert_dir = opendir(linux_cert_directories_[i]);
   	    if (cert_dir != nullptr) { // If directory exists
      		found_cert_dir = linux_cert_directories_[i];
       		closedir(cert_dir);
   	    }
     	}
    	if (cert_dir != nullptr && found_cert_dir != nullptr) {
  	    result = CreateRootCertsBundle(found_cert_dir);
      }
    }
  } /*else if (platform.compare("windows")) {
    //TODO Export certs from Windows trust store (certutil?)
  } else if (platform.compare("apple") {
    //TODO Export .pem file from keychain (using API?)
  }*/
  return result;
}

const char* DefaultSslRootStore::CreateRootCertsBundle(const char* path) {
    namespace stdfs = std::experimental::filesystem;

    // TODO where should we save the bundle file? "/etc"?
    const char* bundle_path = "system_roots.pem";
    std::ofstream bundle_ca_file(bundle_path);

    // http://en.cppreference.com/w/cpp/experimental/fs/directory_iterator (unspecified order)
    const stdfs::directory_iterator end{};

    for (stdfs::directory_iterator iter{path}; iter != end; ++iter) {
      if (stdfs::is_regular_file(*iter)) { // ignores subdirectories
        std::ifstream ca_file(iter->path().string());
  	    bundle_ca_file << ca_file.rdbuf();
  	    ca_file.close();
    	}
    }

    bundle_ca_file.close();
    return bundle_path;
}

void DefaultSslRootStore::DetectPlatform() {
#if defined __linux__
     // Linux environment (any GNU/Linux distribution)
     SetPlatform("linux");
#elif defined _WIN32
     // Windows environment (32 and 64 bit)
     SetPlatform("windows");
#elif defined __APPLE__ && __MACH__
     // MacOS / OSX environment
     SetPlatform("apple");
#endif
}

void DefaultSslRootStore::InitRootStore() {
  static gpr_once once = GPR_ONCE_INIT;
  gpr_once_init(&once, DefaultSslRootStore::InitRootStoreOnce);
}

void DefaultSslRootStore::InitRootStoreOnce() {
  default_pem_root_certs_ = ComputePemRootCerts();
  if (!GRPC_SLICE_IS_EMPTY(default_pem_root_certs_)) {
    default_root_store_ =
        tsi_ssl_root_certs_store_create(reinterpret_cast<const char*>(
            GRPC_SLICE_START_PTR(default_pem_root_certs_)));
  }
}

}  // namespace grpc_core
