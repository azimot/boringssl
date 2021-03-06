/* Copyright (c) 2018, Google Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <openssl/bytestring.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include "handshake_util.h"
#include "test_config.h"
#include "test_state.h"

using namespace bssl;

namespace {

ssize_t read_eintr(int fd, void *out, size_t len) {
  ssize_t ret;
  do {
    ret = read(fd, out, len);
  } while (ret < 0 && errno == EINTR);
  return ret;
}

ssize_t write_eintr(int fd, const void *in, size_t len) {
  ssize_t ret;
  do {
    ret = write(fd, in, len);
  } while (ret < 0 && errno == EINTR);
  return ret;
}

bool HandbackReady(SSL *ssl, int ret) {
  return ret < 0 && SSL_get_error(ssl, ret) == SSL_ERROR_HANDBACK;
}

bool Handshaker(const TestConfig *config, int rfd, int wfd,
                Span<const uint8_t> input, int control) {
  UniquePtr<SSL_CTX> ctx = config->SetupCtx(/*old_ctx=*/nullptr);
  if (!ctx) {
    return false;
  }
  UniquePtr<SSL> ssl = config->NewSSL(ctx.get(), nullptr, nullptr);

  // Set |O_NONBLOCK| in order to break out of the loop when we hit
  // |SSL_ERROR_WANT_READ|, so that we can send |kControlMsgWantRead| to the
  // proxy.
  if (fcntl(rfd, F_SETFL, O_NONBLOCK) != 0) {
    perror("fcntl");
    return false;
  }
  SSL_set_rfd(ssl.get(), rfd);
  SSL_set_wfd(ssl.get(), wfd);

  CBS cbs, handoff;
  CBS_init(&cbs, input.data(), input.size());
  if (!CBS_get_asn1_element(&cbs, &handoff, CBS_ASN1_SEQUENCE) ||
      !DeserializeContextState(&cbs, ctx.get()) ||
      !SetTestState(ssl.get(), TestState::Deserialize(&cbs, ctx.get())) ||
      !GetTestState(ssl.get()) ||
      !SSL_apply_handoff(ssl.get(), handoff)) {
    fprintf(stderr, "Handoff application failed.\n");
    return false;
  }

  int ret = 0;
  for (;;) {
    ret = CheckIdempotentError(
        "SSL_do_handshake", ssl.get(),
        [&]() -> int { return SSL_do_handshake(ssl.get()); });
    if (SSL_get_error(ssl.get(), ret) == SSL_ERROR_WANT_READ) {
      // Synchronize with the proxy, i.e. don't let the handshake continue until
      // the proxy has sent more data.
      char msg = kControlMsgWantRead;
      if (write_eintr(control, &msg, 1) != 1 ||
          read_eintr(control, &msg, 1) != 1 ||
          msg != kControlMsgWriteCompleted) {
        fprintf(stderr, "read via proxy failed\n");
        return false;
      }
      continue;
    }
    if (!RetryAsync(ssl.get(), ret)) {
      break;
    }
  }
  if (!HandbackReady(ssl.get(), ret)) {
    ERR_print_errors_fp(stderr);
    return false;
  }

  ScopedCBB output;
  CBB handback;
  if (!CBB_init(output.get(), 1024) ||
      !CBB_add_u24_length_prefixed(output.get(), &handback) ||
      !SSL_serialize_handback(ssl.get(), &handback) ||
      !SerializeContextState(ctx.get(), output.get()) ||
      !GetTestState(ssl.get())->Serialize(output.get())) {
    fprintf(stderr, "Handback serialisation failed.\n");
    return false;
  }

  char msg = kControlMsgHandback;
  if (write_eintr(control, &msg, 1) == -1 ||
      write_eintr(control, CBB_data(output.get()), CBB_len(output.get())) ==
          -1) {
    perror("write");
    return false;
  }
  return true;
}

int SignalError() {
  const char msg = kControlMsgError;
  if (write_eintr(kFdControl, &msg, 1) != 1) {
    return 2;
  }
  return 1;
}

}  // namespace

int main(int argc, char **argv) {
  TestConfig initial_config, resume_config, retry_config;
  if (!ParseConfig(argc - 1, argv + 1, &initial_config, &resume_config,
                   &retry_config)) {
    return SignalError();
  }
  const TestConfig *config = initial_config.handshaker_resume
      ? &resume_config : &initial_config;
#if defined(BORINGSSL_UNSAFE_DETERMINISTIC_MODE)
  if (initial_config.handshaker_resume) {
    // If the PRNG returns exactly the same values when trying to resume then a
    // "random" session ID will happen to exactly match the session ID
    // "randomly" generated on the initial connection. The client will thus
    // incorrectly believe that the server is resuming.
    uint8_t byte;
    RAND_bytes(&byte, 1);
  }
#endif  // BORINGSSL_UNSAFE_DETERMINISTIC_MODE

  // read() will return the entire message in one go, because it's a datagram
  // socket.
  constexpr size_t kBufSize = 1024 * 1024;
  std::vector<uint8_t> handoff(kBufSize);
  ssize_t len = read_eintr(kFdControl, handoff.data(), handoff.size());
  if (len == -1) {
    perror("read");
    return 2;
  }
  if (!Handshaker(config, kFdProxyToHandshaker, kFdHandshakerToProxy, handoff,
                  kFdControl)) {
    return SignalError();
  }
  return 0;
}
