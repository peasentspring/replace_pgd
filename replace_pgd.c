/*
 * userspace.c
 *
 *  Created on: Jul 13, 2017
 *      Author: spring
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <asm/asm.h>
#include <linux/compat.h>
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/miscdevice.h>
#include <linux/bitops.h>
#include <asm/mman.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/ioctl.h>
#include <asm/tlbflush.h>
#include <linux/gfp.h>
/*
 * Prototypes − this would normally go in a .h file
 */
int init_module(void);
void cleanup_module(void);



#define SUCCESS 0

long imp_ioctl(struct file *filep, unsigned int cmd, unsigned long arg);
static const struct file_operations imp_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= imp_ioctl,
};

static struct miscdevice imp_dev = {
	.name	= "imp-virtual",
	.fops	= &imp_fops,
	.mode   = S_IRUGO | S_IWUGO,
};

typedef long (*imp_ioctl_t)(struct file *filep, unsigned int cmd,
			     unsigned long arg);

struct imp_switchpgd {
	__u64 	addr;
	__u64	pgdaddr;
} __attribute__((packed));

struct imp_getpgd {
	__u64 	addr;
	__u64   pgdaddr;
	__u64 	size;
} __attribute__((packed));

#define IMP_MAGIC 0xA4

#define IMP_IOC_SWITCHPGD \
	_IOW(IMP_MAGIC, 0x03, struct imp_switchpgd)
#define IMP_IOC_GET_PGD \
	_IOW(IMP_MAGIC, 0x04, struct imp_getpgd)

void follow_pte(struct mm_struct * mm, unsigned long address, pte_t * entry)
{
    pgd_t * pgd = pgd_offset(mm, address);


    entry->pte = 0;
    if (!pgd_none(*pgd) && !pgd_bad(*pgd)) {
        pud_t * pud = pud_offset(pgd, address);
        struct vm_area_struct * vma = find_vma(mm, address);

        if (pud_none(*pud)) {
            printk("  pud = empty\n");
            return;
        }
        if (pud_large(*pud) && vma->vm_flags & VM_HUGETLB) {
            entry->pte = pud_val(*pud);
            printk("  pud = huge\n");
            return;
        }

        if (!pud_bad(*pud)) {
            pmd_t * pmd = pmd_offset(pud, address);

            if (pmd_none(*pmd)) {
                printk("   pmd = empty\n");
                return;
            }
            if (pmd_large(*pmd) && vma->vm_flags & VM_HUGETLB) {
                entry->pte = pmd_val(*pmd);
                printk("   pmd = huge\n");
                return;
            }
            if (pmd_trans_huge(*pmd)) {
                entry->pte = pmd_val(*pmd);
//                printk("   pmd = trans_huge\n");
                return;
            }
            if (!pmd_bad(*pmd)) {
                pte_t * pte = pte_offset_map(pmd, address);
                if (!pte_none(*pte)) {
                    entry->pte = pte_val(*pte);
//                    printk("    pte = %lx\n", pte_val(*pte));
                } else {
                    printk("    pte = empty\n");
                }
                pte_unmap(pte);
            }
        }
    }
}

typedef void (*funcp) (__u64 addr, __u64 value);
static long copy_pte_level(pte_t *pte_dst, pte_t *pte_src, int *pfree_nums, __u64 base, __u64 offset);
static long copy_pmd_level(pmd_t *pmd_dst, pmd_t *pmd_src, int *pfree_nums, __u64 base, __u64 offset);
static long copy_pud_level(pud_t *pud_dst, pud_t *pud_src, int *pfree_nums, __u64 base, __u64 offset);
static long copy_pgd_level(pgd_t *pgd_dst, pgd_t *pgd_src, int *pfree_nums, __u64 base, __u64 offset);
static long copy_just_pgd_level(pgd_t *pgd_dst, pgd_t *pgd_src, int *pfree_nums, __u64 base, __u64 offset);

static __u64 set_flags(__u64 dst, __u64 src){
	//12bit - 51bit
	__u64 flags = 0x0000fffffffff000;
	return (dst & flags) |(src & (~flags));
}

