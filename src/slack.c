/* src/slack.c
 * Optional libcurl Slack webhook sender (compile with -DHAVE_CURL or
 * auto-detected). Rate-limited, templated alerts for hard breaches and
 * terminations. Phase 7. 2-space.
 */

#include "postgres.h"
#include "fmgr.h"

#include "compat.h"

#ifdef HAVE_CURL
#include <curl/curl.h>
#include <time.h>
#endif

/* GUC from pg_rolequota.c (non-static) */
extern char *pg_rolequota_slack_webhook_url;

/* Simple process-local rate limit (BGW is single-threaded; 5s min between
 * posts) */
#ifdef HAVE_CURL
static time_t slack_last_sent = 0;
static const int SLACK_RATE_LIMIT_SECS = 5;
#endif

void pg_rolequota_slack_send(const char *message) {
  if (message == NULL || message[0] == '\0')
    return;

#ifdef HAVE_CURL
  const char *url = pg_rolequota_slack_webhook_url;
  time_t now = time(NULL);

  if (url == NULL || url[0] == '\0')
    return;

  /* SSRF protection + token hygiene: strict https://hooks.slack.com/ prefix
   * only. Never log the URL (may contain secret token).
   */
  if (strncmp(url, "https://hooks.slack.com/", 24) != 0) {
    elog(WARNING, "pg_rolequota slack: webhook URL does not start with "
                  "https://hooks.slack.com/ - refusing to send (SSRF guard)");
    return;
  }

  if (now - slack_last_sent < SLACK_RATE_LIMIT_SECS) {
    elog(DEBUG1, "pg_rolequota slack: rate-limited (last sent < %d s ago)",
         SLACK_RATE_LIMIT_SECS);
    return;
  }

  CURL *curl = curl_easy_init();
  if (!curl)
    return;

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  /* Minimal safe JSON (our messages have no " inside; replace for
   * defense-in-depth) */
  char payload[512];
  char safe[256];
  int j = 0;
  for (int i = 0; message[i] && j < (int)sizeof(safe) - 1; i++) {
    char c = message[i];
    if (c == '"')
      safe[j++] = '\'';
    else if (c == '\\')
      safe[j++] = '/';
    else
      safe[j++] = c;
  }
  safe[j] = '\0';
  snprintf(payload, sizeof(payload), "{\"text\":\"pg_rolequota: %s\"}", safe);

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  /* No redirect follow to further reduce SSRF surface */
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

  CURLcode res = curl_easy_perform(curl);
  if (res == CURLE_OK) {
    slack_last_sent = now;
    elog(DEBUG1, "pg_rolequota slack: notification sent successfully");
  } else {
    elog(WARNING, "pg_rolequota slack: POST failed: %s",
         curl_easy_strerror(res));
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
#else
  elog(LOG, "pg_rolequota slack: libcurl not compiled in - notification is a "
            "graceful no-op");
#endif
}

/* Test function always present */
Datum pg_rolequota_slack_test(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pg_rolequota_slack_test);

Datum pg_rolequota_slack_test(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(
      true); /* linkage proof for the .c file (AGENTS.md); send() still respects
                HAVE_CURL at compile time for present/absent paths */
}
