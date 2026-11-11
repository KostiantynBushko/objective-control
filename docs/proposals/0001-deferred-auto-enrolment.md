# Proposal 0001 -- deferred (tracked) auto-enrolment

| | |
|---|---|
| Status | Proposed |
| Date | 2026-11-11 |
| Area | `--auto-enroll`, face enrolment pipeline |
| Touches | `apps/camera_demo.cpp`, new `objective-control` face component, `face-detection` submodule (one additive API) |
| Supersedes | the behaviour described in README.md, "Automatic enrolment (`--auto-enroll`)" |

---

## 1. Summary

Today `--auto-enroll` writes a person to the database **the moment a face is
detected**. One detection is worth one row, one embedding and one picture --
whatever the quality of that single frame.

This proposal changes that: with `--auto-enroll` the application keeps an
unknown face **in memory only**, tracks it across the following frames,
collects a list of **images and embeddings** of that face (different views of
the same person), and only writes the record to the database when the face
has been **recognised with high confidence** -- that is, when the collected
views agree with each other and none of them contradicts the database.

The record that is written holds **all** the collected samples, so a person is
stored with several resources (pictures and embeddings) instead of one. That
is the point of the change: one sample is a single point of failure -- one
pose, one lighting, one distance -- and a person enrolled from a single
sample is recognised badly or not at all.

Nothing is written to the database while the face is being collected. A face
that never becomes confident leaves no trace behind.

---

## 2. Investigation: how `--auto-enroll` works today

### 2.1 Where the code is

| What | Where |
|------|-------|
| The switch | `apps/camera_demo.cpp:100` (`isBooleanOption`), `:704` (parsing) |
| The help text | `apps/camera_demo.cpp:258` |
| The validation | `apps/camera_demo.cpp:771-780` (needs `--face-db`, cannot be combined with `--enroll`) |
| The id pool | `apps/camera_demo.cpp:453` (`class PersonIdPool`) |
| The id allocation | `apps/camera_demo.cpp:1063` (`allocatePersonId`) |
| The per-frame analysis | `apps/camera_demo.cpp:1116` (`analyseFaces`) |
| The "probably known" band | `apps/camera_demo.cpp:1168-1175` |
| The enrolment itself | `apps/camera_demo.cpp:1178-1210` |
| The documentation | README.md, "Automatic enrolment (`--auto-enroll`)" |

The face library itself (`third_party/face-detection`, the submodule) knows
nothing about auto-enrolment. It provides:

* `fd::FaceDetector::detect()` -- boxes and landmarks (YuNet),
* `fd::FaceRecognizer::embed()` -- a 128-float, L2-normalised embedding
  (SFace); the cosine *distance* between two embeddings is `1 - dot(a, b)`,
* `fd::FaceDatabase::match()` -- the closest sample in the in-memory
  database, with a `matched` flag when the distance is within the threshold
  (`fd::FaceDatabase::Match`),
* `fd::FaceStore::add()` -- appends one sample to the store.

### 2.2 The current flow, step by step

For every frame, for every detected face (`analyseFaces`,
`apps/camera_demo.cpp:1116`):

1. The embedding of the face is computed (`:1140`).
2. The face is matched against the in-memory `fd::FaceDatabase`
   (`:1149`).
3. **Matched** (distance <= `--face-max-distance`, default `0.4`) -- the face
   is a known person. Nothing is written (`:1153-1161`).
4. **Not matched, but close** (distance between `--face-max-distance` and
   `--face-same-distance`, default `0.6`) -- the face is reported as
   `probably <name>` and *nothing is written either* (`:1168-1175`).
5. **Not matched and not close** -- the face is enrolled **right now**
   (`:1178-1210`):
   * a person id is allocated (`allocatePersonId()`, which re-reads the list
     of people from the store every single time -- one query per enrolment),
   * the sample is put into the in-memory database
     (`faceDatabase.enroll(...)`),
   * the sample is written to the store (`faceStore->add(entry, error)`),
   * if the store write fails, the sample is taken out of the in-memory
     database again (`faceDatabase.remove(personId)`, `:1204`) and the error
     is printed.

So one detection is one row. The person is "known" from the next frame on,
and the second frame of the same person is a no-op (step 3).

### 2.3 What the store and the schema can already do

* The PostgreSQL schema is **one row per sample**
  (`third_party/face-detection/sql/schema.sql`):

  ```sql
  CREATE TABLE face_enrollments (
      id          bigserial   PRIMARY KEY,
      person_id   integer     NOT NULL,
      embedding   real[]      NOT NULL,
      user_name   text,
      photo       bytea,
      metadata    jsonb       NOT NULL DEFAULT '{}'::jsonb,
      enrolled_at timestamptz NOT NULL DEFAULT now()
  );
  ```

  Several rows may share a `person_id` -- the schema explicitly allows it
  ("A person may be enrolled more than once"). **The multi-sample storage
  this proposal needs already exists; no migration is required.**

