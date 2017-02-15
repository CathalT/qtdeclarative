/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qv4engine_p.h"
#include "qv4object_p.h"
#include "qv4objectproto_p.h"
#include "qv4mm_p.h"
#include "qv4qobjectwrapper_p.h"
#include <QtCore/qalgorithms.h>
#include <QtCore/private/qnumeric_p.h>
#include <qqmlengine.h>
#include "PageReservation.h"
#include "PageAllocation.h"
#include "PageAllocationAligned.h"
#include "StdLibExtras.h"

#include <QElapsedTimer>
#include <QMap>
#include <QScopedValueRollback>

#include <iostream>
#include <cstdlib>
#include <algorithm>
#include "qv4alloca_p.h"
#include "qv4profiling_p.h"

#define MM_DEBUG 0

#if MM_DEBUG
#define DEBUG qDebug() << "MM:"
#else
#define DEBUG if (1) ; else qDebug() << "MM:"
#endif

#ifdef V4_USE_VALGRIND
#include <valgrind/valgrind.h>
#include <valgrind/memcheck.h>
#endif

#ifdef V4_USE_HEAPTRACK
#include <heaptrack_api.h>
#endif

#if OS(QNX)
#include <sys/storage.h>   // __tls()
#endif

#if USE(PTHREADS) && HAVE(PTHREAD_NP_H)
#include <pthread_np.h>
#endif

#define MIN_UNMANAGED_HEAPSIZE_GC_LIMIT std::size_t(128 * 1024)

using namespace WTF;

QT_BEGIN_NAMESPACE

namespace QV4 {

enum {
    MinSlotsGCLimit = QV4::Chunk::AvailableSlots*16,
    GCOverallocation = 200 /* Max overallocation by the GC in % */
};

struct MemorySegment {
    enum {
        NumChunks = 8*sizeof(quint64),
        SegmentSize = NumChunks*Chunk::ChunkSize,
    };

    MemorySegment(size_t size)
    {
        size += Chunk::ChunkSize; // make sure we can get enough 64k aligment memory
        if (size < SegmentSize)
            size = SegmentSize;

        pageReservation = PageReservation::reserve(size, OSAllocator::JSGCHeapPages);
        base = reinterpret_cast<Chunk *>((reinterpret_cast<quintptr>(pageReservation.base()) + Chunk::ChunkSize - 1) & ~(Chunk::ChunkSize - 1));
        nChunks = NumChunks;
        if (base != pageReservation.base())
            --nChunks;
    }
    MemorySegment(MemorySegment &&other) {
        qSwap(pageReservation, other.pageReservation);
        qSwap(base, other.base);
        qSwap(nChunks, other.nChunks);
        qSwap(allocatedMap, other.allocatedMap);
    }

    ~MemorySegment() {
        if (base)
            pageReservation.deallocate();
    }

    void setBit(size_t index) {
        Q_ASSERT(index < nChunks);
        quint64 bit = static_cast<quint64>(1) << index;
//        qDebug() << "    setBit" << hex << index << (index & (Bits - 1)) << bit;
        allocatedMap |= bit;
    }
    void clearBit(size_t index) {
        Q_ASSERT(index < nChunks);
        quint64 bit = static_cast<quint64>(1) << index;
//        qDebug() << "    setBit" << hex << index << (index & (Bits - 1)) << bit;
        allocatedMap &= ~bit;
    }
    bool testBit(size_t index) const {
        Q_ASSERT(index < nChunks);
        quint64 bit = static_cast<quint64>(1) << index;
        return (allocatedMap & bit);
    }

    Chunk *allocate(size_t size);
    void free(Chunk *chunk, size_t size) {
        DEBUG << "freeing chunk" << chunk;
        size_t index = static_cast<size_t>(chunk - base);
        size_t end = index + (size - 1)/Chunk::ChunkSize + 1;
        while (index < end) {
            Q_ASSERT(testBit(index));
            clearBit(index);
            ++index;
        }

        size_t pageSize = WTF::pageSize();
        size = (size + pageSize - 1) & ~(pageSize - 1);
        pageReservation.decommit(chunk, size);
    }

    bool contains(Chunk *c) const {
        return c >= base && c < base + nChunks;
    }

    PageReservation pageReservation;
    Chunk *base = 0;
    quint64 allocatedMap = 0;
    uint nChunks = 0;
};

Chunk *MemorySegment::allocate(size_t size)
{
    size_t requiredChunks = (size + sizeof(Chunk) - 1)/sizeof(Chunk);
    uint sequence = 0;
    Chunk *candidate = 0;
    for (uint i = 0; i < nChunks; ++i) {
        if (!testBit(i)) {
            if (!candidate)
                candidate = base + i;
            ++sequence;
        } else {
            candidate = 0;
            sequence = 0;
        }
        if (sequence == requiredChunks) {
            pageReservation.commit(candidate, size);
            for (uint i = 0; i < requiredChunks; ++i)
                setBit(candidate - base + i);
            DEBUG << "allocated chunk " << candidate << hex << size;
            return candidate;
        }
    }
    return 0;
}

struct ChunkAllocator {
    ChunkAllocator() {}

