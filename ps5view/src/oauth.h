// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// PSN OAuth helpers for determining the account id, following chiaki-ng's
// scripts/psn-account-id.py: the client credentials are those of Sony's
// Remote Play Windows client.

#ifndef PV_OAUTH_H
#define PV_OAUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PV_PSN_CLIENT_ID "ba495a24-818c-472b-b12d-ff231c1b5745"
#define PV_PSN_CLIENT_SECRET "mvaiZkRsAsI1IBkY"
#define PV_PSN_REDIRECT_URI "https://remoteplay.dl.playstation.net/remoteplay/redirect"
#define PV_PSN_AUTHORIZE_URL "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize"
#define PV_PSN_TOKEN_URL "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token"
// Test hook: overrides the token URL (and derives the authorize URL from it).
#define PV_PSN_TOKEN_URL_ENV "PS5VIEW_PSN_TOKEN_URL"

char *pv_oauth_login_url(const char *authorize_url);

typedef enum
{
	PV_REDIRECT_OK,
	PV_REDIRECT_EMPTY,
	PV_REDIRECT_NO_CODE,
	PV_REDIRECT_ERROR_PARAM // e.g. ?error=access_denied
} PvRedirectResult;

// Accepts the full redirect URL (or just the code) and extracts the code.
PvRedirectResult pv_oauth_extract_code(const char *input, char *code, size_t code_size, char *error, size_t error_size);

// user_id (decimal string) -> 8 byte little endian -> base64
bool pv_oauth_user_id_to_account_id(const char *user_id, uint8_t raw[8], char *b64, size_t b64_size);

typedef enum
{
	PV_OAUTH_ERR_NONE,
	PV_OAUTH_ERR_CODE_INVALID,      // invalid_grant: code expired or already used
	PV_OAUTH_ERR_TOO_MANY_REQUESTS, // too_many_requests / error code 4177 / HTTP 429
	PV_OAUTH_ERR_CLIENT,            // invalid_client / unauthorized_client
	PV_OAUTH_ERR_REQUEST,           // invalid_request (e.g. truncated code)
	PV_OAUTH_ERR_SERVER,            // HTTP 5xx
	PV_OAUTH_ERR_NOT_JSON,          // unexpected (HTML) answer
	PV_OAUTH_ERR_OTHER
} PvOAuthErrorKind;

typedef struct pv_oauth_error_t
{
	PvOAuthErrorKind kind;
	long http_status;
	char error[64];
	long error_code;
	char description[256];
} PvOAuthError;

void pv_oauth_classify_error(long http_status, const char *body, PvOAuthError *out);

#endif
