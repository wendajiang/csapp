/*
 * mm-segregated.c
 * 
 * implement segregated fits on CSAPP 3e textbook p900
 * 
 * segregated free list
 * - min_block_size: 32
 * - 16 size class, (0,32],(32, 64],(64, 128],...,(2^(i+4), 2^(i+5)],...,(2^19, +inf)
 * - circle explicit free list for each size class
 * - first fit
 * - LIFO (free to the header of size class)
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memlib.h"
#include "mm.h"

/* do not change the following! */
#ifdef DRIVER
/* create aliases for driver tests */
#define malloc mm_malloc
#define free mm_free
#define realloc mm_realloc
#define calloc mm_calloc
#endif /* def DRIVER */

/* Basic constants */
typedef uint32_t word_t;
static const size_t chunksize = (1 << 12);     // requires (chunksize % 8 == 0)
static const size_t wsize = sizeof(word_t);    // word and header size (bytes)
static const size_t dsize = 2 * wsize;         // double word size (bytes)
static const size_t tsize = 3 * wsize;         // triple word size (bytes)
static const size_t ptr_size = sizeof(char *); // pointer size (bytes)
static const size_t size_class_cnt = 16;       // how many size class in segregated list

/*
 * Minimum block size, (header + footer + 2 * pointer + struct inner alignment padding)
 */
static const size_t min_block_size = 2 * wsize + 2 * ptr_size + 2 * wsize;
/**
 * Prologue block size, also is `sizeof(block_t)`
 */
static const size_t prologue_size = min_block_size;
/*
 * Epilogue block size, header
 */
static const size_t epilogue_size = dsize;

static const word_t alloc_mask = 0x1;
static const word_t size_mask = ~(word_t)0x7;

typedef struct block {
    /* Header contains size + allocation flag */
    word_t header;

    /* dummy word to make payload aligned at 8-multiple */
    word_t _dummy1;

    /*
     * We don't know how big the payload will be.
     * Declaring it as an array of size 0 allows computing its starting address using
     * pointer notation.
     */
    char payload[0];

    /*
     * explicit free list pointer
     * pred: predecessor, point to the previous free block in explicit free list
     * succ: successor, point to the next free block in explicit free list
     */
    struct block *pred;
    struct block *succ;

    /* dummy word to make footer aligned at the end word of block */
    word_t _dummy2;

    /*
     * We can't declare the footer as part of the struct, since its starting
     * position is unknown
     */
    // word_t footer; // CANNOT uncomment this line
} block_t;

/**
 * size of block
 */
static const size_t block_size = sizeof(block_t);

/* Global variables */
/* Pointer to first block */
static block_t *heap_listp = NULL;
/* Pointer to segregated free list, of `size_class_cnt` length */
static block_t *seglist = NULL;

/* Function prototypes for internal helper routines */
static block_t *extend_heap(size_t size);
static void place(block_t *block, size_t asize);
static block_t *find_fit(size_t asize);
static block_t *coalesce(block_t *block);

static size_t max(size_t x, size_t y);
static size_t round_up(size_t size, size_t n);
static word_t pack(size_t size, bool alloc);

static size_t extract_size(word_t header);
static size_t get_size(block_t *block);
static size_t get_payload_size(block_t *block);

static bool extract_alloc(word_t header);
static bool get_alloc(block_t *block);

static void write_header(block_t *block, size_t size, bool alloc);
static void write_footer(block_t *block, size_t size, bool alloc);

static block_t *payload_to_header(void *pp);
static void *header_to_payload(block_t *block);

static block_t *find_next(block_t *block);
static word_t *find_prev_footer(block_t *block);
static block_t *find_prev(block_t *block);

static void insert_free_block(block_t *bp);
static void remove_from_free_list(block_t *bp);

static size_t get_asize(size_t size);
static void split_block(block_t *bp, size_t asize);

static word_t *get_header(block_t *block);
static word_t *get_footer(block_t *block);

static uint32_t get_seglist_idx(size_t size);
static uint32_t get_highest_1bit_idx(uint32_t num);

static void dbg_print_heap(bool skip);
#ifdef DEBUG
#define print_heap(...) dbg_print_heap(false)
#else
#define print_heap(...) dbg_print_heap(true)
#endif

