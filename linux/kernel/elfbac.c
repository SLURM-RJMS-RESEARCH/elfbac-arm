#include <asm/mmu_context.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>

#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/resource.h>
#include <linux/rmap.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/uaccess.h>

#include <linux/elfbac.h>

static int parse_ulong(unsigned char **buf, size_t *size, unsigned long *out)
{
	if (*size >= sizeof(unsigned long)) {
		*out = *(unsigned long *)(*buf);
		*buf += sizeof(unsigned long);
		*size -= sizeof(unsigned long);
		return 0;
	}

	return -1;
}

static int elfbac_parse_state(unsigned char **buf, size_t *size,
			      struct elfbac_state *state)
{
	if (parse_ulong(buf, size, &state->stack_id) != 0)
		return -EINVAL;

	state->pgd = NULL;
	memset(&state->context, 0, sizeof(mm_context_t));

	return 0;
}

static int elfbac_parse_section(unsigned char **buf, size_t *size,
				struct elfbac_section *section)
{
	if (parse_ulong(buf, size, &section->base) != 0)
		return -EINVAL;
	if (parse_ulong(buf, size, &section->size) != 0)
		return -EINVAL;
	if (parse_ulong(buf, size, &section->flags) != 0)
		return -EINVAL;

	return 0;
}

static int elfbac_parse_data_transition(unsigned char **buf, size_t *size,
					struct elfbac_data_transition *data_transition)
{
	if (parse_ulong(buf, size, &data_transition->to) != 0)
		return -EINVAL;
	if (parse_ulong(buf, size, &data_transition->from) != 0)
		return -EINVAL;
	if (parse_ulong(buf, size, &data_transition->base) != 0)
		return -EINVAL;
	if (parse_ulong(buf, size, &data_transition->size) != 0)
		return -EINVAL;
	if (parse_ulong(buf, size, &data_transition->flags) != 0)
		return -EINVAL;

	return 0;
}

static int elfbac_parse_call_transition(unsigned char **buf, size_t *size,
					struct elfbac_call_transition *call_transition)
{
	if (parse_ulong(buf, size, &call_transition->to) != 0)
		return -EINVAL;
	if (parse_ulong(buf, size, &call_transition->from) != 0)
		return -EINVAL;
	if (parse_ulong(buf, size, &call_transition->address) != 0)
		return -EINVAL;
	if (parse_ulong(buf, size, &call_transition->param_size) != 0)
		return -EINVAL;
	if (parse_ulong(buf, size, &call_transition->return_size) != 0)
		return -EINVAL;

	return 0;
}

static int elfbac_validate_policy(struct elfbac_policy *policy)
{
	unsigned long num_states = 0;
	struct elfbac_state *state = NULL;
	struct elfbac_section *section = NULL;
	struct elfbac_data_transition *data_transition = NULL;
	struct elfbac_call_transition *call_transition = NULL;

	list_for_each_entry(state, &policy->states_list, list) {
		if (state->stack_id >= policy->num_stacks)
			return -EINVAL;

		num_states++;

		list_for_each_entry(section, &state->sections_list, list) {
			if (section->flags & VM_WRITE && !access_ok(VERIFY_WRITE,
								      (void *)section->base,
								      section->size))
				return -EINVAL;
			else if (!access_ok(VERIFY_READ, (void *)section->base,
					    section->size))
				return -EINVAL;
		}

	}

	if (num_states == 0)
		return -EINVAL;

	if (policy->num_stacks > num_states)
		return -EINVAL;

	list_for_each_entry(data_transition, &policy->data_transitions_list, list) {
		if (data_transition->from > num_states ||
		    data_transition->to > num_states)
			return -EINVAL;

		if (data_transition->flags & VM_WRITE && !access_ok(VERIFY_WRITE,
								      (void *)data_transition->base,
								      data_transition->size))
			return -EINVAL;
		else if (!access_ok(VERIFY_READ, (void *)data_transition->base,
				    data_transition->size))
			return -EINVAL;
	}

	list_for_each_entry(call_transition, &policy->call_transitions_list, list) {
		if (call_transition->from > num_states ||
		    call_transition->to > num_states)
			return -EINVAL;

		if (!access_ok(VERIFY_READ, (void *)call_transition->address,
			       sizeof(unsigned long)))
			return -EINVAL;
	}

	return 0;
}

