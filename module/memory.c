#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/vmstat.h>
#include <linux/gfp.h>

#include <linux/pgtable.h>
#include <asm/io.h>
#include <asm/mmu_context.h>
#include <asm/pgalloc.h>
#include <asm/cacheflush.h>
#include <linux/memcontrol.h>

#include "ksyms.h"
#include "driver.h"
#include "config.h"

#define native_make_pte(x) __pte(x)


static inline void __set_pte_at_ksym(struct mm_struct *mm, unsigned long addr,
				pte_t *ptep, pte_t pte)
{
	if (pte_present(pte) && pte_user_exec(pte) && !pte_special(pte))
		__sync_icache_dcache(pte);

	/*
	 * If the PTE would provide user space access to the tags associated
	 * with it then ensure that the MTE tags are synchronised.  Although
	 * pte_access_permitted() returns false for exec only mappings, they
	 * don't expose tags (instruction fetches don't check tags).
	 */
	if (system_supports_mte() && pte_access_permitted(pte, false) &&
	    !pte_special(pte)) {
		pte_t old_pte = READ_ONCE(*ptep);
		/*
		 * We only need to synchronise if the new PTE has tags enabled
		 * or if swapping in (in which case another mapping may have
		 * set tags in the past even if this PTE isn't tagged).
		 * (!pte_none() && !pte_present()) is an open coded version of
		 * is_swap_pte()
		 */
		if (pte_tagged(pte) || (!pte_none(old_pte) && !pte_present(old_pte)))
			mte_sync_tags_ksym(old_pte, pte);
	}

	__check_racy_pte_update(mm, ptep, pte);

	set_pte(ptep, pte);
}

static inline void set_pte_at_ksym(struct mm_struct *mm, unsigned long addr,
			      pte_t *ptep, pte_t pte)
{
	page_table_check_pte_set(mm, addr, ptep, pte);
	return __set_pte_at_ksym(mm, addr, ptep, pte);
}

static inline bool ptlock_init_ksym(struct page *page)
{
	VM_BUG_ON_PAGE(*(unsigned long *)&page->ptl, page);
	if (!ptlock_alloc_ksym(page))
		return false;
	spin_lock_init(ptlock_ptr(page));
	return true;
}

static inline bool pmd_ptlock_init_ksym(struct page *page)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	page->pmd_huge_pte = NULL;
#endif
	return ptlock_init_ksym(page);
}

static inline bool pgtable_pmd_page_ctor_ksym(struct page *page)
{
	if (!pmd_ptlock_init_ksym(page))
		return false;
	__SetPageTable(page);
	inc_lruvec_page_state(page, NR_PAGETABLE);
	return true;
}

static inline void pmd_ptlock_free_ksym(struct page *page)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	VM_BUG_ON_PAGE(page->pmd_huge_pte, page);
#endif
	ptlock_free_ksym(page);
}


static inline void pgtable_pmd_page_dtor_ksym(struct page *page)
{
	pmd_ptlock_free_ksym(page);
	__ClearPageTable(page);
	dec_lruvec_page_state(page, NR_PAGETABLE);
}

static inline void pmd_free_ksym(struct mm_struct *mm, pmd_t *pmd)
{
	BUG_ON((unsigned long)pmd & (PAGE_SIZE-1));
	pgtable_pmd_page_dtor_ksym(virt_to_page(pmd));
	free_page((unsigned long)pmd);
}

static inline bool pgtable_pte_page_ctor_ksym(struct page *page)
{
	if (!ptlock_init_ksym(page))
		return false;
	__SetPageTable(page);
	inc_lruvec_page_state(page, NR_PAGETABLE);
	return true;
}

static inline void pgtable_pte_page_dtor_ksym(struct page *page)
{
	ptlock_free_ksym(page);
	__ClearPageTable(page);
	dec_lruvec_page_state(page, NR_PAGETABLE);
}


static inline void pte_free_ksym(struct mm_struct *mm, struct page *pte_page)
{
	pgtable_pte_page_dtor_ksym(pte_page);
	__free_page(pte_page);
}

