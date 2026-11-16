/* The only SMCs qseeprobe may issue (read-only QSEE queries + the shmbridge buffer registration they may need). */
#include <stddef.h>
#include "qsee_wl.h"

const struct qsee_wl_ent qsee_whitelist[] = {
	{ QSEE_SMC_FEAT_VERSION, QSEE_ARGS_FEAT_VERSION, "TZ_INFO_GET_FEATURE_VERSION" },
	{ QSEE_SMC_APP_LOOKUP, QSEE_ARGS_APP_LOOKUP, "TZ_OS_APP_LOOKUP" },
	{ QSEE_SMC_SHMB_CREATE, QSEE_ARGS_SHMB_CREATE, "TZ_SHM_BRIDGE_CREATE" },
	{ QSEE_SMC_SHMB_DELETE, QSEE_ARGS_SHMB_DELETE, "TZ_SHM_BRIDGE_DELETE" },
};
const unsigned qsee_whitelist_n = sizeof(qsee_whitelist) / sizeof(qsee_whitelist[0]);

const struct qsee_wl_ent *qsee_allowed(uint64_t x0, uint64_t x1)
{
	for (unsigned i = 0; i < qsee_whitelist_n; i++)
		if (qsee_whitelist[i].x0 == x0)
			return qsee_whitelist[i].arginfo == x1 ? &qsee_whitelist[i] : NULL;
	return NULL;
}