static long imp_ioctl_getpgd(struct file *filep, unsigned int cmd,
				    unsigned long arg);

static long imp_ioctl_switchpgd(struct file *filep, unsigned int cmd,
				    unsigned long arg)
{
	pte_t pte;
	struct imp_switchpgd *switchpgd = (struct imp_switchpgd *) arg;

	__u64 cr3_src = read_cr3();
	__u64 pgd_dst = switchpgd->pgdaddr;
	follow_pte(current->mm, pgd_dst, &pte);
	__u64 cr3_dst = set_flags(pte_val(pte), cr3_src);

	printk(KERN_INFO "set the current mm pgd\n");
	write_cr3(cr3_dst);
	__flush_tlb();
	__flush_tlb_global();
	printk(KERN_INFO "flush has been exec\n");

	return 0;
}

static long imp_ioctl_getpgd(struct file *filep, unsigned int cmd,
				    unsigned long arg)
{
	pgd_t* pgd_dst;
	pte_t pte;
	int ret = -EINVAL;
	int free_nums = 0;
	struct imp_getpgd *getpgd = (struct imp_getpgd *) arg;

	__u64 base = getpgd->addr;


	__u64 offset = getpgd->addr - getpgd->pgdaddr;

	pgd_t* pgd_src = current->mm->pgd;
	follow_pte(current->mm, pgd_src, &pte);
	printk(KERN_INFO "orignal process pgd is 0x%p\n", pgd_src);
	printk(KERN_INFO "orignal process cr3 is 0x%p\n", pte_val(pte));


	pgd_dst = (pgd_t *)base;
	pgd_dst = (pgd_t *)__get_free_page(GFP_KERNEL | __GFP_NOTRACK | __GFP_REPEAT | __GFP_ZERO);
	getpgd->addr = (__u64)pgd_dst;
	free_nums = getpgd->size / PAGE_SIZE;
	free_nums--;

	if (copy_just_pgd_level(pgd_dst, pgd_src, &free_nums, base, 0) < 0){
		printk(KERN_INFO "copy_just_pgd_level error\n");
		return ret;
	}


	printk(KERN_INFO "free_nums is %d\n", free_nums);

	__u64 cr3_src = read_cr3();
	printk(KERN_INFO "orignal process pgd is 0x%p\n", pgd_src);
	printk(KERN_INFO "orignal process cr3 is 0x%p\n", (void *)cr3_src);
	write_cr3(cr3_src);

	follow_pte(current->mm, pgd_dst, &pte);
	__u64 cr3_dst = set_flags(pte_val(pte), cr3_src);
	printk(KERN_INFO "new process pgd is: 0x%p\n", pgd_dst);
	printk(KERN_INFO "new process cr3 is: 0x%p\n", cr3_dst);


	return 0;
}


static long copy_just_pgd_level(pgd_t *pgd_dst, pgd_t *pgd_src, int *pfree_nums, __u64 base, __u64 offset){
	int ret = -EINVAL;
	int i;
	pte_t pte;
	pgd_dst = (pgd_t *)((__u64)pgd_dst + offset);
	for(i = 0; i < PTRS_PER_PGD; i++){
		pgd_t pgd_src_entry = pgd_src[i];
		if (!pgd_none(pgd_src_entry)){ //&& !pgd_bad(pgd_src_entry)

			pgd_dst[i] = pgd_src_entry;

			printk(KERN_INFO "i:%d  pgd_dst:0x%p  pgd_dst[]:0x%p pgd_src[]:0x%p",
					i, pgd_dst, (void *)pgd_val(pgd_dst[i]), (void *)pgd_val(pgd_src[i]));

		}
	}
	clone_pgd_range(pgd_dst,
					pgd_src,
					PTRS_PER_PGD);
	return 0;
}

