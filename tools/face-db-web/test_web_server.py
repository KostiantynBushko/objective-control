"""Tests for the web version of the face database editor.

The tests that talk to a database need a PostgreSQL server, so they only
run when one is named in the environment -- the same rule the C++ test
suite and the desktop editor follow:

    FACE_DETECTION_TEST_DSN='host=localhost dbname=faces' python3 test_web_server.py

They never touch the table of a real installation. They create a table
of their own, with a name of their own, and drop it again when they are
done.

Run them with::

    python3 -m unittest discover -s tools/face-db-web -p 'test_*.py'
"""

from __future__ import annotations

import os
import unittest
from typing import Any

import psycopg
from fastapi.testclient import TestClient
from psycopg.types.json import Jsonb

from web_server import create_app, parse_sources
from web_store import FaceDbError, FaceDbService, NotConnectedError, describe_dsn

#: The table the tests work on. It is not the table of an installation.
TEST_TABLE = "face_enrollments_web_test"

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
    """Creates the table of the tests with the shape of a real one."""
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
    """The connection string that is shown to the browser."""

    def test_the_password_of_a_url_is_taken_out(self) -> None:
        shown = describe_dsn(
            "postgres://objective_control:secret@127.0.0.1:5432/objective_control"
        )
        self.assertNotIn("secret", shown)
        self.assertIn("objective_control", shown)
        self.assertIn("127.0.0.1:5432", shown)
        self.assertIn("objective_control", shown.split("@")[-1])

    def test_a_url_without_a_password_is_left_alone(self) -> None:
        dsn = "postgres://objective_control@127.0.0.1:5432/objective_control"
        self.assertEqual(describe_dsn(dsn), dsn)

    def test_the_password_of_a_keyword_string_is_taken_out(self) -> None:
        shown = describe_dsn("host=localhost dbname=faces password=secret")
        self.assertNotIn("secret", shown)
        self.assertIn("host=localhost", shown)


class SourcesTest(unittest.TestCase):
    """The ids of the people to merge come in the address of the request."""

    def test_the_ids_are_read(self) -> None:
        self.assertEqual(parse_sources("1,2,3"), [1, 2, 3])

    def test_the_spaces_are_forgiven(self) -> None:
        self.assertEqual(parse_sources(" 1 , 2 "), [1, 2])

    def test_an_empty_list_is_an_empty_list(self) -> None:
        self.assertEqual(parse_sources(""), [])

    def test_something_that_is_not_an_id_is_refused(self) -> None:
        with self.assertRaises(FaceDbError):
            parse_sources("1,abc")


class ServiceTest(unittest.TestCase):
    """The service without a database."""

    def test_a_table_that_is_not_open_cannot_be_read(self) -> None:
        service = FaceDbService("postgres://user@127.0.0.1:1/nothing")
        with self.assertRaises(NotConnectedError):
            service.list_people()

    def test_the_state_of_a_closed_table(self) -> None:
        service = FaceDbService("postgres://user@127.0.0.1:1/nothing")
        status = service.status()
        self.assertFalse(status["connected"])
        self.assertEqual(status["table"], "face_enrollments")
        self.assertIsNone(status["error"])

    def test_a_connection_string_that_is_not_a_database_is_refused(self) -> None:
        service = FaceDbService("faces.db")
        with self.assertRaises(FaceDbError):
            service.connect()

    def test_the_metadata_is_formatted_the_way_the_desktop_editor_does(self) -> None:
        text = FaceDbService.format_metadata_text('{"b": 2, "a": 1}')
        self.assertEqual(text, '{\n  "a": 1,\n  "b": 2\n}')

    def test_a_broken_metadata_is_refused(self) -> None:
        with self.assertRaises(FaceDbError):
            FaceDbService.format_metadata_text("{role: admin}")


