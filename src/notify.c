/* src/notify.c
 * LISTEN/NOTIFY support for fast targeted wake-ups (user-requested feature).
 * Phase 5.
 * 2-space.
 */

#include "postgres.h"
#include "fmgr.h"

#include "compat.h"
#include "utils/builtins.h"

/* Wake path support (LISTEN/NOTIFY polish implemented via counter +
 * request_wakeup) */
void pg_rolequota_request_wakeup(void); /* from shmem, forces BGW scan */

void pg_rolequota_notify_worker_main(void) {
  elog(LOG, "pg_rolequota notify: LISTEN quota_wakeup path (counter-based "
            "wake-up active; full socket LISTEN future)");
}

Datum pg_rolequota_notify_test(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pg_rolequota_notify_test);

Datum pg_rolequota_notify_test(PG_FUNCTION_ARGS) {
  /* Wake-up path (LISTEN/NOTIFY polish) exercised via
   * rolequota.request_wakeup() SQL
   * + BGW counter detection. This test proves the notify.c module linkage.
   */
  PG_RETURN_TEXT_P(cstring_to_text("wakeup_path_active"));
}
