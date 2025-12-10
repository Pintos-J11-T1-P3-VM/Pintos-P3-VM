/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "lib/kernel/bitmap.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

#define PGSIZE 4096
#define SECTORS_PER_PAGE (PGSIZE / DISK_SECTOR_SIZE)
/* DO NOT MODIFY BELOW LINE */
static struct disk* swap_disk;
static bool anon_swap_in(struct page* page, void* kva);
static bool anon_swap_out(struct page* page);
static void anon_destroy(struct page* page);

static struct bitmap* swap_table;
static struct lock swap_lock;

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
    .swap_in = anon_swap_in,
    .swap_out = anon_swap_out,
    .destroy = anon_destroy,
    .type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void vm_anon_init(void)
{
    /* TODO: Set up the swap_disk. */
    swap_disk = disk_get(1, 1);
    disk_sector_t swap_size = disk_size(swap_disk);
    swap_table = bitmap_create(swap_size);
    lock_init(&swap_lock);
}

/* Initialize the file mapping */
bool anon_initializer(struct page* page, enum vm_type type, void* kva)
{
    /* Set up the handler */
    page->operations = &anon_ops;

    struct anon_page* anon_page = &page->anon;
    anon_page->swap_slot = -1;
    return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool anon_swap_in(struct page* page, void* kva)
{
    struct anon_page* anon_page = &page->anon;

    disk_sector_t slot = anon_page->swap_slot;

    if (slot == -1)
        return true;

    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        disk_read(swap_disk, slot + i, kva + i * DISK_SECTOR_SIZE);
    }
    lock_acquire(&swap_lock);
    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        bitmap_reset(swap_table, slot + i);
    }
    lock_release(&swap_lock);
    anon_page->swap_slot = -1;

    return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool anon_swap_out(struct page* page)
{
    struct anon_page* anon_page = &page->anon;
    lock_acquire(&swap_lock);
    disk_sector_t slot = bitmap_scan_and_flip(swap_table, 0, SECTORS_PER_PAGE, false); // find continuous
    lock_release(&swap_lock);
    if (slot == BITMAP_ERROR) {
        return false;
    }
    void* kva = page->frame->kva;
    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        disk_write(swap_disk, slot + i, kva + i * DISK_SECTOR_SIZE);
    }
    anon_page->swap_slot = slot;
    pml4_clear_page(thread_current()->pml4, page->va);
    page->frame = NULL;
    return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy(struct page* page)
{
    struct anon_page* anon_page = &page->anon;

    if (anon_page->swap_slot != -1) {
        lock_acquire(&swap_lock);
        for (int i = 0; i < SECTORS_PER_PAGE; i++) {
            bitmap_reset(swap_table, anon_page->swap_slot + i);
        }
        lock_release(&swap_lock);
    }

    if (page->frame != NULL) {
        struct thread* curr = thread_current();
        if (pml4_get_page(curr->pml4, page->va) != NULL) {
            pml4_clear_page(curr->pml4, page->va);
            page->frame->page = NULL;
        }
        list_remove(&page->frame->elem);
        palloc_free_page(page->frame->kva);
        free(page->frame);
    }
}