/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/thread.h"
#include "hash.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
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
        struct page* new_page = (struct page*)malloc(sizeof(struct page));
        if (new_page == NULL)
            goto err;
        if (VM_TYPE(type) == VM_ANON)
            uninit_new(new_page, upage, init, type, aux, anon_initializer);
        else if (VM_TYPE(type) == VM_FILE)
            uninit_new(new_page, upage, init, type, aux, file_backed_initializer);
        else {
            free(new_page);
            goto err;
        } // VM_PAGE_CACHE
        new_page->writable = writable;                 // uninit_new 뒤에 해주기
        if (spt_insert_page(spt, new_page) == false) { // false if same exists
            free(new_page);
            goto err;
        }
    } else
        goto err;
    return true;
err:
    return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page* spt_find_page(struct supplemental_page_table* spt UNUSED, void* va UNUSED)
{
    struct page page; // should not use malloc. declare in stack
    struct hash* hash_spt = &(spt->hash_spt);
    page.va = pg_round_down(va);
    struct hash_elem* e = hash_find(hash_spt, &page.hash_elem); // hash , hash_elem
    if (e == NULL) {
        return NULL;
    } else {
        struct page* ret = hash_entry(e, struct page, hash_elem); // elem,struct,member / convert elem to struct
        return ret;
    }
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table* spt UNUSED, struct page* page UNUSED)
{
    int succ = false;
    /* TODO: Fill this function. */
    if (hash_insert(&spt->hash_spt, &page->hash_elem) == NULL) // same not exists -> null
        succ = true;
    return succ;
}

void spt_remove_page(struct supplemental_page_table* spt, struct page* page)
{
    vm_dealloc_page(page);
    return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame* vm_get_victim(void)
{
    struct frame* victim = NULL;
    /* TODO: The policy for eviction is up to you. */

    return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame* vm_evict_frame(void)
{
    struct frame* victim UNUSED = vm_get_victim();
    /* TODO: swap out the victim and return the evicted frame. */

    return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame* vm_get_frame(void)
{
    struct frame* frame;
    /* TODO: Fill this function. */

    void* kva = palloc_get_page(PAL_USER);
    if (kva == NULL) {
        frame = vm_evict_frame(); // not yet
    } else {
        frame = (struct frame*)malloc(sizeof(struct frame));
        if (frame == NULL) {
            palloc_free_page(kva);
            return NULL;
        }
        frame->kva = kva;
        frame->page = NULL;
    }
    ASSERT(frame != NULL);
    ASSERT(frame->page == NULL);
    return frame;
}

/* Growing the stack. */
static void vm_stack_growth(void* addr UNUSED)
{
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page* page UNUSED)
{
}

/* Return true on success */
bool vm_try_handle_fault(
    struct intr_frame* f UNUSED, void* addr UNUSED, bool user UNUSED, bool write UNUSED,
    bool not_present UNUSED) // f, fault_addr, user, write, not_present  , user: true write: true not_present: false
{
    struct supplemental_page_table* spt UNUSED = &thread_current()->spt;
    /* TODO: Validate the fault */
    addr = pg_round_down(addr);
    struct page* page = spt_find_page(spt, addr); // if addr is in spt, valid
    if (page == NULL)
        return false;
    /* TODO: Your code goes here */
    // user, write, not_present not used yet
    return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page* page)
{
    destroy(page);
    free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void* va UNUSED)
{
    /* TODO: Fill this function */
    struct page* page = spt_find_page(&thread_current()->spt, va);
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
    /* Set links */
    frame->page = page;
    page->frame = frame;
    /* TODO: Insert page table entry to map page's VA to frame's PA. */
    if (pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable))
        return swap_in(page, frame->kva); // todo: 나중에 frame, frame->kva free 해주기
    palloc_free_page(frame->kva);
    free(frame);
    return false;
}

/* Initialize new supplemental page table */
uint64_t hash_func(const struct hash_elem* e, void* aux UNUSED)
{
    struct page* page = hash_entry(e, struct page, hash_elem);
    return hash_bytes(&page->va, sizeof(page->va));
}
bool less_func(const struct hash_elem* a, const struct hash_elem* b, void* aux UNUSED)
{
    struct page* page_a = hash_entry(a, struct page, hash_elem);
    struct page* page_b = hash_entry(b, struct page, hash_elem);
    return page_a->va < page_b->va;
}

void supplemental_page_table_init(struct supplemental_page_table* spt UNUSED)
{
    hash_init(&spt->hash_spt, hash_func, less_func, NULL);
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table* dst UNUSED, // dst: child / src: parent
                                  struct supplemental_page_table* src UNUSED)
{
    if (dst == NULL || src == NULL)
        return false;
    struct hash_iterator i;
    hash_first(&i, &src->hash_spt);
    while (hash_next(&i)) {
        struct page* src_page = hash_entry(hash_cur(&i), struct page, hash_elem);
        enum vm_type type = page_get_type(src_page);

        if (src_page->operations->type == VM_UNINIT) {
            struct args_load_segment *dst_aux, *src_aux = src_page->uninit.aux;
            if (src_aux == NULL) // src_aux is null in setup_stack
                dst_aux = NULL;
            else if ((dst_aux = malloc(sizeof(struct args_load_segment))) == NULL) // free in destroy
                return false;
            else {
                dst_aux->offset = src_aux->offset;
                dst_aux->read_bytes = src_aux->read_bytes;
                dst_aux->zero_bytes = src_aux->zero_bytes;
                dst_aux->file = file_reopen(src_aux->file); // close in destroy
            }
            if (!vm_alloc_page_with_initializer(type, src_page->va, src_page->writable, src_page->uninit.init,
                                                dst_aux)) {
                file_close(dst_aux->file);
                free(dst_aux);
                return false;
            }
        } else {
            struct page* dst_page;
            if (!vm_alloc_page(type, src_page->va, src_page->writable) || !vm_claim_page(src_page->va) ||
                (dst_page = spt_find_page(dst, src_page->va)) == NULL) { // do alloc, claim, find
                file_close
            }
            return false;
            memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
        }
    }
    return true;
}
void free_page(struct hash_elem* e, void* aux UNUSED)
{ // define hash_action_func. for spt kill
    struct page* page = hash_entry(e, struct page, hash_elem);
    vm_dealloc_page(page);
}
/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table* spt UNUSED)
{
    /* TODO: Destroy all the supplemental_page_table hold by thread and
     * TODO: writeback all the modified contents to the storage. */
    if (spt == NULL)
        return;
    hash_destroy(&spt->hash_spt, free_page);
}
