/* The only SMCs qseeprobe may issue (QSEE queries, the shmbridge registration, and since ws-qsee-b app start/send-data/shutdown plus the listener-abort path). */
#include <stddef.h>
#include "qsee_wl.h"

const struct qsee_wl_ent qsee_whitelist[] = {
	{ QSEE_SMC_FEAT_VERSION, QSEE_ARGS_FEAT_VERSION, "TZ_INFO_GET_FEATURE_VERSION" },
	{ QSEE_SMC_APP_LOOKUP, QSEE_ARGS_APP_LOOKUP, "TZ_OS_APP_LOOKUP" },
	{ QSEE_SMC_SHMB_CREATE, QSEE_ARGS_SHMB_CREATE, "TZ_SHM_BRIDGE_CREATE" },
	{ QSEE_SMC_SHMB_DELETE, QSEE_ARGS_SHMB_DELETE, "TZ_SHM_BRIDGE_DELETE" },
	{ QSEE_SMC_APP_START, QSEE_ARGS_APP_START, "TZ_OS_APP_START" },
	{ QSEE_SMC_APP_SHUTDOWN, QSEE_ARGS_APP_SHUTDOWN, "TZ_OS_APP_SHUTDOWN" },
	{ QSEE_SMC_SEND_DATA, QSEE_ARGS_SEND_DATA, "TZ_APP_QSAPP_SEND_DATA" },
	{ QSEE_SMC_SEND_DATA_WL, QSEE_ARGS_SEND_DATA_WL, "TZ_APP_QSAPP_SEND_DATA_WITH_WHITELIST" },
	{ QSEE_SMC_LSTNR_RESP, QSEE_ARGS_LSTNR_RESP, "TZ_OS_LISTENER_RESPONSE_HANDLER" },
	{ QSEE_SMC_CONT_BLOCKED, QSEE_ARGS_CONT_BLOCKED, "TZ_OS_CONTINUE_BLOCKED_REQUEST" },
	{ QSEE_SMC_CONT_BLOCKED_SI, QSEE_ARGS_CONT_BLOCKED, "TZ_OS_CONTINUE_BLOCKED_REQUEST_SMCINVOKE" },
	/* gatekeeper phase 2 (qsee_lsnr.inc): our own RPMB / file system / time listeners */
	{ QSEE_SMC_LSTNR_REG_SI, QSEE_ARGS_LSTNR_REG, "TZ_OS_REGISTER_LISTENER_SMCINVOKE" },
	{ QSEE_SMC_LSTNR_REG, QSEE_ARGS_LSTNR_REG, "TZ_OS_REGISTER_LISTENER" },
	{ QSEE_SMC_LSTNR_DEREG, QSEE_ARGS_LSTNR_DEREG, "TZ_OS_DEREGISTER_LISTENER" },
	{ QSEE_SMC_LSTNR_RESP_WL, QSEE_ARGS_LSTNR_RESP_WL, "TZ_OS_LISTENER_RESPONSE_HANDLER_WITH_WHITELIST" },
};
const unsigned qsee_whitelist_n = sizeof(qsee_whitelist) / sizeof(qsee_whitelist[0]);

const struct qsee_wl_ent *qsee_allowed(uint64_t x0, uint64_t x1)
{
	for (unsigned i = 0; i < qsee_whitelist_n; i++)
		if (qsee_whitelist[i].x0 == x0)
			return qsee_whitelist[i].arginfo == x1 ? &qsee_whitelist[i] : NULL;
	return NULL;
}
