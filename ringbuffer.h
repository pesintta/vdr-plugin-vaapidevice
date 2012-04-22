///
///	@file ringbuffer.h	@brief Ringbuffer module header file
///
///	Copyright (c) 2009, 2011 by Johns.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
///	$Id$
//////////////////////////////////////////////////////////////////////////////

/// @addtogroup Ringbuffer
/// @{

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

/// @}
