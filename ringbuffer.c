//////////////////////////////////////////////////////////////////////////////
///
/// Copyright (c) 2009, 2011, 2014  by Johns.  All Rights Reserved.
///
/// Contributor(s):
///
/// License: AGPLv3
///
/// This program is free software: you can redistribute it and/or modify
/// it under the terms of the GNU Affero General Public License as
/// published by the Free Software Foundation, either version 3 of the
/// License.
///
/// This program is distributed in the hope that it will be useful,
/// but WITHOUT ANY WARRANTY; without even the implied warranty of
/// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
/// GNU Affero General Public License for more details.
///
//////////////////////////////////////////////////////////////////////////////

///
/// @defgroup Ringbuffer The ring buffer module.
///
/// Lock free ring buffer with only one writer and one reader.
///

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iatomic.h"
#include "ringbuffer.h"

    /// ring buffer structure
struct _ring_buffer_
{
    char *Buffer;			///< ring buffer data
    const char *BufferEnd;		///< end of buffer
    size_t Size;			///< bytes in buffer (for faster calc)

    const char *ReadPointer;		///< only used by reader
    char *WritePointer;			///< only used by writer

    /// The only thing modified by both
    atomic_t Filled;			///< how many of the buffer is used
};

/**
**	Reset ring buffer pointers.
**
**	@param rb	Ring buffer to reset read/write pointers.
*/
void RingBufferReset(RingBuffer * rb)
{
    rb->ReadPointer = rb->Buffer;
    rb->WritePointer = rb->Buffer;
    atomic_set(&rb->Filled, 0);
}

/**
**	Allocate a new ring buffer.
**
**	@param size	Size of the ring buffer.
**
**	@returns	Allocated ring buffer, must be freed with
**			RingBufferDel(), NULL for out of memory.
*/
RingBuffer *RingBufferNew(size_t size)
{
    RingBuffer *rb;

    if (!(rb = malloc(sizeof(*rb)))) {	// allocate structure
	return rb;
    }
    if (!(rb->Buffer = malloc(size))) { // allocate buffer
	free(rb);
	return NULL;
    }

    rb->Size = size;
    rb->BufferEnd = rb->Buffer + size;
    RingBufferReset(rb);

    return rb;
}

/**
**	Free an allocated ring buffer.
*/
void RingBufferDel(RingBuffer * rb)
{
    free(rb->Buffer);
    free(rb);
}

/**
**	Advance write pointer in ring buffer.
**
**	@param rb	Ring buffer to advance write pointer.
**	@param cnt	Number of bytes to be adavanced.
**
**	@returns	Number of bytes that could be advanced in ring buffer.
*/
size_t RingBufferWriteAdvance(RingBuffer * rb, size_t cnt)
{
    size_t n;

    n = rb->Size - atomic_read(&rb->Filled);
    if (cnt > n) {			// not enough space
	cnt = n;
    }
    //
    //	Hitting end of buffer?
    //
    n = rb->BufferEnd - rb->WritePointer;
    if (n > cnt) {			// don't cross the end
	rb->WritePointer += cnt;
    } else {				// reached or cross the end
	rb->WritePointer = rb->Buffer;
	if (n < cnt) {
	    n = cnt - n;
	    rb->WritePointer += n;
	}
    }

    //
    //	Only atomic modification!
    //
    atomic_add(cnt, &rb->Filled);
    return cnt;
}

/**
**	Write to a ring buffer.
**
**	@param rb	Ring buffer to write to.
**	@param buf	Buffer of @p cnt bytes.
**	@param cnt	Number of bytes in buffer.
**
**	@returns	The number of bytes that could be placed in the ring
**			buffer.
*/
size_t RingBufferWrite(RingBuffer * rb, const void *buf, size_t cnt)
{
    size_t n;

    n = rb->Size - atomic_read(&rb->Filled);
    if (cnt > n) {			// not enough space
	cnt = n;
    }
    //
    //	Hitting end of buffer?
    //
    n = rb->BufferEnd - rb->WritePointer;
    if (n > cnt) {			// don't cross the end
	memcpy(rb->WritePointer, buf, cnt);
	rb->WritePointer += cnt;
    } else {				// reached or cross the end
	memcpy(rb->WritePointer, buf, n);
	rb->WritePointer = rb->Buffer;
	if (n < cnt) {
	    buf += n;
	    n = cnt - n;
	    memcpy(rb->WritePointer, buf, n);
	    rb->WritePointer += n;
	}
    }

    //
    //	Only atomic modification!
    //
    atomic_add(cnt, &rb->Filled);
    return cnt;
}

