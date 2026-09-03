/*
 * test_harness.h - Single-header CHECK macros, crash handler, and hang isolation
 *
 * Basilisk II / Cockatrice III tests share a global Macintosh address space
 * and swappable 680x0 engines. A full library (Catch2/doctest) does not help
 * with fork+alarm isolation, so this header is the whole framework.
 *
 * Hang-prone work (Execute68k, opcode images, ROM snippets) must go through
 * run_isolated(): a child that exceeds TEST_DEFAULT_TIMEOUT seconds is killed
 * and reported as a failure so the parent can continue.
 */

#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Default per-test spin timeout in seconds. Healthy tests finish far sooner. */
#ifndef TEST_DEFAULT_TIMEOUT
#define TEST_DEFAULT_TIMEOUT 30
#endif

extern int g_pass;
extern int g_fail;

#ifdef __cplusplus
}
#endif

/*
 * Prints a crash dump then exits.
 *
 * Arguments:
 *   sig: POSIX signal number.
 *   info: Signal info including the faulting address.
 *   ucontext: Host ucontext_t for register dump.
 */
static void test_crash_handler(int sig, siginfo_t *info, void *ucontext)
{
	printf("\n*** CRASH SIGNAL %d at address %p ***\n", sig, info ? info->si_addr : NULL);
#if defined(__APPLE__) && defined(__arm64__)
	ucontext_t *uc = (ucontext_t *)ucontext;
	if (uc) {
		printf("  PC: 0x%llx, LR: 0x%llx, SP: 0x%llx\n",
		       uc->uc_mcontext->__ss.__pc,
		       uc->uc_mcontext->__ss.__lr,
		       uc->uc_mcontext->__ss.__sp);
	}
#else
	(void)ucontext;
#endif
	fflush(stdout);
	_exit(sig);
}

/*
 * Installs SIGSEGV/SIGBUS/SIGILL handlers used by every test binary.
 */
static void test_install_crash_handler(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = test_crash_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
}

#define CHECK(expr, msg) do { \
	if (expr) { \
		g_pass++; \
		printf("  [PASS] %s\n", msg); \
	} else { \
		g_fail++; \
		printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
	} \
	fflush(stdout); \
} while (0)

#define CHECK_ENG(expr, engine, msg) do { \
	char _eng_msg[320]; \
	snprintf(_eng_msg, sizeof(_eng_msg), "[%s] %s", engine, msg); \
	CHECK((expr), _eng_msg); \
} while (0)

/*
 * Runs fn in a forked child and merges its pass/fail counts.
 *
 * The child arms alarm(timeout_sec) so an infinite Execute68k becomes one
 * reported failure instead of a wedged suite. The parent keeps its Mac RAM
 * and JIT cache (copy-on-write); the child's writes do not leak back.
 *
 * Arguments:
 *   name: Label printed on timeout/crash.
 *   fn: Test body. May call CHECK/CHECK_ENG.
 *   timeout_sec: Wall time before SIGALRM; 0 uses TEST_DEFAULT_TIMEOUT.
 */
#ifdef __cplusplus
template<typename Fn>
static void run_isolated(const char *name, Fn fn, int timeout_sec = TEST_DEFAULT_TIMEOUT)
{
	if (timeout_sec <= 0)
		timeout_sec = TEST_DEFAULT_TIMEOUT;

	int pipefd[2];
	if (pipe(pipefd) != 0) {
		char msg[256];
		snprintf(msg, sizeof(msg), "%s: pipe() failed; running without isolation", name);
		CHECK(false, msg);
		fn();
		return;
	}

	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		close(pipefd[0]);
		signal(SIGALRM, [](int) { _exit(99); });
		alarm((unsigned)timeout_sec);
		cpu_engine_invalidate_code(0, ~0u);
		int before_pass = g_pass, before_fail = g_fail;
		fn();
		alarm(0);
		fflush(stdout);
		int counts[2] = { g_pass - before_pass, g_fail - before_fail };
		ssize_t written = write(pipefd[1], counts, sizeof(counts));
		(void)written;
		close(pipefd[1]);
		_exit(0);
	}

	close(pipefd[1]);
	int counts[2] = { 0, 0 };
	ssize_t n = read(pipefd[0], counts, sizeof(counts));
	close(pipefd[0]);
	int status = 0;
	waitpid(pid, &status, 0);

	if (n == (ssize_t)sizeof(counts)) {
		g_pass += counts[0];
		g_fail += counts[1];
		return;
	}

	char msg[320];
	if (WIFEXITED(status) && WEXITSTATUS(status) == 99)
		snprintf(msg, sizeof(msg),
		         "%s: timed out after %ds (child killed) -- possible infinite loop",
		         name, timeout_sec);
	else if (WIFSIGNALED(status))
		snprintf(msg, sizeof(msg),
		         "%s: child crashed (signal %d)", name, WTERMSIG(status));
	else
		snprintf(msg, sizeof(msg),
		         "%s: child exited abnormally (wait status 0x%x)", name, status);
	CHECK(false, msg);
}
#endif /* __cplusplus */

#endif /* TEST_HARNESS_H */
