/*
 * redfin camera back end (ws-cam-icp): the Titan 175 ICP (Cortex-A5 running Qualcomm's CAMERA_ICP.elf) driven from
 * userland through /dev/pmem, like phone/video (Venus) and adspboot. Our own code; protocol facts from the GPL
 * redbull kernel (techpack/camera/drivers/cam_icp: hfi.c, hfi_reg.h, hfi_sys_defs.h, hfi_session_defs.h,
 * cam_icp_hw_mgr.c) and the GPL device tree (lito-camera.dtsi, camcc-lito.c). Firmware-internal command layouts
 * (IPE/BPS config and frame data) are NOT here: they live in the private camera tree (icp-iface.h, icp_ipe.c).
 */
#ifndef ICP_H
#define ICP_H
#include <stdint.h>
#include <stddef.h>

/* ---- clocks, power (camcc-lito.c, lito-gdsc.dtsi) ---- */
#define CAMCC		0x0ad00000ull
#define GCC_CAM		0x0010b000ull	/* GCC 0xb000 page: camera AHB (0x08, critical), HF AXI (0x28), SF AXI (0x2c) */
#define CC_PLL0		0x0000u		/* Lucid, 1200 MHz: even /2 600, odd /3 400 */
#define CC_PLL1		0x1000u		/* Lucid, 600 MHz: even /2 300 (IPE) */
#define GDSC_TITAN	0xc1c4u
#define GDSC_IPE0	0x8004u
#define GDSC_BPS	0x7004u
#define GDSC_CLK	0xc1a0u		/* camcc's GDSC clock (critical) */

/* ---- ICP A5 (DT a5_sierra 0xac10000, a5_csr 0xac18000): hfi_reg.h offsets, all on the sierra base ---- */
#define A5_SIERRA	0x0ac10000ull
#define A5_CSR		0x0ac18000ull
#define HFI_A5_HW_VERSION	0x000
#define HFI_A5_NSEC_RESET	0x004
#define HFI_A5_CONTROL		0x008
#define HFI_A5_A2HOSTINTEN	0x010
#define HFI_A5_A2HOSTINTCLR	0x018
#define HFI_A5_A2HOSTINTSTATUS	0x01c
#define HFI_A5_HOST2ICPINT	0x030
#define HFI_A5_CSR_ACCESS	0x03c		/* qos-val 0xa0a (a5_core.c) */
#define HFI_FW_VERSION		0x044
#define HFI_HOST_INIT_REQ	0x048
#define HFI_ICP_INIT_RESP	0x04c
#define HFI_SHARED_MEM_PTR	0x050
#define HFI_SHARED_MEM_SIZE	0x054
#define HFI_QTBL_PTR		0x058
#define HFI_UNCACHED_HEAP_PTR	0x05c
#define HFI_UNCACHED_HEAP_SIZE	0x060
#define HFI_SFR_PTR		0x068
#define HFI_QDSS_IOVA		0x06c
#define HFI_QDSS_SIZE		0x070
#define HFI_IO_REGION_IOVA	0x074
#define HFI_IO_REGION_SIZE	0x078
#define HFI_IO2_REGION_IOVA	0x07c
#define HFI_IO2_REGION_SIZE	0x080
#define HFI_A5_STATUS		0x200
#define A5_CTL_WAKE_UP_EN	(1u << 4)
#define A5_CTL_EN		(1u << 9)
#define A5_CTL_CLKGATE_WFI	(1u << 12)
#define A5_STATUS_WFI		(1u << 7)

/* ---- HFI packets (hfi_sys_defs.h / hfi_session_defs.h) ---- */
#define HFI_CMD_SYS_INIT		0x00010001u
#define HFI_CMD_SYS_PC_PREP		0x00010002u
#define HFI_CMD_SYS_SET_PROPERTY	0x00010003u
#define HFI_CMD_SYS_PING		0x00010005u
#define HFI_CMD_IPEBPS_CREATE_HANDLE	0x01010008u
#define HFI_CMD_IPEBPS_ASYNC_DIRECT	0x0101000au
#define HFI_CMD_IPEBPS_ASYNC_INDIRECT	0x0101000eu
#define HFI_MSG_SYS_INIT_DONE		0x00020001u
#define HFI_MSG_SYS_PC_PREP_DONE	0x00020002u
#define HFI_MSG_SYS_DEBUG		0x00020003u
#define HFI_MSG_SYS_PING_ACK		0x00020006u
#define HFI_MSG_EVENT_NOTIFY		0x00020008u
#define HFI_MSG_IPEBPS_CREATE_HANDLE_ACK 0x01020008u
#define HFI_MSG_IPEBPS_ASYNC_DIRECT_ACK	0x0102000au
#define HFI_MSG_IPEBPS_ASYNC_INDIRECT_ACK 0x0102000eu
#define HFI_PROP_SYS_DEBUG_CFG		0x1u
#define HFI_PROP_SYS_UBWC_CFG		0x2u
#define HFI_DEBUG_MODE_QUEUE		0x1u
#define HFI_HANDLE_BPS			1u
#define HFI_HANDLE_IPE_RT		2u
#define HFI_HANDLE_IPE_NON_RT		3u
#define HFI_OP_BPS_CONFIG_IO		1u
#define HFI_OP_BPS_FRAME_PROCESS	2u
#define HFI_OP_BPS_DESTROY		4u
#define HFI_OP_IPE_CONFIG_IO		5u
#define HFI_OP_IPE_FRAME_PROCESS	6u
#define HFI_OP_IPE_DESTROY		8u

