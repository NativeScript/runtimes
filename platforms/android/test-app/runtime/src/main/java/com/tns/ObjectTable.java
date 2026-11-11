package com.tns;

import java.lang.ref.WeakReference;

final class ObjectTable {
    static final int NO_ID = -1;

    static final long FRESH = 1L << 32;

    interface IdSource {
        int newId();
    }

    private final Stamps stamps;

    ObjectTable() {
        this(false);
    }

    ObjectTable(boolean optimisticReads) {
        stamps = optimisticReads ? new Stamps() : null;
    }

    private static final class Stamps {
        private final java.util.concurrent.locks.StampedLock lock = new java.util.concurrent.locks.StampedLock();

        long beginWrite() {
            return lock.writeLock();
        }

        void endWrite(long stamp) {
            lock.unlockWrite(stamp);
        }

        long beginRead() {
            return lock.tryOptimisticRead();
        }

        long readLock() {
            return lock.readLock();
        }

        void unlockRead(long stamp) {
            lock.unlockRead(stamp);
        }

        boolean validate(long stamp) {
            return lock.validate(stamp);
        }
    }

    private int findIdUnlocked(Object obj) {
        int[] index = identityIndex;
        int[] hashes = identityIndexHashes;
        int mask = index.length - 1;
        int hash = System.identityHashCode(obj);
        int i = mix(hash) & mask;
        for (int probes = 0; probes <= mask; probes++) {
            int entry = index[i];
            if (entry == 0) {
                return NO_ID;
            }
            if (hashes[i] == hash && getUnlocked(entry - 1) == obj) {
                return entry - 1;
            }
            i = (i + 1) & mask;
        }
        return NO_ID;
    }

    private Object getUnlocked(int id) {
        if (id == NO_ID) {
            return null;
        }
        Chunk[] cs = chunks;
        int c = id >>> CHUNK_SHIFT;
        if (c >= cs.length) {
            return null;
        }
        Chunk chunk = cs[c];
        if (chunk == null) {
            return null;
        }
        Leaf leaf = chunk.leaves[(id >>> LEAF_BITS) & CHUNK_MASK];
        if (leaf == null) {
            return null;
        }
        Object value = leaf.values[id & LEAF_MASK];
        return value == null ? null : deref(value);
    }

    private static Object deref(Object value) {
        return value instanceof WeakRef ? ((WeakRef) value).get() : value;
    }

    private static final int LEAF_BITS = 6;
    private static final int LEAF_SIZE = 1 << LEAF_BITS;
    private static final int LEAF_MASK = LEAF_SIZE - 1;
    private static final int CHUNK_BITS = 6;
    private static final int CHUNK_MASK = (1 << CHUNK_BITS) - 1;
    private static final int CHUNK_SHIFT = LEAF_BITS + CHUNK_BITS;

    private static final class Leaf {
        final Object[] values = new Object[LEAF_SIZE];
        final int[] hashes = new int[LEAF_SIZE];
        int live;
    }

    private static final class Chunk {
        final Leaf[] leaves = new Leaf[1 << CHUNK_BITS];
        int liveLeaves;
    }

    private Chunk[] chunks = new Chunk[16];
    private int newestLeaf = 0;
    private int idCount = 0;

    private int[] identityIndex = new int[1024];
    private int[] identityIndexHashes = new int[1024];
    private int identityCount = 0;

    void putStrong(int id, Object obj) {
        if (stamps == null) {
            putStrongLocked(id, obj);
            return;
        }
        long stamp = stamps.beginWrite();
        try {
            putStrongLocked(id, obj);
        } finally {
            stamps.endWrite(stamp);
        }
    }

    private void putStrongLocked(int id, Object obj) {
        putStrongLocked(id, obj, System.identityHashCode(obj));
    }

    private void putStrongLocked(int id, Object obj, int hash) {
        Leaf existingLeaf = leafFor(id);
        if (existingLeaf != null) {
            Object value = existingLeaf.values[id & LEAF_MASK];
            if (value != null) {
                if (deref(value) == obj) {
                    existingLeaf.values[id & LEAF_MASK] = obj;
                    return;
                }
                removeLocked(id);
            }
        }

        insertEntry(id, obj, hash);
        idCount++;

        if ((identityCount + 1) * 2 > identityIndex.length) {
            growIdentity();
        }
        insertIdentity(hash, id);
        identityCount++;
    }

