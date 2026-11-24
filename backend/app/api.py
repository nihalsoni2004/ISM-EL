from typing import Annotated

from fastapi import APIRouter, File, Path, Query, Request, UploadFile
from starlette.responses import StreamingResponse

from . import schemas as s
from . import telemetry_schemas as t
from .errors import ApiError
from .exchange import Exchange, checked_name

router = APIRouter(prefix="/api", responses={
    status: {"model": s.ErrorResponse} for status in (400, 403, 404, 409, 413, 422, 503, 504, 507)
})
Lba = Annotated[int, Path(ge=0, le=s.MAX_LBA)]


def call(request: Request, operation: str, **params):
    state = request.app.state
    if not state.engine.available or not state.catalog_ready:
        raise ApiError("ENGINE_UNAVAILABLE", "The storage engine is not ready.")
    return state.engine.request(operation, params)


@router.get("/telemetry", response_model=t.TelemetrySnapshot, response_model_exclude_unset=True, tags=["telemetry"])
def telemetry(request: Request, include_samples: bool = False, event_limit: Annotated[int, Query(ge=0, le=256)] = 0):
    return call(request, "telemetry.snapshot", include_samples=include_samples, event_limit=event_limit)


@router.post("/telemetry/reset", response_model=t.TelemetryWindow, tags=["telemetry"])
def reset_telemetry(request: Request):
    return call(request, "telemetry.reset")


@router.get("/telemetry/rebuilds/{job_id}", response_model=t.RebuildTelemetry, tags=["telemetry"])
def rebuild_telemetry(request: Request, job_id: s.JobId):
    return call(request, "telemetry.rebuild", job_id=job_id)


@router.get("/model/profiles", response_model=t.ModelProfiles, tags=["model"])
def model_profiles(request: Request):
    return call(request, "model.profiles")


@router.post("/model/simulate", response_model=t.ModelSimulation, tags=["model"])
def simulate_model(request: Request, body: t.SimulateModel):
    return call(request, "model.simulate", **body.model_dump(exclude_none=True))


@router.get("/disks", response_model=s.Disks, tags=["disks"])
def disks(request: Request):
    return {"disks": call(request, "disk.list")}


@router.get("/disks/{disk_id}", response_model=s.Disk, tags=["disks"])
def disk(request: Request, disk_id: s.DiskId):
    return call(request, "disk.info", disk_id=disk_id)


@router.post("/disks", response_model=s.Disk, status_code=201, tags=["disks"])
def create_disk(request: Request, body: s.CreateDisk):
    return call(request, "disk.create", **body.model_dump())


@router.get("/raids", response_model=s.Raids, tags=["arrays"])
def arrays(request: Request):
    return {"arrays": call(request, "raid.list")}


@router.post("/raids", response_model=s.Raid, status_code=201, tags=["arrays"])
def create_array(request: Request, body: s.CreateRaid):
    return call(request, "raid.create", **body.model_dump(exclude_none=True))


@router.get("/raids/{array_id}", response_model=s.Raid, tags=["arrays"])
def array(request: Request, array_id: s.ArrayId):
    return call(request, "raid.info", array_id=array_id)


@router.get("/raids/{array_id}/status", response_model=s.RaidStatus, tags=["arrays"])
def array_status(request: Request, array_id: s.ArrayId):
    return call(request, "raid.status", array_id=array_id)


@router.get("/raids/{array_id}/members", response_model=s.Members, tags=["arrays"])
def members(request: Request, array_id: s.ArrayId):
    return {"members": call(request, "raid.members", array_id=array_id)}


@router.post("/raids/{array_id}/fail", response_model=s.RaidStatus, tags=["arrays"])
def fail_member(request: Request, array_id: s.ArrayId, body: s.DiskReference):
    return call(request, "raid.fail", array_id=array_id, disk_id=body.disk_id)


@router.post("/raids/{array_id}/verify", response_model=s.RaidVerification, tags=["arrays"])
def verify_array(request: Request, array_id: s.ArrayId):
    return call(request, "raid.verify", array_id=array_id)


@router.get("/raids/{array_id}/map/{lba}", response_model=s.RaidMap, tags=["arrays"])
def map_array(request: Request, array_id: s.ArrayId, lba: Lba):
    return call(request, "raid.map", array_id=array_id, lba=lba)


@router.get("/spares", response_model=s.Spares, tags=["rebuilds"])
def spares(request: Request):
    return {"spares": call(request, "spare.list")}


@router.post("/spares", response_model=s.Spare, status_code=201, tags=["rebuilds"])
def register_spare(request: Request, body: s.DiskReference):
    return call(request, "spare.register", disk_id=body.disk_id)


@router.get("/rebuilds", response_model=s.Rebuilds, tags=["rebuilds"])
def rebuilds(request: Request):
    return {"jobs": call(request, "rebuild.list")}


@router.get("/rebuild-policies", response_model=s.RebuildPolicies, tags=["rebuilds"])
def rebuild_policies(request: Request):
    return call(request, "rebuild.policies")


@router.post("/rebuilds", response_model=s.Rebuild, status_code=201, tags=["rebuilds"])
def prepare_rebuild(request: Request, body: s.PrepareRebuild):
    return call(request, "rebuild.prepare", **body.model_dump(exclude_none=True))


@router.get("/rebuilds/{job_id}", response_model=s.Rebuild, tags=["rebuilds"])
def rebuild(request: Request, job_id: s.JobId):
    return call(request, "rebuild.status", job_id=job_id)


@router.get("/rebuilds/{job_id}/scheduler", response_model=s.RebuildScheduler, tags=["rebuilds"])
def rebuild_scheduler(request: Request, job_id: s.JobId):
    return call(request, "rebuild.scheduler", job_id=job_id)