* `fd::FaceDatabase::match()` compares against the closest **single**
  sample, so adding more samples per person widens the margin
  automatically -- the recognition of a person with three views is better
  than with one, without a single change to the matcher.

* `fd::FaceStore::add()` appends one sample. The PostgreSQL store appends
  one row (`postgres_store.cpp:866`); the file store rewrites the whole file
  (`store.cpp:250`). There is **no batch append** in the API yet -- see
  §9.2.

* The properties of a person (name, picture, metadata) are stored with
  *every* sample of that person, so the store can answer "who is this?"
  without a second lookup. The face database editor
  (`tools/face-db-editor`) relies on this: it writes a corrected name or
  metadata to all the samples of the person.

### 2.4 What the current design costs us

| # | Finding | Consequence |
|---|---------|-------------|
| F1 | One detection = one row | A person is stored from a single frame: one pose, one distance, one lighting. Recognition of that person is fragile, and there is nothing to fall back on. |
| F2 | The row is written before anything is known about the face | A face that is caught in profile, half-hidden, blurred or 40 px wide is stored with the same weight as a good one. The database fills up with rows that will never match anybody. |
| F3 | The "probably known" band blocks every later improvement | Once a person is in the database with one bad sample, every later sighting of that person lands in the band of §2.2 step 4 and is dropped. **The bad sample can never be replaced**, and the person stays badly recognisable forever. |
| F4 | The person id is allocated before the write and is not given back | A failed store write burns the id (the id is reserved in `PersonIdPool` before `faceStore->add()` is called). Ids grow for ever. |
| F5 | `allocatePersonId()` re-reads the whole list of people from the store on every enrolment | One `SELECT` per enrolment, on the hot path of the frame loop, for a number that only changes when somebody is enrolled. |
| F6 | There is no tracking | Nothing links the face in frame *n* to the face in frame *n+1*. "The same person" is decided by a single pairwise distance, with no history and no accumulation of evidence. |
| F7 | There is no way to throw a bad enrolment away except by hand | `--face-db-remove=<id>` removes a person, but the person has to be spotted first, by id, in the database or in the editor. |
| F8 | The mode cannot be told apart from a normal enrolment in the data | The metadata is `{"auto_enrolled": true}`. It does not say *when* the person was seen, how many views were collected or how confident the match was. |

### 2.5 The behaviour we want instead

| The face ... | What happens today | What should happen |
|--------------|--------------------|--------------------|
| is not in the database yet | is enrolled and stored immediately | is **tracked in memory**; a list of pictures and embeddings is collected; it is stored only when the face is recognised with high confidence |
| is in the database, with a name | nothing is stored | nothing is stored (unchanged) |
| is in the database, without a name | nothing is stored | nothing is stored; the person may be *enriched* with more samples (optional, §7.1) |
| is close to somebody in the database | reported as `probably <name>` | reported as `probably <name>`; the candidate is not started (unchanged) |
| never becomes confident | a row is in the database | **nothing is in the database** |

---

## 3. The problem

The camera is left running and people walk in front of it. The application
has to decide, on its own, whether what it sees is a new person, and it has
to write that person down in a way that the recognition can use afterwards.

The decision is currently made from a **single frame**, and the result is
written down **immediately**. Both are wrong:

* a single frame is not enough evidence to say "this is a new person" -- a
  detection can be a reflection, a poster, a face that belongs to somebody
  who is already enrolled but looks different in this light, or a partial
  face at the edge of the frame;
* a single frame is not enough material to recognise a person afterwards.
  SFace embeddings are stable across small changes, but a face seen once,
  in one pose, at one distance, is a single point in the embedding space.

The application therefore needs a small amount of **memory**: it has to
watch an unknown face for a while, decide whether it is really a new person,
and gather more than one picture of it before it writes anything down.

---

## 4. Goals and non-goals

### Goals

* **G1** -- with `--auto-enroll`, no record is written to the database
  before the face has been recognised with high confidence.
* **G2** -- a person is stored with **several samples** (pictures and
  embeddings) taken from different views of the face, whenever the camera
  offers them.
* **G3** -- the decision "is this a new person?" is based on the whole
  tracked history of the face, not on a single frame.
* **G4** -- a face that never becomes confident leaves **nothing** behind in
  the database.
* **G5** -- the behaviour stays compatible with the rest of the project: the
  same database, the same editor, the same file format, the same CLI shape.

### Non-goals

* **N1** -- changing the face detector, the recogniser or the matcher. The
  proposal uses `fd::` as it is.
* **N2** -- a real multi-object tracker (Kalman filter, SORT, ...). The
  association needed here is one embedding comparison per frame.
* **N3** -- naming the person. The name is still filled in later, by hand,
  with the face database editor.
