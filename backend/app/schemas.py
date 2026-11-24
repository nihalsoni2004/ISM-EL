from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, StringConstraints, field_validator, model_validator

MIB = 2**20
MAX_LBA = (2**64 - 1) // 4096

DiskId = Annotated[str, StringConstraints(pattern=r"^disk(0|[1-9][0-9]{0,19})$")]
ArrayId = Annotated[str, StringConstraints(pattern=r"^array(0|[1-9][0-9]{0,19})$")]
PoolId = Annotated[str, StringConstraints(pattern=r"^pool(0|[1-9][0-9]{0,19})$")]
LunId = Annotated[str, StringConstraints(pattern=r"^lun(0|[1-9][0-9]{0,19})$")]
JobId = Annotated[str, StringConstraints(pattern=r"^rebuild[1-9][0-9]{0,19}$")]
FileId = Annotated[str, StringConstraints(pattern=r"^file[1-9][0-9]{0,18}$")]
BlockHex = Annotated[str, StringConstraints(min_length=8192, max_length=8192, pattern=r"^[0-9a-f]+$")]
HexDigest = Annotated[str, StringConstraints(min_length=64, max_length=64, pattern=r"^[0-9a-f]+$")]
Health = Literal["HEALTHY", "DEGRADED", "FAILED", "INCONSISTENT"]


class Input(BaseModel):
    model_config = ConfigDict(extra="forbid")


