---
noteId: "875e3e52e80f11f1a150872f649e1a14"
tags: []

---

# Faculty viva questions and answers

1. **What is a virtual disk here?** A regular file addressed as fixed 4,096-byte blocks.
2. **What does striping change?** Logical addresses alternate across data positions according to the stripe unit and layout.
3. **What redundancy does RAID 0 provide?** None. Its usable capacity is the sum of equal member capacities.
4. **How does RAID 1 recover?** It reads the matching logical block on a surviving mirror and copies that data during rebuild.
5. **How much usable capacity does four-member RAID 5 provide?** Three members' worth with equal capacities.
6. **Why does RAID 5 distribute parity?** Rotating parity avoids concentrating parity writes on one assigned member.
7. **How can XOR recover missing data?** XOR of the parity and all other data values leaves the missing value because XOR cancels equal operands.
8. **How many failures can RAID 6 tolerate?** Two recoverable member failures with valid surviving data and parity.
9. **What distinguishes P from Q?** P is XOR. Q is an independent weighted equation over GF(2^8).
10. **Why GF(2^8)?** Each byte is a field element and finite-field coefficients make independent equations solvable over byte-sized data.
11. **Where can you see parity rotation?** In the actual `array6` mappings at LBA 0 and 2 with the faculty fixture's one-block stripe unit.
12. **What is a hot spare?** An eligible blank image reserved for reconstructing one failed member. Promotion makes it active.
13. **What makes a rebuild checkpoint durable?** The engine flushes spare writes, compares readback, then atomically publishes the checkpoint.
14. **What happens if the process crashes during rebuild?** The worker lease drops. An orphaned RUNNING job becomes INTERRUPTED and resumes from a validated durable checkpoint.
15. **How are duplicate workers prevented?** A Windows interprocess job lease and catalog synchronization coordinate job state and spare ownership.
16. **How do you know rebuilt data is correct?** The engine rederives and verifies the replacement. Tests check healthy consistency, exact reference bytes, image digests and untouched source members.
17. **Can users write while rebuilding?** Foreground writes to the degraded array are blocked. Recoverable foreground reads remain supported.
18. **What is thin provisioning?** Logical capacity is presented before backing chunks are assigned.
19. **How large is the demo chunk?** Sixteen 4 KiB blocks, or 64 KiB.
20. **What does an unallocated read return?** Logical zeros. It does not allocate a chunk.
21. **What is overprovisioning?** Presenting more logical address space than available physical pool bytes. Real writes still require free chunks.
22. **How are logical and physical sizes different?** Logical capacity is addressable space. Allocated physical pool capacity counts mapped RAID chunks. Neither is a direct measure of host filesystem consumption.
23. **What does SQLite store?** Names, file IDs, extent positions, sizes, states and SHA-256 metadata.
24. **What is an extent?** A range of logical blocks from a starting LBA. The range may span multiple thin chunks.
25. **What does SHA-256 provide?** A digest for detecting byte changes. It does not by itself authenticate the sender.
26. **How do the Python and C++ components communicate?** A persistent child exchanges versioned newline-delimited JSON through stdin/stdout with correlated responses.
27. **What does FastAPI own?** HTTP validation, process lifecycle, protocol bridging, and bounded file exchange with the engine.
28. **What does React own?** The local dashboard, forms, polling, and visualization of returned state.
29. **What is a p95 latency?** The nearest-rank sample with at least 95% of the observed latencies at or below it. p99 describes a deeper tail.
30. **How do IOPS and throughput differ?** IOPS counts operations per second. MiB/s measures logical bytes per second using 2²⁰ bytes per MiB.
31. **What is I/O amplification?** Here, physical blocks read or written per foreground logical write block. Parity and chunk initialization can amplify I/O.
32. **Why did RAID 6 sometimes appear faster than RAID 5?** The result depends on request alignment, complete stripe rows, cache, flush/journal behavior, and variation. For example, the 256 KiB arm covered full RAID 6 rows and could include partial RAID 5 rows. The archive does not isolate each cause.
33. **What is the device model?** A deterministic hypothetical service and queue model for HDD, SATA SSD and NVMe profiles using declared requests and parameters.
34. **Why do you record scheduled arrival separately?** A busy worker can submit late. Scheduled-arrival-to-completion latency includes that queueing.
35. **Why use a degraded-only control?** It measures degraded-read behavior without contention from a rebuilding worker.
36. **How are policy repetitions matched?** The runner uses fresh equivalent arrays and the same seed, logical addresses, offered schedule and geometry. Order rotates by repetition.
37. **What do full speed, fixed throttle and adaptive control?** The delay between durable batches. Reconstruction and verification use the same protocol.
38. **Why use arrivals and outstanding reads?** A busy rebuild can suppress completions. In-flight demand gives the controller evidence even when completed IOPS is low.
39. **Why did fixed throttle produce lower p95 than adaptive?** The calibrated fixed arm used longer pacing and spent less elapsed time holding the catalog. It also delayed verified promotion.
40. **What keeps adaptive from starving reconstruction?** The delay is capped and the controller admits another safe batch after each wait. Catalog acquisition and I/O remain host-dependent.
41. **What did you prove statistically?** The observed paired differences support the hypothesis descriptively for this setup. Five repetitions do not establish statistical significance.
42. **Does the simulator use independent physical drives?** Member files share the same host SSD and Windows cache.
43. **What is the project's contribution?** An integrated educational storage stack with durable rebuilding, observable thin/file storage, and reproducible policy comparisons based on real simulator I/O.
44. **What would you evaluate next?** More repetitions, independent devices, multiple foreground workers, alternate controller tuning, and genuinely matched rebuild durations.

