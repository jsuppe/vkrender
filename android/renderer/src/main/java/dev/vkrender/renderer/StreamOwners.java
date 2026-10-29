package dev.vkrender.renderer;

import android.os.IBinder;
import android.os.RemoteException;
import java.util.HashMap;

/** Serializes ownership transitions, including callbacks racing reacquisition.
 * Legacy acquisition intentionally revokes a tracked lease without releasing
 * the reader; legacy callers retain their historical explicit-release contract.
 */
final class StreamOwners<T> {
    interface Backend<T> {
        T acquire(int uid, int slot);
        void release(int uid, int slot);
    }
    private final Backend<T> backend;
    private final HashMap<Long, Lease> owners = new HashMap<>();
    StreamOwners(Backend<T> backend) { this.backend = backend; }
    private static long key(int uid, int slot) { return ((long)uid << 32) | (slot & 0xffffffffL); }
    private final class Lease implements IBinder.DeathRecipient {
        final int uid, slot;
        final IBinder token;
        Lease(int uid, int slot, IBinder token) { this.uid=uid; this.slot=slot; this.token=token; }
        @Override public void binderDied() {
            synchronized (StreamOwners.this) {
                if (owners.get(key(uid,slot)) != this) return;
                owners.remove(key(uid,slot));
                backend.release(uid,slot);
            }
        }
    }
    private void forget(long key) {
        Lease old = owners.remove(key);
        if (old != null) old.token.unlinkToDeath(old, 0);
    }
    synchronized T acquire(int uid, int slot, IBinder token) {
        long key = key(uid, slot);
        Lease old = owners.get(key);
        if (token == null) {
            forget(key);
            return backend.acquire(uid,slot);
        }
        if (old != null && old.token.equals(token)) return backend.acquire(uid,slot);
        // Link first: a token already dead must not evict a healthy owner.
        Lease lease = new Lease(uid,slot,token);
        try { token.linkToDeath(lease,0); }
        catch (RemoteException e) { return null; }
        forget(key);
        // Also replace any untracked legacy reader on opt-in acquisition.
        backend.release(uid,slot);
        owners.put(key,lease);
        T surface = backend.acquire(uid,slot);
        if (surface == null) forget(key);
        return surface;
    }
    synchronized void release(int uid, int slot, IBinder token) {
        long key=key(uid,slot);
        Lease lease=owners.get(key);
        if (token != null && (lease == null || !lease.token.equals(token))) return;
        forget(key);
        backend.release(uid,slot);
    }
    synchronized void close() {
        for (Lease lease : owners.values()) {
            lease.token.unlinkToDeath(lease,0);
            backend.release(lease.uid,lease.slot);
        }
        owners.clear();
    }
}
