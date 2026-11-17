/*
 * vmpeek PID ADDR [LEN]          hex dump LEN bytes (default 16) of another process at ADDR
 * vmpeek PID ADDR w BYTE...      write bytes at ADDR (writable pages only), then dump them
 * ADDR is an unslid shared-cache address (0x18..-0x1f..: the process's shared cache slide is added) or, with a
 * leading '=', an absolute address. Prints the slide. Needs task_for_pid (pstack's entitlements).
 */
#include <mach/mach.h>
#include <mach-o/dyld_images.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: vmpeek PID ADDR [LEN] | vmpeek PID ADDR w BYTE...\n");
		return 2;
	}
	task_t t;
	kern_return_t kr = task_for_pid(mach_task_self(), atoi(argv[1]), &t);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "task_for_pid: %s\n", mach_error_string(kr));
		return 1;
	}
	uint64_t slide = 0;
	struct task_dyld_info di;
	mach_msg_type_number_t cnt = TASK_DYLD_INFO_COUNT;
	if (task_info(t, TASK_DYLD_INFO, (task_info_t)&di, &cnt) == KERN_SUCCESS) {
		struct dyld_all_image_infos ai;
		vm_size_t got = 0;
		if (vm_read_overwrite(t, di.all_image_info_addr, sizeof(ai), (vm_address_t)&ai, &got) == KERN_SUCCESS)
			slide = ai.sharedCacheSlide;
	}
	uint64_t a;
	if (argv[2][0] == '=')
		a = strtoull(argv[2] + 1, NULL, 0);
	else
		a = strtoull(argv[2], NULL, 0) + slide;
	printf("slide 0x%llx addr 0x%llx\n", (unsigned long long)slide, (unsigned long long)a);
	size_t len = 16;
	if (argc > 3 && !strcmp(argv[3], "w")) {
		unsigned char buf[256];
		int n = 0;
		for (int i = 4; i < argc && n < 256; i++)
			buf[n++] = (unsigned char)strtoul(argv[i], NULL, 0);
		kr = vm_write(t, (vm_address_t)a, (vm_offset_t)buf, n);
		if (kr != KERN_SUCCESS) {
			fprintf(stderr, "vm_write: %s\n", mach_error_string(kr));
			return 1;
		}
		len = n;
	} else if (argc > 3) {
		len = strtoul(argv[3], NULL, 0);
	}
	unsigned char *b = malloc(len);
	vm_size_t got = 0;
	kr = vm_read_overwrite(t, (vm_address_t)a, len, (vm_address_t)b, &got);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "vm_read: %s\n", mach_error_string(kr));
		return 1;
	}
	for (size_t i = 0; i < len; i++)
		printf("%02x%s", b[i], (i % 16 == 15 || i + 1 == len) ? "\n" : " ");
	return 0;
}