static inline bool in_swapper_pgdir_ksym(void *addr)
{
	return ((unsigned long)addr & PAGE_MASK) ==
	        ((unsigned long)swapper_pg_dir_ksym & PAGE_MASK);
}


static inline void set_pud_ksym(pud_t *pudp, pud_t pud)
{
#ifdef __PAGETABLE_PUD_FOLDED
	if (in_swapper_pgdir_ksym(pudp)) {
		set_swapper_pgd_ksym((pgd_t *)pudp, __pgd(pud_val(pud)));
		return;
	}
#endif /* __PAGETABLE_PUD_FOLDED */

	WRITE_ONCE(*pudp, pud);

	if (pud_valid(pud)) {
		dsb(ishst);
		isb();
	}
}

static inline void __pud_populate_ksym(pud_t *pudp, phys_addr_t pmdp, pudval_t prot)
{
	set_pud_ksym(pudp, __pud(__phys_to_pud_val(pmdp) | prot));
}

static inline void pud_populate_ksym(struct mm_struct *mm, pud_t *pudp, pmd_t *pmdp)
{
	pudval_t pudval = PUD_TYPE_TABLE;

	pudval |= (mm == init_mm_ksym) ? PUD_TABLE_UXN : PUD_TABLE_PXN;
	__pud_populate_ksym(pudp, __pa(pmdp), pudval);
}

static inline void set_p4d_ksym(p4d_t *p4dp, p4d_t p4d)
{
	if (in_swapper_pgdir_ksym(p4dp)) {
		set_swapper_pgd_ksym((pgd_t *)p4dp, __pgd(p4d_val(p4d)));
		return;
	}

	WRITE_ONCE(*p4dp, p4d);
	dsb(ishst);
	isb();
}

static inline void __p4d_populate_ksym(p4d_t *p4dp, phys_addr_t pudp, p4dval_t prot)
{
	set_p4d_ksym(p4dp, __p4d(__phys_to_p4d_val(pudp) | prot));
}

static inline void p4d_populate_ksym(struct mm_struct *mm, p4d_t *p4dp, pud_t *pudp)
{
	p4dval_t p4dval = P4D_TYPE_TABLE;

	p4dval |= (mm == init_mm_ksym) ? P4D_TABLE_UXN : P4D_TABLE_PXN;
	__p4d_populate_ksym(p4dp, __pa(pudp), p4dval);
}

/**
 * exmap_pte_alloc_one - allocate a page for PTE-level user page table
 * @mm: the mm_struct of the current context
 * @gfp: GFP flags to use for the allocation
 *
 * Allocates a page and runs the pgtable_pte_page_ctor().
 *
 * This function is intended for architectures that need
 * anything beyond simple page allocation or must have custom GFP flags.
 *
 * Return: `struct page` initialized as page table or %NULL on error
 */
static inline pgtable_t exmap_pte_alloc_one(struct mm_struct *mm)
{
	struct page *pte;

	pte = alloc_page(GFP_PGTABLE_USER);
	if (!pte)
		return NULL;
	if (!pgtable_pte_page_ctor_ksym(pte)) {
		__free_page(pte);
		return NULL;
	}

	return pte;
}

void pmd_install(struct mm_struct *mm, pmd_t *pmd, pgtable_t *pte)
{
	spinlock_t *ptl = pmd_lock(mm, pmd);

	if (likely(pmd_none(*pmd))) {   /* Has another populated it ? */
		mm_inc_nr_ptes(mm);
		/*
		 * Ensure all pte setup (eg. pte page lock and page clearing) are
		 * visible before the pte is made visible to other CPUs by being
		 * put into page tables.
		 *
		 * The other side of the story is the pointer chasing in the page
		 * table walking code (when walking the page table without locking;
		 * ie. most of the time). Fortunately, these data accesses consist
		 * of a chain of data-dependent loads, meaning most CPUs (alpha
		 * being the notable exception) will already guarantee loads are
		 * seen in-order. See the alpha page table accessors for the
		 * smp_rmb() barriers in page table walking code.
		 */
		smp_wmb(); /* Could be smp_wmb__xxx(before|after)_spin_lock */
		pmd_populate(mm, pmd, *pte);
		*pte = NULL;
	}
	spin_unlock(ptl);
}

