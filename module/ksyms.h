#pragma once
#include <linux/mm_types.h>

extern bool (*ptlock_alloc_ksym)(struct page *page);
extern void (*ptlock_free_ksym)(struct page *page);
extern pgd_t *swapper_pg_dir_ksym;
extern void (*set_swapper_pgd_ksym)(pgd_t *pgdp, pgd_t pgd);
extern struct mm_struct *init_mm_ksym;
extern void (*mte_sync_tags_ksym)(pte_t old_pte, pte_t pte);

// void flush_tlb_mm_range(struct mm_struct *mm, unsigned long start,
// 						   unsigned long end, unsigned int stride_shift,
// 						   bool freed_tables);

void *__vmalloc_array(size_t n, size_t size, gfp_t flags);


// #define flush_tlb_mm(mm)									\
// 	flush_tlb_mm_range(mm, 0UL, TLB_FLUSH_ALL, 0UL, true)

// #define flush_tlb_range(vma, start, end)					\
// 	flush_tlb_mm_range((vma)->vm_mm, start, end,			\
// 					   ((vma)->vm_flags & VM_HUGETLB)		\
// 					   ? huge_page_shift(hstate_vma(vma))	\
// 					   : PAGE_SHIFT, false)


int exmap_acquire_ksyms(void);