* **N4** -- keeping the pending candidates across a restart of the
  application. The candidates live in memory only (see §8.1).
* **N5** -- changing the schema of the database.

---

## 5. The proposal

### 5.1 Terminology

| Term | Meaning |
|------|---------|
| **sample** | One embedding + one picture + the properties of the person. One row of `face_enrollments`. |
| **committed enrolment** | A person that is in the database. What the application calls "enrolled" today. |
| **candidate** | An unknown face that the application is watching. It lives in memory only and owns a list of samples. |
| **view** | One sample of a candidate. Two views of the same person differ in pose, distance or lighting -- that is the "different resources" this proposal is about. |
| **confidence** | How sure the application is that the views of a candidate belong to the same person. Measured as the largest cosine distance inside the candidate. |

### 5.2 The lifecycle of a candidate

```
                    detection, not in the database,
                    not close to anybody in it
                              |
                              v
                    +-------------------+   sample rejected by a quality gate
                    |   Collecting      |<---------------------------+
                    |  (in memory only) |                            |
                    +-------------------+                            |
                              |  view accepted                        |
                              |  (quality gate + not a duplicate)    |
                              v                                     |
                    +-------------------+                            |
                    |  view accepted    |----------------------------+
                    +-------------------+
                              |
              target number of views reached
              and the identity is confident
                              |
                              v
                    +-------------------+
                    |    Confirmed      |
                    +-------------------+
                              |
                              v
                    +-------------------+
                    |    Committed      |--> person id allocated, N rows written
                    +-------------------+    to the store, N samples added to
                                               the in-memory database
```

A candidate leaves the collecting state in one of three ways:

| Event | Condition | Outcome |
|-------|-----------|---------|
| **Confirmed** | the target number of views is reached and the identity is confident | the person is committed: one person id, one row per view |
| **Expired** | the collection window is over, or the face was not seen for `--auto-enroll-timeout` seconds | the candidate is **discarded** -- nothing is written |
| **Expired with views** | the window is over, but the candidate holds at least one good view and its identity is confident | the candidate is committed **with the views it has** (best effort). `--auto-enroll-strict` turns this into a discard. |

The last row is the "if possible" of the requirement: a person who stands in
front of the camera for two seconds and leaves is still enrolled, but with
one view instead of three.

### 5.3 Associating a detection with a candidate

Every frame, every detection is offered to the candidates first. A detection
belongs to a candidate when its embedding is close to one of the candidate's
views:

```
distance(embedding, closest view of the candidate) <= --auto-enroll-consistency
```

The candidate whose closest view wins takes the detection. The distance is
the same cosine distance the recogniser uses everywhere else, so there is
one notion of "the same face" in the whole application.

Two candidates must not claim the same detection: the closest one wins, and
the others are left alone.

A candidate that is not seen for `--auto-enroll-timeout` seconds is dropped.
The timeout is what keeps a person who walked past from haunting the memory
of the application.

### 5.4 The quality gate of a view

A detection only becomes a view of a candidate when it passes the gates
below. A rejected detection is simply not stored -- it costs one embedding
and nothing else.

| Gate | Rule | Default | Why |
|------|------|---------|-----|
| **size** | the crop of the face is at least `--auto-enroll-min-size` pixels wide | `64` | An embedding of a 30 px face is noise. The picture would be useless for the editor as well. |
| **detection** | the detector score is at least `--face-score` | `0.6` | The detector already computes it; the gate is the same knob the application already has. |
| **duplicate** | the view is at least `--auto-enroll-diversity` away from every view the candidate already holds | `0.08` | Two views that are almost the same picture are not two resources. This is what makes the collected views *different*. |
| **identity** | the view is at most `--auto-enroll-consistency` away from every view the candidate already holds | `0.35` | A candidate must stay the same person. A face that is far away from all of its views starts a candidate of its own. |

The identity gate and the duplicate gate pull in opposite directions: the
candidate has to hold views that are *the same person* (within
`--auto-enroll-consistency`) but *not the same picture* (at least
`--auto-enroll-diversity` apart). The defaults leave room in between.

The size gate is measured on the **crop** (the face with its padding), which
is the picture that is stored, so the gate describes what is actually kept.

### 5.5 The confidence of a candidate

The confidence of a candidate is the **largest** cosine distance between two
of its views:

```
confidence = max over all pairs of views ( distance(view_a, view_b) )
```