int exmap__pte_alloc(struct mm_struct *mm, pmd_t *pmd)
{
	pgtable_t new = exmap_pte_alloc_one(mm);
	if (!new)
		return -ENOMEM;

	pmd_install(mm, pmd, &new);
	if (new)
		pte_free_ksym(mm, new);
	return 0;
}

#define exmap_pte_alloc(mm, pmd) (unlikely(pmd_none(*(pmd))) && exmap__pte_alloc(mm, pmd))


/**
 * exmap_pmd_alloc_one - allocate a page for PMD-level page table
 * @mm: the mm_struct of the current context
 *
 * Allocates a page and runs the pgtable_pmd_page_ctor().
 * Allocations use %GFP_PGTABLE_USER in user context and
 * %GFP_PGTABLE_KERNEL in kernel context.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
static inline pmd_t *exmap_pmd_alloc_one(struct mm_struct *mm, unsigned long addr)
{
	struct page *page;

	page = alloc_pages(GFP_PGTABLE_USER, 0);
	if (!page)
		return NULL;
	if (!pgtable_pmd_page_ctor_ksym(page)) {
		__free_pages(page, 0);
		return NULL;
	}
	return (pmd_t *)page_address(page);
}

/**
 * exmap_pud_alloc_one - allocate a page for PUD-level page table
 * @mm: the mm_struct of the current context
 *
 * Allocates a page using %GFP_PGTABLE_USER for user context and
 * %GFP_PGTABLE_KERNEL for kernel context.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
static inline pud_t *exmap_pud_alloc_one(struct mm_struct *mm, unsigned long addr)
{
	return (pud_t *)get_zeroed_page(GFP_PGTABLE_USER);
}

static inline p4d_t *exmap_p4d_alloc_one(struct mm_struct *mm, unsigned long addr)
{
	return (p4d_t *)get_zeroed_page(GFP_KERNEL_ACCOUNT);
}


/*
 * Allocate p4d page table.
 * We've already handled the fast-path in-line.
 */
static
int exmap_default_p4d_alloc(struct mm_struct *mm, pgd_t *pgd, unsigned long address)
{
#ifndef __PAGETABLE_P4D_FOLDED
	p4d_t *new = exmap_p4d_alloc_one(mm, address);
	if (!new)
		return -ENOMEM;

	spin_lock(&mm->page_table_lock);
	if (pgd_present(*pgd)) {    /* Another has populated it */
		p4d_free(mm, new);
	} else {
		smp_wmb(); /* See comment in pmd_install() */
		pgd_populate(mm, pgd, new);
	}
	spin_unlock(&mm->page_table_lock);
#endif /* __PAGETABLE_P4D_FOLDED */
	return 0;
}


/*
 * Allocate page upper directory.
 * We've already handled the fast-path in-line.
 */
static
int exmap_default_pud_alloc(struct mm_struct *mm, p4d_t *p4d, unsigned long address)
{
#ifndef __PAGETABLE_PUD_FOLDED
	pud_t *new = exmap_pud_alloc_one(mm, address);
	if (!new)
		return -ENOMEM;

	spin_lock(&mm->page_table_lock);
	if (!p4d_present(*p4d)) {
		mm_inc_nr_puds(mm);
		smp_wmb(); /* See comment in pmd_install() */
		p4d_populate_ksym(mm, p4d, new);
	} else  /* Another has populated it */
		pud_free(mm, new);
	spin_unlock(&mm->page_table_lock);
#endif /* __PAGETABLE_PUD_FOLDED */
	return 0;
}



/*
 * Allocate page middle directory.
 * We've already handled the fast-path in-line.
 */
