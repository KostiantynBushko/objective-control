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
The embedding of the face -- the feature vector the recognition model
computed -- is read as well, so that the editor can show what the person
was recognised by.

The layer can also merge people: a face that was enrolled twice ends up
as two people in the table, and ``merge_people`` puts such a pair back
together. The rows keep their identity and the time the face was
enrolled; only the person they belong to changes. That is what
``objective-control-app --face-db-merge=<from>:<to>`` does.
"""

from __future__ import annotations

import json
import math
import re
from collections.abc import Sequence
from dataclasses import dataclass
from datetime import datetime
from typing import Any

import psycopg
from psycopg import sql
from psycopg.types.json import Jsonb

__all__ = [
    "DEFAULT_TABLE",
    "EMBEDDING_DIMENSION",
    "PROPERTY_COLUMNS",
    "FaceDbError",
    "FaceStore",
    "MergeResult",
    "Person",
    "Sample",
    "embedding_as_json",
    "embedding_norm",
    "format_embedding",
    "format_metadata",
    "is_postgres_dsn",
    "parse_metadata",
]

#: The table the C++ store writes to by default.
DEFAULT_TABLE = "face_enrollments"

#: The columns that hold the properties of a person. They were added after
#: the table was introduced, so a table that predates them is used without
#: them -- see ``FaceStore.missing_properties``.
PROPERTY_COLUMNS = ("user_name", "photo", "metadata")

#: The number of values the embedding of a face has. SFace produces a
#: vector of this length, and the C++ library expects the same.
EMBEDDING_DIMENSION = 128

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
class MergeResult:
    """What a merge of people did.

    The samples of the *sources* were moved to the *target*. The rows
    keep their identity and the time the face was enrolled; only the
    person they belong to changed.

    ``kept`` is the number of samples the target already had, and
    ``name`` is the name the merged person has afterwards.
    """

    target_id: int
    sources: tuple[int, ...]
    moved: int
    kept: int
    name: str | None


@dataclass(frozen=True)
class Sample:
    """One enrolled sample of a person.

    The photo is the bytes of an encoded image (a JPEG, in practice) and
    the metadata is the parsed JSON document. Both are ``None`` when the
    column is missing or the row does not fill it.

    The embedding is the feature vector the face-recognition model
    computed for the face. It is ``None`` when the table has no such a
    column; the list is empty when the column holds an empty vector.
    """

    id: int
    person_id: int
    user_name: str | None
    photo: bytes | None
    metadata: Any
    enrolled_at: datetime | None
    embedding: list[float] | None = None


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


def embedding_norm(values: Sequence[float]) -> float:
    """The length of an embedding.

    The C++ library normalises the vector before it stores it, so an
    embedding of a real face has a length of one. A length that is far
    from that says that the row was written by something else.
    """
    return math.sqrt(sum(float(value) * float(value) for value in values))


def format_embedding(values: Sequence[float]) -> str:
    """Renders an embedding as the text the editor shows.

    Every value is on a line of its own, with its index in front of it,
    so that a value can be picked out of it. The numbers are shown with
    six decimals, which is more than a single-precision float holds.
    """
    return "\n".join(
        f"{index:3d}  {float(value): .6f}" for index, value in enumerate(values)
    )


def embedding_as_json(values: Sequence[float]) -> str:
    """The embedding as a JSON array, ready to be pasted somewhere else."""
    return json.dumps([float(value) for value in values])


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
        self._has_embedding = False

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
                (
                    self.table,
                    list(PROPERTY_COLUMNS) + ["enrolled_at", "embedding"],
                ),
            )
            present = frozenset(row[0] for row in cur.fetchall())
            self._properties = frozenset(
                present.intersection(PROPERTY_COLUMNS)
            )
            self._has_enrolled_at = "enrolled_at" in present
            self._has_embedding = "embedding" in present

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
        if self._has_embedding:
            columns.append("embedding")

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
            embedding = None
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
                index += 1
            if self._has_embedding:
                embedding = row[index]

            samples.append(
                Sample(
                    id=int(row[0]),
                    person_id=int(row[1]),
                    user_name=user_name,
                    photo=photo,
                    metadata=metadata,
                    enrolled_at=enrolled_at,
                    embedding=embedding,
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

    # -- merging ------------------------------------------------------------

    def resolve_merge_name(
        self, source_ids: Sequence[int], target_id: int
    ) -> str | None:
        """The name the merged person will have.

        It is the name of the target when the target has one, and the
        name of one of the sources otherwise. That is the rule the C++
        application follows in ``--face-db-merge``: a person that was
        merged into keeps the name of the person it was merged into, and
        a target without a name takes the name of the person that was
        merged into it.

        The names are read the way ``list_people`` reads them: the
        first name of the rows of a person is the name of that person.
        """
        if "user_name" not in self._properties:
            return None

        ids = [int(value) for value in source_ids]
        conn = self._require_connection()
        query = (
            "SELECT person_id, user_name FROM "
            + self._quoted_table()
            + " WHERE person_id = ANY(%s) ORDER BY id"
        )
        try:
            with conn.cursor() as cur:
                cur.execute(query, ([target_id] + ids,))
                rows = cur.fetchall()
        except psycopg.Error as exc:
            raise FaceDbError(f"could not read the people to merge: {exc}") from exc

        names: dict[int, str] = {}
        for row in rows:
            person_id = int(row[0])
            user_name = row[1]
            if user_name and person_id not in names:
                names[person_id] = user_name

        if target_id in names:
            return names[target_id]
        for person_id in ids:
            if person_id in names:
                return names[person_id]
        return None

    def merge_people(self, source_ids: Sequence[int], target_id: int) -> MergeResult:
        """Moves the samples of some people to another one.

        The people whose samples are moved are the *sources*, and the
        person they are moved to is the *target*. The rows keep their
        identity and the time the face was enrolled; only the person
        they belong to changes. This is what
        ``objective-control-app --face-db-merge=<from>:<to>`` does.

        The name of the merged person is the name of the target, or the
        name of a source when the target has none -- see
        ``resolve_merge_name``. It is written to every sample of the
        merged person, because the name is a property of the person and
        is stored with every sample of it.

        The metadata of a moved sample says where it came from: the id
        of the person it belonged to is added to it as
        ``merged_from_person_id``, unless it is already there. A sample
        that was moved before keeps the id it came from: that is the
        more useful of the two. The metadata of the samples of the
        target is left alone -- they did not come from anywhere.

        Everything happens in one transaction: the people are either
        merged completely or not at all.
        """
        target_id = int(target_id)
        sources: list[int] = []
        for value in source_ids:
            person_id = int(value)
            if person_id != target_id and person_id not in sources:
                sources.append(person_id)

        if not sources:
            raise FaceDbError(
                "a merge needs at least one person that is merged into another one."
            )

        conn = self._require_connection()

        # The people have to be there: a merge of people that are not in
        # the table would quietly do nothing.
        counts = self._sample_counts([target_id] + sources)
        if counts.get(target_id, 0) == 0:
            raise FaceDbError(f"the person {target_id} is not in the table.")
        if not any(counts.get(person_id, 0) for person_id in sources):
            raise FaceDbError(
                "none of the people to merge is in the table: "
                + ", ".join(str(person_id) for person_id in sources)
                + "."
            )

        kept = counts.get(target_id, 0)
        name = self.resolve_merge_name(sources, target_id)

        moved = 0
        with conn.transaction():
            for person_id in sources:
                moved += self._move_samples(person_id, target_id, name)
            if name is not None and "user_name" in self._properties:
                # The name is a property of the person, so every sample
                # of the merged person carries it.
                with conn.cursor() as cur:
                    cur.execute(
                        f"UPDATE {self._quoted_table()} SET user_name = %s "
                        "WHERE person_id = %s",
                        (name, target_id),
                    )

        return MergeResult(
            target_id=target_id,
            sources=tuple(sources),
            moved=moved,
            kept=kept,
            name=name,
        )

    # -- helpers ------------------------------------------------------------

    def _sample_counts(self, person_ids: Sequence[int]) -> dict[int, int]:
        """How many samples each of the people has."""
        conn = self._require_connection()
        query = (
            "SELECT person_id, count(*) FROM "
            + self._quoted_table()
            + " WHERE person_id = ANY(%s) GROUP BY person_id"
        )
        try:
            with conn.cursor() as cur:
                cur.execute(query, (list(person_ids),))
                rows = cur.fetchall()
        except psycopg.Error as exc:
            raise FaceDbError(f"could not read the people to merge: {exc}") from exc
        return {int(row[0]): int(row[1]) for row in rows}

    def _move_samples(self, person_id: int, target_id: int, name: str | None) -> int:
        """Moves the samples of one person to another one.

        The row of every sample keeps its identity, its picture and its
        embedding; only the person it belongs to, the name of that
        person and the origin of the sample are changed.
        """
        conn = self._require_connection()
        assignments = ["person_id = %s"]
        params: list[Any] = [target_id]

        if "user_name" in self._properties:
            assignments.append("user_name = %s")
            params.append(name)

        if "metadata" in self._properties:
            # The metadata of a person says where it came from, so that
            # the origin of a sample is not lost.
            assignments.append(
                "metadata = CASE WHEN metadata ? 'merged_from_person_id' "
                "THEN metadata "
                "ELSE metadata || jsonb_build_object('merged_from_person_id', "
                "%s::int) END"
            )
            params.append(person_id)

        params.append(person_id)
        query = (
            f"UPDATE {self._quoted_table()} SET "
            + ", ".join(assignments)
            + " WHERE person_id = %s"
        )
        try:
            with conn.cursor() as cur:
                cur.execute(query, params)
                return cur.rowcount
        except psycopg.Error as exc:
            raise FaceDbError(f"could not merge the person {person_id}: {exc}") from exc

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