    boolean allowsConcurrentRegistration() {
        return stamps != null;
    }

    long findOrRegister(Object obj, IdSource ids) {
        long stamp = stamps.beginWrite();
        try {
            int hash = System.identityHashCode(obj);
            int id = findIdLocked(obj, hash);
            if (id != NO_ID) {
                return id & 0xFFFFFFFFL;
            }
            id = ids.newId();
            putStrongLocked(id, obj, hash);
            return (id & 0xFFFFFFFFL) | FRESH;
        } finally {
            stamps.endWrite(stamp);
        }
    }

    int findId(Object obj) {
        if (stamps == null) {
            return findIdLocked(obj);
        }
        long stamp = stamps.beginRead();
        if (stamp != 0) {
            try {
                int id = findIdUnlocked(obj);
                if (stamps.validate(stamp)) {
                    return id;
                }
            } catch (RuntimeException e) {
            }
        }
        stamp = stamps.readLock();
        try {
            return findIdLocked(obj);
        } finally {
            stamps.unlockRead(stamp);
        }
    }

    private int findIdLocked(Object obj) {
        return findIdLocked(obj, System.identityHashCode(obj));
    }

    private int findIdLocked(Object obj, int hash) {
        int mask = identityIndex.length - 1;
        int i = mix(hash) & mask;
        while (true) {
            int entry = identityIndex[i];
            if (entry == 0) {
                return NO_ID;
            }
            int id = entry - 1;
            if (identityIndexHashes[i] == hash && getLocked(id) == obj) {
                return id;
            }
            i = (i + 1) & mask;
        }
    }

    Object get(int id) {
        if (stamps == null) {
            return getLocked(id);
        }
        long stamp = stamps.beginRead();
        if (stamp != 0) {
            try {
                Object value = getUnlocked(id);
                if (stamps.validate(stamp)) {
                    return value;
                }
            } catch (RuntimeException e) {
            }
        }
        stamp = stamps.readLock();
        try {
            return getLocked(id);
        } finally {
            stamps.unlockRead(stamp);
        }
    }

    private Object getLocked(int id) {
        Object value = rawLocked(id);
        return value == null ? null : deref(value);
    }

    boolean contains(int id) {
        if (stamps == null) {
            return rawLocked(id) != null;
        }
        long stamp = stamps.readLock();
        try {
            return rawLocked(id) != null;
        } finally {
            stamps.unlockRead(stamp);
        }
    }

    boolean weakenAndCheckAlive(int id) {
        if (stamps == null) {
            return weakenAndCheckAliveLocked(id);
        }
        long stamp = stamps.beginWrite();
        try {
            return weakenAndCheckAliveLocked(id);
        } finally {
            stamps.endWrite(stamp);
        }
    }

    void weakenAndCheckAlive(int[] ids, int count) {
        if (count == 0) {
            return;
        }
        long stamp = stamps == null ? 0 : stamps.beginWrite();
        try {
            for (int i = 0; i < count; i++) {
                if (!weakenAndCheckAliveLocked(ids[i])) {
                    ids[i] = NO_ID;
                }
            }
        } finally {
            if (stamps != null) {
                stamps.endWrite(stamp);
            }
        }
    }

    private boolean weakenAndCheckAliveLocked(int id) {
        Leaf leaf = leafFor(id);
        Object value = leaf == null ? null : leaf.values[id & LEAF_MASK];
        if (value == null) {
            return false;
        }
        if (value instanceof WeakRef) {
            if (((WeakRef) value).get() != null) {
                return true;
            }
            removeLocked(id);
            return false;
        }
        leaf.values[id & LEAF_MASK] = new WeakRef(value);
        return true;
    }

    void remove(int id) {
        if (stamps == null) {
            removeLocked(id);
            return;
        }
        long stamp = stamps.beginWrite();
        try {
            removeLocked(id);
        } finally {
            stamps.endWrite(stamp);
        }
    }

    private void removeLocked(int id) {
        Leaf leaf = leafFor(id);
        int i = id & LEAF_MASK;
        if (leaf == null || leaf.values[i] == null) {
            return;
        }
        int hash = leaf.hashes[i];
        leaf.values[i] = null;
        leaf.hashes[i] = 0;
        leaf.live--;
        int leafNumber = id >>> LEAF_BITS;
        if (leafNumber != newestLeaf) {
            releaseIfEmpty(leafNumber);
        }
        idCount--;
        removeIdentity(hash, id);
        identityCount--;
    }

