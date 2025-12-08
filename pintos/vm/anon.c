/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "threads/synch.h"
#include "threads/thread.h"
#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/mmu.h"
#include "bitmap.h"
#include <stdint.h>
#define SECTOR_PER_PAGE 8
/* DO NOT MODIFY BELOW LINE */
static struct disk* swap_disk;
static bool anon_swap_in(struct page* page, void* kva);
static bool anon_swap_out(struct page* page);
static void anon_destroy(struct page* page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
    .swap_in = anon_swap_in,
    .swap_out = anon_swap_out,
    .destroy = anon_destroy,
    .type = VM_ANON,
};

static struct bitmap* swap_table;
static struct lock swap_lock;

/* Initialize the data for anonymous pages */
void vm_anon_init(void)
{
    /* TODO: Set up the swap_disk. */
    // slot 정의: A swap slot is a page-size region of disk space in the swap partition(VM introduction swap slots 첫 문장)
    swap_disk = disk_get(1, 1); // disk.c의 line 182: 1:1 - swap
    size_t total_slot_cnt = disk_size(swap_disk) / SECTOR_PER_PAGE; // 1slot = 8sectors(1sector = 512bytes)
    swap_table = bitmap_create(total_slot_cnt); // bitmap으로 swap table 관리
    lock_init(&swap_lock); // filesys_lock과 별개로 swap_lock 생성, disk가 다름, bitmap_ 함수 전용 lock(slot race condition 방지)
}

/* Initialize the file mapping */
bool anon_initializer(struct page* page, enum vm_type type, void* kva)
{
    /* Set up the handler */
    page->operations = &anon_ops;

    struct anon_page* anon_page = &page->anon;
    anon_page->slot_idx = SIZE_MAX; // slot index 설정(unsigned라서 -1 대신 SIZE_MAX 사용)
    return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool anon_swap_in(struct page* page, void* kva)
{
    struct anon_page* anon_page = &page->anon;
    size_t slot_idx = anon_page->slot_idx;
    if (slot_idx == SIZE_MAX)
        return false;
    // sector 단위로 읽기 때문에 8을 곱하고, sector_size만큼 버퍼 크기 증가, disk 함수는 lock 내부에서 동기화 처리, 따라서 별도 lock 불필요
    for (size_t i = 0; i < SECTOR_PER_PAGE; i++) 
        disk_read(swap_disk, slot_idx * SECTOR_PER_PAGE + i, (uint8_t*)kva + i * DISK_SECTOR_SIZE);
    // read 실패처리는 반환 값이 없으므로 따로 하지 않음
    // swap in을 했으니, 해당 slot을 비워줌, slot 접근은 lock 필요
    lock_acquire(&swap_lock);
    bitmap_reset(swap_table, slot_idx);
    lock_release(&swap_lock);
    anon_page->slot_idx = SIZE_MAX; // slot index 초기화
    return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool anon_swap_out(struct page* page)
{
    struct anon_page* anon_page = &page->anon;
    lock_acquire(&swap_lock);
    size_t slot_idx = bitmap_scan_and_flip(swap_table, 0, 1, false); // 빈 slot 찾기 및 할당
    lock_release(&swap_lock);
    if (slot_idx == SIZE_MAX)
        PANIC("swap disk is full");
    // sector 단위로 쓰기 때문에 8을 곱하고, sector_size만큼 버퍼 크기 증가, disk 함수는 lock 내부에서 동기화 처리, 따라서 별도 lock 불필요
    for (size_t i = 0; i < SECTOR_PER_PAGE; i++) 
        disk_write(swap_disk, slot_idx * SECTOR_PER_PAGE + i, (uint8_t*)page->frame->kva + i * DISK_SECTOR_SIZE);
    // write 실패처리는 반환 값이 없으므로 따로 하지 않음
    anon_page->slot_idx = slot_idx; // slot index 저장
    pml4_clear_page(page->accessible_thread->pml4, page->va); // 페이지 매핑 해제
    page->frame = NULL;
    return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy(struct page* page)
{
    struct anon_page* anon_page = &page->anon;
    struct thread* cur = thread_current();
    
    /* frame이 할당되어 있으면 해제 */
    if (page->frame != NULL)
    {
        /* pml4에서 매핑 해제 (pml4_destroy에서 double free 방지) */
        pml4_clear_page(cur->pml4, page->va);
        page->frame->page = NULL; // vm_free_frame의 ASSERT 통과용
        vm_free_frame(page->frame);
        page->frame = NULL;
    }

    if (anon_page->slot_idx != SIZE_MAX)
    {
        /* swap slot 해제 */
        lock_acquire(&swap_lock);
        bitmap_reset(swap_table, anon_page->slot_idx);
        lock_release(&swap_lock);
        anon_page->slot_idx = SIZE_MAX;
    }
}
