/* vm.c: Generic interface for virtual memory objects. */

#include "hash.h"
#include "list.h"
#include "string.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "vm/inspect.h"
#include <stdint.h>
#include "vm/vm.h"
#define STACK_MAX_SIZE (1 << 20)

static struct list frame_table;
static struct lock frame_lock;
static struct list_elem* next;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void)
{
    vm_anon_init();
    vm_file_init();
#ifdef EFILESYS /* For project 4 */
    pagecache_init();
#endif
    register_inspect_intr();
    /* DO NOT MODIFY UPPER LINES. */
    /* TODO: Your code goes here. */
    list_init(&frame_table);
    lock_init(&frame_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page* page)
{
    int ty = VM_TYPE(page->operations->type);
    switch (ty) {
    case VM_UNINIT:
        return VM_TYPE(page->uninit.type);
    default:
        return ty;
    }
}

/* Helpers */
static bool vm_do_claim_page(struct page* page);
static struct frame* vm_evict_frame(void);
static bool rollback_claim(struct thread* current, struct frame* frame, struct page* page, bool mapping_set);
static struct frame* vm_get_victim(void);
static struct frame* clock(struct list_elem* start);
static struct list_elem* get_next(struct list_elem* elem);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void* upage, bool writable, vm_initializer* init, void* aux)
{

    ASSERT(VM_TYPE(type) != VM_UNINIT)
    struct supplemental_page_table* spt = &thread_current()->spt;

    /* Check wheter the upage is already occupied or not. */
    if (spt_find_page(spt, upage) == NULL) {
        /* TODO: Create the page, fetch the initialier according to the VM type,
         * TODO: and then create "uninit" page struct by calling uninit_new. You
         * TODO: should modify the field after calling the uninit_new. */
        /* TODO: Insert the page into the spt. */
        struct page* p = (struct page*)malloc(sizeof(struct page));
        if (p == NULL)
            return false;
        bool (*page_initializer)(struct page*, enum vm_type, void*);

        switch (VM_TYPE(type)) {
        case VM_ANON:
            page_initializer = anon_initializer;
            break;
        case VM_FILE:
            page_initializer = file_backed_initializer;
            break;
        }
        uninit_new(p, upage, init, type, aux, page_initializer);
        p->writable = writable;
        p->accessible_thread = thread_current();
        if (!spt_insert_page(spt, p)) {
            free(p);
            return false;
        }
        return true;
    }
    return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page* spt_find_page(struct supplemental_page_table* spt, void* va)
{
    /* TODO: Fill this function. */
    struct hash_elem* hash_e;
    struct page tmp_page;
    tmp_page.va = pg_round_down(va);
    hash_e = hash_find(&spt->hash_table, &tmp_page.hash_elem);
    if (hash_e == NULL)
        return NULL;
    else
        return hash_entry(hash_e, struct page, hash_elem);
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table* spt, struct page* page)
{
    /* TODO: Fill this function. */
    if (hash_insert(&spt->hash_table, &page->hash_elem) == NULL)
        return true;
    return false;
}

void spt_remove_page(struct supplemental_page_table* spt, struct page* page)
{
    hash_delete(&spt->hash_table, &page->hash_elem);
    vm_dealloc_page(page);
    return;
}

static struct list_elem* get_next(struct list_elem* elem)
{
    struct list_elem* next_elem = list_next(elem);
    if (next_elem == list_end(&frame_table))
        next_elem = list_begin(&frame_table);
    return next_elem;
}

/* Get the struct frame, that will be evicted. */
static struct frame* vm_get_victim(void)
{
    ASSERT(!list_empty(&frame_table));
    if (next == NULL || next == list_end(&frame_table))
        next = list_begin(&frame_table);

    struct list_elem* start = next;
    return clock(start);
}

static struct frame* clock(struct list_elem* start)
{
    ASSERT(!list_empty(&frame_table));
    struct frame* victim = NULL;
    /*
        eviction 우선순위
          1) 비어있는 프레임
          2) 접근 비트 0인 페이지
             - dirty 0 우선, 그래도 없으면 dirty 1
          3) 접근 비트 1이면 접근 비트를 0으로 내리고 다음으로 이동

        trial 의미
          - 첫 바퀴(trial=0)에서 못 찾는 경우:
              * 모든 페이지가 accessed=1이어서 모두 0으로 내린 상황
              * accessed=0이더라도 모두 dirty=1인 경우
          - 두 번째 바퀴(trial=1)에서는 accessed=0인 페이지를 dirty 여부와 상관없이 처음 만나는 대로 선택

        → second chance 개념을 clock으로 구현
    */
    for (int trial = 0; trial < 2 && victim == NULL; trial++) {
        do {
            struct frame* f = list_entry(next, struct frame, frame_elem);
            struct page* p = f->page;

            if (p == NULL) {
                victim = f;
                break;
            }

            bool accessed = pml4_is_accessed(p->accessible_thread->pml4, p->va);
            if (accessed) {
                pml4_set_accessed(p->accessible_thread->pml4, p->va, false);
            } else {
                if (trial == 0) {
                    if (!pml4_is_dirty(p->accessible_thread->pml4, p->va)) {
                        victim = f;
                        break;
                    }
                } else {
                    victim = f;
                    break;
                }
            }

            next = get_next(next);
        } while (next != start);

        if (victim == NULL) {
            next = get_next(start);
            start = next;
        }
    }