A candidate is *confident* when this value is at most
`--auto-enroll-consistency` (default `0.35`, i.e. clearly inside the
`--face-max-distance` of `0.4` that the application treats as "the same
person").

This is the "recognise the tracked image with high confidence" of the
requirement: the tracked face is recognised **against its own collected
views**, and the person is only written down when the application can
recognise the face it is tracking.

A candidate with a single view has a confidence of `0` by definition, which
is why the single-view case needs the quality gate to carry the decision.

### 5.6 The decision table

For every detection of a frame, in this order:

| # | The detection ... | Decision |
|---|-------------------|----------|
| 1 | matches the database within `--face-max-distance` | a known person. Nothing is written. |
| 2 | belongs to a candidate (within `--auto-enroll-consistency` of one of its views) | the view is offered to that candidate. The candidate may be committed by it. |
| 3 | is close to somebody in the database (within `--face-same-distance`) but not within `--face-max-distance` | `probably <name>`. No candidate is started. (Unchanged from today.) |
| 4 | is nobody | a **new candidate** is started with this detection as its first view. |

Rule 2 comes after rule 1 on purpose: a face that is in the database is
never collected, so a person who is already enrolled is never "enriched" by
accident. The optional enrichment of §7.1 is a separate, explicit step.

### 5.7 Committing a candidate

When a candidate is committed:

1. **Re-check against the database.** Another instance of the application (or
   another camera) may have enrolled the same person in the meantime. The
   views of the candidate are matched against the database one last time:
   * a match within `--face-max-distance` -- the person is already in the
     database. The candidate is dropped (and, with `--auto-enroll-attach`,
     its views are attached to that person, §7.1);
   * no match -- the candidate becomes a new person.
2. **Allocate the person id** (`allocatePersonId()`). This happens *now*,
   not when the candidate was started, so a candidate that is never
   committed does not burn an id (F4).
3. **Write the views.** One row per view, all with the same `person_id`, in
   one batch (§9.2). Either all of them are written or none is.
4. **Add the views to the in-memory database**, so that the person is
   recognised from the next frame on, and so that the matcher uses all the
   views from now on.
5. **Report it**, with the number of views that were stored.

If the write fails, the candidate stays in memory and is retried with the
next view; the person is not added to the in-memory database, so a broken
database does not fill the memory with people that are not there.

### 5.8 What is stored

| Column | Value |
|--------|-------|
| `person_id` | the id allocated at commit time |
| `embedding` | the embedding of the view |
| `user_name` | empty -- the name is filled in later, by the editor |
| `photo` | the picture of the view |
| `metadata` | `{"auto_enrolled": true, "views": 3, "confidence": 0.21}` |
| `enrolled_at` | the default (`now()`) |

The metadata is stored with **every** view of the person, as the editor
expects. It is therefore a property of the *person*, not of the view: it
says how many views there are and how confident the application was, and it
must not contain anything that is specific to a single view.

`auto_enrolled` stays as it is, so that the existing queries over the
metadata keep working. The two new fields are additive.

### 5.9 The command line

```
  --auto-enroll          Enrol the faces that are not in the database yet.
                         A face is watched in memory first: the application
                         keeps the pictures and the embeddings of the face
                         and writes the person to the database only when the
                         face has been recognised with high confidence. The
                         person is stored with every view that was
                         collected. Implies --faces and --recognize and
                         requires --face-db.
  --auto-enroll-mode=<m> deferred (default) or immediate. "immediate" is the
                         old behaviour: every face that is not in the
                         database is enrolled and stored at once.
  --auto-enroll-samples=<n>
                         Number of views to collect before the person is
                         stored (default: 3)
  --auto-enroll-window=<s>
                         Seconds to collect the views of a face. When the
                         window is over, a face that is confident is stored
                         with the views it has (default: 20)
  --auto-enroll-timeout=<s>
                         Seconds a face may be out of sight before it is
                         forgotten (default: 10)
  --auto-enroll-consistency=<f>
                         Largest cosine distance between two views of the
                         same face. A face whose views are further apart
                         than this is not the same face (default: 0.35)
  --auto-enroll-diversity=<f>
                         Smallest cosine distance between two views of the
                         same face. Two views that are closer than this are
                         the same picture and only one of them is kept
                         (default: 0.08)
  --auto-enroll-min-size=<px>
                         Smallest face that is worth keeping, in pixels
                         (default: 64)
  --auto-enroll-strict   Throw a face away instead of storing it with the
                         views it has when the window is over
  --auto-enroll-attach   Add the collected views to a person that is
                         already in the database instead of creating a new
                         one
```

The existing options keep their meaning:

* `--face-max-distance` -- the distance at which a face counts as a match
  against the database;
* `--face-same-distance` -- the distance above which a face is certainly
  somebody new; the band in between is reported as `probably <name>`.

### 5.10 Where the code lives

The tracker is **not** part of `apps/camera_demo.cpp`: that file is already
1 700 lines and the tracker deserves to be tested on its own. It goes into
this repository, next to the camera library:

```
include/objective-control/face/auto_enroller.hpp
src/face/auto_enroller.cpp
tests/auto_enroller_test.cpp
```

`oc::AutoEnroller` is a small, dependency-light class:

```cpp
namespace oc {

/// A view of a face: its embedding and its picture.
struct FaceView {
    fd::Embedding embedding;
    std::vector<std::uint8_t> photo;
    int width = 0;                       // the size of the crop, in pixels
    float detection_score = 0.0f;
    std::uint64_t timestamp_us = 0;
};

/// A face the application is watching, before it is written to the store.
struct FaceCandidate {
    std::vector<FaceView> views;
    std::uint64_t first_seen_us = 0;
    std::uint64_t last_seen_us = 0;
    float confidence() const;             // the largest distance inside
};

/// Watches the faces that are not in the database yet.
class AutoEnroller {
public:
    struct Options {
        int target_samples = 3;
        std::uint64_t window_us = 20'000'000;
        std::uint64_t timeout_us = 10'000'000;
        float consistency = 0.35f;
        float diversity = 0.08f;
        int min_size = 64;
        bool strict = false;
        bool attach = false;
    };

    struct Decision {
        enum class Kind { None, ViewAccepted, ViewRejected,
                          CandidateStarted, CandidateCommitted,
                          CandidateDiscarded };
        Kind kind = Kind::None;
        int candidate = -1;               // the index of the candidate
        int views = 0;                    // the views it holds now
        float confidence = 1.0f;
        int person_id = -1;               // set when it was committed
    };

    /// Offers a detection to the tracker.
    Decision observe(const FaceView& view, std::uint64_t now_us);

    /// The candidates that are ready to be written to the store.
    std::vector<const FaceCandidate*> ready() const;

    /// Drops the candidates that timed out or ran out of time.
    std::vector<Decision> expire(std::uint64_t now_us);
};

} // namespace oc
```

The class knows nothing about the camera, the detector or the store: it
takes embeddings and pictures and gives answers. That makes it testable
without a camera, a model or a database -- which the tests in `tests/` do.

The application keeps the glue:

* `apps/camera_demo.cpp` calls `observe()` for every detection, and
  `expire()` once per frame;
* when a candidate is committed, the application allocates the person id,
  writes the views to the store and adds them to the in-memory
  `fd::FaceDatabase`.

The tracker needs `fd::Embedding` (for the distances) and nothing else from
the submodule. It is compiled only in a build with
`OBJECTIVE_CONTROL_ENABLE_FACE=ON`, like the rest of the face code.

### 5.11 The flow in the application

```
frame
  |
  +-- detect the faces
  |
  +-- for every face:
  |     +-- compute the embedding and the crop
  |     +-- match against the database
  |     |     +-- known          -> label it, done
  |     |     +-- probably known -> label it, done
  |     +-- offer it to the tracker
  |           +-- belongs to a candidate -> the candidate may be committed
  |           +-- nobody                -> a new candidate is started
  |
  +-- expire the candidates that timed out
  +-- commit the candidates that are ready
```

### 5.12 What the user sees

```
Frame 0: 1920x1080 (3 ch), 6220800 bytes, t=6454us.
  faces: 1 (412,180 213x280 score=0.918774) -> tracking a new face
Frame 1: 1920x1080 (3 ch), 6220800 bytes, t=71204us.
  faces: 1 (412,180 213x280 score=0.918774) -> tracking a new face (1 view)
Frame 2: 1920x1080 (3 ch), 6220800 bytes, t=138220us.
  faces: 1 (412,180 213x280 score=0.918774) -> tracking a new face (2 views)
Frame 3: 1920x1080 (3 ch), 6220800 bytes, t=205118us.
  faces: 1 (412,180 213x280 score=0.918774) -> tracking a new face (3 views)
  Enrolled person 1 with 3 view(s).
```

and at the end of the run:

```
Captured 240 frame(s).
Detected 240 face(s) in total.
Recognised 237 face(s) against the database.
Enrolled 1 new person with 3 view(s) in postgres://user@localhost/objective_control.
```

The overlay shows a face that is being tracked in blue, with the number of
views it has, so that the operator can see that the application is still
deciding:

```
  +-----------+
  | person 1  |     <- green, committed
  +-----------+
  +-----------+
  | tracking  |     <- blue, 2 views so far
  | 2 views   |
  +-----------+
```

---

## 6. Interaction with the rest of the application

| Area | Effect |
|------|--------|
| `--enroll=<id>` | unchanged. It enrols the best face of the first frame that has one, with one sample. It is the manual path and stays manual. |
| `--recognize` | unchanged for the faces that are in the database. A face that is being tracked is shown as `tracking`. |
| `--face-db-list`, `--face-db-remove`, `--face-db-init` | unchanged. A person is listed with the number of samples it has -- `3` instead of `1` for an auto-enrolled person. |
| The face database editor | unchanged. It already shows every sample of a person and already writes the name and the metadata to all of them. |
| The file store (`faces.db`) | the format is unchanged (`FDB2`): one entry per sample, several entries may share a person id. |
| The PostgreSQL schema | unchanged. The rows are written the way they are written today, several rows per person instead of one. |
| `--preview` / `--stream` | the overlay gains the blue "tracking" state. |
| The exit code | unchanged. |

---

## 7. Failure modes and edge cases

| # | Case | What happens |
|---|------|--------------|
| E1 | The person walks away after one frame | The candidate expires with one view and a confidence of `0`. It is committed (best effort) unless `--auto-enroll-strict` is on. |
| E2 | The person walks away before the face is big enough | No view passes the size gate, the candidate is empty and it is discarded. Nothing is written. |
| E3 | Two people stand next to each other | Two candidates. The embedding decides which one a detection belongs to; a face that is far from both starts a third candidate. |
| E4 | The same person comes back ten minutes later | The first candidate expired. The person is a candidate again -- and, if the person is already in the database, rule 1 or rule 3 of §5.6 catches it first. |
| E5 | The database is not reachable | The commit fails, the error is printed, the candidate stays in memory and is retried with the next view. The in-memory database is not touched, so the application does not claim to know people it has not stored. |
| E6 | The application is stopped while candidates are pending | The candidates are lost. They were never in the database, so nothing has to be cleaned up. The next start begins with an empty database and the people have to be seen again. |
| E7 | A face is detected in a reflection or on a poster | It becomes a candidate. It never collects several consistent views, so it expires and is discarded. |
| E8 | The camera shakes and the same face is detected twice in one frame | The two detections are within the diversity threshold of each other; only one of them is kept as a view. |
| E9 | The database already holds the person (another instance enrolled it meanwhile) | The re-check in §5.7 catches it at commit time: the candidate is dropped, or attached to the person with `--auto-enroll-attach`. No duplicate is created. |
| E10 | The id that is allocated is taken by somebody else in the meantime | The same race as today: the ids are read back from the store before one is handed out. A duplicate id would mean two persons share an id -- rare, and it is the behaviour of the current code as well. |
| E11 | The memory grows | The candidates are bounded: at most `--auto-enroll-samples` views each, and the candidates that time out are dropped. A hard cap on the number of candidates (default 8) keeps the worst case bounded. |

### 7.1 The optional enrichment (`--auto-enroll-attach`)

A person that is already in the database with a single, bad sample stays bad
(F3). With `--auto-enroll-attach`, the views that are collected for a face
that matches an existing person are added to that person instead of being
thrown away -- the person gets more resources, and the recognition improves.

It is **off** by default, because it writes to people that were enrolled by
hand, and because it is the opposite of "only new people are enrolled". The
decision to use it belongs to the deployment, not to the application.

When it is on, the flow of §5.6 changes: rule 1 (known person) starts a
candidate as well, and the commit of that candidate attaches the views to
the existing person instead of allocating a new id.

---

## 8. Trade-offs and open points

### 8.1 The candidates live in memory only

The candidates are not written anywhere, not even to a staging table. A
restart of the application throws them away.

The alternative -- a `face_candidates` table -- would survive a restart and
would make the mode auditable, but it also means writing to the database
before the face is confirmed, which is exactly what this proposal is about.
It is a reasonable follow-up, once the mode has proved itself.

### 8.2 The default number of views

Three views is a compromise: it is enough for the recognition to be
meaningfully better than with one, and it is short enough for a person to
stand still for a few seconds. A deployment that cares more about speed
than about quality can set `--auto-enroll-samples=1` (which behaves like
today, with the quality gate in front of it) or `--auto-enroll-samples=2`.

### 8.3 The best-effort commit

Committing a candidate that has fewer views than the target is a trade-off
between "the person is in the database" and "the person is in the database
with good samples". The proposal chooses the former, with
`--auto-enroll-strict` for the latter.

---

## 9. Impact

### 9.1 On this repository

| File | Change |
|------|--------|
| `include/objective-control/face/auto_enroller.hpp` | new -- the tracker |
| `src/face/auto_enroller.cpp` | new |
| `src/CMakeLists.txt` | the new source, only when `OC_HAVE_FACE` is on |
| `apps/camera_demo.cpp` | the glue: the options, the loop, the overlay, the report |
| `tests/auto_enroller_test.cpp` | new -- the tests of §10.1 |
| `README.md` | the new behaviour and the new options |
| `tools/face-db-editor/` | nothing to change; the editor already shows several samples per person |

### 9.2 On the `face-detection` submodule

One **additive** change is desirable: the tracker writes several rows for
one person at once, and the store API only appends one sample at a time.

```cpp
/// Appends several samples at once.
///
/// The default implementation appends them one by one. A backend that can
/// do it in one go -- PostgreSQL -- overrides it, so that the samples of
/// one person land in a single transaction.
virtual bool addAll(const std::vector<FaceEntry>& entries, std::string& error);
```

Without it the application can call `add()` in a loop, which is correct but
slow for the file store (it rewrites the whole file per sample) and puts
the samples of one person in as many transactions as there are samples.

Nothing else in the submodule has to change: the tracker only needs
`fd::Embedding` and the cosine distance, both of which are already public.

### 9.3 On the database

None. The rows are the rows that are written today; there are just more of
them per person, and the schema has allowed that from the start.

### 9.4 On the performance

* one embedding per detection, as today;
* one extra cosine distance per detection (against the views of the
  candidates) -- 128 multiplications, negligible next to the embedding
  itself;
* the pictures of the pending views are held in memory: a view is a JPEG of
  about 10-20 kB, a candidate holds at most `--auto-enroll-samples` of them,
  and there are at most 8 candidates -- well under 1 MB;
* the writes happen **less** often than today (once per person instead of
  once per detection), and `allocatePersonId()` is called once per person
  instead of once per detection (F5).

---

## 10. Testing

### 10.1 Unit tests (`tests/auto_enroller_test.cpp`)

The tracker takes embeddings and pictures, so it is tested without a camera
and without a model. The tests use hand-made embeddings: a unit vector is
"person A", another one is "person B", and a small rotation of a unit vector
is "the same person, another pose".

| Test | What it checks |
|------|----------------|
| A new detection starts a candidate | no candidate, no decision to commit |
| A second, similar detection joins the candidate | the candidate holds two views |
| A dissimilar detection starts a second candidate | two candidates, not one |
| The target is reached -> the candidate is committed | the decision carries the number of views |
| A view that is too close to an existing one is rejected | the duplicate gate |
| A view that is too far from the candidate is rejected | the identity gate |
| A view that is too small is rejected | the size gate |
| A candidate that times out is discarded | nothing is committed |
| A candidate whose window is over is committed with its views | the best-effort path |
| `--auto-enroll-strict` discards it instead | the strict path |
| The confidence is the largest distance inside the candidate | the confidence itself |

The tests follow the style of `tests/camera_test.cpp`: no test framework,
a `check(condition, "what")` helper and a non-zero exit code on failure.

### 10.2 Integration test

A run of the application against a synthetic camera with `--auto-enroll` and
a file store:

* the database is **empty** while the faces are being collected;
* after the collection, the database holds the person with the number of
  views that were collected;
* a second run over the same frames does not create a second person.

### 10.3 Manual test

* a person walks in front of the camera, stops, turns their head, leaves:
  the person is stored with several views, and the overlay shows the face
  being tracked;
* a poster with a face on it is shown for a minute: nothing is stored;
* the application is stopped and started again: the people who were stored
  are recognised, the pending candidates are gone.

---

## 11. Observability

The application reports, at the end of a run:

```
Tracked 12 face(s); 9 were stored, 3 were discarded.
Enrolled 9 new person(s) with 27 view(s) in postgres://...
```

and, per candidate, one line when it starts, when it is committed and when
it is discarded:

```
  Tracking a new face (candidate 1).
  Candidate 1 discarded: 1 view, no confidence.
  Enrolled person 1 with 3 view(s) (confidence 0.21).
```

A `--auto-enroll-verbose` switch would print every accepted and rejected
view with its distance, which is what one needs to tune the thresholds on a
real camera.

---

## 12. Risks

| Risk | How it is handled |
|------|-------------------|
| Fewer people are enrolled than today (the window is too short, the gates are too strict) | the gates are all options; `--auto-enroll-mode=immediate` restores today's behaviour; the thresholds are tuned on a real camera before the mode is rolled out |
| More rows in the database (one row per view) | the rows are the same size; the person count is what matters, and it does not change |
| A candidate holds the memory of a face that is never stored | bounded by the number of candidates, the number of views and the timeout |
| The tracker drifts onto another person | the identity gate; a candidate that changes person is discarded, not committed |

---

## 13. Open questions

1. **Should the pending candidates survive a restart?** (See §8.1.)
2. **Should the enrichment of §7.1 be on by default?** It fixes F3, but it
   writes to people that were enrolled by hand.
3. **Should the confidence be the largest distance inside the candidate, or
   the distance to the centre of the candidate?** The first is stricter and
   simpler; the second tolerates a slowly drifting face better.
4. **Should the views be stored at all, or only the best one?** The whole
   point is to have several, but the storage grows with them.
5. **Should the mode be a separate flag (`--auto-enroll-deferred`) instead
   of a mode of `--auto-enroll`?** A separate flag keeps `--auto-enroll`
   backwards compatible; a mode keeps one switch.

---

## 14. The phases of the implementation

The work is split into three phases. Each phase leaves the tree in a state
that builds and that is tested.

### Phase 1 -- the tracker (done)

The tracker moves into the library, where it can be tested without a
camera, a model or a database:

| File | What |
|------|------|
| `include/objective-control/face/auto_enroller.hpp` | `oc::FaceView`, `oc::FaceCandidate`, `oc::AutoEnroller` |
| `src/face/auto_enroller.cpp` | the tracker itself |
| `tests/auto_enroller_test.cpp` | the tests of the table of §10.1 |

The application is not touched in this phase: `--auto-enroll` behaves
exactly as it does today. The tracker is a new class that nothing uses
yet, which keeps the change small and easy to review.

### Phase 2 -- the application

The application starts using the tracker:

* the new options of §5.9 (`--auto-enroll-mode`, `--auto-enroll-samples`,
  `--auto-enroll-window`, `--auto-enroll-timeout`,
  `--auto-enroll-consistency`, `--auto-enroll-diversity`,
  `--auto-enroll-min-size`, `--auto-enroll-strict`),
* the decision table of §5.6 in `analyseFaces()`,
* the commit of §5.7: the person id is allocated when the candidate is
  committed, the views are written, and the samples are added to the
  in-memory database,
* the overlay of §5.12 (a face that is being tracked is drawn in blue,
  with the number of views it has) and the reports of §11.

### Phase 3 -- the store, the tests and the documentation

* the batch write of §9.2 (`fd::FaceStore::addAll`), so that the views of
  one person land in one transaction,
* the integration test of §10.2,
* the README and the help text of the application.

### 14.1 What phase 1 changed against the sketch of §5.10

The sketch of §5.10 is the plan; the implementation that came out of it
differs in a few places, all of them because the tracker turned out to
need a little more from its caller:

| The sketch | What was built | Why |
|------------|----------------|-----|
| `Decision::Kind::CandidateCommitted` is returned by `observe()` | the tracker never commits; it reports `CandidateReady` and the application commits | the tracker does not know the store, so it cannot know whether the write worked. The application decides, allocates the id and writes the views |
| `Decision::candidate` is "the index of the candidate" | it is the id of the candidate, which does not change when another candidate is dropped | an index would point at a different candidate after a removal |
| `ready()` returns the candidates that are ready | same, plus: a candidate that is ready stays in the tracker until the application removes it | a store that could not be written is retried with the next frame (E5) |
| `expire()` drops the candidates that timed out | same, plus: a candidate that was thrown away is dropped by the tracker, and one that is ready is reported again | the views of a candidate that is ready are still worth something |
| the face that is far from all the views of a candidate starts a candidate of its own | same: the *closest* candidate claims a detection, and the identity gate throws the view away when it does not fit | a face that is far from every view of every candidate starts a new one; a face that one candidate claims but that does not fit it is rejected |

The gates, the defaults and the lifecycle are the ones of §5.4 and §5.5.

## Appendix A -- the code map of the current implementation

| Concern | Where |
|---------|-------|
| The option is declared | `apps/camera_demo.cpp:100`, `:115` |
| The help text | `apps/camera_demo.cpp:258-262` |
| The option is parsed | `apps/camera_demo.cpp:704` |
| The thresholds are read | `apps/camera_demo.cpp:716-731` |
| The validation | `apps/camera_demo.cpp:771-780` |
| The detector and the recogniser are opened | `apps/camera_demo.cpp:962-1046` |
| The people of the database are reserved | `apps/camera_demo.cpp:1053-1056` |
| The id is allocated | `apps/camera_demo.cpp:1063-1074` |
| The per-face analysis | `apps/camera_demo.cpp:1116-1215` |
| The "probably known" band | `apps/camera_demo.cpp:1163-1175` |
| The enrolment | `apps/camera_demo.cpp:1177-1210` |
| The overlay | `apps/camera_demo.cpp:1221-1268` |
| The summary | `apps/camera_demo.cpp:1425-1431`, `:1651-1659` |

## Appendix B -- the data of a person, before and after

Before (today), one face, one row:

| person_id | embedding | photo | metadata |
|-----------|-----------|-------|----------|
| 1 | `[0.11, -0.42, ...]` | `<jpeg>` | `{"auto_enrolled": true}` |

After (this proposal), one face, three rows:

| person_id | embedding | photo | metadata |
|-----------|-----------|-------|----------|
| 1 | `[0.11, -0.42, ...]` | `<jpeg, pose 1>` | `{"auto_enrolled": true, "views": 3, "confidence": 0.21}` |
| 1 | `[0.09, -0.40, ...]` | `<jpeg, pose 2>` | `{"auto_enrolled": true, "views": 3, "confidence": 0.21}` |
| 1 | `[0.14, -0.39, ...]` | `<jpeg, pose 3>` | `{"auto_enrolled": true, "views": 3, "confidence": 0.21}` |

The matcher compares against the closest of the three, so the person is
recognised from more than one angle.
