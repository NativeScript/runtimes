package com.tns;

import android.os.Handler;
import android.os.Looper;

/*
    Lets the main thread run JavaScript in a runtime attached to a host (React
    Native), whose JavaScript thread is not the main thread.

    A jsi::Runtime may be entered from any thread as long as one thread at a
    time is in it. The main thread asks the JavaScript thread to stop where it is
    holding nothing mid-flight -- between two of its looper messages, or blocked
    in runOnMainThread -- and runs while that thread waits. React Native's
    executeSynchronouslyOnSameThread_CAN_DEADLOCK hands its runtime over the
    same way.

    Only the main thread borrows. Other threads keep posting to the JavaScript
    thread and waiting, as before.
 */
final class MainThreadLoan {
    interface Body<T> {
        T run() throws NativeScriptException;
    }

    private interface Condition {
        boolean isMet();
    }

    private static final Condition ALWAYS = () -> true;

    private final Runtime runtime;
    private final ThreadScheduler jsThread;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private final Object lock = new Object();
    // Guarded by lock.
    private boolean jsThreadWaiting;
    // Guarded by lock. The JavaScript thread stopped between looper messages,
    // so React Native has drained its microtasks and is in no task of its own.
    private boolean jsThreadAtBoundary;
    private boolean mainWaiting;
    private boolean mainRunning;
    private boolean parkPosted;
    private boolean closed;

    // Main thread only.
    private int depth;

    MainThreadLoan(Runtime runtime, ThreadScheduler jsThread) {
        this.runtime = runtime;
        this.jsThread = jsThread;
    }

    static boolean isMainThread() {
        return Looper.myLooper() == Looper.getMainLooper();
    }

    /*
        Runs `body` on the calling (main) thread with the JavaScript thread
        stopped. Re-entrant. Answers null without running `body` once the
        runtime is gone, as a call posted to a stopped JavaScript thread does.
     */
    <T> T borrow(Body<T> body) throws NativeScriptException {
        if (depth > 0) {
            depth++;
            try {
                return body.run();
            } finally {
                depth--;
            }
        }

        boolean atBoundary;
        synchronized (lock) {
            mainWaiting = true;
            boolean interrupted = false;
            while (!jsThreadWaiting && !closed) {
                if (!parkPosted) {
                    parkPosted = jsThread.post(this::park);
                    if (!parkPosted) {
                        closed = true;
                        break;
                    }
                }
                try {
                    lock.wait();
                } catch (InterruptedException e) {
                    interrupted = true;
                }
            }
            if (interrupted) {
                Thread.currentThread().interrupt();
            }
            mainWaiting = false;
            if (closed) {
                lock.notifyAll();
                return null;
            }
            mainRunning = true;
            atBoundary = jsThreadAtBoundary;
        }

        depth = 1;
        // A Java object JavaScript constructs here (a listener, a subclass)
        // registers with the current runtime.
        Runtime previous = runtime.enterOnThisThread();
        try {
            return body.run();
        } finally {
            try {
                // Nothing else will run what this call queued until React Native
                // next dispatches a task, and the JavaScript thread can sit idle.
                // Mid-task, React Native drains them itself when the task ends.
                if (atBoundary) {
                    runtime.drainMicrotasks();
                }
            } finally {
                Runtime.restoreOnThisThread(previous);
                depth = 0;
                synchronized (lock) {
                    mainRunning = false;
                    lock.notifyAll();
                }
            }
        }
    }

    /*
        Called on the JavaScript thread: runs the native task on the main thread
        and returns once it has run, lending the runtime meanwhile.
     */
    void runOnMainThread(final long task) {
        final boolean[] done = {false};
        final Runnable onMain = () -> {
            try {
                borrow(() -> {
                    runtime.runMainThreadTask(task);
                    return null;
                });
            } finally {
                synchronized (lock) {
                    done[0] = true;
                    lock.notifyAll();
                }
            }
        };
        lend(false, () -> done[0], () -> {
            if (!mainHandler.post(onMain)) {
                done[0] = true;
            }
        });
    }

    /* Wakes a main thread waiting on a JavaScript thread that will never stop for it. */
    void close() {
        synchronized (lock) {
            closed = true;
            lock.notifyAll();
        }
    }

    private void park() {
        synchronized (lock) {
            parkPosted = false;
            if (!mainWaiting) {
                return;
            }
        }
        lend(true, ALWAYS, null);
    }

    private void lend(boolean atBoundary, Condition finished, Runnable start) {
        // The JavaScript thread can be inside a Java->JS call, holding the engine
        // lock the borrower's calls take. It owns nothing while parked.
        int engineLockDepth = runtime.releaseEngineLock();
        try {
            synchronized (lock) {
                jsThreadWaiting = true;
                jsThreadAtBoundary = atBoundary;
                lock.notifyAll();
                if (start != null) {
                    start.run();
                }
                boolean interrupted = false;
                while (!finished.isMet() || mainWaiting || mainRunning) {
                    try {
                        lock.wait();
                    } catch (InterruptedException e) {
                        interrupted = true;
                    }
                }
                jsThreadWaiting = false;
                jsThreadAtBoundary = false;
                if (interrupted) {
                    Thread.currentThread().interrupt();
                }
            }
        } finally {
            runtime.reacquireEngineLock(engineLockDepth);
        }
    }
}