static
int exmap_default_pmd_alloc(struct mm_struct *mm, pud_t *pud, unsigned long address)
{
#ifndef __PAGETABLE_PMD_FOLDED
	spinlock_t *ptl;
	pmd_t *new = exmap_pmd_alloc_one(mm, address);
	if (!new)
		return -ENOMEM;

	ptl = pud_lock(mm, pud);
	if (!pud_present(*pud)) {
		mm_inc_nr_pmds(mm);
		smp_wmb(); /* See comment in pmd_install() */
		pud_populate_ksym(mm, pud, new);
	} else {    /* Another has populated it */
		pmd_free_ksym(mm, new);
	}
	spin_unlock(ptl);
#endif /* __PAGETABLE_PMD_FOLDED */
	return 0;
}



static inline
p4d_t * exmap_p4d_offset_alloc(struct mm_struct *mm, pgd_t *pgd,
							   unsigned long address)
{
	if (mm_p4d_folded(mm))
		p4d_offset(pgd, address);

	return (unlikely(pgd_none(*pgd)) && exmap_default_p4d_alloc(mm, pgd, address)) ?
		NULL : p4d_offset(pgd, address);
}

static inline
pud_t * exmap_pud_offset_alloc(struct mm_struct *mm, p4d_t *p4d,
							   unsigned long address)
{
	if (mm_pud_folded(mm))
		return pud_offset(p4d, address);

	return (unlikely(p4d_none(*p4d)) && exmap_default_pud_alloc(mm, p4d, address)) ?
		NULL : pud_offset(p4d, address);
}

static inline
pmd_t * exmap_pmd_offset_alloc(
						struct mm_struct *mm, pud_t *pud,
						unsigned long address)
{
	if (mm_pmd_folded(mm))
		return pmd_offset(pud, address);

	return (unlikely(pud_none(*pud)) && exmap_default_pmd_alloc(mm, pud, address))?
		NULL: pmd_offset(pud, address);
}


static pmd_t *walk_to_pmd(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset(mm, addr);
	// pr_info("pgd: %p", pgd);

	p4d = exmap_p4d_offset_alloc(mm, pgd, addr);
	if (!p4d)
		return NULL;
	// pr_info("p4d: %p", pgd);

	pud = exmap_pud_offset_alloc(mm, p4d, addr);
	if (!pud)
		return NULL;

	// pr_info("pud: %p", pud);

	pmd = exmap_pmd_offset_alloc(mm, pud, addr);
	if (!pmd)
		return NULL;

	// pr_info("pmd: %p", pmd);

	VM_BUG_ON(pmd_trans_huge(*pmd));
	return pmd;
}

// For add_mm_counter to work inside a module
void mm_trace_rss_stat(struct mm_struct *mm, int member, long count)
{
}

static int insert_page_into_pte_locked(struct mm_struct *mm, pte_t *pte,
									   unsigned long addr, struct page *page, pgprot_t prot)
{
#ifdef MAPCOUNT
	unsigned int mapcount;
#endif
	/* pr_info("pte_none = %d, page dirty = %d, pte = %p, page = %p", !(pte->pte & ~(_PAGE_DIRTY | _PAGE_ACCESSED)), PageDirty(page), pte, page); */
	/* NOTE this causes EBUSY in insert_pages */
	if (!pte_none(*pte))
		return -EBUSY;
	/* Ok, finally just insert the thing.. */

	// add_mm_counter_fast(mm, mm_counter_file(page), 1);

#ifdef MAPCOUNT
	mapcount = atomic_inc_and_test(&page->_mapcount);
	BUG_ON(mapcount != 1);
#endif

	set_pte_at_ksym(mm, addr, pte, mk_pte(page, prot));
	return 0;
}

static int validate_page_before_insert(struct page *page)
{
	if (PageAnon(page) || PageSlab(page) || page_has_type(page))
		return -EINVAL;
	flush_dcache_page(page);
	return 0;
}


static int insert_page_in_batch_locked(struct mm_struct *mm, pte_t *pte,
									   unsigned long addr, struct page *page, pgprot_t prot)
{
	int err;
	BUG_ON(!page);

	if (!page_count(page))
		return -EINVAL;
	err = validate_page_before_insert(page);
	if (err)
		return err;
	return insert_page_into_pte_locked(mm, pte, addr, page, prot);
}


