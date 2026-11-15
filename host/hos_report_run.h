/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* First call at normal startup, never first called from a fault handler. Returns
   128 system-random bits as 32 lowercase hex digits, or "unknown" on failure.
   Cached for this process; contains no pid, time, machine or user information. */
const char *hos_report_run_id(void);

/* All report workers share DbgHelp's documented single-threaded API boundary.
   Try before suspending any thread. Never block or call this on a failing thread. */
int hos_report_dbghelp_try(void);
void hos_report_dbghelp_done(void);
