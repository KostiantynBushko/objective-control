"""The service layer of the web version of the face database editor.

The desktop editor (see ``../face-db-editor``) opens the PostgreSQL table
of objective-control and lets the enrolled faces be reviewed and
corrected. This module is the same thing without a display: it holds
the connection to the table and offers the operations the editor needs
as plain data structures, so that a web server can put them on the wire.

The storage itself is **not** written again here. The desktop editor
already has a storage layer that is free of any GUI code --
``face_db.FaceStore`` -- and it is used as it is. That is what keeps the
two editors in step: the name of a person is written to every sample of
that person, a merge happens in one transaction, and the metadata is
checked against RFC 8259 before it is stored, all of it by the very same
code.

What this module adds is the state the desktop editor keeps in its
window: whether the database is open, which table it is and what that
table can keep. It also turns the rows into dictionaries, so that they
can be put into JSON without the web layer having to know about psycopg.

The service is safe to use from several threads: the web server runs the
requests in a thread pool, and the store is guarded by a lock.
"""

from __future__ import annotations

import os
import re
import sys
import threading
from collections.abc import Sequence
from contextlib import suppress
from typing import Any
from urllib.parse import urlsplit, urlunsplit

# The storage layer lives in the desktop editor, next door. It is
# imported from there instead of being copied, so that there is only one
# implementation of the SQL and one implementation of the rules about
# the name and the metadata of a person.
try:
    from face_db import (
        DEFAULT_TABLE,
        FaceDbError,
        FaceStore,
        embedding_norm,
        format_metadata,
        parse_metadata,
        sniff_image_format,
    )
except ImportError:  # pragma: no cover - the module sits next door
    sys.path.insert(
        0,
        os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "face-db-editor",
        ),
    )
    from face_db import (
        DEFAULT_TABLE,
        FaceDbError,
        FaceStore,
        embedding_norm,
        format_metadata,
        parse_metadata,
        sniff_image_format,
    )

__all__ = [
    "DEFAULT_TABLE",
    "FaceDbError",
    "FaceDbService",
    "NotConnectedError",
    "describe_dsn",
]


class NotConnectedError(FaceDbError):
    """Raised when an operation needs a database that is not open.

    It is a ``FaceDbError``, so that the caller that does not care about
    the difference can catch that one. The web layer does care: it tells
    the browser that the database has to be opened first.
    """


def describe_dsn(dsn: str) -> str:
    """The connection string without the password in it.

    The browser is told which database the editor is looking at, so that
    the user can see that the right one is open. The password is not part
    of that: it is taken out of the string, so that it does not travel
    over the wire and does not end up in the log of the browser.
    """
    text = dsn.strip()
    if text.startswith(("postgres://", "postgresql://")):
        parts = urlsplit(text)
        if parts.password is None:
            return text
        host = parts.hostname or ""
        if parts.port:
            host = f"{host}:{parts.port}"
        netloc = f"{parts.username}:***@{host}" if parts.username else f":***@{host}"
        return urlunsplit(
            (parts.scheme, netloc, parts.path, parts.query, parts.fragment)
        )

    # The libpq keyword form: "host=localhost dbname=faces password=secret".
    return re.sub(r"(password\s*=\s*)(\S+)", r"\1***", text, flags=re.IGNORECASE)