static int insert_page_fastpath(pte_t *pte, unsigned long addr, struct page *page, pgprot_t prot) {
	int err;
	pte_t ptent, new_ptent;
	err = validate_page_before_insert(page);
	if (err)
		return err;

	ptent = ptep_get(pte);
	if (pte_present(ptent))
		return -EBUSY;

	// We compare and exchange once.
	new_ptent = mk_pte(page, prot);

	if (atomic_long_cmpxchg((atomic_long_t*) &(pte->pte), ptent.pte, new_ptent.pte) != ptent.pte)
		err = -EBUSY;

	return 0;
}

/* insert_pages() amortizes the cost of spinlock operations
 * when inserting pages in a loop. Arch *must* define pte_index.
 */
static int insert_pages(struct vm_area_struct *vma, unsigned long addr, unsigned long num_pages,
						struct free_pages *free_pages, pgprot_t prot,
						exmap_insert_callback cb, struct exmap_alloc_ctx *alloc_ctx)
{
	pmd_t *pmd = NULL;
	pte_t *start_pte, *pte;
	spinlock_t *pte_lock;
	struct mm_struct *const mm = vma->vm_mm;
	unsigned long remaining_pages_total = num_pages;
	unsigned long pages_to_write_in_pmd;
	int ret, err;
more:
	ret = -EFAULT;
	pmd = walk_to_pmd(mm, addr);

	if (!pmd)
		goto out;

	pages_to_write_in_pmd = min_t(unsigned long,
								  remaining_pages_total, PTRS_PER_PTE - pte_index(addr));

	/* Allocate the PTE if necessary; takes PMD lock once only. */
	ret = -ENOMEM;
	if (exmap_pte_alloc(mm, pmd))
		goto out;

	while (pages_to_write_in_pmd) {
		int pte_idx = 0;
		const int batch_size = pages_to_write_in_pmd; // min_t(int, pages_to_write_in_pmd, 8);

#ifdef USE_FASTPATH
		// Fastpath for single page in this PMD
		if (pages_to_write_in_pmd == 1) {
			struct page *page = list_first_entry_or_null(&free_pages->list, struct page, lru);
			BUG_ON(!page);

			pte = pte_offset_map(pmd, addr);
			err = insert_page_fastpath(pte, addr, page, prot);

			if (!err) {
				// We actually used the page
				BUG_ON(free_pages->count == 0);
				list_del(&page->lru);
				free_pages->count --;
			}

			addr += PAGE_SIZE;
			remaining_pages_total -= 1;
			break;
		}
#endif

		start_pte = pte_offset_map_lock(mm, pmd, addr, &pte_lock);
		for (pte = start_pte; pte_idx < batch_size; ++pte, ++pte_idx) {
			struct page *page = list_first_entry_or_null(&free_pages->list, struct page, lru);
			BUG_ON(!page);

			// unsigned long pfn = page_to_pfn(page);
			// pr_info("alloc: addr: %p %p 0x%lx, %p", cb, alloc_ctx, addr - vma->vm_start, page);

			err = insert_page_in_batch_locked(mm, pte, addr, page, prot);

			// If the PTE was busy, we just skip it and use the page
			// for the next PTE.
			if (err == -EBUSY) {
				/* pr_info("i_p: ebusy a_ctx=%p offs=0x%lx page=%p free count=%lu", alloc_ctx, addr - vma->vm_start, page, free_pages->count); */
				// This is OK!
			} else if (unlikely(err)) {
				pte_unmap_unlock(start_pte, pte_lock);
				ret = err;
				remaining_pages_total -= pte_idx;
				goto out;
			} else {
				// We actually used the page
				BUG_ON(free_pages->count == 0);
				list_del(&page->lru);
				free_pages->count --;

				// This might issue a read request
				if (cb) cb(alloc_ctx, addr - vma->vm_start, page);
			}


			addr += PAGE_SIZE;
		}
		pte_unmap_unlock(start_pte, pte_lock);
		pages_to_write_in_pmd -= batch_size;
		remaining_pages_total -= batch_size;
	}
	if (remaining_pages_total)
		goto more;
	ret = 0;
out:
	return ret;
}

