/* reactions.c --
 *
 * Authors:
 *   Steve Grubb <sgrubb@redhat.com>
 *
 */

#include "config.h"
#include <stdio.h>
#include <stdio_ext.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <syslog.h>
#include <time.h>  // nanosleep
#include <errno.h>
#include <pwd.h>
#include <string.h>
#include "ids.h"
#include "reactions.h"
#include "timer-services.h"

// Returns 0 on success and 1 on failure
static int safe_exec(const char *exe, ...)
{
	char **argv;
	va_list ap;
	unsigned int i;
	int pid;
	struct sigaction sa;

	if (exe == NULL) {
		syslog(LOG_ALERT,
			"Safe_exec passed NULL for program to execute");
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		syslog(LOG_ALERT,
			"Audit IDS failed to fork doing safe_exec");
		return 1;
	}
	if (pid)        /* Parent */
		return 0; // FIXME: should we waitpid to know if it succeeded?

	/* Child */
	sigfillset (&sa.sa_mask);
	sigprocmask (SIG_UNBLOCK, &sa.sa_mask, 0);

	va_start(ap, exe);
	for (i = 1; va_arg(ap, char *) != NULL; i++);
	va_end(ap);
	argv = alloca(i * sizeof(char *));

	va_start(ap, exe);
	argv[0] = (char *) exe;
	for (i = 1; (argv[i] = (char *) va_arg(ap, char *)) != NULL; i++);
	va_end(ap);
	argv[i] = NULL;

	execve(exe, argv, NULL);
        syslog(LOG_ALERT, "Audit IDS failed to exec %s", exe);
	exit(1);
}

static void minipause(void)
{
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 120 * 1000 * 1000; // 120 milliseconds
	nanosleep(&ts, NULL);
}

int kill_process(pid_t pid)
{
	if (pid <= 0)
		return 1;

	if (debug)
		my_printf("reaction kill -KILL %d", pid);

	return kill(pid, SIGKILL);
}

int kill_session(int session)
{
	char ses[16];

	// Do not kill session -1 or the system will die
	if (session < 0)
		return 1;

	snprintf(ses, sizeof(ses), "%d", session);
	if (debug)
		my_printf("reaction killall -d %s", ses);
	return safe_exec("/usr/bin/killall", "-d", ses, NULL);
}

static int  uid_min = -1;
static void read_uid_min(void)
{
	FILE *f;
	char buf[100];
	int uid = -1;

	if (uid_min > 0)
		return;

	f = fopen("/etc/login.defs", "r");
	if (f == NULL)
		return;
	__fsetlocking(f, FSETLOCKING_BYCALLER);
	while (fgets(buf, sizeof(buf), f)) {
		if (memcmp(buf, "UID_MIN", 7) == 0) {
			if (sscanf(buf, "UID_MIN %d", &uid) == 1) {
				if (uid != -1) {
					uid_min = uid;
					if (debug)
						my_printf("uid_min set to %d",
							uid_min);
				}
			}
			break;
		}
	}
	fclose(f);
}

/* returns 0 if user account and 1 on anything else */
static int verify_acct(const char *acct)
{
	struct passwd *pw;

	if (acct == NULL)
		return 1;

	// Make sure valid acct
	errno = 0;
	pw = getpwnam(acct);
	if (pw == NULL || errno)
		return 1;

	// Make sure not a daemon
	if (strstr(pw->pw_shell, "nologin"))
		return 1;
	if (uid_min < 0) {
		read_uid_min();
		if (uid_min < 0)
			return 1;
	}
	if ((int)pw->pw_uid < uid_min)
		return 1;

	return 0;
}

int restricted_role(const char *acct)
{
	int rc;

	if (verify_acct(acct))
		return 1;

	// Restrict to guest user
	rc = safe_exec("/usr/sbin/semanage", "login", "-m", "-s",
		"guest_u", acct);
	if (rc)
		return rc;

	// Need to force a logout of all sessions for the user
	return safe_exec("/usr/bin/killall", "--user", acct);
}

int force_password_reset(const char *acct)
{
	if (verify_acct(acct))
		return 1;

	return safe_exec("/usr/bin/chage", "-d", "0", acct);
}

int lock_account(const char *acct)
{
	if (verify_acct(acct))
		return 1;

	return safe_exec("/usr/bin/passwd", "-l", acct);
}

int unlock_account(const char *acct)
{
	if (verify_acct(acct))
		return 1;

	return safe_exec("/usr/bin/passwd", "-u", acct);
}

int lock_account_timed(const char *acct, unsigned long length)
{
	int rc = lock_account(acct);

	if (rc)
		return rc;

	add_timer_job(UNLOCK_ACCOUNT, acct, length);

	return 0;
}

int block_ip_address(const char *addr)
{
	if (debug)
		my_printf("reaction /sbin/iptables -I INPUT -s %s -j DROP",
							addr);
	minipause();
	return safe_exec("/usr/sbin/iptables", "-I", "INPUT", "-s", addr,
			"-j","DROP", NULL);
}

int block_ip_address_timed(const char *addr, unsigned long length)
{
	int rc = block_ip_address(addr);
	if (rc)
		return rc;

	add_timer_job(UNBLOCK_ADDRESS, addr, length);

	return 0;
}

int unblock_ip_address(const char *addr)
{
	if (debug)
		my_printf("reaction /sbin/iptables -D INPUT -s %s -j DROP",
							addr);
	minipause();
	return safe_exec("/usr/sbin/iptables", "-D", "INPUT", "-s", addr,
			"-j","DROP", NULL);
}

int system_reboot(void)
{
	return safe_exec("/sbin/init", "6");
}

int system_single_user(void)
{
	return safe_exec("/sbin/init", "1");
}

int system_halt(void)
{
	return safe_exec("/sbin/init", "0");
}

