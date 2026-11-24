from typing import Annotated, Literal

from pydantic import BaseModel, Field, model_validator

from .schemas import Input, Rebuild

Source = Literal["MEASURED"]
Origin = Literal["INTERNAL", "FOREGROUND", "REBUILD"]
Category = Literal[
    "VIRTUAL_DISK_READ", "VIRTUAL_DISK_WRITE", "RAID_LOGICAL_READ", "RAID_LOGICAL_WRITE",
    "REBUILD_READ", "REBUILD_WRITE", "REBUILD_BATCH", "REBUILD_VERIFY",
]
ProfileId = Literal["HDD_7200", "SATA_SSD", "NVME_SSD"]
Mode = Literal["READ", "WRITE"]
Access = Literal["SEQUENTIAL", "RANDOM"]
Depth = Annotated[int, Field(strict=True, ge=1, le=32)]
Delay = Annotated[float, Field(strict=True, allow_inf_nan=False, ge=0, le=60000)]
Bandwidth = Annotated[float, Field(strict=True, allow_inf_nan=False, ge=0.001, le=1000000)]
Index = Annotated[int, Field(strict=True, ge=0, le=63)]


class Observation(BaseModel):
    generation: int
    start_offset_ns: int
    end_offset_ns: int
    seconds: float


class TelemetryWindow(BaseModel):
    source: Source
    scope: Literal["engine_process"]
    observation: Observation


class Counters(BaseModel):
    attempted: int
    completed: int
    failed: int
    requested_blocks: int
    requested_bytes: int
    blocks: int
    bytes: int
    elapsed_ns: int


class Latency(BaseModel):
    count: int
    sample_count: int
    window: Literal["EMPTY", "ENTIRE_OBSERVATION", "RECENT_SAMPLES"]
    min_ms: float | None
    max_ms: float | None
    mean_ms: float | None
    p50_ms: float | None
    p95_ms: float | None
    p99_ms: float | None


class Series(Counters):
    category: Category
    origin_counts: dict[Origin, Counters]
    latency: Latency
    latency_samples_ns: list[int] | None = None


class ObservationEvent(BaseModel):
    category: Category
    origin: Origin
    disk_id: str | None
    array_id: str | None
    job_id: str | None
    requested_blocks: int
    requested_bytes: int
    blocks: int
    bytes: int
    elapsed_ns: int
    end_offset_ns: int
    success: bool
    error_category: Literal["OPERATION_FAILED"] | None


class LogicalRates(BaseModel):
    origin: Literal["FOREGROUND"]
    completed_requests: int
    bytes: int
    iops: float | None
    mib_per_second: float | None


class TelemetrySnapshot(TelemetryWindow):
    latency_sample_limit_per_category: int
    categories: list[Series]
    recent_events: list[ObservationEvent]
    logical_rates: LogicalRates


class RebuildTelemetry(TelemetryWindow):
    job: Rebuild
    raid_level: Literal[0, 1, 5, 6]
    remaining_blocks: int
    last_checkpoint_at: int | None
    retained: bool
    latency_sample_limit_per_category: int
    categories: list[Series]


class ProfileParameters(Input):
    sequential_read_ms: Delay
    sequential_write_ms: Delay
    random_read_ms: Delay
    random_write_ms: Delay
    read_mib_per_second: Bandwidth
    write_mib_per_second: Bandwidth
    queue_depth: Depth


class ModelProfile(BaseModel):
    profile: ProfileId
    parameters: ProfileParameters


class ModelLimits(BaseModel):
    max_requests: int
    max_devices: int
    max_bytes_per_request: int
    max_dependencies: int
    max_arrival_ms: float
    max_queue_depth: int


class ModelProfiles(BaseModel):
    source: Literal["MODELED"]
    profiles: list[ModelProfile]
    limits: ModelLimits


class ModelRequest(Input):
    device: Index
    arrival_ms: Annotated[float, Field(strict=True, allow_inf_nan=False, ge=0, le=86400000)]
    operation: Mode
    bytes: Annotated[int, Field(strict=True, ge=1, le=64 * 1024 * 1024)]
    access: Access
    depends_on: Annotated[list[Index], Field(max_length=8)] = Field(default_factory=list)


class SimulateModel(Input):
    profile: ProfileId
    devices: Annotated[int, Field(strict=True, ge=1, le=64)]
    workload: Annotated[list[ModelRequest], Field(min_length=1, max_length=64)]
    parameters: ProfileParameters | None = None

    @model_validator(mode="after")
    def validate_workload(self):
        for index, request in enumerate(self.workload):
            if request.device >= self.devices:
                raise ValueError("Each device must be within the scenario")
            if any(dependency >= index for dependency in request.depends_on):
                raise ValueError("Dependencies must refer to earlier requests")
            if len(set(request.depends_on)) != len(request.depends_on):
                raise ValueError("Dependencies must be unique")
        return self


class ModeledRequest(BaseModel):
    index: int
    device: int
    bytes: int
    operation: Mode
    access: Access
    depends_on: list[int]
    requested_arrival_ms: float
    effective_arrival_ms: float
    start_ms: float
    service_ms: float
    queue_wait_ms: float
    completion_ms: float


class ModelSimulation(ModelProfile):
    source: Literal["MODELED"]
    queue_policy: Literal["FIFO_EFFECTIVE_ARRIVAL"]
    devices: int
    request_count: int
    first_arrival_ms: float
    last_completion_ms: float
    total_modeled_elapsed_ms: float
    requests: list[ModeledRequest]
