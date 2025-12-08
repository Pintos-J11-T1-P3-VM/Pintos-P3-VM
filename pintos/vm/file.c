/* file.c: Implementation of memory backed file object (mmaped object). */

#include "list.h"
#include "stddef.h"
#include "threads/mmu.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "userprog/syscall.h"
#include "threads/malloc.h"
#include "userprog/process.h"
#include "string.h"

static bool file_backed_swap_in(struct page* page, void* kva);
static bool file_backed_swap_out(struct page* page);
static void file_backed_destroy(struct page* page);

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
bool file_backed_initializer(struct page* page, enum vm_type type, void* kva)
{
    /* uninit에서 넘어온 aux 정보를 먼저 저장 (page->operations 변경 전에) */
    struct file_page* aux = (struct file_page*)page->uninit.aux;
    
    /* Set up the handler */
    page->operations = &file_ops;

    struct file_page* file_page = &page->file;
    file_page->file = aux->file;
    file_page->ofs = aux->ofs;
    file_page->page_read_bytes = aux->page_read_bytes;
    file_page->page_zero_bytes = aux->page_zero_bytes;
    free(aux);

    return true;
}

/* Swap in the page by read contents from the file. */
static bool file_backed_swap_in(struct page* page, void* kva)
{
    struct file_page* file_page = &page->file;
    
    lock_acquire(&filesys_lock);
    off_t read_bytes = file_read_at(file_page->file, kva, file_page->page_read_bytes, file_page->ofs);
    lock_release(&filesys_lock);

    if (read_bytes != (int)file_page->page_read_bytes)
        return false;
    
    memset(kva + file_page->page_read_bytes, 0, file_page->page_zero_bytes);
    return true;
}

/* Swap out the page by writeback contents to the file. */
static bool file_backed_swap_out(struct page* page)
{
    struct file_page* file_page = &page->file;
    struct thread* cur = thread_current();
    
    // dirty면 파일에 write back
    if (pml4_is_dirty(cur->pml4, page->va))
    {
        lock_acquire(&filesys_lock);
        file_write_at(file_page->file, page->frame->kva, file_page->page_read_bytes, file_page->ofs);
        lock_release(&filesys_lock);
        pml4_set_dirty(cur->pml4, page->va, false);
    }
    
    // pml4에서 매핑 해제
    pml4_clear_page(cur->pml4, page->va);
    page->frame = NULL;
    return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page* page)
{
    struct file_page* file_page = &page->file;
    struct thread* cur = thread_current();
    
    // 로드된 상태이고 dirty면 파일에 write back
    if (page->frame != NULL && pml4_is_dirty(cur->pml4, page->va))
    {
        lock_acquire(&filesys_lock);
        file_write_at(file_page->file, page->frame->kva, file_page->page_read_bytes, file_page->ofs);
        lock_release(&filesys_lock);
    }
    
    // pml4에서 매핑 해제
    if (page->frame != NULL)
        pml4_clear_page(cur->pml4, page->va);
    
    // frame 해제
    if (page->frame != NULL)
    {
        palloc_free_page(page->frame->kva);
        free(page->frame);
        page->frame = NULL;
    }
}

