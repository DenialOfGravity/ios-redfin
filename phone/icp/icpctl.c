/*
 * icpctl (ws-cam-icp): bring up the camera back end's ICP (A5 + CAMERA_ICP.elf) from userland.
 *   icpctl status            read-only: clocks, GDSCs, A5 registers (only while its clock runs)
 *   icpctl alive [FW]        M1: power (titan top, PLL0, CPAS/ICP + BPS/IPE as Linux hw_open), SMMU bank for the 9
 *                            ICP streams, firmware load, HFI init, SYS_INIT, debug level, PING; then everything off
 *   icpctl ipe [FW] [ARGS]   M2 (private build only): alive + one IPE frame (see icp_ipe.c)
 *   icpctl down              undo what a run that died left (recorded in /var/tmp/icpctl.state)
 * Env: ICPCTL_V=1 verbose, ICPCTL_MIN=1 A5 + CPAS only, ICPCTL_NOIPE=1 no IPE power for alive, ICPCTL_NOIRQ=1 keep
 * A2HOSTINTEN 0, ICPCTL_TBUVOTE=1 vote the SF TBU if it is off, ICPCTL_SHARED=1 accept a running PLL0 (camera on),
 * ICPCTL_DBG=0xNN firmware debug level (default 0x1e: MEDIUM|HIGH|ERROR|FATAL), ICPCTL_KEEP_MS=N stay up N ms.
 * Default firmware: /var/local/scratch/camicp/CAMERA_ICP.elf. Nothing runs unless the camera is idle (PLL0 off).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../video/pmem.h"
#include "icp.h"

#ifdef ICP_PRIVATE
int icp_ipe_run(int argc, char **argv);		/* private/camera/tools/icp_ipe.c */
#endif

static void on_signal(int sig)
{
	printf("icp: signal %d: cleaning up\n", sig);
	icp_cleanup();
	_exit(3);
}

static double tstage;
static void stage(const char *name)
{
	double now = icp_ms();
	printf("icp: stage %s: %.0f ms\n", name, now - tstage);
	tstage = now;
}

static int alive(const char *fw)
{
	uint32_t m[64];
	tstage = icp_ms();
	if (icp_power_up(0))
		return -1;
	stage("power");
	if (icp_smmu_up())
		return -1;
	stage("smmu");
	if (icp_load_fw(fw))
		return -1;
	stage("load");
	if (icp_boot())
		return -1;
	stage("init handshake");
	uint32_t dbg[2] = { getenv("ICPCTL_DBG") ? (uint32_t)strtoul(getenv("ICPCTL_DBG"), NULL, 0) : 0x1e,
			    HFI_DEBUG_MODE_QUEUE };
	if (getenv("ICPCTL_WAIT0")) {	/* diagnosis: watch the firmware idle before any command */
		unsigned w = (unsigned)atoi(getenv("ICPCTL_WAIT0"));
		(void)icp_wait(0xffffffffu, m, sizeof(m), w);
	}
	if (getenv("ICPCTL_DBGFIRST") && icp_set_prop(HFI_PROP_SYS_DEBUG_CFG, dbg, 2))
		return -1;
	uint32_t init[2] = { 8, HFI_CMD_SYS_INIT };
	if (icp_cmd(init) || icp_wait(HFI_MSG_SYS_INIT_DONE, m, sizeof(m), 5000))
		return -1;
	printf("icp: SYS_INIT_DONE err %u num_prop %u data %08x %08x %08x %08x %08x %08x\n", m[2], m[3], m[4], m[5], m[6],
	       m[7], m[8], m[9]);
	if (m[2])
		return -1;
	stage("SYS_INIT");
	if (icp_set_prop(HFI_PROP_SYS_DEBUG_CFG, dbg, 2))
		return -1;
	uint32_t ping[4] = { 16, HFI_CMD_SYS_PING, 0x1c0ffee, 0x5eed };
	if (icp_cmd(ping) || icp_wait(HFI_MSG_SYS_PING_ACK, m, sizeof(m), 2000))
		return -1;
	printf("icp: PING_ACK user_data %08x%08x (%s)\n", m[3], m[2], m[2] == 0x1c0ffee && m[3] == 0x5eed ? "matches" :
	       "DIFFERENT");
	stage("ping");
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2) {
		fprintf(stderr, "usage: icpctl status | alive [FW] | ipe [FW] [ARGS] | down\n");
		return 2;
	}
	icp_verbose = getenv("ICPCTL_V") && atoi(getenv("ICPCTL_V"));
	pm_init();
	if (!strcmp(argv[1], "status")) {
		icp_status();
		return 0;
	}
	if (!strcmp(argv[1], "down"))
		return icp_state_down() ? 1 : 0;
	if (strcmp(argv[1], "alive") && strcmp(argv[1], "ipe")) {
		fprintf(stderr, "icpctl: unknown command %s\n", argv[1]);
		return 2;
	}
#ifndef ICP_PRIVATE
	if (!strcmp(argv[1], "ipe")) {
		fprintf(stderr, "icpctl: ipe needs the private build (ICP_PRIVATE)\n");
		return 2;
	}
#endif
	const char *fw = argc > 2 && argv[2][0] == '/' ? argv[2] : "/var/local/scratch/camicp/CAMERA_ICP.elf";
	if (access("/var/tmp/icpctl.state", F_OK) == 0) {
		printf("icp: /var/tmp/icpctl.state exists (an earlier run died): run icpctl down first\n");
		return 1;
	}
	if (icp_mem_check())
		return 1;
	if (!icp_camera_idle() && !getenv("ICPCTL_SHARED")) {
		printf("icp: camcc PLL0 is running: the camera (IFE) is in use; close it first\n");
		return 1;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGALRM, on_signal);
	signal(SIGPIPE, SIG_IGN);
	alarm(120);			/* never leave the hardware on longer than this */
	double t0 = icp_ms();
	int rc = alive(fw);
#ifdef ICP_PRIVATE
	if (!rc && !strcmp(argv[1], "ipe"))
		rc = icp_ipe_run(argc - 2, argv + 2);
#endif
	if (!rc && getenv("ICPCTL_KEEP_MS")) {
		unsigned keep = (unsigned)atoi(getenv("ICPCTL_KEEP_MS"));
		for (unsigned i = 0; i < keep / 10; i++) {
			icp_drain_dbg();
			usleep(10000);
		}
	}
	icp_drain_dbg();
	icp_smmu_faults("end");
	tstage = icp_ms();
	icp_cleanup();
	stage("teardown");
	printf("icp: %s %s in %.0f ms\n", argv[1], rc ? "FAILED" : "ok", icp_ms() - t0);
	return rc ? 1 : 0;
}
