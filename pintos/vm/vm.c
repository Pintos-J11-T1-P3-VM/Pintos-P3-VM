/* vm.c: Generic interface for virtual memory objects. */

#include "filesys/file.h"
#include "list.h"
#include "stddef.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "vm/vm.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "vm/inspect.h"
#include "vm/file.h"
#include "userprog/syscall.h"
#include "hash.h"
#include "threads/vaddr.h"
#include "string.h"
#include "userprog/process.h"
#include <stdint.h>
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
static struct frame* vm_get_victim(void);
static bool vm_do_claim_page(struct page* page);
static struct frame* vm_evict_frame(void);

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
err:
    return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page* spt_find_page(struct supplemental_page_table* spt, void* va)
{
    struct page* page = NULL;
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
}

/* Helper function for circular list traversal */
static struct list_elem* get_next_frame_elem(struct list_elem *elem) {
    struct list_elem *next_elem = list_next(elem);
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
    
    struct list_elem *start = next;
    struct frame *victim = NULL;

    /*
        1. 우선순위
            - 비어있는 프레임
        2. 그 다음으로는
            - 접근 비트 0이면서,
                - dirty 비트 0
                - dirty 비트 1
        3. 접근 비트 1인 경우에는 접근 비트를 0으로 만들고 다음 프레임으로 이동
        
        trial가 1이 되는 경우는 2가지가 존재
            1. 한 바퀴를 돌았는데 victim이 선정되지 않은 경우
                - 모두 accessed 가 1인 경우
                - 모두 accessed 가 0이지만 dirty 가 1인 경우
            2. 두 번째 탐색에서는 무조건 victim이 선정됨
                - 이전에 모든 접근비트를 0으로 만들었기 때문에 이후 발견되는 첫번째 접근비트 0인 프레임이 victim이 됨
    */
    // second chance(개념)를 clock(구현체)으로 구현
    for (int trial = 0; trial < 2 && victim == NULL; trial++) {
        do { // 무조건 한번은 실행
            struct frame *f = list_entry(next, struct frame, frame_elem);
            struct page *p = f->page;

            // 비어있는 프레임이면 바로 선택
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

            next = get_next_frame_elem(next);
        } while (next != start);

        // 다음 탐색 위치 설정
        if (victim == NULL) {
            next = get_next_frame_elem(start);
            start = next;
        }
    }

    // next를 victim 다음으로 이동
    if (victim != NULL) {
        next = get_next_frame_elem(&victim->frame_elem);
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
        if (write && !page->writable)
            return false;
        if (!vm_do_claim_page(page))
            return false;
        return true;
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
    if (!pml4_set_page(current->pml4, page->va, frame->kva, page->writable)) {
        if (!frame->in_table) {
            lock_acquire(&frame_lock);
            list_remove(&frame->frame_elem);
            lock_release(&frame_lock);
            palloc_free_page(frame->kva);
            free(frame);
        } else {
            frame->page = NULL;
        }
        page->frame = NULL;
        return false;
    }

    if (!swap_in(page, frame->kva)) {
        /* swap_in 실패 시 정리 */
        pml4_clear_page(current->pml4, page->va);
        if (!frame->in_table) {
            lock_acquire(&frame_lock);
            list_remove(&frame->frame_elem);
            lock_release(&frame_lock);
            palloc_free_page(frame->kva);
            free(frame);
        } else {
            frame->page = NULL;
        }
        page->frame = NULL;
        return false;
    }
    return true;
}

uint64_t page_hash(const struct hash_elem* hash_e, void* aux)
{
    struct page* page = hash_entry(hash_e, struct page, hash_elem);
    return hash_bytes(&page->va, sizeof(page->va));
}

bool page_less(const struct hash_elem* a, const struct hash_elem* b, void* aux)
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
        
        // 기본 과제에서는 mmap 상속 불필요. 
        // 단, extra(cow) 구현 시에는 이 continue를 제거하고 
        // 파일 객체 복제(file_reopen) 및 페이지 공유(Read-Only Mapping) 로직으로 대체해야 함.
        if (src_type == VM_FILE)
            continue;
        
        if (src_type == VM_UNINIT) {
            struct uninit_page* src_uninit = &src_page->uninit;
            // VM_FILE로 바뀔 UNINIT 페이지도 복사하지 않음
            enum vm_type final_type = VM_TYPE(src_uninit->type);
            if (final_type == VM_FILE)
                continue;
            if (src_uninit->aux != NULL) {
                struct file_page* copy_aux = malloc(sizeof(struct file_page));
                if (copy_aux == NULL)
                    goto err;
                memcpy(copy_aux, src_uninit->aux, sizeof(struct file_page));
                if (!vm_alloc_page_with_initializer(src_uninit->type, upage, writable, src_uninit->init, copy_aux)) {
                    if (src_uninit->init == lazy_load_segment) {
                        lock_acquire(&filesys_lock);
                        file_close(copy_aux->file);
                        lock_release(&filesys_lock);
                    }
                    free(copy_aux);
                    goto err;
                }
            } else {
                if (!vm_alloc_page(src_uninit->type, upage, writable))
                    goto err;
            }
            continue;
        }
        if (!vm_alloc_page(src_type, upage, writable))
            goto err;
        if (!vm_claim_page(upage))
            goto err;
        struct page* dst_page = spt_find_page(dst, upage);
        memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
    }
    return true;

err:
    /* 실패 시 이미 복사된 페이지들 정리 */
    supplemental_page_table_kill(dst);
    return false;
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

void vm_free_frame(struct frame* frame)                               
{                                                                     
    ASSERT(frame != NULL);                                            
    ASSERT(frame->page == NULL);                                      
                                                                     
    lock_acquire(&frame_lock);                                        
    list_remove(&frame->frame_elem);                                  
    lock_release(&frame_lock);                                        
                                                                     
    palloc_free_page(frame->kva);                                     
    free(frame);                                                      
}