class CreateDisk(Input):
    disk_id: DiskId
    capacity_mib: int = Field(strict=True, ge=1, le=(2**63 - 1) // MIB)


class CreateRaid(Input):
    array_id: ArrayId
    raid_level: int = Field(strict=True, json_schema_extra={"enum": [0, 1, 5, 6]})
    members: list[DiskId] = Field(min_length=2, max_length=257)
    stripe_unit_blocks: int | None = Field(default=None, strict=True, ge=1, le=MAX_LBA)

    @field_validator("raid_level")
    @classmethod
    def supported_level(cls, value: int) -> int:
        if value not in (0, 1, 5, 6):
            raise ValueError("Supported RAID levels are 0, 1, 5 and 6")
        return value


class DiskReference(Input):
    disk_id: DiskId


class Scheduling(Input):
    schema_version: int = Field(1, strict=True, ge=1, le=1)
    policy: Literal["FULL_SPEED", "FIXED_THROTTLE", "ADAPTIVE"] = "FULL_SPEED"
    fixed_delay_us: int = Field(3000, strict=True, ge=1, le=50000)
    observation_window_ms: int = Field(250, strict=True, ge=50, le=2000)
    control_interval_ms: int = Field(80, strict=True, ge=25, le=2000)
    idle_ms: int = Field(200, strict=True, ge=25, le=5000)
    target_read_p95_us: int = Field(8000, strict=True, ge=100, le=200000)
    high_arrival_iops: int = Field(80, strict=True, ge=1, le=10000)
    minimum_delay_us: int = Field(0, strict=True, ge=0, le=50000)
    maximum_delay_us: int = Field(6000, strict=True, ge=0, le=50000)
    delay_step_us: int = Field(1000, strict=True, ge=1, le=50000)

    @model_validator(mode="after")
    def paired_limits(self):
        if (self.control_interval_ms > min(self.observation_window_ms, self.idle_ms)
                or self.minimum_delay_us > self.maximum_delay_us):
            raise ValueError("Invalid timing or delay bounds")
        return self


class PrepareRebuild(Input):
    array_id: ArrayId
    failed_disk_id: DiskId
    spare_disk_id: DiskId
    scheduling: Scheduling | None = None


class RebuildStart(Input):
    checkpoint_limit: int | None = Field(default=None, strict=True, ge=1, le=MAX_LBA)


class CreatePool(Input):
    pool_id: PoolId
    array_id: ArrayId
    chunk_blocks: int = Field(default=16, strict=True, ge=1, le=1024)
    max_ratio: int = Field(default=0, strict=True, ge=0, le=MAX_LBA)
    warning_percent: int = Field(default=70, strict=True, ge=1, le=99)
    critical_percent: int = Field(default=90, strict=True, ge=2, le=100)


class CreateLun(Input):
    lun_id: LunId
    pool_id: PoolId
    capacity_mib: int = Field(strict=True, ge=1, le=(2**64 - 1) // MIB)
    mode: Literal["raw", "file"] = "raw"


class WriteBlock(Input):
    data_hex: BlockHex


class EngineStatus(BaseModel):
    engine_available: bool
    catalog_accessible: bool
    ready: bool
    process_id: int | None = None
    version: str | None = None
    array_health: dict[Health, int] = Field(default_factory=dict)
    error_code: str | None = None


class Disk(BaseModel):
    disk_id: DiskId
    capacity_bytes: int
    block_size: int
    block_count: int


class Disks(BaseModel):
    disks: list[Disk]


class Raid(BaseModel):
    array_id: ArrayId
    raid_level: Literal[0, 1, 5, 6]
    members: list[DiskId]
    stripe_unit_blocks: int
    capacity_blocks: int
    capacity_bytes: int
    layout: str
    block_size: int
    health: Health


class Raids(BaseModel):
    arrays: list[Raid]


class RaidStatus(BaseModel):
    array_id: ArrayId
    raid_level: Literal[0, 1, 5, 6]
    health: Health
    pending_write: bool
    reads_permitted: bool
    writes_permitted: bool
    pool_owned: bool
    tolerated_failures: int


class Member(BaseModel):
    disk_id: DiskId
    state: str
    cause: str
    present: bool
    available: bool


class Members(BaseModel):
    members: list[Member]


class Location(BaseModel):
    disk_id: DiskId
    role: Literal["data", "mirror", "p", "q"]
    physical_lba: int


class RaidMap(BaseModel):
    array_id: ArrayId
    lba: int
    locations: list[Location]


class RaidVerification(BaseModel):
    array_id: ArrayId
    passed: bool


class Spare(BaseModel):
    disk_id: DiskId
    capacity_bytes: int
    state: str
    assignment: JobId | None
    available: bool


class Spares(BaseModel):
    spares: list[Spare]


class Rebuild(BaseModel):
    job_id: JobId
    array_id: ArrayId
    failed_disk_id: DiskId
    spare_disk_id: DiskId
    status: Literal["PREPARED", "RUNNING", "PAUSED", "INTERRUPTED", "COMPLETED", "FAILED"]
    completed_blocks: int
    total_blocks: int
    progress_percent: float
    batch_blocks: int
    started_at: int
    checkpoint_at: int
    worker_active: bool
    pause_requested: bool
    worker_error: bool
    scheduling: Scheduling = Field(default_factory=Scheduling)


class RebuildPolicies(BaseModel):
    controller_version: str
    policies: list[Scheduling]


class SchedulingDecision(BaseModel):
    elapsed_ms: float
    delay_us: int
    pressure: str
    reason: str
    submitted_iops: float | None
    completed_iops: float | None
    p95_ms: float | None
    outstanding: int | None
    completed_blocks: int


class RebuildScheduler(BaseModel):
    source: Literal["MEASURED"]
    job_id: JobId
    status: str
    worker_active: bool
    completed_blocks: int
    total_blocks: int
    configuration: Scheduling
    current: SchedulingDecision | None
    decisions: list[SchedulingDecision]


class Rebuilds(BaseModel):
    jobs: list[Rebuild]


class Pool(BaseModel):
    pool_id: PoolId
    array_id: ArrayId
    health: Health
    physical_capacity_bytes: int
    allocated_bytes: int
    reserved_bytes: int
    free_bytes: int
    provisioned_bytes: int
    chunk_bytes: int
    capacity_state: Literal["NORMAL", "WARNING", "CRITICAL", "FULL"]
    total_chunks: int
    warning_percent: int
    critical_percent: int


class VerifiedPool(Pool):
    passed: bool


class Pools(BaseModel):
    pools: list[Pool]


class Lun(BaseModel):
    lun_id: LunId
    pool_id: PoolId
    capacity_bytes: int
    block_count: int
    chunk_blocks: int
    allocated_chunks: int
    mode: Literal["raw", "file"]


class Luns(BaseModel):
    luns: list[Lun]


class LunMap(BaseModel):
    lun_id: LunId
    lba: int
    logical_chunk: int
    offset: int
    physical_chunk: int | None
    raid_lba: int | None


class ReadResult(BaseModel):
    lun_id: LunId
    lba: int
    mapped: bool
    data_hex: BlockHex


class WriteResult(BaseModel):
    lun_id: LunId
    lba: int
    new_chunks: int
    allocated_chunks: int


class FileRecord(BaseModel):
    file_id: FileId
    lun_id: LunId
    name: str
    size_bytes: int
    start_lba: int
    block_count: int
    sha256: HexDigest | None
    status: Literal["PENDING", "WRITING", "VERIFYING", "COMPLETED", "FAILED"]
    created_at: str
    updated_at: str


class FileRecords(BaseModel):
    files: list[FileRecord]


class FileVerification(BaseModel):
    file_id: FileId
    name: str
    size_bytes: int
    expected_sha256: HexDigest
    actual_sha256: HexDigest | None
    passed: bool
    error_code: str | None


class FileVerifications(BaseModel):
    files: list[FileVerification]


class ErrorDetails(BaseModel):
    code: str
    message: str
    request_id: str | None = None


class ErrorResponse(BaseModel):
    error: ErrorDetails
