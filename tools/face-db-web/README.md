# The web version of the face database editor

The desktop editor ([`../face-db-editor`](../face-db-editor)) opens the
PostgreSQL table of objective-control in a window and lets the enrolled
faces be reviewed and corrected. This is the same editor as a web
application: the browser shows the people, the samples of the selected
person, the picture of a face, the name, the metadata and the
embedding, and it can merge the people that are the same person and
remove a person.

Nothing is written twice. The storage is the `FaceStore` of the desktop
editor, driven by the service layer of this directory, and the rules are
therefore the very same rules:

* the name of a person and the metadata are stored with **every** sample
  of that person, so that the store can answer "who is this?" without a
  second lookup,
* the metadata is checked against RFC 8259 before it is stored, with the
  same parser the C++ library uses,
* a merge happens in **one transaction**: the people are either merged
  completely or not at all,
* a table that predates the property columns is used as it is, and the
  editor says which columns it cannot keep.

## What it does

* lists the enrolled people, with their id and how many samples each of
  them has, and filters the list as you type,
* shows the samples of the selected person, with the picture of the face
  that was captured at the time,
* shows the name and the metadata of the person and stores them again,
* shows the embedding of the selected sample -- the vector the
  recognition model computed for the face -- and copies it to the
  clipboard,
* saves the picture of a face to a file (the browser downloads it),
* merges the people that are the same person into one another,
* removes a person and every sample of that person.

## Running it

The editor needs Python 3.10 or newer and a PostgreSQL server.

```sh
pip install -r requirements.txt

python3 tools/face-db-web/web_server.py \
    --dsn='postgres://objective_control:objective_control@127.0.0.1:5432/objective_control'
```

and then open <http://127.0.0.1:8000/> in a browser.

The connection string is taken from `--dsn`, else from `$FACE_DB_URL`,
else from `$DATABASE_URL` -- the same order the C++ application and the
desktop editor look them up in:

```sh
export FACE_DB_URL='postgres://objective_control:objective_control@127.0.0.1:5432/objective_control'
python3 tools/face-db-web/web_server.py
```

`--table` names the table to open; it is `face_enrollments` by default,
the same name `PostgresFaceStore::Options::table` uses. `--host` and
`--port` say where the server listens; the default is `127.0.0.1:8000`.

The table is opened when the server starts. A database that is not there
does not stop the server: the browser says that the table is closed and
offers to open it again, which is what the desktop editor does as well.

### Who may use it

The server has **no authentication of its own** and it edits the table of
the installation. It binds to `127.0.0.1` for that reason. Putting it on
a network needs a reverse proxy that does the authentication, or the
`--host` of a machine that only the user can reach.

The connection string is never sent to the browser: the server tells it
which database is open with the password taken out of it.

## The API

| Method | Path | What it does |
|--------|------|--------------|
| GET | `/api/status` | whether the table is open, which one it is, what it can keep |
| POST | `/api/connect` | opens the table again |
| POST | `/api/disconnect` | closes the table |
| GET | `/api/people` | the enrolled people |
| GET | `/api/people/{id}` | the samples of one person |
| PUT | `/api/people/{id}` | stores the name and the metadata of a person |
| DELETE | `/api/people/{id}` | removes every sample of a person |
| GET | `/api/people/{id}/merge-preview?sources=1,2` | what a merge would do |
| POST | `/api/people/{id}/merge` | merges the people into the selected one |
| GET | `/api/people/{id}/samples/{sid}/photo` | the picture of a face |
| POST | `/api/metadata/format` | rewrites a metadata document in a readable shape |

An error carries the message of the store in the field `error`:

```json
{"error": "the metadata is not a valid JSON document: ..."}
```

A request that needs a database that is not open is answered with `503`
and the same shape. A request that the database refuses is answered with
`400`.

The name and the metadata of a person are sent as they are typed:

```sh
curl -X PUT http://127.0.0.1:8000/api/people/9 \
     -H 'Content-Type: application/json' \
     -d '{"name": "Ada Lovelace", "metadata": "{\"role\": \"admin\"}"}'
```

## Tests

The tests need a PostgreSQL server, so the ones that talk to it only run
when one is named in the environment -- the same rule the C++ test suite
and the desktop editor follow. They never touch the table of a real
installation: they create a table of their own, with a name of their
own, and drop it again when they are done.

```sh
FACE_DETECTION_TEST_DSN='host=localhost dbname=faces' \
    python3 -m unittest discover -s tools/face-db-web -p 'test_*.py'
```

Without the environment variable the tests that need a server are
skipped, and the rest still run.

## Layout

| File | Purpose |
|------|---------|
| `web_store.py` | The service layer: the connection, the state of the editor and the rows as plain data. |
| `web_server.py` | The FastAPI application: the routes and the server that runs it. |
| `static/index.html` | The page of the editor. |
| `static/app.js` | The browser side: the people, the samples, the properties and the merge. |
| `static/style.css` | The look of the editor. |
| `test_web_server.py` | The tests of the service and of the routes. |

The storage itself is **not** in this directory: it is
`face_db.py` of the desktop editor, which is free of any GUI code and is
imported from there. That is what keeps the two editors in step.
