This repo just is used to understand the page table.
replace_pgd.c:
    this file is a kernel module, this module is to copy the part of page table of current process. for example, you can just copy the pgd, or copy pgd, pmd, and so on. follow_pte will get the physical address of the new page which used to store the pgd item or pmd item.

test_replace_pgd.c:
    this file is used to test whether your replacement is right. copy_pgd() notify kernel module to copy a new pgd, switch_pgd() notify kernel module to switch to new page table. when the process running, your just enter crtl + D, this will switch page table.
