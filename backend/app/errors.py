STATUS_CODES = {
    **dict.fromkeys(("DISK_NOT_FOUND", "ARRAY_NOT_FOUND", "POOL_NOT_FOUND", "LUN_NOT_FOUND",
                     "FILE_NOT_FOUND", "REBUILD_NOT_FOUND"), 404),
    **dict.fromkeys(("DUPLICATE_RESOURCE", "INVALID_RESOURCE_STATE", "INVALID_MEMBER_STATE",
                     "ARRAY_DEGRADED", "ARRAY_FAILED", "ARRAY_INCONSISTENT", "CHECKSUM_MISMATCH",
                     "VERIFICATION_FAILED"), 409),
    "INVALID_PARAMS": 422,
    "INVALID_RAID_CONFIGURATION": 422,
    "OUT_OF_SPACE": 507,
    "STORAGE_LOCKED": 503,
    "ENGINE_UNAVAILABLE": 503,
    "ENGINE_TIMEOUT": 504,
    "OPERATION_OUTCOME_UNKNOWN": 504,
    "REQUEST_TOO_LARGE": 413,
    "RESPONSE_TOO_LARGE": 502,
    "STAGING_REJECTED": 400,
    "ORIGIN_FORBIDDEN": 403,
    "INVALID_REQUEST": 400,
    "MALFORMED_JSON": 400,
}


class ApiError(Exception):
    def __init__(self, code: str, message: str, *, request_id: str | None = None,
                 uncertain: bool = False) -> None:
        super().__init__(message)
        self.code = code
        self.status = STATUS_CODES.get(code, 500)
        self.request_id = request_id
        self.uncertain = uncertain

    def body(self) -> dict:
        error = {"code": self.code, "message": str(self)}
        if self.request_id:
            error["request_id"] = self.request_id
        return {"error": error}