static long copy_pgd_level(pgd_t *pgd_dst, pgd_t *pgd_src, int *pfree_nums, __u64 base, __u64 offset){
	int ret = -EINVAL;
	int i;
	pte_t pte;
	pgd_dst = (pgd_t *)((__u64)pgd_dst + offset);
	for(i = 0; i < PTRS_PER_PGD; i++){
		pgd_t pgd_src_entry = pgd_src[i];
		if (!pgd_none(pgd_src_entry)){ //&& !pgd_bad(pgd_src_entry)
			//alloc a new page for pud, and get its physical addr
			if ((*pfree_nums) <= 0){
				printk(KERN_WARNING "enclave heap is too small, alloc pud failed\n");
				return ret;
			}
			__u64 virt_new_pgd = base + offset + (*pfree_nums) * PAGE_SIZE;
			follow_pte(current->mm, virt_new_pgd, &pte);
			(*pfree_nums)--;


			//covert pgd_dst content to new addr;
			__u64 new_entry = set_flags(native_pte_val(pte), (__u64)native_pgd_val(pgd_src_entry));

			copy_to_user((pgd_dst + i), &new_entry, sizeof(new_entry));

			printk(KERN_INFO "virt_new_pgd:0x%p  pte:0x%llx i:%d  pgd_dst:0x%p  pgd_dst[]:0x%llx  pgd_src[]:0x%llx",
					virt_new_pgd, pte_val(pte), i, pgd_dst, pgd_dst[i], pgd_src[i]);

			if ((copy_pud_level((pud_t *)(virt_new_pgd), (pud_t *)pgd_page_vaddr(pgd_src_entry), pfree_nums, base, offset)) < 0 ){
				printk(KERN_INFO "copy_pud_level error\n");
				return ret;
			}

		}
	}

	return 0;
}

//pud_dst is a linear address
static long copy_pud_level(pud_t *pud_dst, pud_t *pud_src, int *pfree_nums, __u64 base, __u64 offset){
	pte_t pte;
	int i;
	int ret = -EINVAL;
	pud_dst = (pud_t *)((__u64)pud_dst + offset);

	for(i = 0; i < PTRS_PER_PUD; i++){
		pud_t pud_src_entry = pud_src[i];
		if (!pud_none(pud_src_entry) ){ //&& !pud_bad(pud_src_entry)
			//alloc a new page for pmd, and get its physical addr
			if ((*pfree_nums) <= 0){
				printk(KERN_INFO "enclave heap is too small, alloc pmd failed\n");
				return ret;
			}
			if (!pud_large(pud_src_entry)){
				__u64 virt_new_pud = base + offset + (*pfree_nums) * PAGE_SIZE;

				follow_pte(current->mm, virt_new_pud, &pte);
				(*pfree_nums)--;
				//convert pud_dst content to new addr;
				__u64 new_entry = set_flags(native_pte_val(pte), (__u64)native_pud_val(pud_src_entry));

				copy_to_user((__u64 *)(pud_dst + i), &new_entry, sizeof(new_entry));

				printk(KERN_INFO "virt_new_pud:0x%p  pte:0x%llx i:%d  pud_dst:0x%p  pud_dst[]:0x%llx  pud_src[]:0x%llx",
						virt_new_pud, pte_val(pte), i, pud_dst, pud_dst[i], pud_src[i]);

				if ((copy_pmd_level((pmd_t *)virt_new_pud, (pmd_t *)pud_page_vaddr(pud_src_entry), pfree_nums, base, offset)) < 0){
					printk(KERN_INFO "copy_pmd_level error\n");
					return ret;
				}
			}
			else{
            	copy_to_user((pud_dst + i), &pud_src_entry, sizeof(pud_src_entry));
				printk(KERN_INFO "large pud; i:%d  pud_dst:0x%p  pud_dst[]:0x%llx  pud_src[]:0x%llx",
						i, pud_dst, pud_dst[i], pud_src[i]);
			}
		}

	}
	return 0;
}

