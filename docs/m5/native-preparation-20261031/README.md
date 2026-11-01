# Native resource preparation: causal fix and remaining presentation stalls

Code: FrameLink f69dc83; renderer e101348. Same ring12/pending6/common100ms. Matching Windows Release builds passed; final Linux renderer22/22 tests passed. The native45s positive predates only subsequent error-propagation fixes; successful preparation/data path is identical. No original demo modified.

## Proven original NV12 loss mechanism

The tracing-overhead capture lostPTS100000000,166666666,216666666ns in the single latest-wins IPC mailbox. Each arrived during first-use service.input_cpu_conversion lasting18.644,17.467,17.635ms, longer than16.667ms source cadence. First interval: mapping0.989ms, pixel conversion1.480ms, unmap0.0096ms, upload1.582ms; approximately13.6ms remained in addOwnedSurface allocation/layout transition/vkQueueWaitIdle. The render thread could not acquire the unread mailbox before the next steady source arrival. Producer epoch/startup order was already correct; native destination resource readiness was missing.

The fix prepares the bounded conversion targets on the render thread before producer PRIMED/final epoch. No unproduced source pixels are read, no extra playback queue or delay is added. NV12 CPU conversion remains a fallback, not zero-copy GPU import.

## Results

- Affected native NV1260,45s, no injected fault: **2700/2700** all stages, exact independent manifest closed,59.9993fps. Zero startup/steady/end loss. Maximum unique accepted-presentation gap16.97ms including startup. Preparation26.93ms finished50.53ms before first source; max conversion5.86ms rather than previous cold17–18ms scopes.
- Deterministic headless250ms preparation delay: **180/180** source IDs reached GPU completion; preparation completed before first source. Same250ms delay under explicit legacy first-use control:158/180;22 startup drops (10 ring-full,5 pending replacement,7 late). Complete source/outcome accounting in both cases. Headless is an ownership/order regression, not native presentation qualification.
- Final strict native failure and timeout guards: both processes exit[1,1], zero GO/video. Failure completed2.79s; timeout6.84s including setup/teardown with the existing4000ms per-phase budget; no budget enlargement. Preparation errors terminate the native session, rather than escape render thread or trigger stale-session reconnect retries.

## Preserved failures, not waived

The first native250ms delayed-preparation positive delivered159/180 even though setup finished50.74ms before first source. Exact classification:16 late +5 IPC replacement, all startup. Trace localizes additional stalls to **vkQueuePresent call wall durations163.36ms and95.82ms**, not lazy destination allocation. Native legacy delay also delivered159/180 but with different reasons (10 ring-full,4 replacements,7 late). These equal totals must not be misrepresented as an improvement; native delayed-stall qualification remains open.

The previous BGRA trace-off380ms schedule excursion remains unattributed. The new measured present-call stalls are a concrete separate observation, not proof of that earlier event's cause. CPU wall spans do not distinguish driver execution, compositor waits or OS descheduling.

Early prototype error tests exposed fail-fast on render-thread exception, then excessive reconnect retries. Both failures are retained; final guards require coherent[1,1], not merely nonzero. Headless full-suite log stops at the old error guard; its positive/legacy cases were valid and independently manifest-audited. Final corrected native error guards are separate captures. No claim that a failed full-suite run passed wholesale.

Physical scanout is not measured. Trace completion means fence observation; presentation means API acceptance. This targeted fix does not close every remaining M5 performance qualification gap.
