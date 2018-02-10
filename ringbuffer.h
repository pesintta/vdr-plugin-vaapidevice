/// Copyright (C) 2009, 2011 by Johns. All Rights Reserved.
/// Copyright (C) 2018 by pesintta, rofafor.
///
/// SPDX-License-Identifier: AGPL-3.0-only

    /// ring buffer typedef
typedef struct _ring_buffer_ RingBuffer;

    /// reset ring buffer pointers
extern void RingBufferReset(RingBuffer *);

    /// create new ring buffer
extern RingBuffer *RingBufferNew(size_t);

    /// free ring buffer
extern void RingBufferDel(RingBuffer *);

    /// write into ring buffer
extern size_t RingBufferWrite(RingBuffer *, const void *, size_t);

    /// get write pointer of ring buffer
extern size_t RingBufferGetWritePointer(RingBuffer *, void **);

    /// advance write pointer of ring buffer
extern size_t RingBufferWriteAdvance(RingBuffer *, size_t);

    /// read from ring buffer
extern size_t RingBufferRead(RingBuffer *, void *, size_t);

    /// get read pointer of ring buffer
extern size_t RingBufferGetReadPointer(RingBuffer *, const void **);

    /// advance read pointer of ring buffer
extern size_t RingBufferReadAdvance(RingBuffer *, size_t);

    /// free bytes ring buffer
extern size_t RingBufferFreeBytes(RingBuffer *);

    /// used bytes ring buffer
extern size_t RingBufferUsedBytes(RingBuffer *);
