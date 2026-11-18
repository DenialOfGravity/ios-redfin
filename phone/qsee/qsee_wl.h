/*
 * qseeprobe's SMC IDs and the hard whitelist (ws-qsee-a). Values from Linux 4.19 (Pixel 5 redbull kernel,
 * LineageOS android_kernel_google_redbull lineage-22.2 d784d02):
 *   include/soc/qcom/qseecomi.h   TZ_SYSCALL_CREATE_SMC_ID (390), PARAM_ID (395-417), TZ_OS_APP_LOOKUP_ID (465-470),
 *                                 TZ_INFO_GET_FEATURE_VERSION_ID (561-565), QSEOS_RESULT_* (79-83), QSEOS_APP_ID (29)
 *   drivers/soc/qcom/scm.c        x0 = fn_id | SMC64_MASK 0x40000000 (48, 366); x1 = arginfo, x2..x4 = args[0..2],
 *                                 x5 = args[3] (326), x6 = 0 (105); ret[0..2] = x1..x3
 *   drivers/misc/qseecom.c        version: qseecom_init_control feature = 10 (9340-9353) through SCM_SVC_INFO 3
 *                                 (553-561); lookup: QSEOS_APP_LOOKUP_COMMAND (627-647): args[0] = pa of the name,
 *                                 args[1] = strlen(name); x1..x3 -> result, resp_type, data (1178-1180)
 *   drivers/soc/qcom/qtee_shmbridge.c  CREATE 0x02000C1E (37-43), DELETE 0x02000C1D (49-54), arg packing (69-90),
 *                                 qtee_shmbridge_register (201-252), _deregister (262-273), default bridge with
 *                                 VMID_HLOS 3 and R|W (342-396)
 * Nothing outside qsee_whitelist[] may reach PMEM_SMC (qseeprobe.c: qsmc()).
 */
#ifndef QSEE_WL_H
#define QSEE_WL_H
#include <stdint.h>

#define TZ_SMC_ID(o, s, f)	((uint32_t)((((o) & 0x3f) << 24) | ((s) & 0xff) << 8 | ((f) & 0xff)))
#define SMC64			0x40000000u	/* scm.c SMC64_MASK, the scm_version_mask on arm64 */
#define TZ_OWNER_SIP		2
#define TZ_OWNER_QSEE_OS	50
#define TZ_OWNER_TZ_APPS	48
#define TZ_SVC_APP_MGR		1
#define TZ_SVC_LISTENER		2
#define TZ_SVC_APP_ID_PLACEHOLDER 0
#define TZ_SVC_INFO		6
#define TZ_SVC_MEMORY_PROTECTION 12
#define TZ_ARGS(n)		((uint64_t)(n) & 0xf)
#define TZ_ARG(i, t)		((uint64_t)(t) << (4 + 2 * (i)))	/* type of argument i (0-based) */
#define TZ_VAL			0
#define TZ_BUF_RW		2

/* read-only calls (ws-qsee-a; x0 as issued) */
#define QSEE_SMC_FEAT_VERSION	(TZ_SMC_ID(TZ_OWNER_SIP, TZ_SVC_INFO, 0x03) | SMC64)		/* 0x42000603 */
#define QSEE_SMC_APP_LOOKUP	(TZ_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 0x03) | SMC64)	/* 0x72000103 */
#define QSEE_SMC_SHMB_CREATE	(TZ_SMC_ID(TZ_OWNER_SIP, TZ_SVC_MEMORY_PROTECTION, 0x1e) | SMC64) /* 0x42000c1e */
#define QSEE_SMC_SHMB_DELETE	(TZ_SMC_ID(TZ_OWNER_SIP, TZ_SVC_MEMORY_PROTECTION, 0x1d) | SMC64) /* 0x42000c1d */

/* ws-qsee-b: app load / send / unload and the abort path for listener requests (qseecomi.h 451-462, 521-548,
 * 674-694; qseecom.c qseecom_scm_call2 590-890). Never: LOAD_SERVICES_IMAGE, listener register, RPMB, keystore,
 * TZ_SHM_BRIDGE_ENABLE. */
#define QSEE_SMC_APP_START	(TZ_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 0x01) | SMC64)	/* 0x72000101 */
#define QSEE_SMC_APP_SHUTDOWN	(TZ_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 0x02) | SMC64)	/* 0x72000102 */
#define QSEE_SMC_LSTNR_RESP	(TZ_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x03) | SMC64)	/* 0x72000203 */
#define QSEE_SMC_CONT_BLOCKED	(TZ_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x04) | SMC64)	/* 0x72000204 */
#define QSEE_SMC_CONT_BLOCKED_SI (TZ_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x07) | SMC64)	/* 0x72000207 */
#define QSEE_SMC_SEND_DATA	(TZ_SMC_ID(TZ_OWNER_TZ_APPS, TZ_SVC_APP_ID_PLACEHOLDER, 0x01) | SMC64) /* 0x70000001 */
#define QSEE_SMC_SEND_DATA_WL	(TZ_SMC_ID(TZ_OWNER_TZ_APPS, TZ_SVC_APP_ID_PLACEHOLDER, 0x06) | SMC64) /* 0x70000006 */