int exmap_insert_pages(struct vm_area_struct *vma, unsigned long addr,
					   unsigned long num_pages, struct free_pages *pages,
					   exmap_insert_callback cb, struct exmap_alloc_ctx *data)
{
	const unsigned long end_addr = addr + (pages->count * PAGE_SIZE) - 1;

	if (addr < vma->vm_start || end_addr >= vma->vm_end)
		return -EFAULT;
	if (!(vma->vm_flags & VM_MIXEDMAP)) {
		BUG_ON(mmap_read_trylock(vma->vm_mm));
		BUG_ON(vma->vm_flags & VM_PFNMAP);
		vma->vm_flags |= VM_MIXEDMAP;
	}
	/* Defer page refcount checking till we're about to map that page. */
	return insert_pages(vma, addr, num_pages, pages,
						vma->vm_page_prot, cb, data);
}

/* EXPORT_SYMBOL(exmap_insert_pages); */


////////////////////////////////////////////////////////////////
// Freeing memory

void pgd_clear_bad(pgd_t *pgd)
{
	pgd_ERROR(*pgd);
	pgd_clear(pgd);
}

#ifndef __PAGETABLE_P4D_FOLDED
void p4d_clear_bad(p4d_t *p4d)
{
	p4d_ERROR(*p4d);
	p4d_clear(p4d);
}
#endif

#ifndef __PAGETABLE_PUD_FOLDED
void pud_clear_bad(pud_t *pud)
{
	pud_ERROR(*pud);
	pud_clear(pud);
}
#endif

/*
 * Note that the pmd variant below can't be stub'ed out just as for p4d/pud
 * above. pmd folding is special and typically pmd_* macros refer to upper
 * level even when folded
 */
void pmd_clear_bad(pmd_t *pmd)
{
	pmd_ERROR(*pmd);
	pmd_clear(pmd);
}

static inline unsigned long
exmap_zap_pte_range(struct vm_area_struct *vma, pmd_t *pmd,
					unsigned long addr, unsigned long end,
					struct free_pages *pages)
{
	struct mm_struct *const mm = vma->vm_mm;
	spinlock_t *ptl;
	pte_t *start_pte;
	pte_t *pte;
	unsigned long freed_pages = 0;


	// pr_info("PTE zap: 0x%lx-%lx %lx", addr, end, end - addr);

	start_pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
	pte = start_pte;
	// pte = pte_offset_map(pmd, addr);
	do {
		pte_t ptent = ptep_get_and_clear(mm, addr, pte);
		if (pte_none(ptent))
			continue;

		if (pte_present(ptent)) {
			unsigned long pfn = pte_pfn(ptent);
			struct page *page = pfn_to_page(pfn);
			unsigned int mapcount;

			/* TODO maybe return EBUSY at some point */
			if (PageUnevictable(page)) {
				/* pr_info("page %p unevictable", page); */
				continue;
			}

			BUG_ON(!pte_none(*pte));
			// pr_info("clear: addr: %lx -> %lu (%p) (none: %d)", addr, pfn, page, pte_none(*pte));
			BUG_ON(!page);

			list_add(&page->lru, &pages->list);
			freed_pages ++;

#ifdef MAPCOUNT
			mapcount = atomic_add_negative(-1, &page->_mapcount);
			BUG_ON(mapcount != 1); // Our pages are mapped exactly once

			if (unlikely(page_mapcount(page) < 0)) {
				pr_info("bad pte %p at %lx: %d", page, addr, page_mapcount(page));
			}
#endif
		}
		// FIXME: Guess: full=true
		// FIXME: Is this duplicated?
		// pte_clear_not_present_full(mm, addr, pte, true);
	} while (pte++, addr += PAGE_SIZE, addr != end);

	pages->count += freed_pages;

	pte_unmap_unlock(start_pte, ptl);

	return addr;
}

static inline unsigned long exmap_zap_pmd_range(
												struct vm_area_struct *vma, pud_t *pud,
												unsigned long addr, unsigned long end,
												struct free_pages *pages)
{
	pmd_t *pmd;
	unsigned long next;

