"""Tests for the storage layer of the face database editor.

The tests need a PostgreSQL server, so they only run when one is named in
the environment -- the same rule the C++ test suite follows:

    FACE_DETECTION_TEST_DSN='host=localhost dbname=faces' python3 test_face_db.py

The tests never touch the table of a real installation. They create a
table of their own, with a name of their own, and drop it again when they
are done. A table that is left over from a run that was killed is dropped
at the start.

Run them with::

    python3 -m unittest discover -s tools/face-db-editor -p 'test_*.py'
"""

from __future__ import annotations

import os
import unittest

import psycopg
from psycopg.types.json import Jsonb

from face_db import (
    DEFAULT_TABLE,
    FaceDbError,
    FaceStore,
    format_metadata,
    is_postgres_dsn,
    parse_metadata,
)

#: The table the tests work on. It is not the table of an installation.
TEST_TABLE = "face_enrollments_editor_test"

#: The bytes of a tiny JPEG, so that the tests have a picture to store.
TINY_JPEG = bytes.fromhex(
    "ffd8ffe000104a46494600010100000100010000ffdb004300"
    + "08060607060508070707090908080a0c140d0c0b0b0c191213"
    + "0f141d1a1f1e1d1a1c1c20242e2720222c231c1c2837292c30"
    + "313434341f27393d38323c2e333432"
    + "ffc0000b080001000101011100ffc400"
    + "1f0000010501010101010100000000000000000102030405060708090a0b"
    + "ffc400b5100002010303020403050504040000017d01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738393a434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa"
    + "ffda0008010100003f00d2cf20ffd9"
)


def test_dsn() -> str | None:
    """The server the tests talk to, or None when there is none."""
    return os.environ.get("FACE_DETECTION_TEST_DSN") or None


def make_table(conn: psycopg.Connection) -> None:
    """Creates the table of the tests with the shape of a real one.

    The table is made with a plain connection rather than with a store:
    the store reads the shape of the table when it connects, and it
    refuses a table that is not there yet.
    """
    with conn.cursor() as cur:
        cur.execute(f"DROP TABLE IF EXISTS {TEST_TABLE}")
        cur.execute(
            f"""
            CREATE TABLE {TEST_TABLE} (
                id          bigserial   PRIMARY KEY,
                person_id   integer     NOT NULL,
                embedding   real[]      NOT NULL,
                user_name   text,
                photo       bytea,
                metadata    jsonb       NOT NULL DEFAULT '{{}}'::jsonb,
                enrolled_at timestamptz NOT NULL DEFAULT now()
            )
            """
        )


def drop_table(conn: psycopg.Connection) -> None:
    with conn.cursor() as cur:
        cur.execute(f"DROP TABLE IF EXISTS {TEST_TABLE}")


class DsnTest(unittest.TestCase):
    """The rule that tells a PostgreSQL target from a file."""

    def test_url_is_postgres(self) -> None:
        self.assertTrue(is_postgres_dsn("postgres://user@localhost/faces"))
        self.assertTrue(is_postgres_dsn("postgresql://user@localhost/faces"))

    def test_keyword_string_is_postgres(self) -> None:
        self.assertTrue(is_postgres_dsn("host=localhost dbname=faces"))
        self.assertTrue(is_postgres_dsn("  host=localhost  "))

    def test_the_default_table_is_the_one_of_the_schema(self) -> None:
        # The editor has to look at the same table the C++ store writes.
        self.assertEqual(DEFAULT_TABLE, "face_enrollments")

    def test_a_path_is_not_postgres(self) -> None:
        self.assertFalse(is_postgres_dsn("faces.db"))
        self.assertFalse(is_postgres_dsn("/var/lib/objective-control/faces.db"))
        self.assertFalse(is_postgres_dsn(""))


