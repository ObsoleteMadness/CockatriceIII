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
 *
 * Windows has no fork(), so there run_isolated() starts the test binary again
 * with COCKATRICE_TEST_CASE=<n>. The child runs main() as usual, so all setup
 * is repeated, but skips every isolated test except the n-th, runs that one
 * between two marker lines and exits. The parent forwards the output between
 * the markers and adds the counts printed on the closing one, so reporting is
 * the same as with fork(), and a hang or crash fails only that test. Isolated
 * tests must therefore be reached in the same order on every run.
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
#include <errno.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif
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

#ifdef _WIN32
/*
 * Prints the exception code and address of an unhandled host exception, then
 * exits. Guest bus faults are handled earlier by the emulator's vectored
 * handler, so only real crashes reach this filter.
 *
 * Arguments:
 *   info: Exception record and context from Windows.
 *
 * Returns:
 *   Does not return.
 */
static LONG WINAPI test_crash_filter(EXCEPTION_POINTERS *info)
{
	printf("\n*** CRASH: exception 0x%08lx at address %p ***\n",
	       (unsigned long)info->ExceptionRecord->ExceptionCode,
	       info->ExceptionRecord->ExceptionAddress);
	fflush(stdout);
	_exit(1);
	return EXCEPTION_EXECUTE_HANDLER;
}

/*
 * Installs the unhandled-exception filter used by every test binary.
 */
static void test_install_crash_handler(void)
{
	SetUnhandledExceptionFilter(test_crash_filter);
}
#else
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
#endif /* _WIN32 */

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
#if defined(__cplusplus) && defined(_WIN32)
/* Isolated-test bookkeeping, defined in test_env.cpp. */
extern "C" int g_isolated_case_next;	// Index the next run_isolated() call gets
extern "C" int g_isolated_case_target;	// In a child: the one index to run; -1 in the parent

/* Lines that bracket a child's isolated test in its output. */
#define TEST_CASE_BEGIN_MARK "@@cockatrice-test-case-begin"
#define TEST_CASE_END_MARK "@@cockatrice-test-case-end"

/*
 * Runs isolated test number index in a child copy of this process and merges
 * its result, reporting a timeout, crash or missing result as one failure.
 *
 * Arguments:
 *   name: Label printed on failure.
 *   index: The test's run_isolated() call number, passed to the child.
 *   timeout_sec: Wall time before the child is killed.
 */
static void test_run_case_in_child(const char *name, int index, int timeout_sec)
{
	char msg[320];

	// The child's stdout and stderr go to a temporary file the parent parses
	char dir[MAX_PATH + 1], out_path[MAX_PATH + 1];
	if (!GetTempPathA(sizeof(dir), dir) || !GetTempFileNameA(dir, "ctc", 0, out_path)) {
		snprintf(msg, sizeof(msg), "%s: no temporary file for the child's output", name);
		CHECK(false, msg);
		return;
	}
	SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };	// Inheritable handle
	HANDLE out = CreateFileA(out_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
	                         CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
	if (out == INVALID_HANDLE_VALUE) {
		snprintf(msg, sizeof(msg), "%s: cannot open %s", name, out_path);
		CHECK(false, msg);
		return;
	}

	// Same executable and arguments; the environment selects the test
	char exe[MAX_PATH + 1];
	GetModuleFileNameA(NULL, exe, sizeof(exe));
	char *cmdline = _strdup(GetCommandLineA());
	char index_str[16];
	snprintf(index_str, sizeof(index_str), "%d", index);
	SetEnvironmentVariableA("COCKATRICE_TEST_CASE", index_str);

	STARTUPINFOA si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = out;
	si.hStdError = out;
	PROCESS_INFORMATION pi;
	fflush(stdout);
	BOOL started = CreateProcessA(exe, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
	SetEnvironmentVariableA("COCKATRICE_TEST_CASE", NULL);
	free(cmdline);
	CloseHandle(out);
	if (!started) {
		snprintf(msg, sizeof(msg), "%s: cannot start the child (error %lu)", name,
		         (unsigned long)GetLastError());
		CHECK(false, msg);
		DeleteFileA(out_path);
		return;
	}

	// Wait for the test, killing it if it overruns
	bool timed_out = WaitForSingleObject(pi.hProcess, (DWORD)timeout_sec * 1000) != WAIT_OBJECT_0;
	if (timed_out) {
		TerminateProcess(pi.hProcess, 99);
		WaitForSingleObject(pi.hProcess, INFINITE);
	}
	DWORD exit_code = 0;
	GetExitCodeProcess(pi.hProcess, &exit_code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	// Forward the test's own output, and take its counts from the closing marker
	bool have_counts = false;
	int passed = 0, failed = 0;
	FILE *fp = fopen(out_path, "rb");
	if (fp) {
		char line[1024];
		bool inside = false;
		while (fgets(line, sizeof(line), fp)) {
			line[strcspn(line, "\r\n")] = 0;
			if (strcmp(line, TEST_CASE_BEGIN_MARK) == 0) {
				inside = true;
			} else if (strncmp(line, TEST_CASE_END_MARK, strlen(TEST_CASE_END_MARK)) == 0) {
				have_counts = sscanf(line + strlen(TEST_CASE_END_MARK), "%d %d", &passed, &failed) == 2;
				inside = false;
			} else if (inside) {
				printf("%s\n", line);
			}
		}
		fclose(fp);
	}
	DeleteFileA(out_path);
	fflush(stdout);

	if (have_counts && !timed_out) {
		g_pass += passed;
		g_fail += failed;
		return;
	}
	if (timed_out)
		snprintf(msg, sizeof(msg), "%s: timed out after %ds (child killed) -- possible infinite loop",
		         name, timeout_sec);
	else
		snprintf(msg, sizeof(msg), "%s: child exited without a result (exit code 0x%lx)", name,
		         (unsigned long)exit_code);
	CHECK(false, msg);
}

/*
 * Runs fn in its own process (see the file comment for the Windows scheme).
 *
 * In the parent, starts a child for this test and merges its result. In a
 * child, returns at once for every test but the selected one; for that one,
 * runs fn between the marker lines and exits.
 *
 * Arguments:
 *   name: Label printed on timeout/crash.
 *   fn: Test body. May call CHECK/CHECK_ENG.
 *   timeout_sec: Wall time before the child is killed; 0 uses TEST_DEFAULT_TIMEOUT.
 */
template<typename Fn>
static void run_isolated(const char *name, Fn fn, int timeout_sec = TEST_DEFAULT_TIMEOUT)
{
	if (timeout_sec <= 0)
		timeout_sec = TEST_DEFAULT_TIMEOUT;
	int index = g_isolated_case_next++;

	if (g_isolated_case_target < 0) {
		test_run_case_in_child(name, index, timeout_sec);
		return;
	}
	if (index != g_isolated_case_target)
		return;

	// This process exists to run this one test; match the POSIX child's empty cache
	printf("%s\n", TEST_CASE_BEGIN_MARK);
	cpu_engine_invalidate_code(0, ~0u);
	int before_pass = g_pass, before_fail = g_fail;
	fn();
	printf("%s %d %d\n", TEST_CASE_END_MARK, g_pass - before_pass, g_fail - before_fail);
	fflush(stdout);
	_exit(0);
}
#elif defined(__cplusplus)
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