	// pr_info("PMD zap: 0x%lx-%lx", addr, end);
	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		/*
		 * Here there can be other concurrent MADV_DONTNEED or
		 * trans huge page faults running, and if the pmd is
		 * none or trans huge it can change under us. This is
		 * because MADV_DONTNEED holds the mmap_lock in read
		 * mode.
		 */

		/*
		 * pmd_none_or_trans_huge_or_clear_bad was used, but
		 * didnt work FOR SOME REASON
		 */
		if (pmd_none_or_clear_bad(pmd))
			continue;
		next = exmap_zap_pte_range(vma, pmd, addr, next, pages);
	} while (pmd++, addr = next, addr != end);

	return addr;
}



static inline unsigned long exmap_zap_pud_range(
										  struct vm_area_struct *vma, p4d_t *p4d,
										  unsigned long addr, unsigned long end,
										  struct free_pages *pages)
{
	pud_t *pud;
	unsigned long next;

	// pr_info("PUD zap: 0x%lx-%lx", addr, end);

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_none_or_clear_bad(pud))
			continue;
		next = exmap_zap_pmd_range(vma, pud, addr, next, pages);
	} while (pud++, addr = next, addr != end);

	return addr;
}



static inline unsigned long exmap_zap_p4d_range(
												struct vm_area_struct *vma, pgd_t *pgd,
												unsigned long addr, unsigned long end,
												struct free_pages *pages)
{
	p4d_t *p4d;
	unsigned long next;

	//	pr_info("P4D zap: 0x%lx-%lx", addr, end);
	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none_or_clear_bad(p4d))
			continue;
		next = exmap_zap_pud_range(vma, p4d, addr, next, pages);
	} while (p4d++, addr = next, addr != end);

	return addr;
}


static struct page*
unmap_page_fastpath(pte_t *pte) {
	int err;
	struct page* page;
	pte_t ptent, new_ptent;

	ptent = ptep_get(pte);
	if (pte_present(ptent)) {
		unsigned long pfn = pte_pfn(ptent);
		struct page *page = pfn_to_page(pfn);

		if (PageUnevictable(page))
			return NULL;

		new_ptent = native_make_pte(0);

		if (atomic_long_cmpxchg((atomic_long_t*) &(pte->pte), ptent.pte, new_ptent.pte) == ptent.pte)
			return page;
	}

	return NULL;
}



/* insert_pages() amortizes the cost of spinlock operations
 * when inserting pages in a loop. Arch *must* define pte_index.
 */