/* ---- memory: kept-out RAM below 4 GB that XNU never owns at xram tier 1 (1054: pil_camera, pil_npu, pil_cdsp) ---- */
#define PA_FW		0x86000000ull	/* pil_camera_mem, 5 MB: the firmware region, IOVA 0 */
#define FW_LEN		0x00500000u
#define PA_HFI		0x86500000ull	/* pil_npu (5 MB): HFI 1 MB | secheap 1 MB | qdss 1 MB | 2 MB unused */
#define PA_SECHEAP	0x86600000ull
#define PA_QDSS		0x86700000ull
#define PA_SHBUF	0x86800000ull	/* pil_npu's last 2 MB: firmware-CPU-visible buffers at IOVA_SHBUF */
#define SHBUF_LEN	0x00200000u
#define PA_IO		0x87400000ull	/* pil_cdsp (30 MB): 28 MB buffers | 1 MB unused | 1 MB SMMU tables */
#define IO_LEN		0x01c00000u
#define PA_TBL		0x89100000ull
#define TBL_LEN		0x00100000u
/* IOVAs: the Linux layout of the "icp" bank (lito-camera.dtsi iova-mem-map) */
#define IOVA_SHARED	0x07400000u
#define SHARED_LEN	0x09600000u	/* told to the firmware; we back the first 1 MB */
#define IOVA_SHBUF	0x07800000u	/* in the shared region, given to the A5 MMU by MEM_MAP (0x7500000+64 KB is mapped by the firmware itself); the A5's own MMU maps it (the IO region it does not:
				 * a CONFIG_IO buffer at IOVA 0x10c00000 took a data abort, FSR 0x805) */
#define IOVA_SECHEAP	0x10a00000u
#define SECHEAP_LEN	0x00100000u
#define IOVA_QDSS	0x10b00000u
#define QDSS_LEN	0x00100000u
#define IOVA_IO		0x10c00000u
#define IO_REGION_LEN	0xcf300000u	/* told to the firmware; we back IO_LEN */
/* inside the HFI MB (offsets; IOVA = IOVA_SHARED + off, PA = PA_HFI + off) */
#define HFI_QTBL	0x00000u
#define HFI_CMDQ	0x10000u
#define HFI_MSGQ	0x20000u
#define HFI_DBGQ	0x40000u
#define HFI_SFR		0x80000u
#define CMDQ_BYTES	4096u
#define MSGQ_BYTES	4096u
#define DBGQ_BYTES	102400u
#define SFR_BYTES	4096u

/* ---- API (icp_hw.c) ---- */
extern int icp_verbose;
int icp_mem_check(void);			/* boot-args: the kept-out ranges really are outside XNU RAM */
int icp_camera_idle(void);			/* 1: PLL0 off (nobody's camera clocks running) */
int icp_power_up(int ipe);			/* titan top, PLL0, CPAS/ICP clocks (+ IPE0 GDSC, PLL1, IPE0 clocks) */
void icp_power_down(void);			/* undo exactly what icp_power_up changed */
int icp_ipe_power_up(void);
int icp_smmu_up(void);				/* translating bank for the 9 ICP streams; 0 = ok */
void icp_smmu_down(void);
void icp_smmu_faults(const char *tag);
int icp_load_fw(const char *path);		/* zero the 5 MB, copy the PT_LOAD segments */
int icp_boot(void);				/* hfi_init: A5 on, queues, init handshake */
void icp_halt(void);				/* PC prep, A5 off */
int icp_cmd(const void *pkt);			/* append to the command queue + doorbell */
/* wait for a message of TYPE (0 = any) in the message queue, printing debug-queue text meanwhile; the packet is
 * copied to OUT (up to OUTLEN bytes). 0 = got it, -1 = timeout. */
int icp_wait(uint32_t type, void *out, size_t outlen, unsigned ms);
void icp_drain_dbg(void);
int icp_set_prop(uint32_t prop, const uint32_t *vals, unsigned n);
/* IO region buffers (bump allocator over PA_IO): IOVA, PA */
uint32_t icp_io_alloc(uint32_t len, uint64_t *pa);
uint64_t icp_io_pa(uint32_t iova);
uint32_t icp_sh_alloc(uint32_t len, uint64_t *pa);	/* shared-region buffers (commands the firmware reads/writes) */
void icp_regs(const char *tag);
double icp_ms(void);
void icp_status(void);
void icp_cleanup(void);				/* best effort: everything back (atexit and on signals) */
int icp_state_down(void);			/* icpctl down: undo what a dead run recorded */

#endif
