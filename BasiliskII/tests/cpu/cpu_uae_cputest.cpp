/*
 * cpu_uae_cputest.cpp - Run vendored WinUAE cputest smoke preset
 *
 * Executes tests/obj/uae_cputest (CPU_TESTER generator) for the CockatriceSmoke
 * preset in amiberry/cputest/cockatrice_cputest.ini. Only wired for UAE configs.
 */

#include "cpu_tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#ifndef UAE_CPUTEST_BIN
#define UAE_CPUTEST_BIN "tests/obj/uae_cputest"
#endif

static const char *uae_cputest_bin(void)
{
	const char *env = getenv("UAE_CPUTEST_BIN");
	if (env && env[0])
		return env;
#ifdef UAE_CPUTEST_BIN_PATH
	return UAE_CPUTEST_BIN_PATH;
#else
	return UAE_CPUTEST_BIN;
#endif
}

void test_uae_cputest_smoke(const char *engine)
{
	if (strcmp(engine, "uae") != 0 &&
	    strncmp(engine, "uae+", 4) != 0)
		return;

	printf("Running UAE cputest smoke (CPU_TESTER) (%s)...\n", engine);

	const char *bin = uae_cputest_bin();
	if (access(bin, X_OK) != 0) {
		CHECK_ENG(false, engine,
		          "uae_cputest missing (run: BasiliskII/scripts/vendor-uae-cputest.sh && make -C BasiliskII/tests uae_cputest)");
		return;
	}

	char dirbuf[512];
	strncpy(dirbuf, bin, sizeof(dirbuf) - 1);
	dirbuf[sizeof(dirbuf) - 1] = '\0';
	char *slash = strrchr(dirbuf, '/');
	if (slash)
		*slash = '\0';
	else
		strcpy(dirbuf, ".");

	char cmd[768];
	snprintf(cmd, sizeof(cmd), "cd '%s' && '%s'", dirbuf, bin);
	int rc = system(cmd);
	if (rc == 0) {
		CHECK_ENG(true, engine, "UAE cputest CockatriceSmoke preset passed");
		return;
	}

	char msg[320];
	if (rc == -1)
		snprintf(msg, sizeof(msg), "UAE cputest system() failed");
	else if (WIFEXITED(rc))
		snprintf(msg, sizeof(msg), "UAE cputest failed (exit %d)", WEXITSTATUS(rc));
	else if (WIFSIGNALED(rc))
		snprintf(msg, sizeof(msg), "UAE cputest crashed (signal %d)", WTERMSIG(rc));
	else
		snprintf(msg, sizeof(msg), "UAE cputest failed (status %d)", rc);
	CHECK_ENG(false, engine, msg);
}