@router.post("/rebuilds/{job_id}/start", response_model=s.Rebuild, status_code=202, tags=["rebuilds"])
def start(request: Request, job_id: s.JobId, body: s.RebuildStart | None = None):
    return call(request, "rebuild.start", job_id=job_id, **(body.model_dump(exclude_none=True) if body else {}))


@router.post("/rebuilds/{job_id}/pause", response_model=s.Rebuild, status_code=202, tags=["rebuilds"])
def pause(request: Request, job_id: s.JobId):
    return call(request, "rebuild.pause", job_id=job_id)


@router.post("/rebuilds/{job_id}/resume", response_model=s.Rebuild, status_code=202, tags=["rebuilds"])
def resume(request: Request, job_id: s.JobId, body: s.RebuildStart | None = None):
    return call(request, "rebuild.resume", job_id=job_id, **(body.model_dump(exclude_none=True) if body else {}))


@router.get("/pools", response_model=s.Pools, tags=["provisioning"])
def pools(request: Request):
    return {"pools": call(request, "pool.list")}


@router.get("/pools/{pool_id}", response_model=s.Pool, tags=["provisioning"])
def pool(request: Request, pool_id: s.PoolId):
    return call(request, "pool.info", pool_id=pool_id)


@router.post("/pools", response_model=s.Pool, status_code=201, tags=["provisioning"])
def create_pool(request: Request, body: s.CreatePool):
    return call(request, "pool.create", **body.model_dump())


@router.post("/pools/{pool_id}/verify", response_model=s.VerifiedPool, tags=["provisioning"])
def verify_pool(request: Request, pool_id: s.PoolId):
    return call(request, "pool.verify", pool_id=pool_id)


@router.get("/luns", response_model=s.Luns, tags=["provisioning"])
def luns(request: Request):
    return {"luns": call(request, "lun.list")}


@router.get("/luns/{lun_id}", response_model=s.Lun, tags=["provisioning"])
def lun(request: Request, lun_id: s.LunId):
    return call(request, "lun.info", lun_id=lun_id)


@router.post("/luns", response_model=s.Lun, status_code=201, tags=["provisioning"])
def create_lun(request: Request, body: s.CreateLun):
    return call(request, "lun.create", **body.model_dump())


@router.get("/luns/{lun_id}/map/{lba}", response_model=s.LunMap, tags=["provisioning"])
def map_lun(request: Request, lun_id: s.LunId, lba: Lba):
    return call(request, "lun.map", lun_id=lun_id, lba=lba)


@router.get("/luns/{lun_id}/blocks/{lba}", response_model=s.ReadResult, tags=["provisioning"])
def read_block(request: Request, lun_id: s.LunId, lba: Lba):
    return call(request, "lun.read", lun_id=lun_id, lba=lba)


@router.put("/luns/{lun_id}/blocks/{lba}", response_model=s.WriteResult, tags=["provisioning"])
def write_block(request: Request, lun_id: s.LunId, lba: Lba, body: s.WriteBlock):
    return call(request, "lun.write", lun_id=lun_id, lba=lba, data_hex=body.data_hex)


@router.get("/files", response_model=s.FileRecords, tags=["files"])
def files(request: Request, lun_id: s.LunId | None = None):
    return {"files": call(request, "file.list", **({"lun_id": lun_id} if lun_id is not None else {}))}


@router.get("/files/{file_id}", response_model=s.FileRecord, tags=["files"])
def file_info(request: Request, file_id: s.FileId):
    return call(request, "file.info", file_id=file_id)


@router.get("/files/{file_id}/verify", response_model=s.FileVerification, tags=["files"])
def verify_file(request: Request, file_id: s.FileId):
    return call(request, "file.verify", file_id=file_id)


@router.get("/luns/{lun_id}/files/verify", response_model=s.FileVerifications, tags=["files"])
def verify_files(request: Request, lun_id: s.LunId):
    return {"files": call(request, "file.verify_all", lun_id=lun_id)}


@router.post("/luns/{lun_id}/files", response_model=s.FileRecord, status_code=201, tags=["files"])
def upload(request: Request, lun_id: s.LunId, file: Annotated[UploadFile, File()]):
    settings = request.app.state.settings
    name = checked_name(file.filename)
    exchange = Exchange(settings.storage_root, "uploads", name)
    retain = False
    try:
        exchange.copy_upload(file, settings.max_upload_bytes)
        return call(request, "file.import", lun_id=lun_id, token=exchange.token, name=name)
    except ApiError as error:
        retain = error.uncertain
        raise
    finally:
        if not retain:
            exchange.cleanup()


@router.get("/files/{file_id}/download", response_class=StreamingResponse, tags=["files"],
            responses={200: {"content": {"application/octet-stream": {
                "schema": {"type": "string", "format": "binary"},
            }}, "description": "Checksum-verified binary file"}})
def download(request: Request, file_id: s.FileId):
    exchange = Exchange(request.app.state.settings.storage_root, "downloads", "payload.bin")
    try:
        result = s.FileVerification.model_validate(call(request, "file.export", file_id=file_id, token=exchange.token))
        if not result.passed or result.actual_sha256 is None or result.actual_sha256 != result.expected_sha256:
            raise ApiError("CHECKSUM_MISMATCH", "The exported file failed checksum verification.")
        return exchange.stream_download(name=result.name, size=result.size_bytes, checksum=result.actual_sha256)
    except ApiError as error:
        if not error.uncertain:
            exchange.cleanup()
        raise
    except Exception:
        exchange.cleanup()
        raise
