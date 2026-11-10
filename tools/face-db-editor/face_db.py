"""Storage layer of the face database editor.

The editor reviews the enrolled faces of the objective-control project and
lets a user fix up what is stored about them. It talks to the very same
PostgreSQL table the C++ library writes, so an enrolment made with
``objective-control-app --enroll=...`` can be renamed, given metadata or a
different picture here, and the application picks the change up the next
time it loads the store.

The layer is deliberately free of any GUI code. The editor itself lives in
``face_db_editor.py``; the tests in ``test_face_db.py`` drive this module
without a display, and the SQL it builds can be read on its own.

The column names and the semantics of the rows are the ones of
``third_party/face-detection/sql/schema.sql``: one row per enrolled sample,
with the properties of the person duplicated into every row of that person.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from datetime import datetime
from typing import Any

import psycopg
from psycopg import sql
from psycopg.types.json import Jsonb

__all__ = [
    "DEFAULT_TABLE",
    "PROPERTY_COLUMNS",
    "FaceDbError",
    "FaceStore",
    "Person",
    "Sample",
    "is_postgres_dsn",
    "parse_metadata",
    "format_metadata",
]

#: The table the C++ store writes to by default.
DEFAULT_TABLE = "face_enrollments"

#: The columns that hold the properties of a person. They were added after
#: the table was introduced, so a table that predates them is used without
#: them -- see ``FaceStore.missing_properties``.
PROPERTY_COLUMNS = ("user_name", "photo", "metadata")

# A libpq connection string in keyword form, e.g. "host=localhost dbname=x".
_KEYWORD_DSN = re.compile(r"^\s*[A-Za-z_][A-Za-z0-9_]*\s*=")


class FaceDbError(RuntimeError):
    """Raised when the database cannot be read or written.

    The message is meant for the user of the editor, so it says what went
    wrong rather than where.
    """


@dataclass(frozen=True)
class Person:
    """One enrolled person, as the list of them shows it."""

    person_id: int
    samples: int
    user_name: str | None


@dataclass(frozen=True)
class Sample:
    """One enrolled sample of a person.

    The photo is the bytes of an encoded image (a JPEG, in practice) and
    the metadata is the parsed JSON document. Both are ``None`` when the
    column is missing or the row does not fill it.
    """

    id: int
    person_id: int
    user_name: str | None
    photo: bytes | None
    metadata: Any
    enrolled_at: datetime | None


def is_postgres_dsn(dsn: str) -> bool:
    """True when *dsn* names a PostgreSQL database rather than a file.

    The C++ store draws the line the same way: a target that starts with
    ``postgres://`` or ``postgresql://`` is a PostgreSQL one, everything
    else is a file. This tool only manages PostgreSQL, so it accepts a
    libpq keyword connection string (``host=... dbname=...``) as well --
    libpq understands that form and so does psycopg.
    """
    text = dsn.strip()
    if text.startswith(("postgres://", "postgresql://")):
        return True
    return bool(_KEYWORD_DSN.match(text))


def parse_metadata(text: str) -> Any:
    """Parses a metadata document and returns it as a Python value.

    The check follows RFC 8259, like the C++ library's ``isValidJson()``:
    the six value types are accepted, trailing commas and unquoted keys are
    not. Python's ``json`` module accepts ``NaN`` and ``Infinity`` by
    default, which are not JSON, so they are rejected here.

    Raises ``FaceDbError`` with a message that names the problem.
    """
    stripped = text.strip()
    if not stripped:
        return {}
    try:
        return json.loads(stripped, parse_constant=_reject_constant)
    except ValueError as exc:
        raise FaceDbError(f"the metadata is not a valid JSON document: {exc}") from exc


def _reject_constant(name: str) -> Any:
    """Rejects the ``NaN``/``Infinity``/``-Infinity`` extension of JSON."""
    raise ValueError(f"{name} is not a JSON value")


def format_metadata(metadata: Any) -> str:
    """Renders a metadata value as the text the editor shows and stores."""
    if metadata is None:
        return "{}"
    if isinstance(metadata, str):
        # A JSON string is stored as a document, so it is shown quoted.
        return json.dumps(metadata, indent=2, ensure_ascii=False)
    return json.dumps(metadata, indent=2, ensure_ascii=False, sort_keys=True)


class FaceStore:
    """The enrolled faces of one PostgreSQL table.

    The store opens the connection itself and keeps it for its lifetime.
    Every method raises ``FaceDbError`` when the database refuses the
    request; the editor turns that into a message for the user.
    """

    def __init__(self, dsn: str, table: str = DEFAULT_TABLE) -> None:
        if not is_postgres_dsn(dsn):
            raise FaceDbError(
                f"{dsn!r} is not a PostgreSQL connection string. Use "
                f"postgres://user@host/database, or a libpq keyword string "
                f"such as \"host=localhost dbname=objective_control\"."
            )
        self.dsn = dsn
        self.table = table
        self._conn: psycopg.Connection | None = None
        self._properties: frozenset[str] = frozenset()
        self._has_enrolled_at = False

    # -- connection --------------------------------------------------------

    @property
    def connected(self) -> bool:
        """True while the store can be used."""
        return self._conn is not None and not self._conn.closed

    def connect(self) -> None:
        """Opens the connection and finds out what the table holds.

        Raises ``FaceDbError`` when the server refuses the connection or
        when the table does not exist.
        """
        if self.connected:
            return
        try:
            # autocommit, so that a change the user made is stored right
            # away and a crash cannot leave a transaction open.
            self._conn = psycopg.connect(self.dsn, autocommit=True)
        except psycopg.Error as exc:
            self._conn = None
            raise FaceDbError(f"could not connect to the database: {exc}") from exc

        try:
            self._read_shape()
        except Exception:
            self.close()
            raise

    def close(self) -> None:
        """Closes the connection. Safe to call more than once."""
        if self._conn is not None:
            try:
                self._conn.close()
            except psycopg.Error:
                pass
            self._conn = None

    def _read_shape(self) -> None:
        """Finds out which columns the table has.

        The properties were added after the table was introduced, and a
        table that the runtime user is not allowed to alter keeps the old
        shape. The editor has to know, so it can say what it cannot keep
        instead of failing on the first write.
        """
        conn = self._require_connection()
        with conn.cursor() as cur:
            cur.execute("SELECT to_regclass(%s)", (self.table,))
            row = cur.fetchone()
            if row is None or row[0] is None:
                raise FaceDbError(
                    f'the table "{self.table}" does not exist. Create it with '
                    f"objective-control-app --face-db-init --face-db=<target>."
                )

            cur.execute(
                "SELECT a.attname FROM pg_attribute a "
                "WHERE a.attrelid = %s::regclass "
                "  AND a.attnum > 0 AND NOT a.attisdropped "
                "  AND a.attname = ANY(%s)",
                (self.table, list(PROPERTY_COLUMNS)),
            )
            self._properties = frozenset(row[0] for row in cur.fetchall())

            cur.execute(
                "SELECT a.attname FROM pg_attribute a "
                "WHERE a.attrelid = %s::regclass "
                "  AND a.attnum > 0 AND NOT a.attisdropped "
                "  AND a.attname = 'enrolled_at'",
                (self.table,),
            )
            self._has_enrolled_at = cur.fetchone() is not None

    # -- what the table can hold -------------------------------------------

    @property
    def available_properties(self) -> frozenset[str]:
        """The properties the table can keep."""
        return self._properties

    @property
    def missing_properties(self) -> frozenset[str]:
        """The properties the table cannot keep.

        A table that predates the properties, and that the runtime user is
        not allowed to alter, is used as it is. The C++ store drops the
        properties on the way in and names the ones that are lost; this
        store does the same, so that the editor can tell the user.
        """
        return frozenset(PROPERTY_COLUMNS) - self._properties

    # -- reading ------------------------------------------------------------

    def list_people(self) -> list[Person]:
        """The enrolled people, ordered by their id.

        The name is the largest one of the samples of a person, which is
        what the C++ store does as well: the name is stored with every
        sample, so any of them will do.
        """
        conn = self._require_connection()
        columns = ["person_id", "count(*)"]
        if "user_name" in self._properties:
            columns.append("max(user_name)")
        query = (
            "SELECT " + ", ".join(columns) + f" FROM {self._quoted_table()} "
            "GROUP BY person_id ORDER BY person_id"
        )
        try:
            with conn.cursor() as cur:
                cur.execute(query)
                rows = cur.fetchall()
        except psycopg.Error as exc:
            raise FaceDbError(f"could not read the enrolled people: {exc}") from exc

        people: list[Person] = []
        for row in rows:
            people.append(
                Person(
                    person_id=int(row[0]),
                    samples=int(row[1]),
                    user_name=row[2] if len(row) > 2 else None,
                )
            )
        return people

    def list_samples(self, person_id: int) -> list[Sample]:
        """The samples of one person, oldest first."""
        conn = self._require_connection()
        columns = ["id", "person_id"]
        if "user_name" in self._properties:
            columns.append("user_name")
        if "photo" in self._properties:
            columns.append("photo")
        if "metadata" in self._properties:
            columns.append("metadata")
        if self._has_enrolled_at:
            columns.append("enrolled_at")

        query = (
            "SELECT " + ", ".join(columns) + f" FROM {self._quoted_table()} "
            "WHERE person_id = %s ORDER BY id"
        )
        try:
            with conn.cursor() as cur:
                cur.execute(query, (person_id,))
                rows = cur.fetchall()
        except psycopg.Error as exc:
            raise FaceDbError(f"could not read the samples of {person_id}: {exc}") from exc

        samples: list[Sample] = []
        for row in rows:
            index = 2
            user_name = None
            photo = None
            metadata = None
            enrolled_at = None
            if "user_name" in self._properties:
                user_name = row[index]
                index += 1
            if "photo" in self._properties:
                photo = row[index]
                index += 1
            if "metadata" in self._properties:
                metadata = row[index]
                index += 1
            if self._has_enrolled_at:
                enrolled_at = row[index]

            samples.append(
                Sample(
                    id=int(row[0]),
                    person_id=int(row[1]),
                    user_name=user_name,
                    photo=photo,
                    metadata=metadata,
                    enrolled_at=enrolled_at,
                )
            )
        return samples

    # -- writing ------------------------------------------------------------

    def set_name(self, person_id: int, name: str | None) -> int:
        """Sets the name of a person and returns the number of changed rows.

        The name is stored with every sample of the person, so all of them
        are updated. Leaving some of them behind would let the C++ store
        pick a stale name, because it takes the largest one.
        """
        if "user_name" not in self._properties:
            raise FaceDbError(
                "the table has no user_name column, so the name of a person "
                "cannot be stored."
            )
        conn = self._require_connection()
        try:
            with conn.cursor() as cur:
                cur.execute(
                    f"UPDATE {self._quoted_table()} SET user_name = %s "
                    "WHERE person_id = %s",
                    (name, person_id),
                )
                return cur.rowcount
        except psycopg.Error as exc:
            raise FaceDbError(f"could not set the name of {person_id}: {exc}") from exc

    def set_metadata(self, person_id: int, metadata: Any) -> int:
        """Sets the metadata of a person and returns the number of changed rows.

        The metadata is stored with every sample of the person, so all of
        them are updated -- the same reason as for the name.
        """
        if "metadata" not in self._properties:
            raise FaceDbError(
                "the table has no metadata column, so the metadata of a "
                "person cannot be stored."
            )
        conn = self._require_connection()
        try:
            with conn.cursor() as cur:
                cur.execute(
                    f"UPDATE {self._quoted_table()} SET metadata = %s "
                    "WHERE person_id = %s",
                    (Jsonb(metadata), person_id),
                )
                return cur.rowcount
        except psycopg.Error as exc:
            raise FaceDbError(
                f"could not set the metadata of {person_id}: {exc}"
            ) from exc

    def remove_person(self, person_id: int) -> int:
        """Removes every sample of a person and returns how many were removed.

        This is what ``objective-control-app --face-db-remove=<id>`` does.
        """
        conn = self._require_connection()
        try:
            with conn.cursor() as cur:
                cur.execute(
                    f"DELETE FROM {self._quoted_table()} WHERE person_id = %s",
                    (person_id,),
                )
                return cur.rowcount
        except psycopg.Error as exc:
            raise FaceDbError(
                f"could not remove the person {person_id}: {exc}"
            ) from exc

    # -- helpers ------------------------------------------------------------

    def _quoted_table(self) -> str:
        """The table name, quoted the way the server expects it.

        The name is an identifier, not a value, so it is quoted instead of
        being passed as a parameter. ``Identifier`` doubles the quotes
        inside the name, which is what keeps a name like
        ``faces"; DROP TABLE`` harmless.
        """
        return sql.Identifier(self.table).as_string(self._require_connection())

    def _require_connection(self) -> psycopg.Connection:
        if self._conn is None or self._conn.closed:
            raise FaceDbError("the editor is not connected to a database.")
        return self._conn

    def __enter__(self) -> "FaceStore":
        self.connect()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()