/*
 * mm_init
 * initialize the heap, it is run once when heap_start == NULL.
 * 
 * prior to any extend_heap operation, this is the heap:
 * start               start+32              start+32*2  start+32*15               start+32*16      start+32*16+32    start+32*16+32+8
 *   | size class (0, 32] | size class (32, 64] | ..., .... | size class (2^19, +inf] | prologue block | epilogue header |
 * 
 * each size class is of type `block_t`, but only `pred`, `succ` matters, because of circle list for each explicit list
 * 
 * prologue block and epilogue header is mainly for coalesce simplicity
 */
int mm_init(void) {
    // Create the initial empty heap
    block_t *start = (block_t *)(mem_sbrk(size_class_cnt * block_size + prologue_size + epilogue_size));

    if (start == (void *)-1)
        return -1;

    /**
     * Initialize the explicit free list of each size class bucket
     * pred and succ point to the belonging size class bucket initially
     */
    for (size_t i = 0; i < size_class_cnt; i++) {
        write_header(&(start[i]), block_size, true);
        write_footer(&(start[i]), block_size, true);
        start[i].pred = &(start[i]);
        start[i].succ = &(start[i]);
    }
    /**
     * set seglist to the very beginning of heap, 
     * which makes seglist an array of `block_t[size_class_cnt]`
     */
    seglist = start;

    // prologue starts immediately after seglist
    block_t *prologue = &(start[size_class_cnt]);
    write_header(prologue, prologue_size, true);
    write_footer(prologue, prologue_size, true);

    // epilogue starts immediately after prologue
    block_t *epilogue = (block_t *)((char *)prologue + prologue_size);
    write_header(epilogue, 0, true);

    // heap starts with prologue
    heap_listp = prologue;

    if (extend_heap(chunksize) == NULL)
        return -1;

    return 0;
}

/*
 * malloc
 * 
 * quote from textbook:
 *      To allocate a block, we determine the size class of the request 
 *      and do a ﬁrst ﬁt search of the appropriate free list for a block that ﬁts. 
 *      If we ﬁnd one, then we (optionally) split it and insert the fragment in the appropriate free list. 
 *      If we cannot ﬁnd a block that ﬁts, then we search the free list for the next larger size class.
 *      We repeat until we ﬁnd a block that ﬁts. If none of the free lists yields a block that ﬁts, 
 *      then we request additional heap memory from the operating system, 
 *      allocate the block out of this new heap memory, 
 *      and place the remainder in the appropriate size class.
 */
void *malloc(size_t size) {
    dbg_requires(mm_checkheap(__LINE__));

    size_t asize;      // Adjusted block size
    size_t extendsize; // Amount to extend heap if no fit is found
    block_t *block;
    void *bp = NULL;

    // Ignore spurious request
    if (size == 0) {
        dbg_ensures(mm_checkheap(__LINE__));
        return bp;
    }

    // Initialize heap if it isn't initialized
    if (heap_listp == NULL) {
        mm_init();
    }

    asize = get_asize(size);
    // Search the free list for a fit
    block = find_fit(asize);

    // If no fit is found, request more memory, and then and place the block
    if (block == NULL) {
        extendsize = max(asize, chunksize);
        block = extend_heap(extendsize);
        // extend_heap returns an error
        if (block == NULL) {
            return bp;
        }
    }

    place(block, asize);
    bp = header_to_payload(block);

    dbg_printf("Malloc size %zd on address %p, with adjusted size %zd.\n", size, bp, asize);
    print_heap();
    dbg_ensures(mm_checkheap(__LINE__));
    return bp;
}

/*
 * free
 * 
 * quote from textbook:
 *      To free a block, we coalesce and place the result on the appropriate free list.
 */
void free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    dbg_printf("free %p\n", ptr);

    block_t *block = payload_to_header(ptr);
    size_t size = get_size(block);

    write_header(block, size, false);
    write_footer(block, size, false);

    coalesce(block);
}

/*
 * realloc
 */
