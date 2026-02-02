#include <cstdio>
#include <cstdint>
#include <cstring>
#include <bit>
#include <new>

/*
Memory layout:
0...7 - bitmap
8 - free head
9 - init key
16...143 - queue table
144...2048 - 8 bytes blocks - 7 + 1 meta

Solution idea:
Custom allocator using a bitmap for queue slots (64 max) and a free list for data blocks 
(8-byte chunks: 7 payload + 1 meta/next).
Tail metadata approach - head and tail offsets are packed into the tail block's metadata byte,
keeping the queue handle size to just 2 bytes.
*/

using Byte = unsigned char;

struct QueueHeader {
    uint8_t head_idx;
    uint8_t tail_idx;
};

using Q = QueueHeader;

constexpr int MEMORY_SIZE = 2048;
constexpr int MAX_QUEUES = 64;

constexpr int ADDR_BITMAP = 0;
constexpr int ADDR_FREE_HEAD = 8;
constexpr int ADDR_INIT = 9;
constexpr int ADDR_QUEUE_TABLE = 16;
constexpr int ADDR_BLOCK_POOL = 144;

constexpr int BLOCK_SIZE = 8;
constexpr int BLOCK_PAYLOAD = 7;
constexpr int META_OFFSET = 7;
constexpr int NUM_BLOCKS = (MEMORY_SIZE - ADDR_BLOCK_POOL) / BLOCK_SIZE;

constexpr uint8_t NULL_IDX = 0xFF;
constexpr uint8_t INIT_VAL = 0xAA;

extern unsigned char data[MEMORY_SIZE];

inline uint8_t pack_offsets(uint8_t head_off, uint8_t tail_off) {
    return (head_off << 4) | (tail_off & 0x0F);
}

inline uint8_t unpack_head_off(uint8_t meta) {
    return (meta >> 4) & 0x0F;
}

inline uint8_t unpack_tail_off(uint8_t meta) {
    return meta & 0x0F;
}
    
inline Byte* get_block_ptr(uint8_t idx) {
    return &data[ADDR_BLOCK_POOL + idx * BLOCK_SIZE];
}

void heavy_initialization();

[[noreturn]] extern void on_out_of_memory();
[[noreturn]] extern void on_illegal_operation();

inline void init_system_if_needed() {
    if (data[ADDR_INIT] == INIT_VAL) [[likely]]
        return;

    heavy_initialization();
}

uint8_t alloc_block_or_die() {
    uint8_t head = data[ADDR_FREE_HEAD];
    if (head == NULL_IDX) {
        on_out_of_memory();
    }

    Byte* blk = get_block_ptr(head);
    uint8_t next = blk[META_OFFSET];
    data[ADDR_FREE_HEAD] = next;

    return head;
}

inline void validate_handle_or_die(Q* q) {
    if (data[ADDR_INIT] != INIT_VAL) {
        on_illegal_operation();
    }

    if (!q) on_illegal_operation();

    uintptr_t q_addr = reinterpret_cast<uintptr_t>(q);
    uintptr_t start_addr = reinterpret_cast<uintptr_t>(&data[ADDR_QUEUE_TABLE]);
    uintptr_t end_addr = start_addr + (MAX_QUEUES * sizeof(Q));

    if (q_addr < start_addr || q_addr >= end_addr || (q_addr - start_addr) % sizeof(Q) != 0) {
        on_illegal_operation();
    }

    uintptr_t offset = q_addr - start_addr;
    int slot_idx = static_cast<int>(offset / sizeof(Q));

    uint64_t bitmap;
    std::memcpy(&bitmap, &data[ADDR_BITMAP], sizeof(uint64_t));

    if ((bitmap & (1ULL << slot_idx)) == 0) {
        on_illegal_operation();
    }
}

void free_block(uint8_t idx) {
    Byte* blk = get_block_ptr(idx);
    blk[META_OFFSET] = data[ADDR_FREE_HEAD];
    data[ADDR_FREE_HEAD] = idx;
}

int find_free_queue_slot() {
    uint64_t bitmap;

    std::memcpy(&bitmap, &data[ADDR_BITMAP], sizeof(uint64_t));

    if (bitmap == static_cast<uint64_t>(-1)) return -1;

    return std::countr_one(bitmap);
}

Q* create_queue() {
    init_system_if_needed();

    int slot_idx = find_free_queue_slot();

    if (slot_idx < 0) {
        on_out_of_memory();
    }

    uint64_t bitmap;

    std::memcpy(&bitmap, &data[ADDR_BITMAP], sizeof(uint64_t));

    bitmap |= (1ULL << slot_idx);

    std::memcpy(&data[ADDR_BITMAP], &bitmap, sizeof(uint64_t));

    Q* q = new (&data[ADDR_QUEUE_TABLE + slot_idx * sizeof(Q)]) Q;

    q->head_idx = NULL_IDX;
    q->tail_idx = NULL_IDX;

    return q;
}

