/*
 * cpu_syn68k_battery.cpp - syn68k native CRC opcode battery
 *
 * Runs tests/obj/syn68k_battery (syn68k/test syngentest) and compares stdout to
 * golden output in syn68k/test/output/. Exercises the syn68k translator directly,
 * separate from the Musashi .bin battery.
 */

#include "cpu_tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#ifndef SYN68K_BATTERY_BIN
#define SYN68K_BATTERY_BIN "tests/obj/syn68k_battery"
#endif

#ifndef SYN68K_BATTERY_GOLDEN
#define SYN68K_BATTERY_GOLDEN "../syn68k/test/output/10000-notnative"
#endif

static const char *syn68k_battery_bin(void)
{
	const char *env = getenv("SYN68K_BATTERY_BIN");
	if (env && env[0])
		return env;
#ifdef SYN68K_BATTERY_BIN_PATH
	return SYN68K_BATTERY_BIN_PATH;
#else
	return SYN68K_BATTERY_BIN;
#endif
}

static const char *syn68k_battery_golden(void)
{
	const char *env = getenv("SYN68K_BATTERY_GOLDEN");
	if (env && env[0])
		return env;
#ifdef SYN68K_BATTERY_GOLDEN_PATH
	return SYN68K_BATTERY_GOLDEN_PATH;
#else
	return SYN68K_BATTERY_GOLDEN;
#endif
}

static int slurp_file(const char *path, char **out, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	long n = ftell(f);
	if (n < 0) {
		fclose(f);
		return -1;
	}
	rewind(f);
	char *buf = (char *)malloc((size_t)n + 1);
	if (!buf) {
		fclose(f);
		return -1;
	}
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	buf[got] = '\0';
	*out = buf;
	*out_len = got;
	return 0;
}

static int run_syn68k_battery(char **stdout_buf, size_t *stdout_len)
{
	int pipefd[2];
	if (pipe(pipefd) != 0)
		return -1;

	pid_t pid = fork();
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execl(syn68k_battery_bin(), syn68k_battery_bin(),
		      "10000", "-notnative", (char *)NULL);
		_exit(127);
	}
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}

	close(pipefd[1]);
	char *buf = NULL;
	size_t cap = 0, len = 0;
	for (;;) {
		if (len + 4096 > cap) {
			cap = cap ? cap * 2 : 65536;
			char *nb = (char *)realloc(buf, cap);
			if (!nb) {
				free(buf);
				close(pipefd[0]);
				waitpid(pid, NULL, 0);
				return -1;
			}
			buf = nb;
		}
		ssize_t n = read(pipefd[0], buf + len, cap - len);
		if (n <= 0)
			break;
		len += (size_t)n;
	}
	close(pipefd[0]);

	int status = 0;
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		free(buf);
		return -1;
	}

	buf = (char *)realloc(buf, len + 1);
	if (buf)
		buf[len] = '\0';
	*stdout_buf = buf;
	*stdout_len = len;
	return 0;
}

void test_syn68k_native_battery(const char *engine)
{
	if (strcmp(engine, "syn68k") != 0)
		return;

	printf("Running syn68k native CRC battery (%s)...\n", engine);

	if (access(syn68k_battery_bin(), X_OK) != 0) {
		CHECK_ENG(false, engine,
		          "syn68k_battery missing (run: make -C BasiliskII/tests syn68k_battery)");
		return;
	}

	char *golden = NULL, *actual = NULL;
	size_t golden_len = 0, actual_len = 0;
	if (slurp_file(syn68k_battery_golden(), &golden, &golden_len) != 0) {
		CHECK_ENG(false, engine, "could not read syn68k golden output file");
		return;
	}

	if (run_syn68k_battery(&actual, &actual_len) != 0) {
		free(golden);
		CHECK_ENG(false, engine, "syn68k_battery failed or crashed");
		return;
	}

	bool ok = (actual_len == golden_len && memcmp(actual, golden, golden_len) == 0);
	if (!ok) {
		/* Report first diverging line for easier debugging. */
		const char *ga = golden, *aa = actual;
		int line = 1;
		while (*ga || *aa) {
			const char *gend = strchr(ga, '\n');
			const char *aend = strchr(aa, '\n');
			size_t glen = gend ? (size_t)(gend - ga) : strlen(ga);
			size_t alen = aend ? (size_t)(aend - aa) : strlen(aa);
			if (glen != alen || memcmp(ga, aa, glen) != 0) {
				char msg[512];
				snprintf(msg, sizeof(msg),
				         "syn68k battery mismatch at line %d (expected golden %s)",
				         line, syn68k_battery_golden());
				CHECK_ENG(false, engine, msg);
				free(golden);
				free(actual);
				return;
			}
			if (!gend && !aend)
				break;
			line++;
			ga = gend ? gend + 1 : ga + glen;
			aa = aend ? aend + 1 : aa + alen;
		}
	}

	free(golden);
	free(actual);
	CHECK_ENG(ok, engine, "syn68k native CRC battery matches golden output");
}