void *realloc(void *oldptr, size_t size) {
    if (size == 0) {
        free(oldptr);
        return NULL;
    }

    if (oldptr == NULL) {
        return malloc(size);
    }

    block_t *old_block = payload_to_header(oldptr);
    size_t oldsize = get_size(old_block);

    size_t asize = get_asize(size);

    // enough space to realloc, just shrunk old space, and free any unused space
    if (oldsize >= asize) {
        // unused space is able to meet another malloc request, so put it into free list
        if ((oldsize - asize) >= min_block_size) {
            split_block(old_block, asize);
        }
        return oldptr;
    }
    // old space is too small, need to alloc another space
    else {
        void *newptr;
        if ((newptr = malloc(size)) == NULL)
            return NULL;

        memcpy(newptr, oldptr, get_payload_size(old_block));
        free(oldptr);

        return newptr;
    }
}

/*
 * calloc
 */
void *calloc(size_t nmemb, size_t size) {
    size_t bytes = nmemb * size;
    void *newptr;

    if ((newptr = malloc(bytes)) != NULL)
        memset(newptr, 0, bytes);
    return newptr;
}

/******** The remaining content below are helper and debug routines ********/

/*
 * extend_heap: Extends the heap with the requested number of bytes, and
 *              recreates epilogue header. Returns a pointer to the result of
 *              coalescing the newly-created block with previous free block, if
 *              applicable, or NULL in failure.
 */
static block_t *extend_heap(size_t size) {
    void *bp;

    // Allocate an even number of words to maintain alignment
    size = round_up(size, dsize);
    if ((bp = mem_sbrk(size)) == (void *)-1) {
        return NULL;
    }

    block_t *old_epi = (block_t *)(((char *)bp) - epilogue_size);
    // Initialize free block header/footer at the position of old epilogue
    write_header(old_epi, size, false);
    write_footer(old_epi, size, false);

    // Create new epilogue header
    block_t *new_epi = find_next(old_epi);
    write_header(new_epi, 0, true);

    // Coalesce in case the previous block was free
    return coalesce(old_epi);
}

/* Coalesce: Coalesces current block with previous and next blocks if either
 *           or both are unallocated; otherwise the block is not modified.
 *           Returns pointer to the coalesced block. After coalescing, the
 *           immediate contiguous previous and next blocks must be allocated.
 * 
 * need to maintain the explicit free list, remove the coalesced free block if necessary
 * and insert the newly free block
 */
static block_t *coalesce(block_t *block) {
    block_t *block_next = find_next(block);
    block_t *block_prev = find_prev(block);

    bool prev_alloc = get_alloc(block_prev);
    bool next_alloc = get_alloc(block_next);
    size_t size = get_size(block);

    // Case 1
    if (prev_alloc && next_alloc) {
    }
    // Case 2
    else if (prev_alloc && !next_alloc) {
        size += get_size(block_next);
        write_header(block, size, false);
        write_footer(block, size, false);

        remove_from_free_list(block_next);
    }
    // Case 3
    else if (!prev_alloc && next_alloc) {
        size += get_size(block_prev);
        write_header(block_prev, size, false);
        write_footer(block_prev, size, false);
        block = block_prev;

        remove_from_free_list(block_prev);
    }
    // Case 4
    else {
        size += get_size(block_next) + get_size(block_prev);
        write_header(block_prev, size, false);
        write_footer(block_prev, size, false);
        block = block_prev;

        remove_from_free_list(block_prev);
        remove_from_free_list(block_next);
    }

    insert_free_block(block);

    dbg_printf("After coalesce\n");
    print_heap();

    return block;
}

/*
 * place: Places block with size of asize at the start of bp. If the remaining
 *        size is at least the minimum block size, then split the block to the
 *        the allocated block and the remaining block as free.
 * 
 * need to maintain explicit free list, remove this allocated block,
 * and insert the remaining free block if necessary
 */
static void place(block_t *block, size_t asize) {
    size_t csize = get_size(block);

    remove_from_free_list(block);

    if ((csize - asize) >= min_block_size) {
        split_block(block, asize);
    } else {
        write_header(block, csize, true);
        write_footer(block, csize, true);
    }
}

/*
 * find_fit: Looks for a free block with at least asize bytes with
 *           first-fit policy. Returns NULL if none is found.
 */
