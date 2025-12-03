/* vm.c: Generic interface for virtual memory objects. */

#include "hash.h"
#include "stddef.h"
#include "threads/malloc.h"
#include "vm/vm.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/inspect.h"
#include "vm/uninit.h"
#include "filesys/file.h"
#include <stdint.h>
#include <string.h>

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
    supplemental_page_table_init(&thread_current()->spt); // initd 프로세스의 spt 초기화
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page* page)
{
    int ty = VM_TYPE(page->operations->type);
    switch (ty) {
    case VM_UNINIT: // 초기화 전 페이지는 uninit.type 반환
        return VM_TYPE(page->uninit.type);
    default: // 그 외에는 페이지의 실제 타입 반환
        return ty;
    }
}

/* Helpers */
static struct frame* vm_get_victim(void);
static bool vm_do_claim_page(struct page* page);
static struct frame* vm_evict_frame(void);
static void page_destroy_func(struct hash_elem* e, void* aux UNUSED);
void vm_release_frame(struct page* page);
static void free_lazy_aux(struct lazy_load_aux* aux);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void* upage, bool writable, vm_initializer* init, void* aux)
{
    ASSERT(VM_TYPE(type) != VM_UNINIT)

    struct supplemental_page_table* spt = &thread_current()->spt;

    /* Check wheter the upage is already occupied or not. */
    if (spt_find_page(spt, upage) != NULL) {
        free_lazy_aux(aux);
        return false;
    }

    /* TODO: Create the page, fetch the initialier according to the VM type,
     * TODO: and then create "uninit" page struct by calling uninit_new. You
     * TODO: should modify the field after calling the uninit_new. */
    struct page* page = malloc(sizeof(struct page));
    if (page == NULL) {
        free_lazy_aux(aux);
        return false;
    }
    switch (VM_TYPE(type)) {
    case VM_ANON:
        uninit_new(page, upage, init, type, aux, anon_initializer);
        break;
    case VM_FILE:
        uninit_new(page, upage, init, type, aux, file_backed_initializer);
        break;
    default:
        free_lazy_aux(aux);
        free(page);
        return false;
    }
    page->va = upage;
    page->writable = writable;
    if (!spt_insert_page(spt, page)) {
        free_lazy_aux(aux);
        free(page);
        return false;
    }
    return true;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page* spt_find_page(struct supplemental_page_table* spt, void* va)
{
    void* page_va = pg_round_down(va);
    struct page p;
    memset(&p, 0, sizeof p);
    p.va = page_va;

    struct hash_elem* e = hash_find(&spt->pages, &p.elem);
    if (e == NULL)
        return NULL;
    return hash_entry(e, struct page, elem);
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table* spt, struct page* page)
{
    if (hash_insert(&spt->pages, &page->elem))
        return false;
    return true;
}

void spt_remove_page(struct supplemental_page_table* spt, struct page* page)
{
    pml4_clear_page(thread_current()->pml4, page->va);
    hash_delete(&spt->pages, &page->elem); // user after free 방지(struct page pointer)
    vm_dealloc_page(page);
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
    void* kva = palloc_get_page(PAL_USER);
    if (kva == NULL)
        PANIC("todo"); // 나중에 eviction 구현 위치
    struct frame* frame = (struct frame*)malloc(sizeof(struct frame));
    if (frame == NULL) {
        palloc_free_page(kva);
        PANIC("todo");
    }
    frame->kva = kva;
    frame->page = NULL;

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
bool vm_try_handle_fault(struct intr_frame* f, void* addr, bool user, bool write, bool not_present)
{
    struct supplemental_page_table* spt = &thread_current()->spt;
    struct page* page = NULL;

    if (!not_present)
        return false;
    page = spt_find_page(spt, addr);
    if (page == NULL)
        return false;

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
bool vm_claim_page(void* va)
{
    struct supplemental_page_table* spt = &thread_current()->spt;
    struct page* page = spt_find_page(spt, va);
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
    if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)) {
        vm_release_frame(page);
        return false;
    }
    if (!swap_in(page, frame->kva)) {
        pml4_clear_page(thread_current()->pml4, page->va);
        vm_release_frame(page);
        return false;
    }
    return true;
}

static uint64_t hash_hash(const struct hash_elem* e, void* aux)
{
    struct page* page = hash_entry(e, struct page, elem);
    return hash_int(pg_no(page->va)); // 우측 비트 시프트 12번, 즉, vpn만 남김 그래서 pg_no
}

static bool hash_less(const struct hash_elem* a, const struct hash_elem* b, void* aux)
{    // find_elem에서 인자 순서만 바꿔서 두번 호출, 크지도 않고, 작지도 않으면 같다라는 성질 이용
    struct page* page_a = hash_entry(a, struct page, elem);
    struct page* page_b = hash_entry(b, struct page, elem);
    return page_a->va < page_b->va;
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table* spt)
{
    hash_init(&spt->pages, hash_hash, hash_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table* dst, struct supplemental_page_table* src)
{
    struct hash_iterator i;                                                  // 해시 순회용 이터레이터
    hash_first(&i, &src->pages);                                             // 부모 SPT 해시 순회 시작
    while (hash_next(&i)) {                                                  // 해시 테이블에 다음 엔트리가 있으면 반복
        struct page* src_page = hash_entry(hash_cur(&i), struct page, elem); // 현재 엔트리를 page로 복원
        enum vm_type type = src_page->operations->type; // 페이지 타입(VM_UNINIT, VM_ANON, VM_FILE 등)
        void* upage = src_page->va;                     // 사용자 가상주소
        bool writable = src_page->writable;             // 쓰기 가능 여부

        if (type == VM_UNINIT) {                          // 아직 로드 안 된 lazy 페이지라면
            vm_initializer* init = src_page->uninit.init; // lazy 로딩 함수 포인터
            void* aux = src_page->uninit.aux;             // 로딩에 필요한 추가 정보 포인터

            if (aux != NULL) { // aux 구조체는 새로 할당해 복사(공유 시 uninit_destroy에서 double free·상태 충돌)
                struct lazy_load_aux* src_aux = (struct lazy_load_aux*)aux;           // 부모 aux
                struct lazy_load_aux* dst_aux = malloc(sizeof(struct lazy_load_aux)); // 자식용 aux 할당
                if (dst_aux == NULL)                                                  // 메모리 부족 시 실패
                    return false;

                memcpy(dst_aux, src_aux, sizeof(struct lazy_load_aux));            // 구조체 내용 복사
                if (src_aux->file) {                                               // 파일 포인터가 있다면
                    dst_aux->file = file_duplicate(src_aux->file);                 // 같은 inode를 가리키는 새 핸들(파일 오프셋은 분리됨)
                    if (dst_aux->file == NULL) {
                        free(dst_aux);
                        return false;
                    }
                }
                aux = dst_aux; // 자식용 aux를 사용하도록 교체
            }

            if (!vm_alloc_page_with_initializer(src_page->uninit.type, upage, writable, init, aux)) // lazy 엔트리 등록
                return false;
        } else {                                       // lazy가 아닌 페이지(익명/파일/스왑 등)
            if (!vm_alloc_page(type, upage, writable)) // 자식 SPT에 엔트리 생성
                return false;

            if (!vm_claim_page(upage)) { // 프레임 즉시 할당/매핑
                struct page* dst_page = spt_find_page(dst, upage);
                if (dst_page)
                    spt_remove_page(dst, dst_page);
                return false;
            }

            struct page* dst_page = spt_find_page(dst, upage);              // 방금 만든 자식 페이지 찾기
            if (dst_page && src_page->frame) {                              // 부모·자식 모두 프레임이 있으면
                memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE); // 내용 한 페이지 복사
            }
        }
    }
    return true; // 전부 성공적으로 복사
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table* spt)
{
    /* TODO: Destroy all the supplemental_page_table hold by thread and
     * TODO: writeback all the modified contents to the storage. */
    hash_destroy(&spt->pages, page_destroy_func);
}

static void page_destroy_func(struct hash_elem* e, void* aux UNUSED)
{
    struct page* page = hash_entry(e, struct page, elem);
    pml4_clear_page(thread_current()->pml4, page->va);
    vm_dealloc_page(page);
}

// kva, frame 메모리 해제
void vm_release_frame(struct page* page)
{
    if (page && page->frame) {
        palloc_free_page(page->frame->kva);
        free(page->frame);
        page->frame = NULL;
    }
}

/* Free helper for lazy_load_aux when vm_alloc_page_with_initializer fails. */
static void free_lazy_aux(struct lazy_load_aux* aux)
{
    if (aux == NULL)
        return;
    if (aux->file)
        file_close(aux->file);
    free(aux);
}
