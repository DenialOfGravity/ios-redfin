/*
 * Built-in replacement for HFS's IOKit glue (core/hfs_iokit.cpp and
 * hfs_encodings/hfs_encodings_kext.cpp) on SWEET.
 *
 * On Apple platforms HFS is a kext: IOKit matches its IOService on
 * IOResources and start() registers the filesystem. SWEET links HFS into
 * the kernel instead, so bsd_init calls sweet_hfs_register() right after
 * vfsinit(), before the root filesystem is mounted. The registration is the
 * kext's start() sequence.
 *
 * The helpers that query IOKit storage keep their answers for SWEET, which
 * has no IOStorageFamily and therefore no IOMedia objects: nothing is
 * ejectable and no media carries an external journal. The platform expert
 * publishes no serial number. Content protection (AppleKeyStore) is not
 * built (sweet_hfs_compat.h).
 */
#include <libkern/OSKextLib.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>

#include "hfs.h"
#include "hfs_cnode.h"
#include "hfs_dbg.h"
#include "hfs_iokit.h"
#include "hfs_encodings/hfs_encodings_internal.h"

#ifndef panic_on_assert
bool panic_on_assert;
#endif

#if DEBUG
bool hfs_corruption_panics = true;
#endif

extern struct vnodeopv_desc hfs_vnodeop_opv_desc;
extern struct vnodeopv_desc hfs_specop_opv_desc;
extern struct vnodeopv_desc hfs_fifoop_opv_desc;
extern struct vfsops hfs_vfsops;

void sweet_hfs_register(void);

static vfstable_t sweet_hfs_vfs_handle;

void
sweet_hfs_register(void)
{
#ifndef panic_on_assert
	panic_on_assert = PE_i_can_has_kernel_configuration() & kPEICanHasAssertions;
#endif

#if DEBUG
	PE_parse_boot_argn("hfs_corruption_panics", &hfs_corruption_panics, sizeof(hfs_corruption_panics));
#endif

	/* The HFSEncodings kext's start(): its converters must exist before any mount. */
	hfs_converterinit();

	struct vnodeopv_desc *op_descs[] = {
		&hfs_vnodeop_opv_desc,
		&hfs_specop_opv_desc,
#if FIFO
		&hfs_fifoop_opv_desc,
#endif
	};

#ifndef VFS_TBLVNOP_SECLUDE_RENAME
#define VFS_TBLVNOP_SECLUDE_RENAME 0
#endif

	struct vfs_fsentry vfe = {
		.vfe_vfsops = &hfs_vfsops,
		.vfe_vopcnt = sizeof(op_descs) / sizeof(op_descs[0]),
		.vfe_opvdescs = op_descs,
		.vfe_fsname = "hfs",
		.vfe_flags = (VFS_TBLNOTYPENUM | VFS_TBLLOCALVOL | VFS_TBLREADDIR_EXTENDED
		    | VFS_TBL64BITREADY | VFS_TBLVNOP_PAGEOUTV2 | VFS_TBLVNOP_PAGEINV2
		    | VFS_TBLTHREADSAFE | VFS_TBLCANMOUNTROOT | VFS_TBLVNOP_SECLUDE_RENAME
		    | VFS_TBLNATIVEXATTR)
	};

	int ret = vfs_fsadd(&vfe, &sweet_hfs_vfs_handle);
	if (ret) {
		panic("SWEET HFS: vfs_fsadd failed: %d", ret);
	}

	hfs_init_zones();
	hfs_sysctl_register();

	uint32_t num_cpus;
	size_t sz = sizeof(num_cpus);

	if (sysctlbyname("hw.physicalcpu", &num_cpus, &sz, NULL, 0) == 0) {
		if ((2 * num_cpus) > MAX_CACHED_ORIGINS_DEFAULT) {
			_hfs_max_origins = 2 * num_cpus;
			_hfs_max_file_origins = 2 * num_cpus;
		} else if ((2 * num_cpus) > MAX_CACHED_FILE_ORIGINS_DEFAULT) {
			_hfs_max_file_origins = 2 * num_cpus;
		}
	}

	printf("SWEET HFS: hfs-627.100.6 registered\n");
}

/* Built in, not a loadable kext: there is no kext to retain per mount. */
OSKextLoadTag
sweet_hfs_current_load_tag(void)
{
	return kOSKextInvalidLoadTag;
}

int
hfs_is_ejectable(__unused const char *cdev_name)
{
	return 0;
}

void
hfs_iterate_media_with_content(__unused const char *content_uuid_cstring,
    __unused int (*func)(const char *bsd_name, const char *uuid_str, void *arg),
    __unused void *arg)
{
}

kern_return_t
hfs_get_platform_serial_number(char *serial_number_str, uint32_t len)
{
	if (len < 1) {
		return 0;
	}
	serial_number_str[0] = '\0';
	return KERN_NOT_SUPPORTED;
}