    if (victim != NULL) {
        next = get_next(&victim->frame_elem);
    }
    return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame* vm_evict_frame(void)
{
    struct frame* victim = vm_get_victim();
    /* TODO: swap out the victim and return the evicted frame. */

    if (victim == NULL || !swap_out(victim->page))
        return NULL;

    victim->page = NULL;
    return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame* vm_get_frame(void)
{
    struct frame* frame = NULL;
    /* TODO: Fill this function. */
    void* kva = palloc_get_page(PAL_USER);
    if (kva == NULL) {
        return vm_evict_frame();
    }
    frame = (struct frame*)malloc(sizeof(struct frame));
    if (frame == NULL) {
        palloc_free_page(kva);
        return NULL;
    }
    frame->kva = kva;
    frame->page = NULL;
    frame->in_table = false;
    ASSERT(frame->page == NULL);
    return frame;
}

/* Growing the stack. */
static void vm_stack_growth(void* addr)
{
    vm_alloc_page(VM_ANON | VM_MARKER_0, pg_round_down(addr), 1);
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page* page)
{
}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame* f, void* addr, bool user, bool write, bool not_present)
{
    struct supplemental_page_table* spt = &thread_current()->spt;
    struct page* page = NULL;
    /* TODO: Validate the fault */
    /* TODO: Your code goes here */
    if (addr == NULL)
        return false;
    if (is_kernel_vaddr(addr))
        return false;
    if (not_present) {
        uintptr_t rsp = f->rsp;
        if (!user)
            rsp = thread_current()->rsp;
        if ((USER_STACK - STACK_MAX_SIZE <= rsp - 8 && rsp - 8 == addr && addr <= USER_STACK) ||
            (USER_STACK - STACK_MAX_SIZE <= rsp && rsp <= addr && addr <= USER_STACK))
            vm_stack_growth(addr);

        page = spt_find_page(spt, addr);
        if (page == NULL)
            return false;
        if (write == 1 && page->writable == 0)
            return false;
        return vm_do_claim_page(page);
    }
    return false;
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page* page)
{
    destroy(page);
    free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void* va)
{
    struct page* page = NULL;
    /* TODO: Fill this function */
    page = spt_find_page(&thread_current()->spt, va);
    if (page == NULL)
        return false;
    return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool vm_do_claim_page(struct page* page)
{
    struct frame* frame = vm_get_frame();
    if (frame == NULL)
        return false;
    if (!frame->in_table) {
        lock_acquire(&frame_lock);
        list_push_back(&frame_table, &frame->frame_elem);
        lock_release(&frame_lock);
        frame->in_table = true;
    }

    /* Set links */
    frame->page = page;
    page->frame = frame;

    /* TODO: Insert page table entry to map page's VA to frame's PA. */
    struct thread* current = thread_current();
    if (!pml4_set_page(current->pml4, page->va, frame->kva, page->writable))
        return rollback_claim(current, frame, page, false);

    if (!swap_in(page, frame->kva))
        return rollback_claim(current, frame, page, true);

    return true;
}

static uint64_t page_hash(const struct hash_elem* hash_e, void* aux UNUSED)
{
    struct page* page = hash_entry(hash_e, struct page, hash_elem);
    return hash_bytes(&page->va, sizeof(page->va));
}

static bool page_less(const struct hash_elem* a, const struct hash_elem* b, void* aux UNUSED)
{
    struct page* page_a = hash_entry(a, struct page, hash_elem);
    struct page* page_b = hash_entry(b, struct page, hash_elem);
    return page_a->va < page_b->va;
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table* spt)
{
    hash_init(&spt->hash_table, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table* dst, struct supplemental_page_table* src)
{
    struct hash_iterator i;
    struct page* src_page;

    hash_first(&i, &src->hash_table);
    while (hash_next(&i)) {
        src_page = hash_entry(hash_cur(&i), struct page, hash_elem);
        enum vm_type src_type = VM_TYPE(src_page->operations->type);
        void* upage = src_page->va;
        bool writable = src_page->writable;

        if (page_get_type(src_page) == VM_FILE)
            continue;

        if (src_type == VM_UNINIT) {
            struct uninit_page* src_uninit = &src_page->uninit;
            if (VM_TYPE(src_uninit->type) == VM_FILE)
                continue;
            if (src_uninit->aux != NULL) {
                struct lazy_load_aux* copy_aux = malloc(sizeof(struct lazy_load_aux));
                if (copy_aux == NULL)
                    return false;
                memcpy(copy_aux, src_uninit->aux, sizeof(struct lazy_load_aux));
                if (!vm_alloc_page_with_initializer(src_uninit->type, upage, writable, src_uninit->init, copy_aux))
                    return false;
            } else {
                if (!vm_alloc_page(src_uninit->type, upage, writable))
                    return false;
            }
            continue;
        }
        if (!vm_alloc_page(src_type, upage, writable))
            return false;
        if (!vm_claim_page(upage))
            return false;
        struct page* dst_page = spt_find_page(dst, upage);
        memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
    }
    return true;
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table* spt)
{
    /* TODO: Destroy all the supplemental_page_table hold by thread and
     * TODO: writeback all the modified contents to the storage. */
    hash_clear(&spt->hash_table, hash_desroy_action);
}

void hash_desroy_action(struct hash_elem* hash_elem, void* aux)
{
    struct page* page = hash_entry(hash_elem, struct page, hash_elem);
    destroy(page);
    free(page);
}

static bool rollback_claim(struct thread* current, struct frame* frame, struct page* page, bool mapping_set)
{
    if (mapping_set)
        pml4_clear_page(current->pml4, page->va);

    page->frame = NULL;
    frame->page = NULL;

    vm_free_frame(frame);
    return false;
}
void vm_free_frame(struct frame* frame)
{
    ASSERT(frame != NULL);
    ASSERT(frame->page == NULL);

    if (frame->in_table) {
        lock_acquire(&frame_lock);
        list_remove(&frame->frame_elem);
        lock_release(&frame_lock);
        frame->in_table = false;
    }

    palloc_free_page(frame->kva);
    free(frame);
}
