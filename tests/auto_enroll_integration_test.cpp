// The integration test of the deferred auto-enrolment.
//
// The unit tests of the tracker (tests/auto_enroller_test.cpp) look at the
// tracker alone. This test looks at the whole path: a face that nobody
// knows is seen, it is watched in memory, and the person is written to the
// store when the application is sure that all of the views show the same
// person.
//
// The faces are made by hand -- a unit vector is "person A", another one is
// "person B", and a small rotation of a unit vector is "the same person,
// another pose" -- so the test needs neither a camera nor a model. The
// store is a real file store, so the file that is written is the one the
// application writes.
//
// The test follows the style of tests/camera_test.cpp: no test framework,
// a check(condition, "what") helper and a non-zero exit code on failure.

#include "objective-control/face/auto_enroller.hpp"

#include "face-detection/face/database.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

// The distance at which a face counts as a match against the database.
constexpr float kMaxDistance = 0.4f;

// The distance above which a face is certainly somebody new. Between the
// two, a face is very likely somebody who is enrolled already, and it is
// left alone.
constexpr float kSameDistance = 0.6f;

// An embedding of a face that stands at the given angle.
//
// The vector is a unit vector in the first two dimensions, so the cosine
// distance between two of them is 1 - cos(angle_a - angle_b). That makes
// the distances of the test exact and easy to reason about.
fd::Embedding embeddingAt(double degrees) {
    const double radians = degrees * 3.14159265358979323846 / 180.0;
    fd::Embedding embedding;
    embedding.values[0] = static_cast<float>(std::cos(radians));
    embedding.values[1] = static_cast<float>(std::sin(radians));
    embedding.valid = true;
    return embedding;
}

// The deferred enrolment, in miniature.
//
// The steps are the ones of the application: the face is matched against
// the database first, a face that is close to somebody who is enrolled
// already is left alone, and a face that nobody knows is offered to the
// tracker. The candidates that are ready are written to the store -- in
// one go, so that the samples of one person land together.
class DeferredEnroller {
public:
    DeferredEnroller(fd::FaceStore& store, fd::FaceDatabase& database)
        : store_(store), database_(database) {}

    /// Offers a face to the tracker. @p nowUs is the time it was seen at.
    void observe(const fd::Embedding& embedding, std::uint64_t nowUs) {
        // Rule 1: the face is in the database already.
        const fd::FaceDatabase::Match match =
            database_.match(embedding, kMaxDistance);
        if (match.matched) {
            return;
        }

        // Rule 3: the face is close to somebody who is enrolled already,
        // but it is not that person. Such a face is reported and left
        // alone, and no candidate is started for it.
        if (match.person_id >= 0 && match.distance <= kSameDistance) {
            return;
        }

        // Rules 2 and 4: the face is offered to the tracker. It either
        // joins a candidate or starts one of its own.
        oc::FaceView view;
        view.embedding = embedding;
        view.width = 128;
        view.height = 128;
        view.detection_score = 0.9f;
        view.timestamp_us = nowUs;
        view.photo = {0xff, 0xd8, 0xff, 0x00, 0x7f};
        tracker_.observe(view, nowUs);

        commit(nowUs);
    }

    /// Reports the candidates that ran out of time.
    void expire(std::uint64_t nowUs) {
        commit(nowUs);
    }

    /// How many people were written to the store.
    [[nodiscard]] int storedPeople() const { return storedPeople_; }

    /// The id of the person that was written last.
    [[nodiscard]] int lastPerson() const { return lastPerson_; }

    /// How many candidates were thrown away.
    [[nodiscard]] int discarded() const { return discarded_; }

private:
    /// Writes the candidates that are ready to the store.
    void commit(std::uint64_t nowUs) {
        // The candidates that ran out of time are reported first.
        for (const oc::AutoEnroller::Decision& decision :
             tracker_.expire(nowUs)) {
            if (decision.kind ==
                oc::AutoEnroller::Decision::Kind::CandidateDiscarded) {
                ++discarded_;
            }
        }

        // The candidates that are ready are stored. Their ids are taken
        // first: storing a candidate takes it out of the tracker, and the
        // pointers would not survive that.
        std::vector<int> ready;
        for (const oc::FaceCandidate* candidate : tracker_.ready()) {
            ready.push_back(candidate->id);
        }
        for (const int id : ready) {
            const oc::FaceCandidate* candidate = tracker_.candidate(id);
            if (candidate == nullptr) {
                continue;
            }

            const int personId = nextPersonId_++;
            const std::vector<fd::FaceEntry> entries =
                oc::makeEnrolmentEntries(*candidate, personId);

            // The views of one person are written in one go, so that the
            // person is either stored with all of them or not at all.
            std::string error;
            if (!store_.addAll(entries, error)) {
                std::cerr << "FAIL: the views of person " << personId
                          << " could not be written: " << error << '\n';
                ++failures;
                continue;
            }

            // The samples are added to the in-memory database, so that the
            // person is recognised from the next frame on.
            for (const fd::FaceEntry& entry : entries) {
                database_.add(entry);
            }

            tracker_.remove(id);
            ++storedPeople_;
            lastPerson_ = personId;
        }
    }

    oc::AutoEnroller tracker_;
    fd::FaceStore& store_;
    fd::FaceDatabase& database_;
    int nextPersonId_ = 1;
    int storedPeople_ = 0;
    int lastPerson_ = -1;
    int discarded_ = 0;
};