class MetadataTest(unittest.TestCase):
    """The metadata has to be a JSON document, like the C++ library wants it."""

    def test_object_is_accepted(self) -> None:
        self.assertEqual(parse_metadata('{"role": "admin"}'), {"role": "admin"})

    def test_empty_text_is_an_empty_document(self) -> None:
        self.assertEqual(parse_metadata(""), {})
        self.assertEqual(parse_metadata("   \n "), {})

    def test_scalars_are_accepted(self) -> None:
        self.assertEqual(parse_metadata("42"), 42)
        self.assertEqual(parse_metadata('"text"'), "text")
        self.assertEqual(parse_metadata("true"), True)
        self.assertEqual(parse_metadata("null"), None)

    def test_broken_document_is_rejected(self) -> None:
        for broken in ("{role: admin}", "{'role': 'admin'}", '{"a": 1,}', "[1, 2"):
            with self.subTest(broken=broken):
                with self.assertRaises(FaceDbError):
                    parse_metadata(broken)

    def test_nan_is_not_json(self) -> None:
        # Python's json module accepts NaN by default; JSON does not.
        with self.assertRaises(FaceDbError):
            parse_metadata("NaN")

    def test_formatting_round_trips(self) -> None:
        document = {"role": "admin", "level": 3}
        self.assertEqual(parse_metadata(format_metadata(document)), document)