class FaceDbService:
    """The enrolled faces of one PostgreSQL table, as plain data.

    The service holds the connection to the table and offers the
    operations of the editor: reading the people, reading the samples of
    one of them, storing the name and the metadata of a person, removing
    a person and merging the people that are the same person.

    Every operation raises ``FaceDbError`` when the database refuses the
    request and ``NotConnectedError`` when there is no database to ask.
    """

    def __init__(self, dsn: str, table: str = DEFAULT_TABLE) -> None:
        self._dsn = dsn
        self._table = table
        self._store: FaceStore | None = None
        self._error: str | None = None
        # The requests of the web server run in a thread pool and the
        # store holds a single connection, so the operations are
        # serialised. They are all short, so a lock is enough.
        self._lock = threading.RLock()

    # -- what the service knows --------------------------------------------

    @property
    def dsn(self) -> str:
        """The connection string the service was built with."""
        return self._dsn

    @property
    def table(self) -> str:
        """The table the service works on."""
        return self._table

    @property
    def connected(self) -> bool:
        """True while the table can be read and written."""
        with self._lock:
            return self._store is not None and self._store.connected

    @property
    def error(self) -> str | None:
        """Why the last attempt to open the table failed, if it did."""
        with self._lock:
            return self._error

    def note_error(self, message: str | None) -> None:
        """Remembers why the table could not be opened."""
        with self._lock:
            self._error = message

    # -- the connection ------------------------------------------------------

    def connect(self) -> dict[str, Any]:
        """Opens the table and returns the new state.

        The table that is already open is closed first, so that a
        connection that went stale is replaced by a new one.

        Raises ``FaceDbError`` when the server refuses the connection or
        when the table is not there.
        """
        with self._lock:
            self._close_store()
            # The store is built inside the try, because a connection
            # string that does not name a database raises here.
            store = FaceStore(self._dsn, self._table)
            store.connect()
            self._store = store
            self._error = None
            return self.status()

    def disconnect(self) -> dict[str, Any]:
        """Closes the table and returns the new state."""
        with self._lock:
            self._close_store()
            self._error = None
            return self.status()

    def _close_store(self) -> None:
        if self._store is not None:
            with suppress(Exception):
                self._store.close()
            self._store = None

    def status(self) -> dict[str, Any]:
        """The state of the service, as the browser shows it."""
        with self._lock:
            store = self._store
            connected = store is not None and store.connected
            return {
                "connected": connected,
                "dsn": describe_dsn(self._dsn),
                "table": self._table,
                "available_properties": sorted(store.available_properties)
                if connected
                else [],
                "missing_properties": sorted(store.missing_properties)
                if connected
                else [],
                "error": self._error,
            }

    def _require_store(self) -> FaceStore:
        """The open store, or an error saying that there is none."""
        if self._store is None or not self._store.connected:
            raise NotConnectedError(
                "the editor is not connected to a database. Open it first."
            )
        return self._store

    # -- reading -------------------------------------------------------------

    def list_people(self) -> list[dict[str, Any]]:
        """The enrolled people, ordered by their id."""
        with self._lock:
            store = self._require_store()
            return [
                {
                    "person_id": person.person_id,
                    "samples": person.samples,
                    "user_name": person.user_name,
                }
                for person in store.list_people()
            ]

    def get_person(self, person_id: int) -> dict[str, Any]:
        """One person with all of their samples.

        The picture of a sample is not part of the answer: it is fetched
        on its own, because a list of pictures would be a heavy answer
        and the browser can ask for one picture at a time.
        """
        with self._lock:
            store = self._require_store()
            samples = store.list_samples(person_id)
            return {
                "person_id": person_id,
                "samples": [self._sample_view(sample) for sample in samples],
            }

    def _sample_view(self, sample: Any) -> dict[str, Any]:
        """One sample, as the browser shows it."""
        embedding = list(sample.embedding) if sample.embedding else None
        return {
            "id": sample.id,
            "person_id": sample.person_id,
            "user_name": sample.user_name,
            "enrolled_at": sample.enrolled_at.isoformat()
            if sample.enrolled_at
            else None,
            "has_photo": bool(sample.photo),
            "metadata": sample.metadata,
            "embedding": embedding,
            "embedding_length": embedding_norm(embedding)
            if embedding
            else None,
        }

    def get_photo(self, person_id: int, sample_id: int) -> tuple[bytes, str]:
        """The picture of a sample, with the format it is stored in.

        The bytes are given as they are stored: the browser shows a JPEG
        as well as a PNG, so there is nothing to convert.
        """
        with self._lock:
            store = self._require_store()
            for sample in store.list_samples(person_id):
                if sample.id != sample_id:
                    continue
                if not sample.photo:
                    raise FaceDbError(f"the sample {sample_id} has no picture.")
                return sample.photo, sniff_image_format(sample.photo)
            raise FaceDbError(
                f"the sample {sample_id} does not belong to the person "
                f"{person_id}."
            )

    # -- writing -------------------------------------------------------------

    def save_person(
        self, person_id: int, name: str | None, metadata_text: str | None
    ) -> dict[str, Any]:
        """Stores the name and the metadata of a person.

        The metadata comes as the text the user typed, and it is checked
        with the same parser the desktop editor uses, so that a document
        the browser accepts is a document the C++ library accepts too.

        The name and the metadata are written to every sample of the
        person, because the store answers "who is this?" without a second
        lookup and keeps that promise by storing the properties with
        every sample.
        """
        with self._lock:
            store = self._require_store()
            missing = store.missing_properties

            # The text is checked even when the table cannot keep it, so
            # that a broken document is told about either way.
            metadata = None
            if metadata_text is not None:
                metadata = parse_metadata(metadata_text)

            updated: dict[str, int] = {}
            if "user_name" not in missing:
                updated["name"] = store.set_name(person_id, name)
            if "metadata" not in missing and metadata is not None:
                updated["metadata"] = store.set_metadata(person_id, metadata)

            return {
                "person_id": person_id,
                "updated": updated,
                "skipped": sorted(missing),
            }

    def delete_person(self, person_id: int) -> dict[str, Any]:
        """Removes every sample of a person."""
        with self._lock:
            store = self._require_store()
            removed = store.remove_person(person_id)
            return {"person_id": person_id, "removed": removed}

    # -- merging -------------------------------------------------------------

    def merge_preview(
        self, target_id: int, source_ids: Sequence[int]
    ) -> dict[str, Any]:
        """What a merge would do, before it is done.

        The desktop editor shows this in the window of the merge, so that
        the user knows what is about to happen. The words are the same.
        """
        with self._lock:
            store = self._require_store()
            people = {person.person_id: person for person in store.list_people()}

            target = people.get(target_id)
            if target is None:
                raise FaceDbError(f"the person {target_id} is not in the table.")

            sources = [
                people[person_id]
                for person_id in source_ids
                if person_id in people and person_id != target_id
            ]
            if not sources:
                raise FaceDbError(
                    "pick the people that are the same person as the one above."
                )

            samples = sum(person.samples for person in sources)
            who = "person" if len(sources) == 1 else "people"
            name = store.resolve_merge_name(
                [person.person_id for person in sources], target_id
            )

            text = (
                f"{samples} sample(s) of {len(sources)} {who} will be moved "
                f"into person {target_id}."
            )
            if name:
                text += f"\nThe name of the merged person will be '{name}'."
            else:
                text += "\nThe merged person will have no name."
            text += "\nA moved sample remembers the person it came from."

            return {
                "target_id": target_id,
                "target_name": target.user_name,
                "sources": [
                    {
                        "person_id": person.person_id,
                        "user_name": person.user_name,
                        "samples": person.samples,
                    }
                    for person in sources
                ],
                "samples": samples,
                "name": name,
                "summary": text,
            }

    def merge(
        self, target_id: int, source_ids: Sequence[int]
    ) -> dict[str, Any]:
        """Merges the people into the target.

        The person that is selected is the one that stays: it keeps its
        id, and the samples of the people that are picked are moved to
        it. This is what ``objective-control-app --face-db-merge`` does,
        only by hand.
        """
        with self._lock:
            store = self._require_store()
            result = store.merge_people(source_ids, target_id)
            return {
                "target_id": result.target_id,
                "sources": list(result.sources),
                "moved": result.moved,
                "kept": result.kept,
                "name": result.name,
            }

    # -- the metadata --------------------------------------------------------

    @staticmethod
    def format_metadata_text(text: str) -> str:
        """Rewrites a metadata document in a readable shape."""
        return format_metadata(parse_metadata(text))
