// Background work of the back end (vk_jobs.cpp): shader translation and pipeline builds run on a few worker threads
// at below-normal priority while the replay goes on, after totk-pc-recomp's vk_pipeline.c background builds. A draw
// whose program or pipeline is not ready yet is left out of its frame, or waits for it (vk_draw.cpp decides).
#pragma once

#include <atomic>
#include <functional>

namespace mkwvk {

/// Queues fn for a worker (urgent: ahead of the others, for work a draw waits on)
/// flag: what the job sets when it is done. A jobs_wait on it while the job is still queued runs the job there and then
/// on_failure: publishes the resource's failed/completed state if fn throws; must not throw.
/// A completion flag alone does not imply success: its resource must describe the failure too.
void jobs_push(std::function<void()> fn, bool urgent = false, const std::atomic<bool>* flag = nullptr,
               std::function<void()> on_failure = {});
/// Queues fn for the one thread that runs such jobs one at a time, in order
void jobs_push_serial(std::function<void()> fn, std::function<void()> on_failure = {});
/// Waits until flag is set by a job (which calls jobs_notify after setting it)
void jobs_wait(const std::atomic<bool>& flag);
/// Waits at most ms for flag, without running a queued job on this thread; whether it was set
bool jobs_wait_for(const std::atomic<bool>& flag, double ms);
void jobs_notify();
int jobs_pending();

} // namespace mkwvk
