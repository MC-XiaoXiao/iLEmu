package com.liulishuo.okdownload.core.file;

import com.liulishuo.okdownload.DownloadTask;
import com.liulishuo.okdownload.core.breakpoint.BreakpointInfo;
import com.liulishuo.okdownload.core.breakpoint.DownloadStore;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Future;
import java.util.concurrent.locks.LockSupport;

/** Keeps OkDownload's downloader and persisted breakpoints, replacing its lost-wakeup wait. */
public final class FirmwareFileStrategy extends ProcessFileStrategy {
    @Override public MultiPointOutputStream createProcessStream(
            DownloadTask task, BreakpointInfo info, DownloadStore store) {
        return new MultiPointOutputStream(task, info, store) {
            @Override void ensureSync(boolean noMoreStream, int blockIndex) {
                // In 1.0.7 the sync worker can finish before a block registers its parked
                // thread. Waiting on the Future also observes that already-finished worker.
                // All blocks announce completion before waiting, so this joins the final
                // disk flush without preventing the other connections from completing.
                Future<?> pending = syncFuture;
                if (pending == null) return;
                LockSupport.unpark(runSyncThread);
                try {
                    pending.get();
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("Download synchronization interrupted", error);
                } catch (ExecutionException error) {
                    throw new IllegalStateException("Download synchronization failed", error.getCause());
                }
            }
        };
    }
}
