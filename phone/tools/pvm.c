/*
 * pvm PID [TOP]: a vmmap -summary of sorts (redfin ws-cam-leak). The task's footprint and compressed
 * size (TASK_VM_INFO), then its regions summed by user tag (VM_MEMORY_*: 1 malloc, 2 malloc small,
 * 3 malloc large, 7 malloc tiny, 11 malloc nano, 30 shared pmap/IOKit, 53 CoreVideo? etc.: numbers
 * printed raw), with virtual, resident, dirty and swapped-out (compressed) page counts, and the TOP
 * (default 12) largest regions with their tag, protection and share mode.
 */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>

struct sum { uint64_t virt, res, dirty, swapped; unsigned n; };
struct big { mach_vm_address_t a; mach_vm_size_t s; unsigned tag, prot, share, res, swapped, dirty; };

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: pvm PID [TOP]\n");
		return 2;
	}
	int pid = atoi(argv[1]), top = argc > 2 ? atoi(argv[2]) : 12;
	mach_port_t t;
	if (task_for_pid(mach_task_self(), pid, &t) != KERN_SUCCESS) {
		fprintf(stderr, "task_for_pid failed\n");
		return 1;
	}
	task_vm_info_data_t vi;
	mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
	if (task_info(t, TASK_VM_INFO, (task_info_t)&vi, &c) == KERN_SUCCESS)
		printf("pid %d virt %lluM res %lluM footprint %lluM compressed %lluM internal %lluM external %lluM purgeable_vol %lluM\n", pid,
		    vi.virtual_size >> 20, vi.resident_size >> 20, vi.phys_footprint >> 20, vi.compressed >> 20, vi.internal >> 20,
		    vi.external >> 20, vi.purgeable_volatile_virtual >> 20);
	static struct sum s[256];
	struct big *b = calloc(top + 1, sizeof(*b));
	int nb = 0;
	mach_vm_address_t a = 0;
	natural_t depth = 0;
	for (;;) {
		mach_vm_size_t size = 0;
		vm_region_submap_info_data_64_t r;
		mach_msg_type_number_t rc = VM_REGION_SUBMAP_INFO_COUNT_64;
		if (mach_vm_region_recurse(t, &a, &size, &depth, (vm_region_recurse_info_t)&r, &rc) != KERN_SUCCESS)
			break;
		if (r.is_submap) {
			depth++;
			continue;
		}
		unsigned tag = r.user_tag & 255;
		s[tag].virt += size;
		s[tag].res += r.pages_resident;
		s[tag].dirty += r.pages_dirtied;
		s[tag].swapped += r.pages_swapped_out;
		s[tag].n++;
		struct big x = { a, size, tag, r.protection, r.share_mode, r.pages_resident, r.pages_swapped_out, r.pages_dirtied };
		int i = nb < top ? nb++ : top;
		if (i == top && (top == 0 || b[top - 1].s >= size))
			goto next;
		if (i == top)
			i = top - 1;
		while (i > 0 && b[i - 1].s < size) {
			b[i] = b[i - 1];
			i--;
		}
		b[i] = x;
	next:
		a += size;
	}
	unsigned long pg = vm_kernel_page_size ? vm_kernel_page_size : 16384;
	printf("tag  regions  virtM  resM  dirtyM  swappedM\n");
	for (int i = 0; i < 256; i++)
		if (s[i].n)
			printf("%3d %8u %6llu %5llu %7llu %9llu\n", i, s[i].n, s[i].virt >> 20, (s[i].res * pg) >> 20, (s[i].dirty * pg) >> 20,
			    (s[i].swapped * pg) >> 20);
	printf("largest regions:\n");
	for (int i = 0; i < nb; i++)
		printf("0x%llx %6lluM tag %u prot %u share %u resM %lu swpM %lu dirtyM %lu\n", b[i].a, b[i].s >> 20, b[i].tag, b[i].prot,
		    b[i].share, (b[i].res * pg) >> 20, (b[i].swapped * pg) >> 20, (b[i].dirty * pg) >> 20);
	return 0;
}
