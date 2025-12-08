/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "userprog/process.h"
static bool file_backed_swap_in(struct page* page, void* kva);
static bool file_backed_swap_out(struct page* page);
static void file_backed_destroy(struct page* page);

#define PGSIZE 4096

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
    .swap_in = file_backed_swap_in,
    .swap_out = file_backed_swap_out,
    .destroy = file_backed_destroy,
    .type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void)
{
}

/* Initialize the file backed page */

static bool mmap_lazy_load_segment(struct page* page, void* aux)
{
    struct lazy_load_aux* new_aux = (struct lazy_load_aux*)aux;
    void* kva = page->frame->kva;
    if (file_read_at(new_aux->file, kva, new_aux->page_read_bytes, new_aux->ofs)) {
        memset(kva + new_aux->page_read_bytes, 0, new_aux->page_zero_bytes);
        return true;
    }
    return false;
}
bool file_backed_initializer(struct page* page, enum vm_type type, void* kva)
{
    /* Set up the handler */
    page->operations = &file_ops;

    struct file_page* file_page = &page->file;
    struct lazy_load_aux* aux = (struct lazy_load_aux*)page->uninit.aux;

    file_page->file = aux->file;
    file_page->offset = aux->ofs;
    file_page->length = aux->page_read_bytes;
    file_page->writable = page->writable;
    return true;
}

/* Swap in the page by read contents from the file. */
static bool file_backed_swap_in(struct page* page, void* kva)
{
    struct file_page* file_page UNUSED = &page->file;
    if (file_read_at(file_page->file, kva, file_page->length, file_page->offset) == (off_t)file_page->length) {
        memset(kva + file_page->length, 0, PGSIZE - file_page->length);
        return true;
    }
    return false;
}

/* Swap out the page by writeback contents to the file. */
static bool file_backed_swap_out(struct page* page)
{
    struct file_page* file_page UNUSED = &page->file;
    if (pml4_is_dirty(thread_current()->pml4, page->va)) {
        file_write_at(file_page->file, page->frame->kva, file_page->length, file_page->offset);
    }
    pml4_clear_page(thread_current()->pml4, page->va);
    page->frame = NULL;
    return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page* page)
{
    struct file_page* file_page UNUSED = &page->file;
    if (page->frame != NULL) {
        // enum intr_level old_level = intr_disable();
        if (pml4_is_dirty(thread_current()->pml4, page->va)) {
            file_write_at(file_page->file, page->frame->kva, file_page->length, file_page->offset);
            pml4_set_dirty(thread_current()->pml4, page->va, false);
        }
        pml4_clear_page(thread_current()->pml4, page->va);
        palloc_free_page(page->frame->kva);
        page->frame->page = NULL;
        free(page->frame);

        // intr_set_level(old_level);
    }
    return;
}
/* Do the mmap */
void* do_mmap(void* addr, size_t length, int writable, struct file* file, off_t offset)
{
    int num_pages = (length + PGSIZE - 1) / PGSIZE;
    struct file* new_file = file_reopen(file);
    for (int i = 0; i < num_pages; i++) {
        struct lazy_load_aux* aux = malloc(sizeof(struct lazy_load_aux));
        if (aux == NULL) {
            do_munmap(addr);
            return NULL;
        }
        aux->file = new_file;
        aux->ofs = offset;
        aux->page_read_bytes = length < PGSIZE ? length : PGSIZE;
        aux->page_zero_bytes = PGSIZE - aux->page_read_bytes;
        offset += aux->page_read_bytes;
        length -= aux->page_read_bytes;
        if (!vm_alloc_page_with_initializer(VM_FILE, addr + i * PGSIZE, writable, mmap_lazy_load_segment, aux)) {
            free(aux);
            do_munmap(addr);
            return NULL;
        }
    }
    return addr;
}

/* Do the munmap */
void do_munmap(void* addr)
{
    struct page* page = spt_find_page(&thread_current()->spt, addr);
    if (page == NULL || page_get_type(page) != VM_FILE)
        return;

    struct file* finding_file;
    if (page->operations->type == VM_UNINIT) {
        struct lazy_load_aux* aux = (struct lazy_load_aux*)page->uninit.aux;
        if (aux == NULL || aux->file == NULL)
            return;
        finding_file = aux->file;
    } else {
        if (page->file.file == NULL)
            return;
        finding_file = page->file.file;
    }
    struct file* current_file;
    while (page != NULL) {
        if (page_get_type(page) == VM_FILE) {
            if (page->operations->type == VM_UNINIT) {
                struct lazy_load_aux* aux = (struct lazy_load_aux*)page->uninit.aux;
                current_file = aux->file;
            } else {
                current_file = page->file.file;
            }
        } else {
            break;
        }
        if (current_file != finding_file) {
            break;
        }
        spt_remove_page(&thread_current()->spt, page);
        addr += PGSIZE;
        page = spt_find_page(&thread_current()->spt, addr);
    }
    file_close(finding_file);
    return;
}