    int size() {
        return idCount;
    }

    private static final class WeakRef extends WeakReference<Object> {
        WeakRef(Object referent) {
            super(referent);
        }
    }

    private Leaf leafFor(int id) {
        if (id == NO_ID) {
            return null;
        }
        int c = id >>> CHUNK_SHIFT;
        Chunk chunk = c < chunks.length ? chunks[c] : null;
        return chunk == null ? null : chunk.leaves[(id >>> LEAF_BITS) & CHUNK_MASK];
    }

    private Object rawLocked(int id) {
        Leaf leaf = leafFor(id);
        return leaf == null ? null : leaf.values[id & LEAF_MASK];
    }

    private void insertEntry(int id, Object value, int hash) {
        int c = id >>> CHUNK_SHIFT;
        if (c >= chunks.length) {
            int length = chunks.length;
            while (length <= c) {
                length *= 2;
            }
            Chunk[] grown = new Chunk[length];
            System.arraycopy(chunks, 0, grown, 0, chunks.length);
            chunks = grown;
        }
        Chunk chunk = chunks[c];
        if (chunk == null) {
            chunk = new Chunk();
            chunks[c] = chunk;
        }
        int l = (id >>> LEAF_BITS) & CHUNK_MASK;
        Leaf leaf = chunk.leaves[l];
        if (leaf == null) {
            leaf = new Leaf();
            chunk.leaves[l] = leaf;
            chunk.liveLeaves++;
        }
        int i = id & LEAF_MASK;
        leaf.values[i] = value;
        leaf.hashes[i] = hash;
        leaf.live++;
        int leafNumber = id >>> LEAF_BITS;
        if (leafNumber > newestLeaf) {
            int previous = newestLeaf;
            newestLeaf = leafNumber;
            releaseIfEmpty(previous);
        }
    }

    private void releaseIfEmpty(int leafNumber) {
        int c = leafNumber >>> CHUNK_BITS;
        Chunk chunk = c < chunks.length ? chunks[c] : null;
        if (chunk == null) {
            return;
        }
        int l = leafNumber & CHUNK_MASK;
        Leaf leaf = chunk.leaves[l];
        if (leaf == null || leaf.live != 0) {
            return;
        }
        chunk.leaves[l] = null;
        if (--chunk.liveLeaves == 0 && c != (newestLeaf >>> CHUNK_BITS)) {
            chunks[c] = null;
        }
    }

    private void insertIdentity(int hash, int id) {
        insertIdentity(identityIndex, identityIndexHashes, hash, id);
    }

    private static void insertIdentity(int[] index, int[] hashes, int hash, int id) {
        int mask = index.length - 1;
        int i = mix(hash) & mask;
        while (index[i] != 0) {
            i = (i + 1) & mask;
        }
        hashes[i] = hash;
        index[i] = id + 1;
    }

    private void removeIdentity(int hash, int id) {
        int mask = identityIndex.length - 1;
        int i = mix(hash) & mask;
        while (identityIndex[i] != id + 1) {
            i = (i + 1) & mask;
        }
        int hole = i;
        int j = i;
        while (true) {
            j = (j + 1) & mask;
            int entry = identityIndex[j];
            if (entry == 0) {
                break;
            }
            int home = mix(identityIndexHashes[j]) & mask;
            if (!isBetween(hole, home, j)) {
                identityIndex[hole] = entry;
                identityIndexHashes[hole] = identityIndexHashes[j];
                hole = j;
            }
        }
        identityIndex[hole] = 0;
        identityIndexHashes[hole] = 0;
    }

    private static boolean isBetween(int hole, int home, int j) {
        return hole <= j ? (hole < home && home <= j) : (hole < home || home <= j);
    }

    private void growIdentity() {
        int[] oldIndex = identityIndex;
        int[] oldHashes = identityIndexHashes;
        int[] index = new int[oldIndex.length * 2];
        int[] hashes = new int[index.length];
        for (int i = 0; i < oldIndex.length; i++) {
            if (oldIndex[i] != 0) {
                insertIdentity(index, hashes, oldHashes[i], oldIndex[i] - 1);
            }
        }
        identityIndexHashes = hashes;
        identityIndex = index;
    }

    private static int mix(int h) {
        h ^= (h >>> 16);
        h *= 0x85ebca6b;
        h ^= (h >>> 13);
        return h;
    }
}
