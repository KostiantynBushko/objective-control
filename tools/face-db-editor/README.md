# Face database editor

A small desktop application that opens the PostgreSQL table of
objective-control and lets the enrolled faces be reviewed and corrected.

The application writes an enrolment once, when a person is enrolled, and
what it stores about that person is rarely right afterwards: a name gets
misspelled, a piece of metadata turns out to be missing, the wrong
picture was captured. This editor opens the same table and fixes that.

It talks to the table the C++ library writes, so an enrolment made with
`objective-control-app --enroll=42` can be renamed, given metadata or a
different picture here, and the application picks the change up the next
time it loads the store.

## What it does

* lists the enrolled people, with their id and how many samples each
  of them has, and filters the list as you type,
* shows the samples of the selected person, with the picture of the face
  that was captured at the time,
* shows the name and the metadata of the person and stores them again,
* shows the embedding of the selected sample -- the vector the recognition
  model computed for the face -- and copies it to the clipboard,
* saves the picture of a face to a file,
* merges the people that are the same person into one another,
* removes a person and every sample of that person.

The properties are stored with *every* sample of a person, so that the
store can answer "who is this?" without a second lookup. The editor
keeps that promise: a change to the name or to the metadata is written to
all the samples of the person, exactly as the C++ store writes them when
it enrols somebody.

## Running it

The editor needs Python 3.10 or newer and a PostgreSQL server. It is a
Tk application, so it needs a display -- an X server or a Wayland
session.

```sh
pip install -r requirements.txt

python3 tools/face-db-editor/face_db_editor.py \
    --dsn='postgres://objective_control:objective_control@127.0.0.1:5432/objective_control'
```

The connection string is taken from `--dsn`, else from `$FACE_DB_URL`,
else from `$DATABASE_URL` -- the same order the C++ application looks
them up in:

```sh
export FACE_DB_URL='postgres://objective_control:objective_control@127.0.0.1:5432/objective_control'
python3 tools/face-db-editor/face_db_editor.py
```

`--table` names the table to open; it is `face_enrollments` by default,
the same name `PostgresFaceStore::Options::table` uses.

Both a URL (`postgres://user@host/database`) and a libpq keyword string
(`host=localhost dbname=objective_control`) are accepted. The C++
application only takes the URL form, because a target that does not start
with `postgres://` is a file there.

## Merging people

A face that was enrolled twice -- in two sessions, with another light and
another distance -- ends up as two people in the table. The picture of a
person does not tell the two apart: they look like one person, which is
exactly why the application could not keep them together.

The **Merge person...** button -- it is under the list of the people --
puts such a pair back together. The person that is selected in the list
is the one that stays: it keeps its id, and the samples of the people
that are picked in the window are moved to it.

```
Merge people

The samples of the people below are moved into person 1 (Ada Lovelace).
The person keeps its id, and the rows keep the time they were enrolled.

  Id   Name          Samples
   2                 1
   3   Bob           2

3 sample(s) of 2 people will be moved into person 1.
The name of the merged person will be 'Ada Lovelace'.
A moved sample remembers the person it came from.
```

What the merge does:

| | |
|---|---|
| the samples of the people that are picked | are moved to the selected person |
| the id of the selected person | does not change |
| the rows themselves | keep their identity, their picture, their embedding and the time the face was enrolled |
| the name | is the name of the selected person, or -- when it has none -- the name of one of the people that are merged into it |
| the metadata of a moved sample | keeps what it had, and says where the sample came from: `merged_from_person_id` |
| the metadata of the samples of the selected person | is left alone |

The name is a property of the person and is stored with every sample of
it, so it is written to every sample of the merged person. The metadata
of a sample that was moved before keeps the id it came from, because that
is the more useful of the two.

Everything happens in one transaction: the people are either merged
completely or not at all.

This is the same thing that
`objective-control-app --face-db-merge=<from>:<to>` does, only by hand:

```sh
./build/debug/bin/objective-control-app \
    --face-db=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control \
    --face-db-merge=9,10:1
```

## The table

The editor works on the table of `third_party/face-detection/sql/schema.sql`:
one row per enrolled sample, with the properties of the person duplicated
into every row of that person.

| Column | Meaning |
|--------|---------|
| `person_id` | the id of the person |
| `embedding` | the 128-element vector SFace produced |
| `user_name` | the name shown on screen when the person is recognised |
| `photo` | a picture of the face, as the bytes of an encoded image (a JPEG) |
| `metadata` | an arbitrary JSON document about the person |
| `enrolled_at` | when the sample was enrolled |

The three property columns were added after the table was introduced. A
table that predates them -- and that the database user is not allowed to
alter -- is used as it is: the editor says which columns it cannot keep
and does not offer to edit them, the same way the C++ store drops the
properties on the way in and names the ones that are lost.

## The embedding

The embedding of a face is the feature vector the recognition model
computed for it -- 128 numbers that the application matches against. The
editor shows it next to the samples, one value per line with its index in
front of it, together with the length of the vector.

The vector is written by the application when the face is enrolled, and
it is what the face is recognised by, so the editor only shows it: it
cannot be edited here. The **Copy** button puts it on the clipboard as a
JSON array, which is handy when the vector is wanted somewhere else.

A vector of a real face has a length of one, because the library
normalises it before it stores it. A length that is far from that says
that the row was written by something that does not normalise the vector.

## The metadata

The metadata is a JSON document, and it is checked before it is stored:
the six value types of RFC 8259 are accepted, trailing commas and
unquoted keys are not. The check is the same one the C++ library does,
so a document the editor accepts is a document the library accepts too.

## Tests

The tests need a PostgreSQL server, so they only run when one is named
in the environment -- the same rule the C++ test suite follows. They
never touch the table of a real installation: they create a table of
their own, with a name of their own, and drop it again when they are
done.

```sh
FACE_DETECTION_TEST_DSN='host=localhost dbname=faces' \
    python3 -m unittest discover -s tools/face-db-editor -p 'test_*.py'
```

Without the environment variable the tests that need a server are
skipped, and the rest still run.

## Layout

| File | Purpose |
|------|---------|
| `face_db.py` | The storage layer: reads and writes the table. Free of any GUI code. |
| `face_db_editor.py` | The window that drives it. |
| `test_face_db.py` | The tests of the storage layer. |
