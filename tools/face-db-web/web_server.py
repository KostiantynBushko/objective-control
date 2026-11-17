"""The web version of the face database editor.

The desktop editor (``../face-db-editor``) opens the PostgreSQL table of
objective-control in a window and lets the enrolled faces be reviewed and
corrected. This is the same editor as a web application: the browser
shows the people, the samples of the selected person, the picture of a
face, the name, the metadata and the embedding, and it can merge the
people that are the same person and remove a person.

The work is not done twice. The storage is the ``FaceStore`` of the
desktop editor, driven by the service layer of this directory
(``web_store.py``), and the rules about the name of a person, about the
metadata and about a merge are therefore the very same rules.

The server is a FastAPI application:

| Method | Path | What it does |
|--------|------|--------------|
| GET | ``/api/status`` | whether the table is open, which one it is, what it can keep |
| POST | ``/api/connect`` | opens the table again |
| POST | ``/api/disconnect`` | closes the table |
| GET | ``/api/people`` | the enrolled people |
| GET | ``/api/people/{id}`` | the samples of one person |
| PUT | ``/api/people/{id}`` | stores the name and the metadata of a person |
| DELETE | ``/api/people/{id}`` | removes every sample of a person |
| GET | ``/api/people/{id}/merge-preview`` | what a merge would do |
| POST | ``/api/people/{id}/merge`` | merges the people into the selected one |
| GET | ``/api/people/{id}/samples/{sid}/photo`` | the picture of a face |
| POST | ``/api/metadata/format`` | rewrites a metadata document in a readable shape |

Run it with::

    python3 tools/face-db-web/web_server.py \\
        --dsn=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control

The connection string is taken from ``--dsn``, else from ``$FACE_DB_URL``,
else from ``$DATABASE_URL`` -- the same order the C++ application and the
desktop editor look them up in.

The server binds to ``127.0.0.1`` and has no authentication of its own:
it edits the table of the installation, so it belongs on the machine of
the installation, or behind a reverse proxy that does the authentication.
"""

from __future__ import annotations

import argparse
import os
import sys
from contextlib import asynccontextmanager
from typing import Any

from fastapi import Depends, FastAPI, Query, Request
from fastapi.responses import FileResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

try:
    from web_store import (
        DEFAULT_TABLE,
        FaceDbError,
        FaceDbService,
        NotConnectedError,
    )
except ImportError:  # pragma: no cover - the module sits next to this file
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from web_store import (
        DEFAULT_TABLE,
        FaceDbError,
        FaceDbService,
        NotConnectedError,
    )

try:
    # The parser of the metadata of the desktop editor. It comes with
    # the storage layer, which the service layer imports from there.
    from face_db import format_metadata, parse_metadata
except ImportError:  # pragma: no cover - the storage sits next door
    sys.path.insert(
        0,
        os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "face-db-editor",
        ),
    )
    from face_db import format_metadata, parse_metadata

#: The files of the browser.
STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")

#: The environment variables the connection string is taken from, in the
#: order the C++ application looks them up in.
DSN_ENV_VARS = ("FACE_DB_URL", "DATABASE_URL")

#: The media type of the pictures of the faces.
MEDIA_TYPES = {
    "jpeg": "image/jpeg",
    "png": "image/png",
}


class PersonUpdate(BaseModel):
    """What the browser sends when the properties of a person are stored."""

    name: str | None = Field(
        default=None, description="the name of the person, empty for none"
    )
    metadata: str | None = Field(
        default=None,
        description="the metadata document as the text the user typed",
    )


class MergeRequest(BaseModel):
    """The people that are merged into the selected one."""

    sources: list[int] = Field(
        default_factory=list, description="the ids of the people to merge in"
    )


class MetadataText(BaseModel):
    """A metadata document as the text the user typed."""

    text: str = Field(default="", description="the metadata document")


def get_service(request: Request) -> FaceDbService:
    """The service of the running application."""
    service = getattr(request.app.state, "service", None)
    if service is None:  # pragma: no cover - the app always has one
        raise NotConnectedError("the editor is not running properly.")
    return service


def parse_sources(text: str) -> list[int]:
    """The ids of the people to merge, out of a list of them.

    The ids come as a comma separated list, because they are put into the
    address of the request and a list of them does not fit into one path
    segment as well.
    """
    ids: list[int] = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            ids.append(int(part))
        except ValueError as exc:
            raise FaceDbError(f"{part!r} is not the id of a person.") from exc
    return ids


