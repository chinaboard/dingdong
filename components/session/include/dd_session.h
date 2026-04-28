#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Token = 32 random bytes hex-encoded → 64 chars + NUL
#define DD_SESSION_TOKEN_LEN 65

esp_err_t dd_session_init(void);

// Allocate a fresh session, write its token (NUL-terminated) into out.
// Caller pastes it into a Set-Cookie header.
esp_err_t dd_session_create(char *token_out, size_t cap);

// True if the token is currently valid (exists & not expired).
bool dd_session_is_valid(const char *token);

// Best-effort revoke. No-op if token not found.
void dd_session_destroy(const char *token);

// Wipe all sessions (e.g. on password change).
void dd_session_destroy_all(void);

#ifdef __cplusplus
}
#endif