    size_t requiredChunkSize(size_t size) {
        size += Chunk::HeaderSize; // space required for the Chunk header
        size_t pageSize = WTF::pageSize();
        size = (size + pageSize - 1) & ~(pageSize - 1); // align to page sizes
        if (size < Chunk::ChunkSize)
            size = Chunk::ChunkSize;
        return size;
    }

    Chunk *allocate(size_t size = 0);
    void free(Chunk *chunk, size_t size = 0);

    std::vector<MemorySegment> memorySegments;
};

Chunk *ChunkAllocator::allocate(size_t size)
{
    size = requiredChunkSize(size);
    for (auto &m : memorySegments) {
        if (~m.allocatedMap) {
            Chunk *c = m.allocate(size);
            if (c)
                return c;
        }
    }

    // allocate a new segment
    memorySegments.push_back(MemorySegment(size));
    Chunk *c = memorySegments.back().allocate(size);
    Q_ASSERT(c);
    return c;
}

void ChunkAllocator::free(Chunk *chunk, size_t size)
{
    size = requiredChunkSize(size);
    for (auto &m : memorySegments) {
        if (m.contains(chunk)) {
            m.free(chunk, size);
            return;
        }
    }
    Q_ASSERT(false);
}


void Chunk::sweep()
{
    //    DEBUG << "sweeping chunk" << this << (*freeList);
    HeapItem *o = realBase();
    for (uint i = 0; i < Chunk::EntriesInBitmap; ++i) {
#if WRITEBARRIER(none)
        Q_ASSERT((grayBitmap[i] | blackBitmap[i]) == blackBitmap[i]); // check that we don't have gray only objects
#endif
        quintptr toFree = objectBitmap[i] ^ blackBitmap[i];
        Q_ASSERT((toFree & objectBitmap[i]) == toFree); // check all black objects are marked as being used
        quintptr e = extendsBitmap[i];
        //        DEBUG << hex << "   index=" << i << toFree;
        while (toFree) {
            uint index = qCountTrailingZeroBits(toFree);
            quintptr bit = (static_cast<quintptr>(1) << index);

            toFree ^= bit; // mask out freed slot
            //            DEBUG << "       index" << hex << index << toFree;

            // remove all extends slots that have been freed
            // this is a bit of bit trickery.
            quintptr mask = (bit << 1) - 1; // create a mask of 1's to the right of and up to the current bit
            quintptr objmask = e | mask; // or'ing mask with e gives all ones until the end of the current object
            quintptr result = objmask + 1;
            Q_ASSERT(qCountTrailingZeroBits(result) - index != 0); // ensure we freed something
            result |= mask; // ensure we don't clear stuff to the right of the current object
            e &= result;

            HeapItem *itemToFree = o + index;
            Heap::Base *b = *itemToFree;
            if (b->vtable()->destroy) {
                b->vtable()->destroy(b);
                b->_checkIsDestroyed();
            }
        }
        objectBitmap[i] = blackBitmap[i];
        grayBitmap[i] = 0;
        extendsBitmap[i] = e;
        o += Chunk::Bits;
    }
    //    DEBUG << "swept chunk" << this << "freed" << slotsFreed << "slots.";
}

void Chunk::freeAll()
{
    //    DEBUG << "sweeping chunk" << this << (*freeList);
    HeapItem *o = realBase();
    for (uint i = 0; i < Chunk::EntriesInBitmap; ++i) {
        quintptr toFree = objectBitmap[i];
        quintptr e = extendsBitmap[i];
        //        DEBUG << hex << "   index=" << i << toFree;
        while (toFree) {
            uint index = qCountTrailingZeroBits(toFree);
            quintptr bit = (static_cast<quintptr>(1) << index);

            toFree ^= bit; // mask out freed slot
            //            DEBUG << "       index" << hex << index << toFree;

            // remove all extends slots that have been freed
            // this is a bit of bit trickery.
            quintptr mask = (bit << 1) - 1; // create a mask of 1's to the right of and up to the current bit
            quintptr objmask = e | mask; // or'ing mask with e gives all ones until the end of the current object
            quintptr result = objmask + 1;
            Q_ASSERT(qCountTrailingZeroBits(result) - index != 0); // ensure we freed something
            result |= mask; // ensure we don't clear stuff to the right of the current object
            e &= result;

            HeapItem *itemToFree = o + index;
            Heap::Base *b = *itemToFree;
            if (b->vtable()->destroy) {
                b->vtable()->destroy(b);
                b->_checkIsDestroyed();
            }
        }
        objectBitmap[i] = 0;
        grayBitmap[i] = 0;
        extendsBitmap[i] = e;
        o += Chunk::Bits;
    }
    //    DEBUG << "swept chunk" << this << "freed" << slotsFreed << "slots.";
}

void Chunk::resetBlackBits()
{
    memset(blackBitmap, 0, sizeof(blackBitmap));
}

#ifndef QT_NO_DEBUG
static uint nGrayItems = 0;
#endif

void Chunk::collectGrayItems(ExecutionEngine *engine)
{
    //    DEBUG << "sweeping chunk" << this << (*freeList);
    HeapItem *o = realBase();
    for (uint i = 0; i < Chunk::EntriesInBitmap; ++i) {
#if WRITEBARRIER(none)
        Q_ASSERT((grayBitmap[i] | blackBitmap[i]) == blackBitmap[i]); // check that we don't have gray only objects
#endif
        quintptr toMark = blackBitmap[i] & grayBitmap[i]; // correct for a Steele type barrier
        Q_ASSERT((toMark & objectBitmap[i]) == toMark); // check all black objects are marked as being used
        //        DEBUG << hex << "   index=" << i << toFree;
        while (toMark) {
            uint index = qCountTrailingZeroBits(toMark);
            quintptr bit = (static_cast<quintptr>(1) << index);

            toMark ^= bit; // mask out marked slot
            //            DEBUG << "       index" << hex << index << toFree;

            HeapItem *itemToFree = o + index;
            Heap::Base *b = *itemToFree;
            Q_ASSERT(b->inUse());
            engine->pushForGC(b);
#ifndef QT_NO_DEBUG
            ++nGrayItems;
//            qDebug() << "adding gray item" << b << "to mark stack";
#endif
        }
        grayBitmap[i] = 0;
        o += Chunk::Bits;
    }
    //    DEBUG << "swept chunk" << this << "freed" << slotsFreed << "slots.";

}

void Chunk::sortIntoBins(HeapItem **bins, uint nBins)
{
//    qDebug() << "sortIntoBins:";
    HeapItem *base = realBase();
#if QT_POINTER_SIZE == 8
    const int start = 0;
#else
    const int start = 1;
#endif
#ifndef QT_NO_DEBUG
    uint freeSlots = 0;
    uint allocatedSlots = 0;
#endif
    for (int i = start; i < EntriesInBitmap; ++i) {
        quintptr usedSlots = (objectBitmap[i]|extendsBitmap[i]);
#if QT_POINTER_SIZE == 8
        if (!i)
            usedSlots |= (static_cast<quintptr>(1) << (HeaderSize/SlotSize)) - 1;
#endif
#ifndef QT_NO_DEBUG
        allocatedSlots += qPopulationCount(usedSlots);
//        qDebug() << hex << "   i=" << i << "used=" << usedSlots;
#endif
        while (1) {
            uint index = qCountTrailingZeroBits(usedSlots + 1);
            if (index == Bits)
                break;
            uint freeStart = i*Bits + index;
            usedSlots &= ~((static_cast<quintptr>(1) << index) - 1);
            while (!usedSlots) {
                ++i;
                if (i == EntriesInBitmap) {
                    usedSlots = (quintptr)-1;
                    break;
                }
                usedSlots = (objectBitmap[i]|extendsBitmap[i]);
#ifndef QT_NO_DEBUG
                allocatedSlots += qPopulationCount(usedSlots);
//                qDebug() << hex << "   i=" << i << "used=" << usedSlots;
#endif
            }
            HeapItem *freeItem = base + freeStart;

            index = qCountTrailingZeroBits(usedSlots);
            usedSlots |= (quintptr(1) << index) - 1;
            uint freeEnd = i*Bits + index;
            uint nSlots = freeEnd - freeStart;
#ifndef QT_NO_DEBUG
//            qDebug() << hex << "   got free slots from" << freeStart << "to" << freeEnd << "n=" << nSlots << "usedSlots=" << usedSlots;
            freeSlots += nSlots;
#endif
            Q_ASSERT(freeEnd > freeStart && freeEnd <= NumSlots);
            freeItem->freeData.availableSlots = nSlots;
            uint bin = qMin(nBins - 1, nSlots);
            freeItem->freeData.next = bins[bin];
            bins[bin] = freeItem;
        }
    }
#ifndef QT_NO_DEBUG
    Q_ASSERT(freeSlots + allocatedSlots == (EntriesInBitmap - start) * 8 * sizeof(quintptr));
#endif
}


template<typename T>
StackAllocator<T>::StackAllocator(ChunkAllocator *chunkAlloc)
    : chunkAllocator(chunkAlloc)
{
    chunks.push_back(chunkAllocator->allocate());
    firstInChunk = chunks.back()->first();
    nextFree = firstInChunk;
    lastInChunk = firstInChunk + (Chunk::AvailableSlots - 1)/requiredSlots*requiredSlots;
}

template<typename T>
void StackAllocator<T>::freeAll()
{
    for (auto c : chunks)
        chunkAllocator->free(c);
}

template<typename T>
void StackAllocator<T>::nextChunk() {
    Q_ASSERT(nextFree == lastInChunk);
    ++currentChunk;
    if (currentChunk >= chunks.size()) {
        Chunk *newChunk = chunkAllocator->allocate();
        chunks.push_back(newChunk);
    }
    firstInChunk = chunks.at(currentChunk)->first();
    nextFree = firstInChunk;
    lastInChunk = firstInChunk + (Chunk::AvailableSlots - 1)/requiredSlots*requiredSlots;
}

template<typename T>
void QV4::StackAllocator<T>::prevChunk() {
    Q_ASSERT(nextFree == firstInChunk);
    Q_ASSERT(chunks.at(currentChunk) == nextFree->chunk());
    Q_ASSERT(currentChunk > 0);
    --currentChunk;
    firstInChunk = chunks.at(currentChunk)->first();
    lastInChunk = firstInChunk + (Chunk::AvailableSlots - 1)/requiredSlots*requiredSlots;
    nextFree = lastInChunk;
}

template struct StackAllocator<Heap::CallContext>;


HeapItem *BlockAllocator::allocate(size_t size, bool forceAllocation) {
    Q_ASSERT((size % Chunk::SlotSize) == 0);
    size_t slotsRequired = size >> Chunk::SlotSizeShift;
#if MM_DEBUG
    ++allocations[bin];
#endif

    HeapItem **last;

    HeapItem *m;

    if (slotsRequired < NumBins - 1) {
        m = freeBins[slotsRequired];
        if (m) {
            freeBins[slotsRequired] = m->freeData.next;
            goto done;
        }
    }

    if (nFree >= slotsRequired) {
        // use bump allocation
        Q_ASSERT(nextFree);
        m = nextFree;
        nextFree += slotsRequired;
        nFree -= slotsRequired;
        goto done;
    }

    //        DEBUG << "No matching bin found for item" << size << bin;
    // search last bin for a large enough item
    last = &freeBins[NumBins - 1];
    while ((m = *last)) {
        if (m->freeData.availableSlots >= slotsRequired) {
            *last = m->freeData.next; // take it out of the list

            size_t remainingSlots = m->freeData.availableSlots - slotsRequired;
            //                DEBUG << "found large free slots of size" << m->freeData.availableSlots << m << "remaining" << remainingSlots;
            if (remainingSlots == 0)
                goto done;

            HeapItem *remainder = m + slotsRequired;
            if (remainingSlots > nFree) {
                if (nFree) {
                    size_t bin = binForSlots(nFree);
                    nextFree->freeData.next = freeBins[bin];
                    nextFree->freeData.availableSlots = nFree;
                    freeBins[bin] = nextFree;
                }
                nextFree = remainder;
                nFree = remainingSlots;
            } else {
                remainder->freeData.availableSlots = remainingSlots;
                size_t binForRemainder = binForSlots(remainingSlots);
                remainder->freeData.next = freeBins[binForRemainder];
                freeBins[binForRemainder] = remainder;
            }
            goto done;
        }
        last = &m->freeData.next;
    }

    if (slotsRequired < NumBins - 1) {
        // check if we can split up another slot
        for (size_t i = slotsRequired + 1; i < NumBins - 1; ++i) {
            m = freeBins[i];
            if (m) {
                freeBins[i] = m->freeData.next; // take it out of the list
//                qDebug() << "got item" << slotsRequired << "from slot" << i;
                size_t remainingSlots = i - slotsRequired;
                Q_ASSERT(remainingSlots < NumBins - 1);
                HeapItem *remainder = m + slotsRequired;
                remainder->freeData.availableSlots = remainingSlots;
                remainder->freeData.next = freeBins[remainingSlots];
                freeBins[remainingSlots] = remainder;
                goto done;
            }
        }
    }

    if (!m) {
        if (!forceAllocation)
            return 0;
        Chunk *newChunk = chunkAllocator->allocate();
        chunks.push_back(newChunk);
        nextFree = newChunk->first();
        nFree = Chunk::AvailableSlots;
        m = nextFree;
        nextFree += slotsRequired;
        nFree -= slotsRequired;
    }

done:
    m->setAllocatedSlots(slotsRequired);
    //        DEBUG << "   " << hex << m->chunk() << m->chunk()->objectBitmap[0] << m->chunk()->extendsBitmap[0] << (m - m->chunk()->realBase());
    return m;
}

void BlockAllocator::sweep()
{
    nextFree = 0;
    nFree = 0;
    memset(freeBins, 0, sizeof(freeBins));

//    qDebug() << "BlockAlloc: sweep";
    usedSlotsAfterLastSweep = 0;
    for (auto c : chunks) {
        c->sweep();
        c->sortIntoBins(freeBins, NumBins);
//        qDebug() << "used slots in chunk" << c << ":" << c->nUsedSlots();
        usedSlotsAfterLastSweep += c->nUsedSlots();
    }
}

void BlockAllocator::freeAll()
{
    for (auto c : chunks) {
        c->freeAll();
        chunkAllocator->free(c);
    }
}

void BlockAllocator::resetBlackBits()
{
    for (auto c : chunks)
        c->resetBlackBits();
}

void BlockAllocator::collectGrayItems(ExecutionEngine *engine)
{
    for (auto c : chunks)
        c->collectGrayItems(engine);

}

#if MM_DEBUG
void BlockAllocator::stats() {
    DEBUG << "MM stats:";
    QString s;
    for (int i = 0; i < 10; ++i) {
        uint c = 0;
        HeapItem *item = freeBins[i];
        while (item) {
            ++c;
            item = item->freeData.next;
        }
        s += QString::number(c) + QLatin1String(", ");
    }
    HeapItem *item = freeBins[NumBins - 1];
    uint c = 0;
    while (item) {
        ++c;
        item = item->freeData.next;
    }
    s += QLatin1String("..., ") + QString::number(c);
    DEBUG << "bins:" << s;
    QString a;
    for (int i = 0; i < 10; ++i)
        a += QString::number(allocations[i]) + QLatin1String(", ");
    a += QLatin1String("..., ") + QString::number(allocations[NumBins - 1]);
    DEBUG << "allocs:" << a;
    memset(allocations, 0, sizeof(allocations));
}
#endif


HeapItem *HugeItemAllocator::allocate(size_t size) {
    Chunk *c = chunkAllocator->allocate(size);
    chunks.push_back(HugeChunk{c, size});
    Chunk::setBit(c->objectBitmap, c->first() - c->realBase());
    return c->first();
}

static void freeHugeChunk(ChunkAllocator *chunkAllocator, const HugeItemAllocator::HugeChunk &c)
{
    HeapItem *itemToFree = c.chunk->first();
    Heap::Base *b = *itemToFree;
    if (b->vtable()->destroy) {
        b->vtable()->destroy(b);
        b->_checkIsDestroyed();
    }
    chunkAllocator->free(c.chunk, c.size);
}

void HugeItemAllocator::sweep() {
    auto isBlack = [this] (const HugeChunk &c) {
        bool b = c.chunk->first()->isBlack();
        if (!b)
            freeHugeChunk(chunkAllocator, c);
        return !b;
    };

    auto newEnd = std::remove_if(chunks.begin(), chunks.end(), isBlack);
    chunks.erase(newEnd, chunks.end());
}

void HugeItemAllocator::resetBlackBits()
{
    for (auto c : chunks)
        Chunk::clearBit(c.chunk->blackBitmap, c.chunk->first() - c.chunk->realBase());
}

void HugeItemAllocator::collectGrayItems(ExecutionEngine *engine)
{
    for (auto c : chunks)
        // Correct for a Steele type barrier
        if (Chunk::testBit(c.chunk->blackBitmap, c.chunk->first() - c.chunk->realBase()) &&
            Chunk::testBit(c.chunk->grayBitmap, c.chunk->first() - c.chunk->realBase())) {
            HeapItem *i = c.chunk->first();
            Heap::Base *b = *i;
            b->mark(engine);
        }
}

void HugeItemAllocator::freeAll()
{
    for (auto &c : chunks) {
        freeHugeChunk(chunkAllocator, c);
    }
}


MemoryManager::MemoryManager(ExecutionEngine *engine)
    : engine(engine)
    , chunkAllocator(new ChunkAllocator)
    , stackAllocator(chunkAllocator)
    , blockAllocator(chunkAllocator)
    , hugeItemAllocator(chunkAllocator)
    , m_persistentValues(new PersistentValueStorage(engine))
    , m_weakValues(new PersistentValueStorage(engine))
    , unmanagedHeapSizeGCLimit(MIN_UNMANAGED_HEAPSIZE_GC_LIMIT)
    , aggressiveGC(!qEnvironmentVariableIsEmpty("QV4_MM_AGGRESSIVE_GC"))
    , gcStats(!qEnvironmentVariableIsEmpty(QV4_MM_STATS))
{
#ifdef V4_USE_VALGRIND
    VALGRIND_CREATE_MEMPOOL(this, 0, true);
#endif
}

#ifndef QT_NO_DEBUG
static size_t lastAllocRequestedSlots = 0;
#endif

Heap::Base *MemoryManager::allocString(std::size_t unmanagedSize)
{
    const size_t stringSize = align(sizeof(Heap::String));
#ifndef QT_NO_DEBUG
    lastAllocRequestedSlots = stringSize >> Chunk::SlotSizeShift;
#endif

    bool didGCRun = false;
    if (aggressiveGC) {
        runGC();
        didGCRun = true;
    }

    unmanagedHeapSize += unmanagedSize;
    if (unmanagedHeapSize > unmanagedHeapSizeGCLimit) {
        if (!didGCRun)
            runGC();

        if (3*unmanagedHeapSizeGCLimit <= 4*unmanagedHeapSize)
            // more than 75% full, raise limit
            unmanagedHeapSizeGCLimit = std::max(unmanagedHeapSizeGCLimit, unmanagedHeapSize) * 2;
        else if (unmanagedHeapSize * 4 <= unmanagedHeapSizeGCLimit)
            // less than 25% full, lower limit
            unmanagedHeapSizeGCLimit = qMax(MIN_UNMANAGED_HEAPSIZE_GC_LIMIT, unmanagedHeapSizeGCLimit/2);
        didGCRun = true;
    }

    HeapItem *m = blockAllocator.allocate(stringSize);
    if (!m) {
        if (!didGCRun && shouldRunGC())
            runGC();
        m = blockAllocator.allocate(stringSize, true);
    }

//    qDebug() << "allocated string" << m;
    memset(m, 0, stringSize);
    return *m;
}

Heap::Base *MemoryManager::allocData(std::size_t size)
{
#ifndef QT_NO_DEBUG
    lastAllocRequestedSlots = size >> Chunk::SlotSizeShift;
#endif

    bool didRunGC = false;
    if (aggressiveGC) {
        runGC();
        didRunGC = true;
    }
#ifdef DETAILED_MM_STATS
    willAllocate(size);
#endif // DETAILED_MM_STATS

    Q_ASSERT(size >= Chunk::SlotSize);
    Q_ASSERT(size % Chunk::SlotSize == 0);

//    qDebug() << "unmanagedHeapSize:" << unmanagedHeapSize << "limit:" << unmanagedHeapSizeGCLimit << "unmanagedSize:" << unmanagedSize;

    if (size > Chunk::DataSize) {
        HeapItem *h = hugeItemAllocator.allocate(size);
//        qDebug() << "allocating huge item" << h;
        return *h;
    }

    HeapItem *m = blockAllocator.allocate(size);
    if (!m) {
        if (!didRunGC && shouldRunGC())
            runGC();
        m = blockAllocator.allocate(size, true);
    }

    memset(m, 0, size);
//    qDebug() << "allocating data" << m;
    return *m;
}

Heap::Object *MemoryManager::allocObjectWithMemberData(std::size_t size, uint nMembers)
{
    Heap::Object *o = static_cast<Heap::Object *>(allocData(size));

    // ### Could optimize this and allocate both in one go through the block allocator
    if (nMembers) {
        std::size_t memberSize = align(sizeof(Heap::MemberData) + (nMembers - 1)*sizeof(Value));
//        qDebug() << "allocating member data for" << o << nMembers << memberSize;
        Heap::Base *m;
        if (memberSize > Chunk::DataSize)
            m = *hugeItemAllocator.allocate(memberSize);
        else
            m = *blockAllocator.allocate(memberSize, true);
        memset(m, 0, memberSize);
        o->memberData.set(engine, static_cast<Heap::MemberData *>(m));
        o->memberData->setVtable(MemberData::staticVTable());
        o->memberData->values.alloc = static_cast<uint>((memberSize - sizeof(Heap::MemberData) + sizeof(Value))/sizeof(Value));
        o->memberData->values.size = o->memberData->values.alloc;
        o->memberData->init();
//        qDebug() << "    got" << o->memberData << o->memberData->size;
    }
//    qDebug() << "allocating object with memberData" << o << o->memberData.operator->();
    return o;
}

void MemoryManager::drainMarkStack(Value *markBase)
{
    while (engine->jsStackTop > markBase) {
        Heap::Base *h = engine->popForGC();
        Q_ASSERT(h); // at this point we should only have Heap::Base objects in this area on the stack. If not, weird things might happen.
        if (h->vtable()->markObjects)
            h->vtable()->markObjects(h, engine);
        if (quint64 m = h->vtable()->markTable) {
//            qDebug() << "using mark table:" << hex << m << "for" << h;
            void **mem = reinterpret_cast<void **>(h);
            while (m) {
                MarkFlags mark = static_cast<MarkFlags>(m & 3);
                switch (mark) {
                case Mark_NoMark:
                    break;
                case Mark_Value:
//                    qDebug() << "marking value at " << mem;
                    reinterpret_cast<Value *>(mem)->mark(engine);
                    break;
                case Mark_Pointer: {
//                    qDebug() << "marking pointer at " << mem;
                    Heap::Base *p = *reinterpret_cast<Heap::Base **>(mem);
                    if (p)
                        p->mark(engine);
                    break;
                }
                case Mark_ValueArray: {
                    Q_ASSERT(m == Mark_ValueArray);
//                    qDebug() << "marking Value Array at offset" << hex << (mem - reinterpret_cast<void **>(h));
                    ValueArray<0> *a = reinterpret_cast<ValueArray<0> *>(mem);
                    Value *v = a->values;
                    const Value *end = v + a->alloc;
                    while (v < end) {
                        v->mark(engine);
                        ++v;
                    }
                    break;
                }
                }

                m >>= 2;
                ++mem;
            }
        }
    }
}

void MemoryManager::mark()
{
    Value *markBase = engine->jsStackTop;

    if (nextGCIsIncremental) {
        // need to collect all gray items and push them onto the mark stack
        blockAllocator.collectGrayItems(engine);
        hugeItemAllocator.collectGrayItems(engine);
    }

    engine->markObjects(nextGCIsIncremental);

    collectFromJSStack();

    m_persistentValues->mark(engine);

    // Preserve QObject ownership rules within JavaScript: A parent with c++ ownership
    // keeps all of its children alive in JavaScript.

    // Do this _after_ collectFromStack to ensure that processing the weak
    // managed objects in the loop down there doesn't make then end up as leftovers
    // on the stack and thus always get collected.
    for (PersistentValueStorage::Iterator it = m_weakValues->begin(); it != m_weakValues->end(); ++it) {
        QObjectWrapper *qobjectWrapper = (*it).as<QObjectWrapper>();
        if (!qobjectWrapper)
            continue;
        QObject *qobject = qobjectWrapper->object();
        if (!qobject)
            continue;
        bool keepAlive = QQmlData::keepAliveDuringGarbageCollection(qobject);

        if (!keepAlive) {
            if (QObject *parent = qobject->parent()) {
                while (parent->parent())
                    parent = parent->parent();

                keepAlive = QQmlData::keepAliveDuringGarbageCollection(parent);
            }
        }

        if (keepAlive)
            qobjectWrapper->mark(engine);

        if (engine->jsStackTop >= engine->jsStackLimit)
            drainMarkStack(markBase);
    }

    drainMarkStack(markBase);
}

void MemoryManager::sweep(bool lastSweep)
{
    if (lastSweep && nextGCIsIncremental) {
        // ensure we properly clean up on destruction even if the GC is in incremental mode
        blockAllocator.resetBlackBits();
        hugeItemAllocator.resetBlackBits();
    }

    for (PersistentValueStorage::Iterator it = m_weakValues->begin(); it != m_weakValues->end(); ++it) {
        Managed *m = (*it).managed();
        if (!m || m->markBit())
            continue;
        // we need to call destroyObject on qobjectwrappers now, so that they can emit the destroyed
        // signal before we start sweeping the heap
        if (QObjectWrapper *qobjectWrapper = (*it).as<QObjectWrapper>())
            qobjectWrapper->destroyObject(lastSweep);

        (*it) = Primitive::undefinedValue();
    }

    // onDestruction handlers may have accessed other QObject wrappers and reset their value, so ensure
    // that they are all set to undefined.
    for (PersistentValueStorage::Iterator it = m_weakValues->begin(); it != m_weakValues->end(); ++it) {
        Managed *m = (*it).managed();
        if (!m || m->markBit())
            continue;
        (*it) = Primitive::undefinedValue();
    }

    // Now it is time to free QV4::QObjectWrapper Value, we must check the Value's tag to make sure its object has been destroyed
    const int pendingCount = m_pendingFreedObjectWrapperValue.count();
    if (pendingCount) {
        QVector<Value *> remainingWeakQObjectWrappers;
        remainingWeakQObjectWrappers.reserve(pendingCount);
        for (int i = 0; i < pendingCount; ++i) {
            Value *v = m_pendingFreedObjectWrapperValue.at(i);
            if (v->isUndefined() || v->isEmpty())
                PersistentValueStorage::free(v);
            else
                remainingWeakQObjectWrappers.append(v);
        }
        m_pendingFreedObjectWrapperValue = remainingWeakQObjectWrappers;
    }

    if (MultiplyWrappedQObjectMap *multiplyWrappedQObjects = engine->m_multiplyWrappedQObjects) {
        for (MultiplyWrappedQObjectMap::Iterator it = multiplyWrappedQObjects->begin(); it != multiplyWrappedQObjects->end();) {
            if (!it.value().isNullOrUndefined())
                it = multiplyWrappedQObjects->erase(it);
            else
                ++it;
        }
    }

    blockAllocator.sweep();
    hugeItemAllocator.sweep();
}

bool MemoryManager::shouldRunGC() const
{
    size_t total = blockAllocator.totalSlots();
    size_t usedSlots = blockAllocator.usedSlotsAfterLastSweep;
    if (total > MinSlotsGCLimit && usedSlots * GCOverallocation < total * 100)
        return true;
    return false;
}

size_t dumpBins(BlockAllocator *b, bool printOutput = true)
{
    size_t totalFragmentedSlots = 0;
    if (printOutput)
        qDebug() << "Fragmentation map:";
    for (uint i = 0; i < BlockAllocator::NumBins; ++i) {
        uint nEntries = 0;
        HeapItem *h = b->freeBins[i];
        while (h) {
            ++nEntries;
            totalFragmentedSlots += h->freeData.availableSlots;
            h = h->freeData.next;
        }
        if (printOutput)
            qDebug() << "    number of entries in slot" << i << ":" << nEntries;
    }
    if (printOutput)
        qDebug() << "  total mem in bins" << totalFragmentedSlots*Chunk::SlotSize;
    return totalFragmentedSlots*Chunk::SlotSize;
}

void MemoryManager::runGC(bool forceFullCollection)
{
    if (gcBlocked) {
//        qDebug() << "Not running GC.";
        return;
    }

    if (forceFullCollection) {
        // do a full GC
        blockAllocator.resetBlackBits();
        hugeItemAllocator.resetBlackBits();
        nextGCIsIncremental = false;
    }

    QScopedValueRollback<bool> gcBlocker(gcBlocked, true);
//    qDebug() << "runGC";

    if (!gcStats) {
//        uint oldUsed = allocator.usedMem();
        mark();
        sweep();
//        DEBUG << "RUN GC: allocated:" << allocator.allocatedMem() << "used before" << oldUsed << "used now" << allocator.usedMem();
    } else {
        bool triggeredByUnmanagedHeap = (unmanagedHeapSize > unmanagedHeapSizeGCLimit);
        size_t oldUnmanagedSize = unmanagedHeapSize;
        const size_t totalMem = getAllocatedMem();
        const size_t usedBefore = getUsedMem();
        const size_t largeItemsBefore = getLargeItemsMem();

        qDebug() << "========== GC ==========";
#ifndef QT_NO_DEBUG
        qDebug() << "    Triggered by alloc request of" << lastAllocRequestedSlots << "slots.";
#endif
        qDebug() << "Incremental:" << nextGCIsIncremental;
        qDebug() << "Allocated" << totalMem << "bytes in" << blockAllocator.chunks.size() << "chunks";
        qDebug() << "Fragmented memory before GC" << (totalMem - usedBefore);
        dumpBins(&blockAllocator);

#ifndef QT_NO_DEBUG
        nGrayItems = 0;
#endif

        QElapsedTimer t;
        t.start();
        mark();
        qint64 markTime = t.restart();
        sweep();
        const size_t usedAfter = getUsedMem();
        const size_t largeItemsAfter = getLargeItemsMem();
        qint64 sweepTime = t.elapsed();

        if (triggeredByUnmanagedHeap) {
            qDebug() << "triggered by unmanaged heap:";
            qDebug() << "   old unmanaged heap size:" << oldUnmanagedSize;
            qDebug() << "   new unmanaged heap:" << unmanagedHeapSize;
            qDebug() << "   unmanaged heap limit:" << unmanagedHeapSizeGCLimit;
        }
        size_t memInBins = dumpBins(&blockAllocator);
#ifndef QT_NO_DEBUG
        if (nextGCIsIncremental)
            qDebug() << "  number of gray items:" << nGrayItems;
#endif
        qDebug() << "Marked object in" << markTime << "ms.";
        qDebug() << "Sweeped object in" << sweepTime << "ms.";
        qDebug() << "Used memory before GC:" << usedBefore;
        qDebug() << "Used memory after GC:" << usedAfter;
        qDebug() << "Freed up bytes:" << (usedBefore - usedAfter);
        size_t lost = blockAllocator.allocatedMem() - memInBins - usedAfter;
        if (lost)
            qDebug() << "!!!!!!!!!!!!!!!!!!!!! LOST MEM:" << lost << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!";
        if (largeItemsBefore || largeItemsAfter) {
            qDebug() << "Large item memory before GC:" << largeItemsBefore;
            qDebug() << "Large item memory after GC:" << largeItemsAfter;
            qDebug() << "Large item memory freed up:" << (largeItemsBefore - largeItemsAfter);
        }
        qDebug() << "======== End GC ========";
    }

    if (aggressiveGC) {
        // ensure we don't 'loose' any memory
        Q_ASSERT(blockAllocator.allocatedMem() == getUsedMem() + dumpBins(&blockAllocator, false));
    }

#if WRITEBARRIER(steele)
    static int count = 0;
    ++count;
    if (aggressiveGC) {
        nextGCIsIncremental = (count % 256);
    } else {
        size_t total = blockAllocator.totalSlots();
        size_t usedSlots = blockAllocator.usedSlotsAfterLastSweep;
        if (!nextGCIsIncremental) {
            // always try an incremental GC after a full one, unless there is anyway lots of memory pressure
            nextGCIsIncremental = usedSlots * 4 < total * 3;
            count = 0;
        } else {
            if (count > 16)
                nextGCIsIncremental = false;
            else
                nextGCIsIncremental = usedSlots * 4 < total * 3; // less than 75% full
        }
    }
#else
    nextGCIsIncremental = false;
#endif
    if (!nextGCIsIncremental) {
        // do a full GC
        blockAllocator.resetBlackBits();
        hugeItemAllocator.resetBlackBits();
    }
}

size_t MemoryManager::getUsedMem() const
{
    return blockAllocator.usedMem();
}

size_t MemoryManager::getAllocatedMem() const
{
    return blockAllocator.allocatedMem() + hugeItemAllocator.usedMem();
}

size_t MemoryManager::getLargeItemsMem() const
{
    return hugeItemAllocator.usedMem();
}

MemoryManager::~MemoryManager()
{
    delete m_persistentValues;

    sweep(/*lastSweep*/true);
    blockAllocator.freeAll();
    hugeItemAllocator.freeAll();
    stackAllocator.freeAll();

    delete m_weakValues;
#ifdef V4_USE_VALGRIND
    VALGRIND_DESTROY_MEMPOOL(this);
#endif
    delete chunkAllocator;
}


void MemoryManager::dumpStats() const
{
#ifdef DETAILED_MM_STATS
    std::cerr << "=================" << std::endl;
    std::cerr << "Allocation stats:" << std::endl;
    std::cerr << "Requests for each chunk size:" << std::endl;
    for (int i = 0; i < allocSizeCounters.size(); ++i) {
        if (unsigned count = allocSizeCounters[i]) {
            std::cerr << "\t" << (i << 4) << " bytes chunks: " << count << std::endl;
        }
    }
#endif // DETAILED_MM_STATS
}

#ifdef DETAILED_MM_STATS
void MemoryManager::willAllocate(std::size_t size)
{
    unsigned alignedSize = (size + 15) >> 4;
    QVector<unsigned> &counters = allocSizeCounters;
    if ((unsigned) counters.size() < alignedSize + 1)
        counters.resize(alignedSize + 1);
    counters[alignedSize]++;
}

#endif // DETAILED_MM_STATS

void MemoryManager::collectFromJSStack() const
{
    Value *v = engine->jsStackBase;
    Value *top = engine->jsStackTop;
    while (v < top) {
        Managed *m = v->managed();
        if (m && m->inUse())
            // Skip pointers to already freed objects, they are bogus as well
            m->mark(engine);
        ++v;
    }
}

} // namespace QV4

QT_END_NAMESPACE