int elfbac_parse_policy(unsigned char *buf, size_t size,
			struct elfbac_policy *out)
{
	enum {
		STATE = 1,
		SECTION,
		DATA_TRANSITION,
		CALL_TRANSITION
	} type;

	int retval;
	unsigned long cur_state_id = 0;

	struct elfbac_policy policy;
	struct elfbac_state *state = NULL;
	struct elfbac_section *section = NULL;
	struct elfbac_data_transition *data_transition = NULL;
	struct elfbac_call_transition *call_transition = NULL;

	INIT_LIST_HEAD(&policy.states_list);
	INIT_LIST_HEAD(&policy.data_transitions_list);
	INIT_LIST_HEAD(&policy.call_transitions_list);

	retval = -EINVAL;
	if (parse_ulong(&buf, &size, &policy.num_stacks) != 0)
		goto out;

	while (size) {
		retval = -EINVAL;
		if (parse_ulong(&buf, &size, (unsigned long *)&type) != 0)
			goto out;

		switch (type) {
		case STATE:
			retval = -ENOMEM;
			state = kmalloc(sizeof(struct elfbac_state),
					GFP_KERNEL);
			if (!state)
				goto out;

			retval = elfbac_parse_state(&buf, &size, state);
			if (retval != 0)
				goto out;

			state->id = cur_state_id++;
			state->pgd = NULL;
			INIT_LIST_HEAD(&state->sections_list);
			list_add_tail(&state->list, &policy.states_list);
			state = NULL;
			break;
		case SECTION:
			if (list_empty(&policy.states_list))
				goto out;
			state = list_last_entry(&policy.states_list,
						struct elfbac_state,
						list);

			retval = -ENOMEM;
			section = kmalloc(sizeof(struct elfbac_section),
					  GFP_KERNEL);
			if (!section)
				goto out;

			retval = elfbac_parse_section(&buf, &size, section);
			if (retval != 0)
				goto out;

			list_add_tail(&section->list, &state->sections_list);
			state = NULL;
			section = NULL;
			break;
		case DATA_TRANSITION:
			retval = -ENOMEM;
			data_transition = kmalloc(
						  sizeof(struct elfbac_data_transition),
						  GFP_KERNEL);
			if (!data_transition)
				goto out;

			retval = elfbac_parse_data_transition(&buf, &size,
							      data_transition);
			if (retval != 0)
				goto out;

			list_add_tail(&data_transition->list, &policy.data_transitions_list);
			data_transition = NULL;
			break;
		case CALL_TRANSITION:
			retval = -ENOMEM;
			call_transition = kmalloc(
						  sizeof(struct elfbac_call_transition),
						  GFP_KERNEL);
			if (!call_transition)
				goto out;

			retval = elfbac_parse_call_transition(&buf, &size,
							      call_transition);
			if (retval != 0)
				goto out;

			list_add_tail(&call_transition->list, &policy.call_transitions_list);
			call_transition = NULL;
			break;
		default:
			goto out;
		}
	}

	retval = -EINVAL;
	if (elfbac_validate_policy(&policy) != 0)
		goto out;

	// TODO: Figure out stacks

	policy.current_state = list_entry(policy.states_list.next,
					  struct elfbac_state, list);
	*out = policy;
	retval = 0;

out:
	elfbac_policy_destroy(&policy);
	kfree(state);
	kfree(section);
	kfree(data_transition);
	kfree(call_transition);
	return retval;
}

void elfbac_policy_destroy(struct elfbac_policy *policy)
{

}

