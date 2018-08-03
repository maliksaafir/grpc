/*
 *
 * Copyright 2018 gRPC authors.
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
#include <stdio.h>

#ifdef GPR_LINUX
#include <grpc/grpc_security.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <sys/param.h>

#include "src/core/lib/gpr/env.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gpr/tmpfile.h"
#include "src/core/lib/iomgr/load_file.h"
#include "src/core/lib/security/context/security_context.h"
#include "src/core/lib/security/security_connector/load_system_roots.h"
#include "src/core/lib/security/security_connector/load_system_roots_linux.h"
#include "src/core/lib/security/security_connector/security_connector.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#include "src/core/tsi/ssl_transport_security.h"
#include "src/core/tsi/transport_security.h"
#include "test/core/util/test_config.h"

#ifndef GRPC_USE_SYSTEM_SSL_ROOTS_ENV_VAR
#define GRPC_USE_SYSTEM_SSL_ROOTS_ENV_VAR "GRPC_USE_SYSTEM_SSL_ROOTS"
#endif  // GRPC_USE_SYSTEM_SSL_ROOTS_ENV_VAR

// Test GetAbsoluteFilePath.
static void test_absolute_cert_path() {
  const char* directory = "nonexistent/test/directory";
  const char* filename = "doesnotexist.txt";
  const char* result_path = static_cast<char*>(gpr_malloc(MAXPATHLEN));
  grpc_core::GetAbsoluteFilePath(result_path, directory, filename);
  GPR_ASSERT(
      strcmp(result_path, "nonexistent/test/directory/doesnotexist.txt") == 0);
  gpr_free((char*)result_path);
}

static void test_cert_bundle_creation() {
  gpr_setenv(GRPC_USE_SYSTEM_SSL_ROOTS_ENV_VAR, "true");

  /* Test that CreateRootCertsBundle returns a correct slice. */
  grpc_slice roots_bundle = grpc_empty_slice();
  GRPC_LOG_IF_ERROR(
      "load_file", grpc_load_file("test/core/security/etc/bundle/bundle.pem", 1,
                                  &roots_bundle));
  /* result_slice should have the same content as roots_bundle. */
  grpc_slice result_slice =
      grpc_core::CreateRootCertsBundle("test/core/security/etc/roots");
  char* result_str = grpc_slice_to_c_string(result_slice);
  char* bundle_str = grpc_slice_to_c_string(roots_bundle);
  GPR_ASSERT(strcmp(result_str, bundle_str) == 0);
  // TODO: fix bazel builds by introducing working absolute path
  if (GRPC_SLICE_IS_EMPTY(result_slice)) {
    printf("Your build is unsupported by 'test_cert_bundle_creation'\n");
  }
  /* TODO: add tests for branches in CreateRootCertsBundle that return empty
   * slices. */

  /* Cleanup. */
  unsetenv(GRPC_USE_SYSTEM_SSL_ROOTS_ENV_VAR);
  unsetenv("GRPC_SYSTEM_SSL_ROOTS_DIR");
  gpr_free(result_str);
  gpr_free(bundle_str);
  grpc_slice_unref(roots_bundle);
  grpc_slice_unref(result_slice);
}

int main() {
  test_absolute_cert_path();
  test_cert_bundle_creation();
  return 0;
}
#else
int main() {
  printf("*** WARNING: this test is only supported on Linux systems ***\n");
  return 0;
}
#endif  // GPR_LINUX