class RouteTest(unittest.TestCase):
    """The routes that do not need a database."""

    def setUp(self) -> None:
        # A database that is not there: the server has to run anyway and
        # say that the table is closed.
        self.app = create_app("postgres://user@127.0.0.1:1/nothing", TEST_TABLE)
        self.client = TestClient(self.app)

    def test_the_state_is_told_even_without_a_database(self) -> None:
        response = self.client.get("/api/status")
        self.assertEqual(response.status_code, 200)
        self.assertFalse(response.json()["connected"])

    def test_reading_the_people_without_a_database_is_refused(self) -> None:
        response = self.client.get("/api/people")
        self.assertEqual(response.status_code, 503)
        self.assertIn("error", response.json())

    def test_a_request_without_a_database_says_what_is_wrong(self) -> None:
        response = self.client.get("/api/status")
        self.assertNotIn("password", response.text)

    def test_the_metadata_can_be_formatted_without_a_database(self) -> None:
        response = self.client.post(
            "/api/metadata/format", json={"text": '{"b": 2, "a": 1}'}
        )
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json()["text"], '{\n  "a": 1,\n  "b": 2\n}')

    def test_a_broken_metadata_is_refused(self) -> None:
        response = self.client.post(
            "/api/metadata/format", json={"text": "{role: admin}"}
        )
        self.assertEqual(response.status_code, 400)
        self.assertIn("not a valid JSON document", response.json()["error"])

    def test_the_page_of_the_editor_is_served(self) -> None:
        response = self.client.get("/")
        self.assertEqual(response.status_code, 200)
        self.assertIn("Face database editor", response.text)

    def test_the_files_of_the_browser_are_served(self) -> None:
        response = self.client.get("/static/app.js")
        self.assertEqual(response.status_code, 200)

    def test_the_password_is_not_told_to_the_browser(self) -> None:
        # A connection string with a password of its own, so that the
        # check cannot be confused by the name of the user.
        app = create_app("postgres://user:hunter2@127.0.0.1:5432/faces", TEST_TABLE)
        client = TestClient(app)
        try:
            response = client.get("/api/status")
            self.assertNotIn("hunter2", response.text)
            self.assertIn("user", response.text)
        finally:
            client.close()

    def test_an_address_that_does_not_exist_is_not_found(self) -> None:
        response = self.client.get("/api/nothing")
        self.assertEqual(response.status_code, 404)