int elfbac_policy_clone(struct elfbac_policy *orig, struct elfbac_policy *new)
{
	return 0;
}

bool elfbac_access_ok(struct elfbac_policy *policy, unsigned long address,
		      unsigned int mask, struct elfbac_state **next_state)
{

	// For now, don't deny any requests
	*next_state = NULL;
	return true;
}

/* Modified routines from mm/memory.c
 * TODO: Potentially factor out common code to reduce duplication
 * TODO: Hugepages support
 */
static inline void init_rss_vec(int *rss)
{
	memset(rss, 0, sizeof(int) * NR_MM_COUNTERS);
}

static inline void add_mm_rss_vec(struct mm_struct *mm, int *rss)
{
	int i;

	if (current->mm == mm)
		sync_mm_rss(mm);
	for (i = 0; i < NR_MM_COUNTERS; i++)
		if (rss[i])
			add_mm_counter(mm, i, rss[i]);
}

static inline unsigned long elfbac_copy_one_pte(struct mm_struct *mm,
		pte_t *dst_pte, pte_t *src_pte, struct vm_area_struct *vma,
		unsigned long addr, int *rss)
{
	pte_t pte = *src_pte;
	struct page *page;

	/* pte contains position in file, so copy. */
	if (unlikely(!pte_present(pte))) {
		swp_entry_t entry = pte_to_swp_entry(pte);

		if (likely(!non_swap_entry(entry))) {
			if (swap_duplicate(entry) < 0)
				return entry.val;
		} else if (is_migration_entry(entry)) {
			page = migration_entry_to_page(entry);

			if (PageAnon(page))
				rss[MM_ANONPAGES]++;
			else
				rss[MM_FILEPAGES]++;
		}
		goto out_set_pte;
	}

	page = vm_normal_page(vma, addr, pte);
	if (page) {
		get_page(page);
		page_dup_rmap(page);
		if (PageAnon(page))
			rss[MM_ANONPAGES]++;
		else
			rss[MM_FILEPAGES]++;
	}

out_set_pte:
	set_pte_at(mm, addr, dst_pte, pte);
	return 0;
}

static int elfbac_copy_pte_range(struct mm_struct *mm, pmd_t *dst_pmd,
		pmd_t *src_pmd, struct vm_area_struct *vma, unsigned long addr,
		unsigned long end)
{
	pte_t *orig_src_pte, *orig_dst_pte;
	pte_t *src_pte, *dst_pte;
	spinlock_t *dst_ptl;
	int progress = 0;
	int rss[NR_MM_COUNTERS];
	swp_entry_t entry = (swp_entry_t){0};

again:
	init_rss_vec(rss);

	dst_pte = pte_alloc_map_lock(mm, dst_pmd, addr, &dst_ptl);
	if (!dst_pte)
		return -ENOMEM;
	src_pte = pte_offset_map(src_pmd, addr);
	orig_src_pte = src_pte;
	orig_dst_pte = dst_pte;
	arch_enter_lazy_mmu_mode();

	do {
		if (pte_none(*src_pte)) {
			progress++;
			continue;
		}
		entry.val = elfbac_copy_one_pte(mm, dst_pte, src_pte,
							vma, addr, rss);
		if (entry.val)
			break;
		progress += 8;
	} while (dst_pte++, src_pte++, addr += PAGE_SIZE, addr != end);

	arch_leave_lazy_mmu_mode();
	pte_unmap(orig_src_pte);
	add_mm_rss_vec(mm, rss);
	pte_unmap_unlock(orig_dst_pte, dst_ptl);
	cond_resched();

	if (entry.val) {
		if (add_swap_count_continuation(entry, GFP_KERNEL) < 0)
			return -ENOMEM;
		progress = 0;
	}

	if (addr != end)
		goto again;
	return 0;
}

