"""Fills a table of the face database with demo people.

The table of an installation is written by the application, when it
enrols a face in front of a camera. That needs a camera and a face, so
it is not the quickest way to see what the editor does with the rows it
finds.

This script writes a few made-up people into a table of their own, with
a drawn picture, a name and a metadata document, so that the editor can
be opened on something. It writes to ``face_enrollments_demo`` by
default and refuses to write to ``face_enrollments`` unless it is told
to, because that is the table of the installation.

Run it with::

    python3 seed_demo_data.py --dsn=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control

and then open the editor on the table it names::

    python3 face_db_editor.py --dsn=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control \\
        --table=face_enrollments_demo
"""

from __future__ import annotations

import argparse
import io
import os
import random
import sys
from typing import Any

import psycopg
from psycopg.types.json import Jsonb

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from face_db import DEFAULT_TABLE, is_postgres_dsn

#: The table the demo people go into. It is not the table of an
#: installation.
DEMO_TABLE = "face_enrollments_demo"

#: The people that are written.
DEMO_PEOPLE = [
    (1, "Ada Lovelace", {"role": "admin", "team": "engineering"}),
    (2, "Grace Hopper", {"role": "user", "team": "engineering"}),
    (3, "Alan Turing", {"role": "user", "team": "research"}),
    (4, "Katherine Johnson", {"role": "user", "team": "research"}),
]

#: The length of the embedding the SFace model produces.
EMBEDDING_SIZE = 128


def draw_face(name: str) -> bytes:
    """Draws a picture of a face and returns it as the bytes of a PNG.

    The picture is not a photograph of anybody; it is a drawing, so that
    the preview of the editor has something to show. The real pictures
    are the JPEG frames the application captures.
    """
    from PIL import Image, ImageDraw

    width = height = 320
    image = Image.new("RGB", (width, height), (58, 72, 92))
    draw = ImageDraw.Draw(image)

    # A face, drawn from an oval and a few features.
    draw.ellipse((80, 50, 240, 250), fill=(226, 189, 160))
    draw.ellipse((115, 120, 145, 136), fill=(40, 40, 48))
    draw.ellipse((175, 120, 205, 136), fill=(40, 40, 48))
    draw.arc((130, 165, 190, 200), start=0, end=180, fill=(150, 90, 80), width=4)
    draw.arc((100, 80, 140, 110), start=200, end=340, fill=(90, 70, 60), width=3)
    draw.arc((180, 80, 220, 110), start=200, end=340, fill=(90, 70, 60), width=3)

    # The name of the person is written under the face, so that the
    # picture says who it is.
    draw.text((12, height - 24), name, fill=(210, 214, 224))

    buffer = io.BytesIO()
    image.save(buffer, format="PNG")
    return buffer.getvalue()


def make_embedding(rng: random.Random) -> list[float]:
    """A random embedding of the length the SFace model produces.

    The values are not the ones of a real face, so the application will
    not recognise anybody with them. They only have to be there, because
    the column is not null.
    """
    values = [rng.gauss(0.0, 1.0) for _ in range(EMBEDDING_SIZE)]
    length = sum(value * value for value in values) ** 0.5
    return [value / length for value in values]


def create_table(conn: psycopg.Connection, table: str) -> None:
    """Creates the table of the demo people, if it is not there yet."""
    with conn.cursor() as cur:
        cur.execute(f'DROP TABLE IF EXISTS "{table}"')
        cur.execute(
            f"""
            CREATE TABLE "{table}" (
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
        cur.execute(
            f'CREATE INDEX IF NOT EXISTS "{table}_person_id_idx" ON "{table}" (person_id)'
        )


def seed(conn: psycopg.Connection, table: str) -> int:
    """Writes the demo people into the table."""
    rng = random.Random(20240611)
    written = 0
    with conn.cursor() as cur:
        for person_id, name, metadata in DEMO_PEOPLE:
            photo = draw_face(name)
            # Two samples of the first person, so that the list of the
            # samples of a person has something in it.
            samples = 2 if person_id == 1 else 1
            for _ in range(samples):
                cur.execute(
                    f'INSERT INTO "{table}" '
                    "(person_id, embedding, user_name, photo, metadata) "
                    "VALUES (%s, %s, %s, %s, %s)",
                    (person_id, make_embedding(rng), name, photo, Jsonb(metadata)),
                )
                written += 1
    return written


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="seed_demo_data.py",
        description="Fill a table of the face database with demo people.",
    )
    parser.add_argument(
        "--dsn",
        metavar="CONNECTION",
        help=(
            "the PostgreSQL database to write to, e.g. "
            "postgres://objective_control:objective_control@127.0.0.1:5432/objective_control. Taken from "
            "$FACE_DB_URL or $DATABASE_URL when it is not given."
        ),
    )
    parser.add_argument(
        "--table",
        default=DEMO_TABLE,
        help=f"the table to write to (default: {DEMO_TABLE})",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help=(
            "allow writing to the table of the installation "
            f"({DEFAULT_TABLE}), which is refused without this flag"
        ),
    )
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    dsn = args.dsn or os.environ.get("FACE_DB_URL") or os.environ.get("DATABASE_URL")
    if not dsn:
        print("No database was named. Use --dsn=..., or set $FACE_DB_URL.", file=sys.stderr)
        return 2

    if not is_postgres_dsn(dsn):
        print(f"{dsn!r} is not a PostgreSQL connection string.", file=sys.stderr)
        return 2

    table = args.table.strip() or DEMO_TABLE
    if table == DEFAULT_TABLE and not args.force:
        print(
            f"Refusing to write to {DEFAULT_TABLE}, which is the table of the "
            "installation. Use --force if that is what you want, or name "
            f"another table with --table (the default is {DEMO_TABLE}).",
            file=sys.stderr,
        )
        return 2

    try:
        conn = psycopg.connect(dsn, autocommit=True)
    except psycopg.Error as exc:
        print(f"Could not connect to the database: {exc}", file=sys.stderr)
        return 1

    try:
        create_table(conn, table)
        written = seed(conn, table)
    except psycopg.Error as exc:
        print(f"Could not write the demo people: {exc}", file=sys.stderr)
        return 1
    finally:
        conn.close()

    print(f"Wrote {written} sample(s) of {len(DEMO_PEOPLE)} people into {table}.")
    print("Open the editor on it with:")
    print(f"    python3 face_db_editor.py --dsn='{dsn}' --table={table}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
