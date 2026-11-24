# Hypothetical device performance model

The faculty script runs the same two-request scenario for all three profiles and confirms deterministic MODELED responses. The research results use actual simulator I/O from the separate MEASURED pipeline. [Scenario commands](DEMO_COMMANDS.md#m-telemetry-and-device-scenarios)

**MODELED** output describes hypothetical devices with configurable assumptions and a deterministic arrival schedule. The values below are illustrative inputs. [Measured telemetry methodology](telemetry_methodology.md)

## Profiles and service time

Each profile defines fixed sequential read/write overhead, fixed random read/write overhead, bandwidth by direction, and service slots per device. Values can be discovered through `GET /api/model/profiles`. Override all seven parameters for a single scenario with the optional `parameters` object.

- **HDD_7200** uses sequential overheads of 0.35 and 0.50 ms and random overheads of 8.50 and 9.00 ms for read and write. Transfer bandwidths are 180 and 150 MiB/s, respectively. The random cost groups hypothetical seek and rotation effects into one constant. Sequential requests represent a device already positioned for the transfer.
- **SATA_SSD** uses sequential overheads of 0.08 and 0.11 ms, random overheads of 0.16 and 0.23 ms, and transfer bandwidths of 520 and 460 MiB/s.
- **NVME_SSD** uses sequential overheads of 0.03 and 0.04 ms, random overheads of 0.06 and 0.09 ms, and transfer bandwidths of 3,000 and 2,500 MiB/s.

Every preset defaults to one service slot. Access classification comes from each scenario request. Service time in milliseconds is `fixed overhead + 1000 × (bytes / 1048576) / bandwidth_mib_per_second`. Fixed costs may be zero. All operands must be finite. Bandwidth ranges from 0.001 to 1,000,000 MiB/s, fixed overhead ranges from 0 through 60,000 ms, and `queue_depth` accepts 1–32. Here `queue_depth` means parallel service slots in the model. The modeled waiting queue is bounded by the scenario request limit.

## FIFO scheduling and dependencies

Each virtual device has its own clock for each service slot. Requests enter FIFO order by effective arrival time and break ties by input index. For each request, the earliest available slot starts at the larger of that slot’s available time and the effective arrival. Queue wait equals start minus effective arrival. Completion equals start plus service duration. The scenario elapsed time spans the first effective arrival to the final completion. The reported start and completion values remain offsets from the scenario's time zero.

Each request specifies a zero-based device, nominal `arrival_ms`, `READ` or `WRITE`, `bytes`, and `RANDOM` or `SEQUENTIAL`. The optional `depends_on` array contains unique, earlier zero-based request indices. Effective arrival is the maximum of the nominal arrival and predecessor completion times. Dependency waiting can be computed as effective arrival minus nominal arrival. Queue waiting excludes dependency waiting.

Requests to different hypothetical devices can overlap. Requests to the same device contend when its service slots are busy. To model foreground and rebuild contention, submit an interleaved trace targeting the same modeled slots and interpret each request by its input index.

Dependencies also support explicit RAID scenarios. For a RAID 5 single-block read-modify-write, schedule old-data and old-parity reads as independent requests. Each replacement write depends on both reads. RAID 6 updates can depend on the old data, P, and Q reads. Full-stripe data and parity writes can be independent when the inputs are already available. Parity CPU time and flush stages are outside this service formula. Explicit arrival offsets can represent additional delay. The simulator exports recent physical events for inspection. Converting those observations into a scenario requires explicit assumptions about dependencies and hypothetical arrival times.

## Limits and API

`POST /api/model/simulate` and the equivalent NDJSON `model.simulate` run a deterministic calculation in C++. Limits are 64 requests, 64 devices, 8 dependencies per request, 64 MiB per request, and nominal arrivals between 0 and 86,400,000 ms. HTTP JSON and IPC frames are also bounded. Identical accepted inputs produce identical results for the same model implementation.

```json
{
  "profile": "HDD_7200",
  "devices": 2,
  "workload": [
    {"device": 0, "arrival_ms": 0, "operation": "READ", "bytes": 4096, "access": "RANDOM"},
    {"device": 1, "arrival_ms": 0, "operation": "READ", "bytes": 4096, "access": "RANDOM"},
    {"device": 0, "arrival_ms": 0, "operation": "WRITE", "bytes": 4096,
     "access": "RANDOM", "depends_on": [0, 1]}
  ]
}
```

Results include actual assumptions used, effective arrivals, start times, service durations, queue waits, completion times, and total elapsed time, all in milliseconds. Simulations use no image I/O or artificial sleeps. There is no calibration, device firmware model, cache model, seek-distance model, or real multidevice experiment. Keep the profile inputs and the specified dependencies with each reported scenario.