void heavy_initialization() {
    for (int i = 0; i < 8; ++i)
        data[ADDR_BITMAP + i] = 0;

    for (int i = 0; i < NUM_BLOCKS - 1; ++i) {
        // block[i].last_byte = i + 1
        Byte* blk = get_block_ptr(i);
        blk[META_OFFSET] = i + 1;
    }

    get_block_ptr(NUM_BLOCKS - 1)[META_OFFSET] = NULL_IDX;

    data[ADDR_FREE_HEAD] = 0;

    data[ADDR_INIT] = INIT_VAL;
}

void destroy_queue(Q* q) {
    validate_handle_or_die(q);

    uint8_t current = q->head_idx;
    uint8_t tail = q->tail_idx;

    while (current != NULL_IDX) {
        Byte* blk = get_block_ptr(current);
        uint8_t next = NULL_IDX;
        if (current != tail) {
            next = blk[META_OFFSET];
        }

        free_block(current);
        current = next;
    }

    q->head_idx = NULL_IDX;
    q->tail_idx = NULL_IDX;

    q->~Q();

    uintptr_t offset = reinterpret_cast<uintptr_t>(q) - reinterpret_cast<uintptr_t>(&data[ADDR_QUEUE_TABLE]);
    int slot_idx = static_cast<int>(offset / sizeof(Q));

    uint64_t bitmap;
    std::memcpy(&bitmap, &data[ADDR_BITMAP], sizeof(uint64_t));
    bitmap &= ~(1ULL << slot_idx);
    std::memcpy(&data[ADDR_BITMAP], &bitmap, sizeof(uint64_t));
}

void enqueue_byte(Q* q, unsigned char b) {
    validate_handle_or_die(q);

    if (q->head_idx == NULL_IDX) {
        uint8_t new_blk = alloc_block_or_die();

        Byte* blk_ptr = get_block_ptr(new_blk);
        blk_ptr[0] = b;
        blk_ptr[META_OFFSET] = pack_offsets(0, 1);
        q->head_idx = new_blk;
        q->tail_idx = new_blk;
        return;
    }

    uint8_t tail_idx = q->tail_idx;
    Byte* tail_ptr = get_block_ptr(tail_idx);
    uint8_t meta = tail_ptr[META_OFFSET];
    uint8_t head_off = unpack_head_off(meta);
    uint8_t tail_off = unpack_tail_off(meta);

    if (tail_off < BLOCK_PAYLOAD) {
        tail_ptr[tail_off] = b;
        tail_off++;
        tail_ptr[META_OFFSET] = pack_offsets(head_off, tail_off);
    }
    else {
        uint8_t new_blk = alloc_block_or_die();

        Byte* new_ptr = get_block_ptr(new_blk);
        tail_ptr[META_OFFSET] = new_blk;

        new_ptr[0] = b;
        new_ptr[META_OFFSET] = pack_offsets(head_off, 1);
        q->tail_idx = new_blk;
    }
}

unsigned char dequeue_byte(Q* q) {
    validate_handle_or_die(q);

    if (q->head_idx == NULL_IDX) {
        on_illegal_operation();
    }

    uint8_t head_idx = q->head_idx;
    uint8_t tail_idx = q->tail_idx;

    Byte* head_ptr = get_block_ptr(head_idx);
    Byte* tail_ptr = get_block_ptr(tail_idx);

    uint8_t meta = tail_ptr[META_OFFSET];
    uint8_t head_off = unpack_head_off(meta);
    uint8_t tail_off_val = unpack_tail_off(meta);

    unsigned char result = head_ptr[head_off];

    head_off++;

    if (head_off == BLOCK_PAYLOAD) {

        if (head_idx == tail_idx) {
            free_block(head_idx);
            q->head_idx = NULL_IDX;
            q->tail_idx = NULL_IDX;
        }
        else {
            uint8_t next_blk = head_ptr[META_OFFSET];

            free_block(head_idx);
            q->head_idx = next_blk;

            tail_ptr[META_OFFSET] = pack_offsets(0, tail_off_val);
        }
    }
    else {
        tail_ptr[META_OFFSET] = pack_offsets(head_off, tail_off_val);

        if (head_idx == tail_idx && head_off == tail_off_val) {
            free_block(head_idx);
            q->head_idx = NULL_IDX;
            q->tail_idx = NULL_IDX;
        }
    }

    return result;
}