static int
unmap_pages(struct vm_area_struct *vma, unsigned long addr, unsigned long num_pages,
						struct free_pages *pages)
{
	pgd_t *pgd = NULL;
	p4d_t *p4d = NULL;
	pud_t *pud = NULL;
	pmd_t *pmd = NULL;
	pte_t *start_pte, *pte;
	spinlock_t *pte_lock;
	struct mm_struct *const mm = vma->vm_mm;
	unsigned long remaining_pages_total = num_pages;
	unsigned long skip_pages, new_addr, pages_to_write_in_pmd;

more:
	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd)) {
		new_addr = (addr + PGDIR_SIZE) & P4D_MASK;
		skip_pages = (new_addr - addr) >> PAGE_SHIFT;
		if (remaining_pages_total <= skip_pages)
			goto out;

		addr += PGDIR_SIZE;
		remaining_pages_total -= skip_pages;
		/* exmap_debug("pgd: %lx: skipping %lu, left %lu", addr, skip_pages, remaining_pages_total); */
		goto more;
	}

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d)) {
		new_addr = (addr + P4D_SIZE) & PUD_MASK;
		skip_pages = (new_addr - addr) >> PAGE_SHIFT;
		if (remaining_pages_total <= skip_pages)
			goto out;

		addr = new_addr;
		remaining_pages_total -= skip_pages;
		/* exmap_debug("p4d: %lx: skipping %lu, left %lu", addr, skip_pages, remaining_pages_total); */
		goto more;
	}

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud)) {
		new_addr = (addr + PUD_SIZE) & PMD_MASK;
		skip_pages = (new_addr - addr) >> PAGE_SHIFT;
		if (remaining_pages_total <= skip_pages)
			goto out;

		addr = new_addr;
		remaining_pages_total -= skip_pages;
		/* exmap_debug("pud: %lx: skipping %lu, left %lu", addr, skip_pages, remaining_pages_total); */
		goto more;
	}

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd)) {
		new_addr = (addr + PMD_SIZE) & PAGE_MASK;
		skip_pages = (new_addr - addr) >> PAGE_SHIFT;
		if (remaining_pages_total <= skip_pages)
			goto out;

		addr = new_addr;
		remaining_pages_total -= skip_pages;
		/* exmap_debug("pmd: %lx: skipping %lu, left %lu", addr, skip_pages, remaining_pages_total); */
		goto more;
	}

	pages_to_write_in_pmd = min_t(unsigned long,
								  remaining_pages_total, PTRS_PER_PTE - pte_index(addr));

	while (pages_to_write_in_pmd) {
		int pte_idx = 0;
		const int batch_size = pages_to_write_in_pmd; //min_t(int, pages_to_write_in_pmd, 8);

#ifdef USE_FASTPATH
		if (pages_to_write_in_pmd == 1) {
			struct page *page;

			pte = pte_offset_map(pmd, addr);
			page = unmap_page_fastpath(pte);

			if (page) {
				list_add(&page->lru, &pages->list);
				pages->count ++;
			}

			remaining_pages_total -=1;
			addr += PAGE_SIZE;
			break;
		}
#endif

		start_pte = pte_offset_map_lock(mm, pmd, addr, &pte_lock);
		for (pte = start_pte; pte_idx < batch_size; ++pte, ++pte_idx) {
			pte_t ptent = ptep_get_and_clear(mm, addr, pte);
			if (pte_present(ptent)) {
				unsigned long pfn = pte_pfn(ptent);
				struct page *page = pfn_to_page(pfn);
				unsigned int mapcount;

				/* TODO maybe return EBUSY at some point */
				if (PageUnevictable(page)) {
					/* pr_info("page %p unevictable", page); */
					continue;
				}

				BUG_ON(!pte_none(*pte));
				// pr_info("clear: addr: %lx -> %lu (%p) (none: %d) %d", addr, pfn, page, pte_none(*pte),
				//    pages->count);
				BUG_ON(!page);

				list_add(&page->lru, &pages->list);
				pages->count ++;

#ifdef MAPCOUNT
				mapcount = atomic_add_negative(-1, &page->_mapcount);
				BUG_ON(mapcount != 1); // Our pages are mapped exactly once

				if (unlikely(page_mapcount(page) < 0)) {
					pr_info("bad pte %p at %lx: %d", page, addr, page_mapcount(page));
				}
#endif
			}

			addr += PAGE_SIZE;
		}
		pte_unmap_unlock(start_pte, pte_lock);
		pages_to_write_in_pmd -= batch_size;
		remaining_pages_total -= batch_size;
	}
	if (remaining_pages_total)
	   goto more;
out:
	return 0;
}


// adapted from: unmap_page_range
int exmap_unmap_pages( struct vm_area_struct *vma,
					  unsigned long addr, unsigned long num_pages,
					  struct free_pages *pages)
{
	const unsigned long end = addr + (num_pages * PAGE_SIZE);
	pgd_t *pgd;
	unsigned long next;

	if (addr < vma->vm_start || end > vma->vm_end)
		return -EFAULT;

	exmap_debug("unmap: 0x%lx-0x%lx (%lu pages)", addr, end, (end - addr + 1) >> PAGE_SHIFT);
	if ((end - addr + 1 ) >> PAGE_SHIFT == 0) {
		/* pr_info("exmap_unmap_pages: called to unmap 0 pages, skipping (num_pages = %lu)", num_pages); */
		return 0;
	}

#if 0 // Old and a little slower
	pgd = pgd_offset(vma->vm_mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd))
			continue;
		next = exmap_zap_p4d_range(vma, pgd, addr, next, pages);
	} while (pgd++, addr = next, addr != end);

	return 0;
#else
	return unmap_pages(vma, addr, num_pages, pages);
#endif

}