def create_app(dsn: str, table: str = DEFAULT_TABLE) -> FastAPI:
    """Builds the application for one table.

    The table is opened when the server starts, but a database that is
    not there does not stop the server: the browser is told that the
    table is closed and offers to open it again, which is what the
    desktop editor does as well.
    """
    service = FaceDbService(dsn, table)

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        try:
            service.connect()
        except FaceDbError as exc:
            service.note_error(str(exc))
        yield
        service.disconnect()

    app = FastAPI(
        title="Face database editor",
        description=(
            "Reviews and corrects the enrolled faces of objective-control."
        ),
        version="1.0.0",
        lifespan=lifespan,
    )
    app.state.service = service

    # -- the errors of the storage ------------------------------------------

    @app.exception_handler(NotConnectedError)
    async def _not_connected(request: Request, exc: NotConnectedError) -> JSONResponse:
        # The database has to be opened first: that is a state the
        # browser can get out of, so it is told about it.
        return JSONResponse({"error": str(exc)}, status_code=503)

    @app.exception_handler(FaceDbError)
    async def _face_db_error(request: Request, exc: FaceDbError) -> JSONResponse:
        # The database refused the request or the table is not in the
        # shape that is needed. The message says what went wrong.
        return JSONResponse({"error": str(exc)}, status_code=400)

    # -- the state -----------------------------------------------------------

    @app.get("/api/status")
    def status(service: FaceDbService = Depends(get_service)) -> dict[str, Any]:
        """Whether the table is open and what it can keep."""
        return service.status()

    @app.post("/api/connect")
    def connect(service: FaceDbService = Depends(get_service)) -> dict[str, Any]:
        """Opens the table again."""
        service.connect()
        return service.status()

    @app.post("/api/disconnect")
    def disconnect(service: FaceDbService = Depends(get_service)) -> dict[str, Any]:
        """Closes the table."""
        return service.disconnect()

    # -- the people ----------------------------------------------------------

    @app.get("/api/people")
    def people(service: FaceDbService = Depends(get_service)) -> dict[str, Any]:
        """The enrolled people, ordered by their id."""
        return {"people": service.list_people()}

    @app.get("/api/people/{person_id}")
    def person(
        person_id: int, service: FaceDbService = Depends(get_service)
    ) -> dict[str, Any]:
        """The samples of one person."""
        return service.get_person(person_id)

    @app.put("/api/people/{person_id}")
    def save_person(
        person_id: int,
        body: PersonUpdate,
        service: FaceDbService = Depends(get_service),
    ) -> dict[str, Any]:
        """Stores the name and the metadata of a person."""
        name = body.name.strip() or None
        return service.save_person(person_id, name, body.metadata)

    @app.delete("/api/people/{person_id}")
    def delete_person(
        person_id: int, service: FaceDbService = Depends(get_service)
    ) -> dict[str, Any]:
        """Removes every sample of a person."""
        return service.delete_person(person_id)

    # -- the merging ---------------------------------------------------------

    @app.get("/api/people/{person_id}/merge-preview")
    def merge_preview(
        person_id: int,
        sources: str = Query("", description="the ids of the people to merge in"),
        service: FaceDbService = Depends(get_service),
    ) -> dict[str, Any]:
        """What a merge would do, before it is done."""
        return service.merge_preview(person_id, parse_sources(sources))

    @app.post("/api/people/{person_id}/merge")
    def merge(
        person_id: int,
        body: MergeRequest,
        service: FaceDbService = Depends(get_service),
    ) -> dict[str, Any]:
        """Merges the people into the selected one."""
        return service.merge(person_id, body.sources)

    # -- the pictures --------------------------------------------------------

    @app.get("/api/people/{person_id}/samples/{sample_id}/photo")
    def photo(
        person_id: int,
        sample_id: int,
        service: FaceDbService = Depends(get_service),
    ) -> Response:
        """The picture of a face, as it is stored."""
        data, kind = service.get_photo(person_id, sample_id)
        return Response(
            content=data,
            media_type=MEDIA_TYPES.get(kind, "application/octet-stream"),
            headers={"Cache-Control": "no-cache"},
        )

    # -- the metadata --------------------------------------------------------

    @app.post("/api/metadata/format")
    def format_metadata_document(body: MetadataText) -> dict[str, Any]:
        """Rewrites a metadata document in a readable shape.

        The parser of the desktop editor is used, so that the browser
        shows the very same text the desktop editor would write into the
        table.
        """
        return {"text": format_metadata(parse_metadata(body.text))}

    # -- the browser ---------------------------------------------------------

    if os.path.isdir(STATIC_DIR):
        app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

        @app.get("/")
        def index() -> FileResponse:
            """The page of the editor."""
            return FileResponse(os.path.join(STATIC_DIR, "index.html"))

    return app


def find_dsn(argument: str | None) -> str | None:
    """The connection string to use: the argument, else the environment."""
    if argument:
        return argument
    for name in DSN_ENV_VARS:
        value = os.environ.get(name)
        if value:
            return value
    return None


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="web_server.py",
        description=(
            "Review and correct the enrolled faces of objective-control in a "
            "browser."
        ),
    )
    parser.add_argument(
        "--dsn",
        metavar="CONNECTION",
        help=(
            "the PostgreSQL database to open, e.g. "
            "postgres://objective_control:objective_control@127.0.0.1:5432/"
            "objective_control. Taken from $FACE_DB_URL or $DATABASE_URL when "
            "it is not given."
        ),
    )
    parser.add_argument(
        "--table",
        default=DEFAULT_TABLE,
        help="the table that holds the enrolments (default: face_enrollments)",
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="the address to listen on (default: 127.0.0.1)",
    )
    parser.add_argument(
        "--port", type=int, default=8000, help="the port to listen on (default: 8000)"
    )
    parser.add_argument(
        "--log-level",
        default="info",
        choices=["critical", "error", "warning", "info", "debug", "trace"],
        help="how much the server writes to its log (default: info)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)

    dsn = find_dsn(args.dsn)
    if not dsn:
        print(
            "No database was named. Use --dsn=..., or set $FACE_DB_URL or "
            "$DATABASE_URL.",
            file=sys.stderr,
        )
        return 2

    try:
        import uvicorn
    except ImportError:
        print(
            "The uvicorn module is missing. Install it with:\n"
            "    pip install -r requirements.txt",
            file=sys.stderr,
        )
        return 1

    app = create_app(dsn, args.table)
    print(f"Face database editor on http://{args.host}:{args.port}/")
    uvicorn.run(app, host=args.host, port=args.port, log_level=args.log_level)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