static long copy_pmd_level(pmd_t *pmd_dst, pmd_t *pmd_src, int *pfree_nums, __u64 base, __u64 offset){
	pte_t pte;

	int ret = -EINVAL;
	int i;

	pmd_dst = (pmd_t *)((__u64)pmd_dst + offset);

	for(i = 0; i < PTRS_PER_PMD; i++){
		pmd_t pmd_src_entry = pmd_src[i];
		if (!pmd_none(pmd_src_entry)){// && !pmd_bad(pmd_src_entry)
			//alloc a new page for pte, and get its physical addr
			if ((*pfree_nums) <= 0){
				printk(KERN_WARNING "enclave heap is too small, alloc pte failed\n");
				return ret;
			}
            if (!pmd_trans_huge(pmd_src_entry)){
		    	__u64 virt_new_pmd = base + offset +(*pfree_nums) * PAGE_SIZE;
		    	follow_pte(current->mm, virt_new_pmd, &pte);
		    	(*pfree_nums)--;
		    	//convert pmd_dst content to new addr;
		    	__u64 new_entry = set_flags(native_pte_val(pte), (__u64)native_pmd_val(pmd_src_entry));

		    	copy_to_user((__u64 *)(pmd_dst + i), &new_entry, sizeof(new_entry));

				printk(KERN_INFO "virt_new_pmd:0x%p  pte:0x%llx i:%d  pmd_dst:0x%p  pmd_dst[]:0x%llx  pmd_src[]:0x%llx",
						virt_new_pmd, pte_val(pte), i, pmd_dst, pmd_dst[i], pmd_src[i]);

		    	if((ret = copy_pte_level((pte_t *)virt_new_pmd, (pte_t *)pmd_page_vaddr(pmd_src_entry), pfree_nums, base, offset)) < 0){
		    		printk(KERN_INFO "copy_pte_level error");
		    		return ret;
		    	}
            } else {
            	copy_to_user((pmd_dst + i), &pmd_src_entry, sizeof(pmd_src_entry));
				printk(KERN_INFO "large pmd; i:%d  pmd_dst:0x%p  pmd_dst[]:0x%llx  pmd_src[]:0x%llx",
						i, pmd_dst, pmd_dst[i], pmd_src[i]);
            }
		}
	}
	return 0;
}

static long copy_pte_level(pte_t *pte_dst, pte_t * pte_src, int* pfree_nums, __u64 base, __u64 offset){
	pte_dst = (pte_t *)((__u64)pte_dst + offset);
	copy_to_user(pte_dst, pte_src, PAGE_SIZE);
	return 0;
}

long imp_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	char data[256];
	imp_ioctl_t handler = NULL;
	long ret;
	unsigned long cr4,tmp;
	asm volatile ("mov %%rax, %0\n\t"
			"mov %%cr4, %%rax\n\t"
			"mov %%rax, %1\n\t"
			"mov %2, %%rax\n\t"
			: "=m"(tmp), "=m"(cr4)
			 :"m"(tmp)
		);
	printk(KERN_INFO "CR4 IS %x \n", cr4);

	switch (cmd) {
	case IMP_IOC_SWITCHPGD:
		//0x4010a403
		printk(KERN_INFO "IMP_IOC_SWITCHPGD %x\n", IMP_IOC_SWITCHPGD);
		handler = imp_ioctl_switchpgd;
		break;
	case IMP_IOC_GET_PGD:
		handler = imp_ioctl_getpgd;
		break;
	default:
		printk(KERN_INFO "default %x\n", cmd);
		return -EINVAL;
	}

	if (copy_from_user(data, (void __user *) arg, _IOC_SIZE(cmd)))
		return -EFAULT;

	ret = handler(filep, cmd, (unsigned long) ((void *) data));

	if (!ret && (cmd & IOC_OUT)) {
		if (copy_to_user((void __user *) arg, data, _IOC_SIZE(cmd)))
			return -EFAULT;
	}

	return ret;
}



/*
 * This function is called when the module is loaded
 */
int init_module(void) {
	int ret;
	ret = misc_register(&imp_dev);
	if (ret) {
		pr_err("imp: misc_register() failed\n");
		return ret;
	}
	printk(KERN_INFO "register imp\n");

	return SUCCESS;
}
/*
 * This function is called when the module is unloaded
 */
void cleanup_module(void) {
	/*
	 * Unregister the device
	 */
	misc_deregister(&imp_dev);;

}