/* Do the mmap */
void* do_mmap(void* addr, size_t length, int writable, struct file* file, off_t offset)
{
    struct thread* cur = thread_current();

    // 1. file_reopen으로 독립적인 파일 참조 생성
    lock_acquire(&filesys_lock);
    struct file* reopened_file = file_reopen(file);
    if (reopened_file == NULL) {
        lock_release(&filesys_lock);
        return NULL;
    }
    size_t file_len = file_length(reopened_file);
    /* offset이 파일 끝을 넘거나 파일 길이가 0이면 실패 */
    if (file_len == 0 || (off_t)file_len <= offset) {
        file_close(reopened_file);
        lock_release(&filesys_lock);
        return NULL;
    }
    size_t available_len = file_len - offset;
    /* 실제 매핑할 길이는 요청 길이만큼 (파일보다 길면 0으로 채움) */
    size_t map_length = length;
    lock_release(&filesys_lock);

    // 2. 페이지 수 계산
    size_t page_cnt = map_length / PGSIZE + (map_length % PGSIZE != 0 ? 1 : 0);

    // 3. overlap 검사: 기존 페이지와 겹치면 실패
    for (size_t i = 0; i < page_cnt; i++)
    {
        void* va = addr + (i * PGSIZE);
        if (spt_find_page(&cur->spt, va) != NULL)
        {
            lock_acquire(&filesys_lock);
            file_close(reopened_file);
            lock_release(&filesys_lock);
            return NULL;
        }
    }

    // 4. mmap_data 생성
    struct mmap_data* m_data = malloc(sizeof(struct mmap_data));
    if (m_data == NULL) {
        lock_acquire(&filesys_lock);
        file_close(reopened_file);
        lock_release(&filesys_lock);
        return NULL;
    }

    m_data->addr = addr;
    m_data->page_cnt = page_cnt;
    m_data->file = reopened_file;

    // 5. 페이지 할당 루프 (lazy loading)
    size_t remain_bytes = available_len > length ? length : available_len;
    // remain_bytes는 파일에서 읽을 수 있는 유효 길이 (요청 길이와 파일 잔여 길이 중 작은 값)
    
    size_t allocated_pages = 0;

    for (size_t i = 0; i < page_cnt; i++)
    {
        void* va = addr + (i * PGSIZE);
        size_t read_bytes = remain_bytes < PGSIZE ? remain_bytes : PGSIZE;
        size_t zero_bytes = PGSIZE - read_bytes;

        struct file_page* aux = malloc(sizeof(struct file_page));
        if (aux == NULL) {
            goto mmap_fail;
        }

        aux->file = reopened_file;
        aux->ofs = offset + (i * PGSIZE);
        aux->page_read_bytes = read_bytes;
        aux->page_zero_bytes = zero_bytes;

        if (!vm_alloc_page_with_initializer(VM_FILE, va, writable, file_backed_initializer, aux))
        {
            free(aux);
            goto mmap_fail;
        }

        allocated_pages++;
        if (remain_bytes >= PGSIZE)
             remain_bytes -= PGSIZE;
        else
             remain_bytes = 0;
    }
    goto mmap_success;

mmap_fail:
    /* 이전에 할당된 페이지들 정리 */
    for (size_t i = 0; i < allocated_pages; i++)
    {
        void* va = addr + (i * PGSIZE);
        struct page* page = spt_find_page(&cur->spt, va);
        if (page != NULL)
            spt_remove_page(&cur->spt, page);
    }
    free(m_data);
    lock_acquire(&filesys_lock);
    file_close(reopened_file);
    lock_release(&filesys_lock);
    return NULL;

mmap_success:
    // 6. mmap_list에 추가 후 성공 반환
    list_push_back(&cur->mmap_list, &m_data->mmap_elem);
    return addr;
}

/* Do the munmap */
void do_munmap(void* addr)
{
    struct thread* cur = thread_current();
    struct mmap_data* m_data = NULL;

    // 1. mmap_list에서 addr로 mmap_data 찾기
    struct list_elem* e;
    for (e = list_begin(&cur->mmap_list); e != list_end(&cur->mmap_list); e = list_next(e))
    {
        struct mmap_data* md = list_entry(e, struct mmap_data, mmap_elem);
        if (md->addr == addr)
        {
            m_data = md;
            break;
        }
    }

    if (m_data == NULL)
        return;

    // 2. 페이지 해제 루프
    for (size_t i = 0; i < m_data->page_cnt; i++)
    {
        void* va = addr + (i * PGSIZE);
        struct page* page = spt_find_page(&cur->spt, va);

        if (page == NULL)
            continue;

        // 페이지가 로드된 상태이고 dirty면 파일에 write back
        if (page->frame != NULL && pml4_is_dirty(cur->pml4, va))
        {
            struct file_page* file_page = &page->file;
            lock_acquire(&filesys_lock);
            file_write_at(m_data->file, page->frame->kva, file_page->page_read_bytes, file_page->ofs);
            lock_release(&filesys_lock);
            pml4_set_dirty(cur->pml4, va, false);
        }

        // SPT에서 제거 (내부에서 destroy 호출됨)
        spt_remove_page(&cur->spt, page);
    }

    // 3. file_close로 reopen한 파일 닫기
    lock_acquire(&filesys_lock);
    file_close(m_data->file);
    lock_release(&filesys_lock);

    // 4. mmap_list에서 제거 후 free
    list_remove(&m_data->mmap_elem);
    free(m_data);
}
