import uvicorn

from .config import Settings


if __name__ == "__main__":
    settings = Settings.from_environment()
    uvicorn.run("backend.app.main:app", host="127.0.0.1", port=settings.port,
                log_level=settings.log_level.lower(), workers=1)