/* gatekeeper phase 2 (read-only): listener register (smcinvoke, legacy), deregister, whitelist response
 * (qseecomi.h 506-521, 718-723; qseecom.c 701-731, 756-781). RPMB provision/erase (0x720004xx) stay refused. */
#define QSEE_SMC_LSTNR_REG	(TZ_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x01) | SMC64)	/* 0x72000201 */
#define QSEE_SMC_LSTNR_DEREG	(TZ_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x02) | SMC64)	/* 0x72000202 */
#define QSEE_SMC_LSTNR_RESP_WL	(TZ_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x05) | SMC64)	/* 0x72000205 */
#define QSEE_SMC_LSTNR_REG_SI	(TZ_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x06) | SMC64)	/* 0x72000206 */
#define QSEE_ARGS_LSTNR_REG	(TZ_ARGS(3) | TZ_ARG(1, TZ_BUF_RW))	/* id, sb pa, sb len: 0x83 */
#define QSEE_ARGS_LSTNR_DEREG	TZ_ARGS(1)				/* 0x01 */
#define QSEE_ARGS_LSTNR_RESP_WL	(TZ_ARGS(4) | TZ_ARG(2, TZ_BUF_RW))	/* id, status, sgl pa, sgl len: 0x204 */

#define QSEE_ARGS_FEAT_VERSION	(TZ_ARGS(1) | TZ_ARG(0, TZ_VAL))				/* 0x01 */
#define QSEE_ARGS_APP_LOOKUP	(TZ_ARGS(2) | TZ_ARG(0, TZ_BUF_RW) | TZ_ARG(1, TZ_VAL))		/* 0x22 */
#define QSEE_ARGS_SHMB_CREATE	TZ_ARGS(4)							/* 0x04 */
#define QSEE_ARGS_SHMB_DELETE	TZ_ARGS(1)							/* 0x01 */
#define QSEE_ARGS_APP_START	TZ_ARGS(3)				/* mdt_len, img_len, pa: 0x03 */
#define QSEE_ARGS_APP_SHUTDOWN	TZ_ARGS(1)				/* app_id: 0x01 */
#define QSEE_ARGS_LSTNR_RESP	TZ_ARGS(2)				/* listener_id, status: 0x02 */
#define QSEE_ARGS_CONT_BLOCKED	TZ_ARGS(1)				/* app or session id: 0x01 */
#define QSEE_ARGS_SEND_DATA	(TZ_ARGS(5) | TZ_ARG(1, TZ_BUF_RW) | TZ_ARG(3, TZ_BUF_RW))			/* 0x885 */
#define QSEE_ARGS_SEND_DATA_WL	(TZ_ARGS(7) | TZ_ARG(1, TZ_BUF_RW) | TZ_ARG(3, TZ_BUF_RW) | TZ_ARG(5, TZ_BUF_RW))	/* 0x8887 */

#define QSEE_FEATURE_QSEE	10		/* qseecom_init_control: uint32_t feature = 10 */
#define QSEE_FEATURE_WHITELIST	15		/* qseecom.c FEATURE_ID_WHITELIST; >= 1.0.0 (1 << 22) -> send-data with whitelist */
#define QSEE_WHITELIST_V1	(1u << 22)
#define RPMB_SERVICE		0x2000
#define SSD_SERVICE		0x3000
#define MAX_ION_FD		4		/* sglistinfo table: 4 entries of {indexAndFlags, sizeOrCount} */
#define SGLIST_SET(c, s, i)	((uint32_t)((((c) & 1u) << 31) | (((s) & 1u) << 30) | ((i) & 0x3fffffffu)))
#define N_EXT_SCM_ARGS		7		/* scm.c: args[3..9] go in a buffer whose pa is x5 when nargs > 4 */
#define QSEE_APP_NAME_MAX	64		/* uapi qseecom.h MAX_APP_NAME_SIZE */
#define QSEOS_RESULT_SUCCESS	0u
#define QSEOS_RESULT_INCOMPLETE	1u
#define QSEOS_RESULT_BLOCKED	2u		/* QSEOS_RESULT_BLOCKED_ON_LISTENER */
#define QSEOS_RESULT_FAILURE	0xffffffffu
#define QSEOS_APP_ID		0xee01u
#define SCM_INTERRUPTED		1
#define SCM_V2_EBUSY		(-12)
/* shmbridge packing (qtee_shmbridge.c) */
#define VMID_HLOS		3
#define SHMB_PERM_RW		6		/* PERM_READ 4 | PERM_WRITE 2 */
#define SHMB_NUM_VM_SHIFT	9

struct qsee_wl_ent {
	uint64_t x0;
	uint64_t arginfo;
	const char *name;
};
extern const struct qsee_wl_ent qsee_whitelist[];
extern const unsigned qsee_whitelist_n;
/* the entry for x0, or NULL; x1 must also match the entry's arginfo */
const struct qsee_wl_ent *qsee_allowed(uint64_t x0, uint64_t x1);

#endif