class WebStoreTest(unittest.TestCase):
    """The editor against a real server, through the web API."""

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

        cls.app = create_app(cls.dsn, TEST_TABLE)
        cls.service: FaceDbService = cls.app.state.service
        cls.service.connect()
        cls.client = TestClient(cls.app)

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.dsn is None:
            return
        try:
            cls.service.disconnect()
            cleanup = psycopg.connect(cls.dsn, autocommit=True)
            try:
                drop_table(cleanup)
            finally:
                cleanup.close()
        finally:
            cls.client.close()

    def cursor(self) -> Any:
        """A cursor on the connection of the service."""
        store = self.service._require_store()  # noqa: SLF001
        return store._require_connection().cursor()  # noqa: SLF001

    def setUp(self) -> None:
        with self.cursor() as cur:
            cur.execute(f"DELETE FROM {TEST_TABLE}")

    def add_sample(
        self,
        person_id: int,
        name: str | None = None,
        metadata: object | None = None,
        photo: bytes | None = None,
    ) -> int:
        if metadata is None:
            metadata = {}
        with self.cursor() as cur:
            cur.execute(
                f"INSERT INTO {TEST_TABLE} (person_id, embedding, user_name, "
                "photo, metadata) VALUES (%s, %s, %s, %s, %s) RETURNING id",
                (person_id, [0.1] * 128, name, photo, Jsonb(metadata)),
            )
            return cur.fetchone()[0]

    # -- the state -----------------------------------------------------------

    def test_the_table_is_open(self) -> None:
        response = self.client.get("/api/status")
        self.assertEqual(response.status_code, 200)
        body = response.json()
        self.assertTrue(body["connected"])
        self.assertEqual(body["table"], TEST_TABLE)
        self.assertEqual(body["missing_properties"], [])

    def test_the_table_can_be_opened_again(self) -> None:
        response = self.client.post("/api/connect")
        self.assertEqual(response.status_code, 200)
        self.assertTrue(response.json()["connected"])

    def test_the_table_can_be_closed(self) -> None:
        response = self.client.post("/api/disconnect")
        self.assertEqual(response.status_code, 200)
        self.assertFalse(response.json()["connected"])

        # And it can be opened again afterwards.
        self.client.post("/api/connect")
        self.assertTrue(self.client.get("/api/status").json()["connected"])

    # -- the people ----------------------------------------------------------

    def test_the_people_are_listed(self) -> None:
        self.add_sample(42, "Ada Lovelace")
        self.add_sample(42, "Ada Lovelace")
        self.add_sample(77, "Grace Hopper")

        response = self.client.get("/api/people")
        self.assertEqual(response.status_code, 200)

        people = response.json()["people"]
        self.assertEqual([person["person_id"] for person in people], [42, 77])
        self.assertEqual(people[0]["samples"], 2)
        self.assertEqual(people[0]["user_name"], "Ada Lovelace")

    def test_the_samples_of_a_person_are_listed(self) -> None:
        self.add_sample(1, "Ada", {"role": "admin"}, TINY_JPEG)

        response = self.client.get("/api/people/1")
        self.assertEqual(response.status_code, 200)

        samples = response.json()["samples"]
        self.assertEqual(len(samples), 1)
        self.assertEqual(samples[0]["user_name"], "Ada")
        self.assertEqual(samples[0]["metadata"], {"role": "admin"})
        self.assertTrue(samples[0]["has_photo"])
        self.assertIsNotNone(samples[0]["enrolled_at"])
        self.assertEqual(len(samples[0]["embedding"]), 128)
        # The rows of the test hold [0.1] * 128, so the length of the
        # vector is the square root of 128 * 0.01.
        self.assertAlmostEqual(
            samples[0]["embedding_length"], (128 * 0.01) ** 0.5, places=6
        )

    def test_a_person_without_samples_gives_an_empty_list(self) -> None:
        response = self.client.get("/api/people/999")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json()["samples"], [])

    # -- the picture ---------------------------------------------------------

    def test_the_picture_of_a_face_is_served(self) -> None:
        sample_id = self.add_sample(1, "Ada", photo=TINY_JPEG)

        response = self.client.get(f"/api/people/1/samples/{sample_id}/photo")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.headers["content-type"], "image/jpeg")
        self.assertEqual(response.content, TINY_JPEG)

    def test_the_picture_of_a_sample_of_another_person_is_refused(self) -> None:
        sample_id = self.add_sample(1, "Ada", photo=TINY_JPEG)

        response = self.client.get(f"/api/people/2/samples/{sample_id}/photo")
        self.assertEqual(response.status_code, 400)
        self.assertIn("does not belong", response.json()["error"])

    def test_a_sample_without_a_picture_says_so(self) -> None:
        sample_id = self.add_sample(1, "Ada")

        response = self.client.get(f"/api/people/1/samples/{sample_id}/photo")
        self.assertEqual(response.status_code, 400)
        self.assertIn("no picture", response.json()["error"])

    # -- storing the changes -------------------------------------------------

    def test_the_name_is_stored_with_every_sample(self) -> None:
        self.add_sample(9, "Wrong")
        self.add_sample(9, "Wrong")

        response = self.client.put(
            "/api/people/9", json={"name": "Right", "metadata": "{}"}
        )
        self.assertEqual(response.status_code, 200)

        samples = self.client.get("/api/people/9").json()["samples"]
        self.assertEqual({sample["user_name"] for sample in samples}, {"Right"})

    def test_the_metadata_is_stored_with_every_sample(self) -> None:
        self.add_sample(9, "Ada")

        response = self.client.put(
            "/api/people/9",
            json={"name": "Ada", "metadata": '{"role": "admin"}'},
        )
        self.assertEqual(response.status_code, 200)

        samples = self.client.get("/api/people/9").json()["samples"]
        self.assertEqual(
            [sample["metadata"] for sample in samples], [{"role": "admin"}]
        )

    def test_an_empty_name_clears_the_name(self) -> None:
        self.add_sample(9, "Ada")

        self.client.put("/api/people/9", json={"name": "  ", "metadata": "{}"})

        samples = self.client.get("/api/people/9").json()["samples"]
        self.assertIsNone(samples[0]["user_name"])

    def test_a_broken_metadata_is_refused_and_nothing_is_stored(self) -> None:
        self.add_sample(9, "Ada")

        response = self.client.put(
            "/api/people/9", json={"name": "Changed", "metadata": "{role: admin}"}
        )
        self.assertEqual(response.status_code, 400)
        self.assertIn("not a valid JSON document", response.json()["error"])

        # The name was not stored either: the whole request failed.
        samples = self.client.get("/api/people/9").json()["samples"]
        self.assertEqual(samples[0]["user_name"], "Ada")

    # -- removing a person ----------------------------------------------------

    def test_a_person_is_removed_with_every_sample(self) -> None:
        self.add_sample(9, "Ada")
        self.add_sample(9, "Ada")
        self.add_sample(10, "Grace")

        response = self.client.delete("/api/people/9")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json()["removed"], 2)

        people = self.client.get("/api/people").json()["people"]
        self.assertEqual([person["person_id"] for person in people], [10])

    # -- the merge ------------------------------------------------------------

    def test_what_a_merge_would_do_is_told_before_it_is_done(self) -> None:
        self.add_sample(1, "Ada")
        self.add_sample(2, "Bob")
        self.add_sample(2, "Bob")

        response = self.client.get("/api/people/1/merge-preview?sources=2")
        self.assertEqual(response.status_code, 200)

        preview = response.json()
        self.assertEqual(preview["samples"], 2)
        self.assertEqual(preview["name"], "Ada")
        self.assertIn("will be moved into person 1", preview["summary"])

    def test_a_merge_without_a_source_is_refused(self) -> None:
        self.add_sample(1, "Ada")

        response = self.client.get("/api/people/1/merge-preview?sources=")
        self.assertEqual(response.status_code, 400)

    def test_the_people_are_merged_into_the_selected_one(self) -> None:
        self.add_sample(1, "Ada")
        self.add_sample(2, "Bob")
        self.add_sample(2, "Bob")

        response = self.client.post("/api/people/1/merge", json={"sources": [2]})
        self.assertEqual(response.status_code, 200)

        result = response.json()
        self.assertEqual(result["moved"], 2)
        self.assertEqual(result["kept"], 1)
        self.assertEqual(result["target_id"], 1)
        self.assertEqual(result["name"], "Ada")

        people = self.client.get("/api/people").json()["people"]
        self.assertEqual([person["person_id"] for person in people], [1])

    def test_a_moved_sample_remembers_where_it_came_from(self) -> None:
        self.add_sample(1, "Ada")
        self.add_sample(2, "Bob")

        self.client.post("/api/people/1/merge", json={"sources": [2]})

        samples = self.client.get("/api/people/1").json()["samples"]
        moved = [sample for sample in samples if sample["metadata"]]
        self.assertEqual(len(moved), 1)
        self.assertEqual(moved[0]["metadata"], {"merged_from_person_id": 2})

    def test_a_merge_of_a_person_that_is_not_there_is_refused(self) -> None:
        self.add_sample(1, "Ada")

        response = self.client.post("/api/people/1/merge", json={"sources": [99]})
        self.assertEqual(response.status_code, 400)


if __name__ == "__main__":
    unittest.main()
