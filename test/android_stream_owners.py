#!/usr/bin/env python3
"""Host JVM regression of real StreamOwners using minimal Binder contract stubs.
Does not claim kernel Binder/death-delivery or GPU/device coverage.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="vkr-owners-") as tmp:
    dst = Path(tmp)
    files = {
        "android/os/IBinder.java": """package android.os;
public interface IBinder {
 interface DeathRecipient { void binderDied(); }
 void linkToDeath(DeathRecipient recipient,int flags) throws RemoteException;
 boolean unlinkToDeath(DeathRecipient recipient,int flags);
}""",
        "android/os/RemoteException.java": "package android.os; public class RemoteException extends Exception {}",
        "dev/vkrender/renderer/OwnersTest.java": """package dev.vkrender.renderer;
import android.os.*;
import java.util.*;
public class OwnersTest {
 static void check(boolean ok) { if(!ok)throw new AssertionError(); }
 static class Token implements IBinder {
  DeathRecipient current, stale; boolean dead;
  public void linkToDeath(DeathRecipient r,int flags)throws RemoteException {
   if(dead)throw new RemoteException(); current=r;
  }
  public boolean unlinkToDeath(DeathRecipient r,int flags) { stale=r;current=null;return true; }
  void die() { dead=true;if(current!=null)current.binderDied(); }
 }
 static class Backend implements StreamOwners.Backend<Integer> {
  int generation=0,releases=0; boolean fail;
  Map<String,Integer> streams=new HashMap<>();
  public Integer acquire(int uid,int slot) {
   if(fail)return null;
   return streams.computeIfAbsent(uid+":"+slot,k->++generation);
  }
  public void release(int uid,int slot) { releases++;streams.remove(uid+":"+slot); }
 }
 public static void main(String[] args) {
  Backend b=new Backend();StreamOwners<Integer> o=new StreamOwners<>(b);
  Token a=new Token(),c=new Token();
  int first=o.acquire(1,0,a);
  check(o.acquire(1,0,a)==first); // repeated acquisition is idempotent
  int second=o.acquire(1,0,c);check(second!=first);
  int n=b.releases;a.stale.binderDied();check(b.releases==n); // queued old death
  o.release(1,0,a);check(b.releases==n); // old owner cannot release new generation
  Token dead=new Token();dead.dead=true;check(o.acquire(1,0,dead)==null);
  check(o.acquire(1,0,c)==second); // dead replacement cannot evict
  o.acquire(2,0,a);o.acquire(1,1,a); // UID and slot isolation
  c.die();check(!b.streams.containsKey("1:0"));
  check(b.streams.containsKey("2:0")&&b.streams.containsKey("1:1"));
  Token legacy=new Token();int tracked=o.acquire(3,0,legacy);
  check(o.acquire(3,0,null)==tracked);n=b.releases;
  legacy.stale.binderDied();check(b.releases==n); // legacy takeover revokes token
  o.release(3,0,null);check(!b.streams.containsKey("3:0"));
  Token failed=new Token();b.fail=true;check(o.acquire(4,0,failed)==null);
  check(failed.current==null);b.fail=false;
  o.close();check(!b.streams.containsKey("2:0")&&!b.streams.containsKey("1:1"));
  System.out.println("StreamOwners: acquisition/replacement/death/stale release/UID-slot isolation/legacy/failure/shutdown passed");
 }
}""",
    }
    files["dev/vkrender/renderer/StreamOwners.java"] = (root / "android/renderer/src/main/java/dev/vkrender/renderer/StreamOwners.java").read_text()
    for name, text in files.items():
        path=dst/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)
    subprocess.run(["javac", "-d", tmp, *[str(dst/name) for name in files]],check=True)
    subprocess.run(["java", "-cp", tmp, "dev.vkrender.renderer.OwnersTest"],check=True)
