/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive for
 * more details.
 *
 * Copyright (C) 2013 Pierre and Marie Curie University
 *  Joël Porquet <joel.porquet@lip6.fr>
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_platform.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/smp.h>

#include <asm/io.h>
#include <asm/numa.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp_map.h>

static struct resource kernel_code_resource = { .name = "Kernel code", };
static struct resource kernel_data_resource = { .name = "Kernel data", };
static struct resource kernel_bss_resource = { .name = "Kernel bss", };

extern struct boot_param_header __dtb_start; /* defined by Linux */
static void *dtb_start = &__dtb_start;

unsigned long *bootloader_param[4];

unsigned long __cpu_logical_map[NR_CPUS] = {
	/* all entries are invalid by default */
	[0 ... NR_CPUS - 1] = INVALID_HWCPUID,
};

static void __init resource_init(void)
{
	struct memblock_region *region;

	kernel_code_resource.start = __pa(_stext);
	kernel_code_resource.end = __pa(_etext) - 1;
	kernel_code_resource.flags = IORESOURCE_BUSY | IORESOURCE_MEM;

	kernel_data_resource.start = __pa(_sdata);
	kernel_data_resource.end = __pa(_edata) - 1;
	kernel_data_resource.flags = IORESOURCE_BUSY | IORESOURCE_MEM;

	kernel_bss_resource.start = __pa(__bss_start);
	kernel_bss_resource.end = __pa(__bss_stop) - 1;
	kernel_bss_resource.flags = IORESOURCE_BUSY | IORESOURCE_MEM;

	for_each_memblock(memory, region) {
		struct resource *res;
		res = __va(memblock_alloc(sizeof(struct resource), SMP_CACHE_BYTES));
		memset(res, 0, sizeof(struct resource));

		/* signal the memory bank to the resource manager */
		res->name = "System RAM";
		res->start = PFN_PHYS(memblock_region_memory_base_pfn(region));
		res->end = PFN_PHYS(memblock_region_memory_end_pfn(region)) - 1;
		res->flags = IORESOURCE_MEM | IORESOURCE_BUSY;

		request_resource(&iomem_resource, res);

		/* let the resource manager insert those segments into the
		 * right memory bank */
		request_resource(res, &kernel_code_resource);
		request_resource(res, &kernel_data_resource);
		request_resource(res, &kernel_bss_resource);
	}
}

/* This function overloads the weak default one. We want to constrain the
 * boundaries of memory banks to be aligned with big pages, so we don't have to
 * allocate second level page tables when mapping the low memory.
 *
 * We do not deal with nodes at the moment because we do not know the size of
 * the grid yet (thus the linear mapping of node numbers), not before we
 * actually analyse all the memory banks. */
void __init early_init_dt_add_memory_arch(u64 base, u64 size)
{
	if ((base & ~PMD_MASK) || (size & ~PMD_MASK)) {
		u64 start = round_up(base, PMD_SIZE);
		u64 end = round_down((base + size), PMD_SIZE);

		pr_warning("Aligning memory block from %#010llx-%#010llx "
				"to %#010llx-%#010llx\n",
				base, base + size,
				start, end);
		base = start;
		size = end - start;
	}

	memblock_add(base, size);
}

void __init early_init_devtree(void *dtb)
{
	const char* machine_name;

	if (!dtb || !early_init_dt_scan(dtb)) {
		panic("Scanning device tree blob failed!\n");
	}

	machine_name = of_flat_dt_get_machine_name();
	if (machine_name)
		pr_info("Model: %s\n", machine_name);
}

void __init setup_arch(char **cmdline_p)
{
#ifndef CONFIG_TSAR_BUILTIN_DTB
	/* get the DTB via the bootloader */
	dtb_start = __va_offset(bootloader_param[1]);
#endif

	/* early parsing of the device tree to setup the machine:
	 * - memory banks (memblock api)
	 * - bootargs (boot_command_line definition)
	 */
	early_init_devtree(dtb_start);

	/* parse early param of boot_command_line:
	 * e.g. 'earlyprintk' */
	parse_early_param();

	/* memory initialization */
	paging_init();

	resource_init();

	/* finish parsing the device tree */
	unflatten_device_tree();

	/* give boot_command_line back to init/main.c */
	*cmdline_p = boot_command_line;

	/* initialize the first entry of the cpu logical map with the current
	 * boot cpu (it allows to use SMP code when on monocpu - e.g. in the
	 * xicu driver) */
	cpu_logical_map(0) = read_c0_hwcpuid();

#ifdef CONFIG_SMP
	/* initialize cpu_logical_map according to the device tree */
	/* we call this function now, to have an up-to-date cpu possible map
	 * asap. it is useful later in init/main(), e.g. when calling
	 * setup_nr_cpu_ids() */
	smp_init_cpus();
#endif
#ifdef CONFIG_NUMA
	cpu_setup_nodes();
#endif

#if defined(CONFIG_VT)
	/* configure a virtual terminal */
	conswitchp = &dummy_con;
#endif
}


/*
 * Device tree population
 */

static int __init tsar_device_probe(void)
{
	if (!of_have_populated_dt())
		panic("Device tree not present!");

	return of_platform_populate(NULL, of_default_bus_match_table, NULL,
			NULL);
}
arch_initcall(tsar_device_probe);


/*
 * /proc/cpuinfo callbacks
 */

static int c_show(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "Processor\t: TSAR-MIPS32\n");

	for_each_online_cpu(i) {
#ifdef CONFIG_SMP
		seq_printf(m, "processor\t: %d\n", i);
#endif
	}

	seq_printf(m, "CPU company\t: %ld\n", (read_c0_prid() >> 16) & 0xff);
	seq_printf(m, "CPU implementation\t: %ld\n", (read_c0_prid() >> 8) & 0xff);
	seq_printf(m, "CPU revision\t: %ld\n", read_c0_prid() & 0xff);

	seq_printf(m, "\n");

	return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	return *pos < 1 ? (void *)1 : NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void c_stop(struct seq_file *m, void *v)
{
}

const struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= c_show,
};