static inline int elfbac_copy_pmd_range(struct mm_struct *mm, pud_t *dst_pud,
		pud_t *src_pud, struct vm_area_struct *vma, unsigned long addr,
		unsigned long end)
{
	pmd_t *src_pmd, *dst_pmd;
	unsigned long next;

	dst_pmd = pmd_alloc(mm, dst_pud, addr);
	if (!dst_pmd)
		return -ENOMEM;
	src_pmd = pmd_offset(src_pud, addr);
	do {
		next = pmd_addr_end(addr, end);
//		if (pmd_trans_huge(*src_pmd)) {
//			int err;
//			VM_BUG_ON(next-addr != HPAGE_PMD_SIZE);
//			err = copy_huge_pmd(mm, src_mm,
//					    dst_pmd, src_pmd, addr, vma);
//			if (err == -ENOMEM)
//				return -ENOMEM;
//			if (!err)
//				continue;
//			/* fall through */
//		}
		if (pmd_none_or_clear_bad(src_pmd))
			continue;
		if (elfbac_copy_pte_range(mm, dst_pmd, src_pmd,
						vma, addr, next))
			return -ENOMEM;
	} while (dst_pmd++, src_pmd++, addr = next, addr != end);
	return 0;
}

static inline int elfbac_copy_pud_range(struct mm_struct *mm, pgd_t *dst_pgd,
		pgd_t *src_pgd, struct vm_area_struct *vma, unsigned long addr,
		unsigned long end)
{
	pud_t *src_pud, *dst_pud;
	unsigned long next;

	dst_pud = pud_alloc(mm, dst_pgd, addr);
	if (!dst_pud)
		return -ENOMEM;
	src_pud = pud_offset(src_pgd, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_none_or_clear_bad(src_pud))
			continue;
		if (elfbac_copy_pmd_range(mm, dst_pud, src_pud,
						vma, addr, next))
			return -ENOMEM;
	} while (dst_pud++, src_pud++, addr = next, addr != end);
	return 0;
}

static int elfbac_copy_page_range(struct mm_struct *mm, pgd_t *dst_pgd,
		struct vm_area_struct *vma, unsigned long addr,
		unsigned long end)
{
	pgd_t *src_pgd = mm->pgd;
	unsigned long next;
	int ret;

	if (vma->vm_start > addr || end > vma->vm_end)
		return -EINVAL;

	/*
	 * Don't copy ptes where a page fault will fill them correctly.
	 * Fork becomes much lighter when there are big shared or private
	 * readonly mappings. The tradeoff is that copy_page_range is more
	 * efficient than faulting.
	 */
//	if (!(vma->vm_flags & (VM_HUGETLB | VM_PFNMAP | VM_MIXEDMAP)) &&
//			!vma->anon_vma)
//		return 0;

//	if (is_vm_hugetlb_page(vma))
//		return copy_hugetlb_page_range(mm, src_mm, vma);

	if (unlikely(vma->vm_flags & VM_PFNMAP)) {
		/*
		 * We do not free on error cases below as remove_vma
		 * gets called on error from higher level routine
		 */
		ret = track_pfn_copy(vma);
		if (ret)
			return ret;
	}

	ret = 0;
	src_pgd = pgd_offset(mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(src_pgd))
			continue;
		if (unlikely(elfbac_copy_pud_range(mm, dst_pgd, src_pgd,
					    vma, addr, next))) {
			ret = -ENOMEM;
			break;
		}
	} while (dst_pgd++, src_pgd++, addr = next, addr != end);

	return ret;
}

int elfbac_copy_mapping(struct elfbac_policy *policy, struct mm_struct *mm,
			struct vm_area_struct *vma, unsigned long addr)
{
	pgd_t *dst_pgd = policy->current_state->pgd + pgd_index(addr);
	unsigned long start = rounddown(addr, PAGE_SIZE);
	unsigned long end = roundup(addr, PAGE_SIZE);

	if (start == end)
		end += PAGE_SIZE;

	return elfbac_copy_page_range(mm, dst_pgd, vma, start, end);
}