class StoreTest(unittest.TestCase):
    """The store against a real server."""

    dsn = test_dsn()

    @classmethod
    def setUpClass(cls) -> None:
        if cls.dsn is None:
            raise unittest.SkipTest(
                "no PostgreSQL server named (set FACE_DETECTION_TEST_DSN)"
            )
        # The table has to exist before the store connects, because the
        # store reads the shape of the table when it connects.
        bootstrap = psycopg.connect(cls.dsn, autocommit=True)
        try:
            make_table(bootstrap)
        finally:
            bootstrap.close()

        cls.store = FaceStore(cls.dsn, TEST_TABLE)
        cls.store.connect()

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.dsn is None:
            return
        try:
            cleanup = psycopg.connect(cls.dsn, autocommit=True)
            try:
                drop_table(cleanup)
            finally:
                cleanup.close()
        finally:
            cls.store.close()

    def setUp(self) -> None:
        with self.store._require_connection().cursor() as cur:  # noqa: SLF001
            cur.execute(f"DELETE FROM {TEST_TABLE}")

    def add_sample(
        self,
        person_id: int,
        name: str | None = None,
        metadata: object | None = None,
        photo: bytes | None = None,
    ) -> None:
        # The metadata column is NOT NULL, and the C++ store writes an
        # empty document when there is no metadata, so the helper does
        # the same.
        if metadata is None:
            metadata = {}
        with self.store._require_connection().cursor() as cur:  # noqa: SLF001
            cur.execute(
                f"INSERT INTO {TEST_TABLE} (person_id, embedding, user_name, "
                "photo, metadata) VALUES (%s, %s, %s, %s, %s)",
                (person_id, [0.1] * 128, name, photo, Jsonb(metadata)),
            )

    # -- reading ------------------------------------------------------------

    def test_a_fresh_table_has_every_property(self) -> None:
        self.assertEqual(self.store.missing_properties, frozenset())
        self.assertEqual(
            self.store.available_properties, frozenset({"user_name", "photo", "metadata"})
        )

    def test_people_are_grouped_and_ordered(self) -> None:
        self.add_sample(77, "Grace Hopper")
        self.add_sample(42, "Ada Lovelace")
        self.add_sample(42, "Ada Lovelace")

        people = self.store.list_people()
        self.assertEqual([p.person_id for p in people], [42, 77])
        self.assertEqual(people[0].samples, 2)
        self.assertEqual(people[0].user_name, "Ada Lovelace")

    def test_the_largest_name_of_the_samples_is_used(self) -> None:
        # This is what the C++ store does, and the editor has to agree.
        self.add_sample(5, "Zoe")
        self.add_sample(5, "Adam")
        self.assertEqual(self.store.list_people()[0].user_name, "Zoe")

    def test_samples_are_read_with_their_properties(self) -> None:
        self.add_sample(1, "Ada", {"role": "admin"}, TINY_JPEG)
        samples = self.store.list_samples(1)
        self.assertEqual(len(samples), 1)
        self.assertEqual(samples[0].user_name, "Ada")
        self.assertEqual(samples[0].metadata, {"role": "admin"})
        self.assertEqual(samples[0].photo, TINY_JPEG)
        self.assertIsNotNone(samples[0].enrolled_at)

    def test_samples_of_one_person_keep_their_own_picture(self) -> None:
        self.add_sample(1, "Ada", None, TINY_JPEG)
        self.add_sample(1, "Ada", None, b"other")
        samples = self.store.list_samples(1)
        self.assertEqual([s.photo for s in samples], [TINY_JPEG, b"other"])

    # -- writing ------------------------------------------------------------

    def test_the_name_is_written_to_every_sample(self) -> None:
        self.add_sample(9, "Wrong")
        self.add_sample(9, "Wrong")
        self.assertEqual(self.store.set_name(9, "Right"), 2)
        for sample in self.store.list_samples(9):
            self.assertEqual(sample.user_name, "Right")

    def test_the_metadata_is_written_to_every_sample(self) -> None:
        self.add_sample(9, "Ada", {"role": "user"})
        self.add_sample(9, "Ada", {"role": "user"})
        self.assertEqual(self.store.set_metadata(9, {"role": "admin"}), 2)
        for sample in self.store.list_samples(9):
            self.assertEqual(sample.metadata, {"role": "admin"})

    def test_a_person_can_be_removed(self) -> None:
        self.add_sample(9, "Ada")
        self.add_sample(9, "Ada")
        self.add_sample(10, "Grace")
        self.assertEqual(self.store.remove_person(9), 2)
        self.assertEqual([p.person_id for p in self.store.list_people()], [10])

    def test_an_empty_name_clears_the_column(self) -> None:
        self.add_sample(9, "Ada")
        self.store.set_name(9, None)
        self.assertIsNone(self.store.list_samples(9)[0].user_name)

    # -- a table without the properties --------------------------------------

    def test_a_table_without_the_properties_is_used_as_it_is(self) -> None:
        # This is what happens when the runtime user is not allowed to
        # alter the table. The C++ store drops the properties and names
        # the ones that are lost; the editor has to know the same.
        #
        # The table has a name of its own, so that the table of the
        # other tests is left alone.
        legacy = f"{TEST_TABLE}_legacy"
        conn = psycopg.connect(self.dsn, autocommit=True)
        try:
            with conn.cursor() as cur:
                cur.execute(f"DROP TABLE IF EXISTS {legacy}")
                cur.execute(
                    f"""
                    CREATE TABLE {legacy} (
                        id          bigserial   PRIMARY KEY,
                        person_id   integer     NOT NULL,
                        embedding   real[]      NOT NULL,
                        enrolled_at timestamptz NOT NULL DEFAULT now()
                    )
                    """
                )
        finally:
            conn.close()

        store = FaceStore(self.dsn, legacy)
        store.connect()
        try:
            self.assertEqual(
                store.missing_properties, frozenset({"user_name", "photo", "metadata"})
            )
            # The people can still be read.
            self.assertEqual(store.list_people(), [])
            with self.assertRaises(FaceDbError):
                store.set_name(1, "Ada")
        finally:
            store.close()
            cleanup = psycopg.connect(self.dsn, autocommit=True)
            try:
                with cleanup.cursor() as cur:
                    cur.execute(f"DROP TABLE IF EXISTS {legacy}")
            finally:
                cleanup.close()


class MissingTableTest(unittest.TestCase):
    """A table that is not there is reported, not crashed on."""

    dsn = test_dsn()

    @unittest.skipIf(test_dsn() is None, "no PostgreSQL server named")
    def test_a_missing_table_is_reported(self) -> None:
        store = FaceStore(self.dsn, "face_enrollments_editor_no_such_table")
        with self.assertRaises(FaceDbError):
            store.connect()
        store.close()


class NotADatabaseTest(unittest.TestCase):
    """A target that is not a database is refused before anything is tried."""

    def test_a_file_is_refused(self) -> None:
        with self.assertRaises(FaceDbError):
            FaceStore("faces.db")

    def test_an_empty_target_is_refused(self) -> None:
        with self.assertRaises(FaceDbError):
            FaceStore("")


if __name__ == "__main__":
    unittest.main()