/**
**	Get write pointer and free bytes at this position of ring buffer.
**
**	@param rb	Ring buffer to write to.
**	@param[out] wp	Write pointer is placed here
**
**	@returns	The number of bytes that could be placed in the ring
**			buffer at the write pointer.
*/
size_t RingBufferGetWritePointer(RingBuffer * rb, void **wp)
{
    size_t n;
    size_t cnt;

    //	Total free bytes available in ring buffer
    cnt = rb->Size - atomic_read(&rb->Filled);

    *wp = rb->WritePointer;

    //
    //	Hitting end of buffer?
    //
    n = rb->BufferEnd - rb->WritePointer;
    if (n <= cnt) {			// reached or cross the end
	return n;
    }
    return cnt;
}

/**
**	Advance read pointer in ring buffer.
**
**	@param rb	Ring buffer to advance read pointer.
**	@param cnt	Number of bytes to be advanced.
**
**	@returns	Number of bytes that could be advanced in ring buffer.
*/
size_t RingBufferReadAdvance(RingBuffer * rb, size_t cnt)
{
    size_t n;

    n = atomic_read(&rb->Filled);
    if (cnt > n) {			// not enough filled
	cnt = n;
    }
    //
    //	Hitting end of buffer?
    //
    n = rb->BufferEnd - rb->ReadPointer;
    if (n > cnt) {			// don't cross the end
	rb->ReadPointer += cnt;
    } else {				// reached or cross the end
	rb->ReadPointer = rb->Buffer;
	if (n < cnt) {
	    n = cnt - n;
	    rb->ReadPointer += n;
	}
    }

    //
    //	Only atomic modification!
    //
    atomic_sub(cnt, &rb->Filled);
    return cnt;
}

/**
**	Read from a ring buffer.
**
**	@param rb	Ring buffer to read from.
**	@param buf	Buffer of @p cnt bytes.
**	@param cnt	Number of bytes to be read.
**
**	@returns	Number of bytes that could be read from ring buffer.
*/
size_t RingBufferRead(RingBuffer * rb, void *buf, size_t cnt)
{
    size_t n;

    n = atomic_read(&rb->Filled);
    if (cnt > n) {			// not enough filled
	cnt = n;
    }
    //
    //	Hitting end of buffer?
    //
    n = rb->BufferEnd - rb->ReadPointer;
    if (n > cnt) {			// don't cross the end
	memcpy(buf, rb->ReadPointer, cnt);
	rb->ReadPointer += cnt;
    } else {				// reached or cross the end
	memcpy(buf, rb->ReadPointer, n);
	rb->ReadPointer = rb->Buffer;
	if (n < cnt) {
	    buf += n;
	    n = cnt - n;
	    memcpy(buf, rb->ReadPointer, n);
	    rb->ReadPointer += n;
	}
    }

    //
    //	Only atomic modification!
    //
    atomic_sub(cnt, &rb->Filled);
    return cnt;
}

/**
**	Get read pointer and used bytes at this position of ring buffer.
**
**	@param rb	Ring buffer to read from.
**	@param[out] rp	Read pointer is placed here
**
**	@returns	The number of bytes that could be read from the ring
**			buffer at the read pointer.
*/
size_t RingBufferGetReadPointer(RingBuffer * rb, const void **rp)
{
    size_t n;
    size_t cnt;

    //	Total used bytes in ring buffer
    cnt = atomic_read(&rb->Filled);

    *rp = rb->ReadPointer;

    //
    //	Hitting end of buffer?
    //
    n = rb->BufferEnd - rb->ReadPointer;
    if (n <= cnt) {			// reached or cross the end
	return n;
    }
    return cnt;
}

/**
**	Get free bytes in ring buffer.
**
**	@param rb	Ring buffer.
**
**	@returns	Number of bytes free in buffer.
*/
size_t RingBufferFreeBytes(RingBuffer * rb)
{
    return rb->Size - atomic_read(&rb->Filled);
}

/**
**	Get used bytes in ring buffer.
**
**	@param rb	Ring buffer.
**
**	@returns	Number of bytes used in buffer.
*/
size_t RingBufferUsedBytes(RingBuffer * rb)
{
    return atomic_read(&rb->Filled);
}