static block_t *find_fit(size_t asize) {
    // determine the size class of the request `asize`
    uint32_t idx = get_seglist_idx(asize);

    block_t *block;
    // repeat until we find a block that fits
    while (idx < size_class_cnt) {
        // check each free block in this size class
        for (block = seglist[idx].succ;
             // not until loop back to the header of this size class
             block != &(seglist[idx]);
             block = block->succ) {
            // only need to check size, since all blocks here are free
            if (asize <= get_size(block)) {
                return block;
            }
        }
        idx++;
    }

    // or none of the free lists yields a block that fits
    return NULL;
}

/*
 * max: returns x if x > y, and y otherwise.
 */
static size_t max(size_t x, size_t y) {
    return (x > y) ? x : y;
}

/*
 * round_up: Rounds size up to next multiple of n
 */
static size_t round_up(size_t size, size_t n) {
    return (n * ((size + (n - 1)) / n));
}

/*
 * pack: returns a header reflecting a specified size and its alloc status.
 *       If the block is allocated, the lowest bit is set to 1, and 0 otherwise.
 */
static word_t pack(size_t size, bool alloc) {
    return alloc ? (size | 1) : size;
}

/*
 * extract_size: returns the size of a given header value based on the header
 *               specification above.
 */
static size_t extract_size(word_t word) {
    return (word & size_mask);
}

/*
 * get_size: returns the size of a given block by clearing the lowest 3 bits
 *           (as the heap is 8-byte aligned).
 */
static size_t get_size(block_t *block) {
    return extract_size(block->header);
}

/*
 * get_payload_size: returns the payload size of a given block, equal to
 *                   the entire block size minus the header and footer sizes.
 * 
 * because of the dummy word to meet alignment requirement, header size is 2 * wsize
 */
static size_t get_payload_size(block_t *block) {
    size_t asize = get_size(block);
    return asize - tsize;
}

/*
 * extract_alloc: returns the allocation status of a given header value based
 *                on the header specification above.
 */
static bool extract_alloc(word_t word) {
    return (bool)(word & alloc_mask);
}

/*
 * get_alloc: returns true when the block is allocated based on the
 *            block header's lowest bit, and false otherwise.
 */
static bool get_alloc(block_t *block) {
    return extract_alloc(block->header);
}

/*
 * write_header: given a block and its size and allocation status,
 *               writes an appropriate value to the block header.
 */
static void write_header(block_t *block, size_t size, bool alloc) {
    block->header = pack(size, alloc);
}

/*
 * write_footer: given a block and its size and allocation status,
 *               writes an appropriate value to the block footer by first
 *               computing the position of the footer.
 */
static void write_footer(block_t *block, size_t size, bool alloc) {
    word_t *footer_ptr = get_footer(block);
    *footer_ptr = pack(size, alloc);
}

/*
 * get_header: return the block header address
 */
static word_t *get_header(block_t *block) {
    return &(block->header);
}

/*
 * get_footer: return the block footer address
 */
static word_t *get_footer(block_t *block) {
    return (word_t *)((block->payload) + get_payload_size(block));
}

/*
 * find_next: returns the next consecutive block on the heap by adding the
 *            size of the block.
 */
static block_t *find_next(block_t *block) {
    dbg_requires(block != NULL);
    block_t *block_next = (block_t *)(((char *)block) + get_size(block));

    dbg_ensures(block_next != NULL);
    return block_next;
}

/*
 * find_prev_footer: returns the footer of the previous block.
 */
static word_t *find_prev_footer(block_t *block) {
    // Compute previous footer position as one word before the header
    return (&(block->header)) - 1;
}

/*
 * find_prev: returns the previous block position by checking the previous
 *            block's footer and calculating the start of the previous block
 *            based on its size.
 */
static block_t *find_prev(block_t *block) {
    word_t *footer_ptr = find_prev_footer(block);
    size_t size = extract_size(*footer_ptr);
    return (block_t *)((char *)block - size);
}

/*
 * payload_to_header: given a payload pointer, returns a pointer to the
 *                    corresponding block.
 */
static block_t *payload_to_header(void *pp) {
    return (block_t *)(((char *)pp) - offsetof(block_t, payload));
}

/*
 * header_to_payload: given a block pointer, returns a pointer to the
 *                    corresponding payload.
 */
static void *header_to_payload(block_t *block) {
    return (void *)(block->payload);
}

/*
 * Insert new alloced block to the corresponding free list, LIFO style
 */