// The people of a store, and how many samples each of them has.
std::vector<fd::PersonSummary> listPeople(fd::FaceStore& store) {
    std::vector<fd::PersonSummary> people;
    std::string error;
    if (!store.list(people, error)) {
        std::cerr << "FAIL: the store could not be listed: " << error << '\n';
        ++failures;
        people.clear();
    }
    return people;
}

} // namespace

int main() {
    // The poses of the person that shows up. The distance between two of
    // them is 1 - cos(angle):
    //
    //     0 deg  <-> 24 deg : 0.0865  the same person, another picture
    //     0 deg  <-> 48 deg : 0.3309  the same person, still
    //    24 deg  <-> 48 deg : 0.0865  the same person, another picture
    //
    // The gates of the tracker accept a view that is 0.08 to 0.35 away
    // from the views of a candidate, so the three poses are accepted and
    // make a confident candidate.
    const double kFirst = 0.0;
    const double kSecond = 24.0;
    const double kThird = 48.0;

    // Somebody else entirely.
    const double kOther = 90.0;

    const std::string path = "auto_enroll_integration_test.db";
    std::remove(path.c_str());

    // ------------------------------------------------------------------
    // The first run: the person is watched, and then stored.
    // ------------------------------------------------------------------
    {
        std::string error;
        fd::FileFaceStore store;
        check(store.open(path, error), "the store opens");

        fd::FaceDatabase database;
        DeferredEnroller enroller(store, database);

        // The first two views are collected, but the person is not stored
        // yet: the application is not sure that they all show the same
        // person.
        enroller.observe(embeddingAt(kFirst), 1000);
        enroller.observe(embeddingAt(kSecond), 2000);

        check(listPeople(store).empty(),
              "the database is empty while the views are collected");

        // The third view completes the candidate: the person is stored
        // with every view that was collected.
        enroller.observe(embeddingAt(kThird), 3000);

        const std::vector<fd::PersonSummary> people = listPeople(store);
        check(people.size() == 1, "the database holds one person");
        if (people.size() == 1) {
            check(people[0].person_id == 1, "the person has the id that was "
                                           "handed out");
            check(people[0].samples == 3,
                  "the person is stored with the three views that were "
                  "collected");
            check(people[0].user_name.empty(),
                  "the person has no name: it is filled in later, by hand");
        }
        check(enroller.storedPeople() == 1, "one person was stored");

        // The samples of the person carry the metadata of the person, and
        // the picture of the face.
        std::vector<fd::FaceEntry> entries;
        check(store.load(entries, error), "the store loads");
        check(entries.size() == 3, "the store holds three samples");
        for (const fd::FaceEntry& entry : entries) {
            check(entry.person_id == 1, "every sample has the same person id");
            check(entry.info.metadata.find("\"auto_enrolled\": true") !=
                      std::string::npos,
                  "every sample says that the person was enrolled "
                  "automatically");
            check(entry.info.metadata.find("\"views\": 3") != std::string::npos,
                  "every sample says how many views the person has");
            check(entry.info.metadata.find("\"confidence\":") !=
                      std::string::npos,
                  "every sample says how confident the application was");
            check(entry.info.photo.size() == 5,
                  "every sample carries the picture of the face");
        }
    }

    // ------------------------------------------------------------------
    // The second run: the person is known, and is not stored again.
    // ------------------------------------------------------------------
    {
        // A new run of the application starts with the store that is on
        // the disk.
        std::string error;
        fd::FileFaceStore store;
        check(store.open(path, error), "the store opens again");

        fd::FaceDatabase database;
        check(database.load(store), "the samples are loaded into the database");
        check(database.size() == 3, "the three samples are loaded");

        // The person of the first run is recognised from the samples that
        // were stored.
        const fd::FaceDatabase::Match match =
            database.match(embeddingAt(kSecond), kMaxDistance);
        check(match.matched, "the person of the first run is recognised");
        check(match.person_id == 1, "the person keeps its id");

        DeferredEnroller enroller(store, database);

        // The same person shows up again, in the same three poses.
        enroller.observe(embeddingAt(kFirst), 1000);
        enroller.observe(embeddingAt(kSecond), 2000);
        enroller.observe(embeddingAt(kThird), 3000);

        check(enroller.storedPeople() == 0,
              "the second run stores nobody: the person is known already");

        const std::vector<fd::PersonSummary> people = listPeople(store);
        check(people.size() == 1, "the database still holds one person");
        if (people.size() == 1) {
            check(people[0].samples == 3,
                  "the person still has the three views of the first run");
        }
    }

    // ------------------------------------------------------------------
    // A face that never becomes confident leaves nothing behind.
    // ------------------------------------------------------------------
    {
        std::string error;
        fd::FileFaceStore store;
        check(store.open(path, error), "the store opens once more");

        fd::FaceDatabase database;
        check(database.load(store), "the samples are loaded into the database");

        DeferredEnroller enroller(store, database);

        // Somebody else shows up once and is seen again in a pose that is
        // too far away to be the same person: the candidate is never
        // confirmed.
        enroller.observe(embeddingAt(kOther), 1000);
        enroller.observe(embeddingAt(kOther + 40.0), 2000);

        // The face goes out of sight: the candidate is forgotten.
        enroller.expire(20'000'000);

        check(enroller.discarded() > 0, "the candidate was thrown away");
        check(enroller.storedPeople() == 0, "nothing was stored");

        const std::vector<fd::PersonSummary> people = listPeople(store);
        check(people.size() == 1, "the database still holds the one person");
        if (people.size() == 1) {
            check(people[0].samples == 3,
                  "the person of the first run was not touched");
        }
    }

    std::remove(path.c_str());

    if (failures != 0) {
        std::cerr << failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "The deferred auto-enrolment works as it should.\n";
    return 0;
}