static void insert_free_block(block_t *bp) {
    // determine the size class of the request
    uint32_t idx = get_seglist_idx(get_size(bp));
    /**
     * pred point to the size class bucket
     * 
     * it is LIFO style: 
     *  put the newly freed block to the head of the explicit free list
     */
    bp->pred = &(seglist[idx]);
    bp->succ = seglist[idx].succ;
    bp->pred->succ = bp;
    bp->succ->pred = bp;
}

/*
 * Remove this block from free list
 */
static void remove_from_free_list(block_t *bp) {
    bp->pred->succ = bp->succ;
    bp->succ->pred = bp->pred;
}

/*
 * Adjust block size to include overhead and to meet alignment requirements
 */
static size_t get_asize(size_t size) {
    return max(round_up(size + tsize, dsize), min_block_size);
}

/**
 * Split block by `asize`
 * 
 * the first splitted block is allocated, 
 * the second one is free,
 * need to maintain the explicit free list: insert the second one to free list
 */
static void split_block(block_t *bp, size_t asize) {
    size_t csize = get_size(bp);
    write_header(bp, asize, true);
    write_footer(bp, asize, true);

    block_t *block_next = find_next(bp);
    write_header(block_next, csize - asize, false);
    write_footer(block_next, csize - asize, false);
    insert_free_block(block_next);
}

/**
 * calculate the corresponding index of a size class that `size` can fit in its size range
 */
static uint32_t get_seglist_idx(size_t size) {
    uint32_t highest_1bit_idx = get_highest_1bit_idx(size);

    // special case for each size class's upper boundary
    if (size == (size_t)(1 << (highest_1bit_idx - 1)))
        highest_1bit_idx--;

    uint32_t idx;
    // first size class
    if (highest_1bit_idx <= 5)
        idx = 0;
    // last size class
    else if (highest_1bit_idx >= 20)
        idx = 15;
    // other normal size classes
    else
        idx = highest_1bit_idx - 5;

    // make sure: 0 <= idx < 16
    dbg_ensures(idx < 16);
    return idx;
}

/**
 * calculate the index of highest `1` in the bit pattern of `num`
 */
static uint32_t get_highest_1bit_idx(uint32_t num) {
    dbg_assert(num > 0);
    uint32_t idx = 0;
    do {
        num >>= 1;
        idx++;
    } while (num > 0);
    return idx;
}

/*
 * mm_checkheap
 */
bool mm_checkheap(int lineno) {
    if (!heap_listp) {
        printf("NULL heap list pointer!\n");
        return false;
    }

    // check header and footer consistency
    block_t *curr = heap_listp;
    block_t *next;
    block_t *hi = mem_heap_hi();
    while ((next = find_next(curr)) + 1 < hi) {
        word_t hdr = curr->header;
        word_t ftr = *find_prev_footer(next);

        if (hdr != ftr) {
            printf("Header (0x%08X) at %p != footer (0x%08X) at %p, lineno: %d\n", hdr, get_header(curr), ftr, get_footer(curr), lineno);
            return false;
        }

        curr = next;
    }

    return true;
}

/*
 * print heap in continuous style, block by block
 * commonly show block position, header, footer
 * 
 * - for *allocated* block, show payload size
 * - for *free* block, show predecessor pointer, successor pointer
 */
static void dbg_print_heap(bool skip) {
    if (skip)
        return;

    block_t *curr = heap_listp;
    block_t *next;
    while (get_size(curr) > 0) {
        next = find_next(curr);

        word_t hdr = curr->header;
        word_t ftr = *find_prev_footer(next);
        if (get_alloc(curr)) {
            printf("@%p->[h:%zd/%s|psize:%zd|f:%zd/%s] ", curr, extract_size(hdr), extract_alloc(hdr) ? "a" : "f",
                   get_payload_size(curr), extract_size(ftr), extract_alloc(ftr) ? "a" : "f");
        } else {
            printf("@%p->[h:%zd/%s|pred:%p,succ:%p|f:%zd/%s] ", curr, extract_size(hdr), extract_alloc(hdr) ? "a" : "f",
                   curr->pred, curr->succ, extract_size(ftr), extract_alloc(ftr) ? "a" : "f");
        }

        curr = next;
    }
    printf("\n");
}
