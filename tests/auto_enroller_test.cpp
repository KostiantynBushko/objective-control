// The tests of the tracker of the faces that are not in the database yet.
//
// The tracker takes embeddings and pictures, so it is tested without a
// camera, without a model and without a database. The embeddings are made
// by hand: a unit vector is "person A", another one is "person B", and a
// small rotation of a unit vector is "the same person, another pose".
//
// The tests follow the style of tests/camera_test.cpp: no test framework,
// a check(condition, "what") helper and a non-zero exit code on failure.

#include "objective-control/face/auto_enroller.hpp"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

// An embedding of a face that stands at the given angle.
//
// The vector is a unit vector in the first two dimensions, so the cosine
// distance between two of them is 1 - cos(angle_a - angle_b). That makes
// the distances of the tests exact and easy to reason about.
fd::Embedding embeddingAt(double degrees) {
    const double radians = degrees * 3.14159265358979323846 / 180.0;
    fd::Embedding embedding;
    embedding.values[0] = static_cast<float>(std::cos(radians));
    embedding.values[1] = static_cast<float>(std::sin(radians));
    embedding.valid = true;
    return embedding;
}

// The cosine distance between two embeddings, computed the long way, so
// that the tests do not depend on the implementation they check.
float distanceBetween(const fd::Embedding& a, const fd::Embedding& b) {
    float dot = 0.0f;
    for (std::size_t i = 0; i < fd::Embedding::kDimension; ++i) {
        dot += a.values[i] * b.values[i];
    }
    return 1.0f - dot;
}

// A view of a face of the given size, seen at the given angle.
oc::FaceView viewAt(double degrees, int width = 128, float score = 0.9f) {
    oc::FaceView view;
    view.embedding = embeddingAt(degrees);
    view.width = width;
    view.height = width;
    view.detection_score = score;
    view.photo = {0x01, 0x02, 0x03};
    return view;
}

// The defaults of the tracker, with the window and the timeout far away,
// so that a test only sees what it is about.
oc::AutoEnroller::Options testOptions() {
    oc::AutoEnroller::Options options;
    options.target_samples = 3;
    options.window_us = 20'000'000;
    options.timeout_us = 10'000'000;
    options.consistency = 0.35f;
    options.diversity = 0.08f;
    options.min_size = 64;
    options.min_score = 0.6f;
    options.max_candidates = 8;
    return options;
}

} // namespace

int main() {
    using oc::AutoEnroller;
    using Decision = AutoEnroller::Decision;

    // The angles the tests use, and the distance they stand at from each
    // other (the cosine distance of two unit vectors is 1 - cos(angle)):
    //
    //     0 deg  <->  5 deg : 0.0038  the same picture
    //     0 deg  <-> 24 deg : 0.0865  the same person, another picture
    //     0 deg  <-> 48 deg : 0.3309  the same person, still
    //    24 deg  <-> 48 deg : 0.0865  the same person, another picture
    //     0 deg  <-> 50 deg : 0.3572  too far to be the same person
    //    24 deg  <-> 50 deg : 0.1012  close enough to be claimed by it
    //     0 deg  <-> 90 deg : 1.0     somebody else
    //
    // The gates of the tracker accept a view that is 0.08 to 0.35 away
    // from the views of a candidate, so the angles above are the ones that
    // exercise every gate.
    const double kSamePerson = 0.0;
    const double kSamePicture = 5.0;
    const double kAnotherPose = 24.0;
    const double kThirdPose = 48.0;
    const double kTooFar = 50.0;
    const double kOtherPerson = 90.0;

    // ------------------------------------------------------------------
    // The gates of a view.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        const Decision tooSmall =
            enroller.observe(viewAt(kSamePerson, 32), 1000);
        check(tooSmall.kind == Decision::Kind::ViewRejected,
              "a face that is too small is rejected");
        check(tooSmall.reason == Decision::Reason::TooSmall,
              "a face that is too small is rejected by the size gate");
        check(enroller.candidateCount() == 0,
              "a face that is too small does not start a candidate");

        const Decision tooLow =
            enroller.observe(viewAt(kSamePerson, 128, 0.2f), 1000);
        check(tooLow.kind == Decision::Kind::ViewRejected,
              "a face the detector is not sure about is rejected");
        check(tooLow.reason == Decision::Reason::ScoreTooLow,
              "a face the detector is not sure about is rejected by the "
              "score gate");
        check(enroller.candidateCount() == 0,
              "a face with a low score does not start a candidate");

        oc::FaceView invalid;
        invalid.width = 128;
        invalid.detection_score = 0.9f;
        const Decision broken = enroller.observe(invalid, 1000);
        check(broken.kind == Decision::Kind::ViewRejected,
              "a face without an embedding is rejected");
        check(broken.reason == Decision::Reason::InvalidEmbedding,
              "a face without an embedding is rejected by the embedding gate");
    }

    // ------------------------------------------------------------------
    // A new detection starts a candidate.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        const Decision started = enroller.observe(viewAt(kSamePerson), 1000);
        check(started.kind == Decision::Kind::CandidateStarted,
              "a face that nobody knows starts a candidate");
        check(started.candidate >= 0, "the decision names the candidate");
        check(started.views == 1, "the candidate holds one view");
        check(!started.ready, "one view is not ready yet");
        check(enroller.candidateCount() == 1, "the tracker watches one face");
        check(enroller.ready().empty(), "the candidate is not ready");
    }

    // ------------------------------------------------------------------
    // A second, similar detection joins the candidate.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        enroller.observe(viewAt(kSamePerson), 1000);
        const Decision second = enroller.observe(viewAt(kAnotherPose), 2000);

        check(second.kind == Decision::Kind::ViewAccepted,
              "a face of the same person joins the candidate");
        check(second.views == 2, "the candidate holds two views");
        check(enroller.candidateCount() == 1, "there is still one candidate");
        check(!second.ready, "two views are not enough yet");
    }

    // ------------------------------------------------------------------
    // A dissimilar detection starts a second candidate.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        enroller.observe(viewAt(kSamePerson), 1000);
        const Decision other = enroller.observe(viewAt(kOtherPerson), 2000);

        check(other.kind == Decision::Kind::CandidateStarted,
              "a face of somebody else starts a second candidate");
        check(enroller.candidateCount() == 2, "two candidates are watched");
    }

    // ------------------------------------------------------------------
    // The target is reached -> the candidate is ready.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        enroller.observe(viewAt(kSamePerson), 1000);
        enroller.observe(viewAt(kAnotherPose), 2000);
        const Decision third = enroller.observe(viewAt(kThirdPose), 3000);

        check(third.kind == Decision::Kind::ViewAccepted,
              "the third view is accepted");
        check(third.views == 3, "the candidate holds three views");
        check(third.ready, "the candidate is ready when the target is reached");

        const std::vector<const oc::FaceCandidate*> ready = enroller.ready();
        check(ready.size() == 1, "the ready candidate is reported");
        if (!ready.empty()) {
            check(ready.front()->views.size() == 3,
                  "the ready candidate holds three views");
        }
    }

    // ------------------------------------------------------------------
    // A view that is too close to an existing one is rejected: it is the
    // same picture, not a second resource.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        enroller.observe(viewAt(kSamePerson), 1000);
        const Decision duplicate = enroller.observe(viewAt(kSamePicture), 2000);

        check(duplicate.kind == Decision::Kind::ViewRejected,
              "a view that is the same picture is rejected");
        check(duplicate.reason == Decision::Reason::Duplicate,
              "a view that is the same picture is rejected by the duplicate "
              "gate");
        check(duplicate.views == 1, "the candidate still holds one view");
    }

    // ------------------------------------------------------------------
    // A view that is too far from the candidate is rejected: it is not the
    // same person.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        enroller.observe(viewAt(kSamePerson), 1000);
        enroller.observe(viewAt(kAnotherPose), 2000);
        // 50 degrees is 0.36 away from the first view and 0.10 away from
        // the second one: the candidate claims it, and the identity gate
        // throws it away.
        const Decision inconsistent = enroller.observe(viewAt(kTooFar), 3000);

        check(inconsistent.kind == Decision::Kind::ViewRejected,
              "a view of somebody else is rejected");
        check(inconsistent.reason == Decision::Reason::Inconsistent,
              "a view of somebody else is rejected by the identity gate");
        check(inconsistent.views == 2, "the candidate still holds two views");
        check(enroller.candidateCount() == 1,
              "a rejected view does not start a candidate");
    }

    // ------------------------------------------------------------------
    // The confidence is the largest distance inside the candidate.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        enroller.observe(viewAt(kSamePerson), 1000);
        const Decision second = enroller.observe(viewAt(kAnotherPose), 2000);

        const float expected =
            distanceBetween(embeddingAt(kSamePerson), embeddingAt(kAnotherPose));
        check(std::fabs(second.confidence - expected) < 1e-5f,
              "the confidence is the distance between the two views");
        check(second.confidence > 0.08f && second.confidence < 0.10f,
              "the confidence is the largest distance inside the candidate");

        const oc::FaceCandidate* candidate = enroller.candidate(second.candidate);
        check(candidate != nullptr, "the candidate can be looked up");
        if (candidate != nullptr) {
            check(std::fabs(candidate->confidence() - expected) < 1e-5f,
                  "the candidate knows its own confidence");
        }
    }

    // ------------------------------------------------------------------
    // A candidate that times out is discarded.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        enroller.observe(viewAt(kSamePerson), 1000);
        // The face was last seen at 1000us and the timeout is 10s.
        const std::vector<Decision> expired = enroller.expire(11'000'001);

        check(expired.size() == 1, "the candidate that timed out is reported");
        if (!expired.empty()) {
            check(expired.front().kind == Decision::Kind::CandidateDiscarded,
                  "a candidate that timed out is discarded");
            check(expired.front().reason == Decision::Reason::TimedOut,
                  "a candidate that timed out is discarded by the timeout");
        }
        check(enroller.candidateCount() == 0,
              "a discarded candidate is dropped by the tracker");
    }

    // ------------------------------------------------------------------
    // A candidate whose window is over is committed with the views it has.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        enroller.observe(viewAt(kSamePerson), 1000);
        // The face is still being seen when the window (20s) is over, so
        // it is stored with the views it has.
        enroller.observe(viewAt(kAnotherPose), 20'500'000);
        const std::vector<Decision> expired = enroller.expire(21'000'000);

        check(expired.size() == 1, "the candidate is reported");
        if (!expired.empty()) {
            check(expired.front().kind == Decision::Kind::CandidateReady,
                  "a candidate whose window is over is stored with the views "
                  "it has");
            check(expired.front().views == 2,
                  "the decision carries the number of views");
            check(expired.front().data != nullptr,
                  "the decision carries the candidate");
        }
    }

    // ------------------------------------------------------------------
    // --auto-enroll-strict throws it away instead.
    // ------------------------------------------------------------------
    {
        AutoEnroller::Options options = testOptions();
        options.strict = true;
        AutoEnroller enroller(options);

        enroller.observe(viewAt(kSamePerson), 1000);
        enroller.observe(viewAt(kAnotherPose), 20'500'000);
        const std::vector<Decision> expired = enroller.expire(21'000'000);

        check(expired.size() == 1, "the candidate is reported");
        if (!expired.empty()) {
            check(expired.front().kind == Decision::Kind::CandidateDiscarded,
                  "a strict tracker throws the candidate away");
            check(expired.front().reason == Decision::Reason::WindowOver,
                  "a strict tracker throws the candidate away when the "
                  "window is over");
        }
        check(enroller.candidateCount() == 0,
              "a discarded candidate is dropped by the tracker");
    }

    // ------------------------------------------------------------------
    // A ready candidate is reported until the application removes it, so
    // that a store that could not be written is retried.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        enroller.observe(viewAt(kSamePerson), 1000);
        enroller.observe(viewAt(kAnotherPose), 2000);
        const Decision third = enroller.observe(viewAt(kThirdPose), 3000);
        check(third.ready, "the candidate is ready");

        // The candidate is ready, so it is reported by ready() ...
        check(enroller.ready().size() == 1,
              "the ready candidate is reported by ready()");
        // ... and it stays ready, so that a store that could not be
        // written is retried with the next frame.
        check(enroller.ready().size() == 1,
              "the ready candidate is reported again");

        check(enroller.remove(third.candidate),
              "the candidate can be removed by the application");
        check(enroller.candidateCount() == 0, "the candidate is gone");
        check(enroller.ready().empty(),
              "a removed candidate is not reported again");
        check(!enroller.remove(third.candidate),
              "removing a candidate twice fails");
    }

    // ------------------------------------------------------------------
    // A candidate that is ready is not thrown away when it times out: the
    // views it holds are worth keeping.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        enroller.observe(viewAt(kSamePerson), 1000);
        enroller.observe(viewAt(kAnotherPose), 2000);
        const Decision third = enroller.observe(viewAt(kThirdPose), 3000);
        check(third.ready, "the candidate is ready");

        const std::vector<Decision> expired = enroller.expire(30'000'000);
        check(expired.size() == 1, "the ready candidate is reported");
        if (!expired.empty()) {
            check(expired.front().kind == Decision::Kind::CandidateReady,
                  "a ready candidate is stored, not thrown away");
        }
    }

    // ------------------------------------------------------------------
    // One view is enough when one view is all that is asked for.
    // ------------------------------------------------------------------
    {
        AutoEnroller::Options options = testOptions();
        options.target_samples = 1;
        AutoEnroller enroller(options);

        const Decision started = enroller.observe(viewAt(kSamePerson), 1000);
        check(started.kind == Decision::Kind::CandidateStarted,
              "the face starts a candidate");
        check(started.ready, "one view is enough when one view is asked for");
    }

    // ------------------------------------------------------------------
    // The tracker does not watch more faces than it is allowed to.
    // ------------------------------------------------------------------
    {
        AutoEnroller::Options options = testOptions();
        options.max_candidates = 2;
        AutoEnroller enroller(options);

        enroller.observe(viewAt(0.0), 1000);
        enroller.observe(viewAt(90.0), 2000);
        const Decision third = enroller.observe(viewAt(180.0), 3000);

        check(third.kind == Decision::Kind::ViewRejected,
              "a face beyond the limit is rejected");
        check(third.reason == Decision::Reason::TooManyCandidates,
              "a face beyond the limit is rejected by the limit");
        check(enroller.candidateCount() == 2, "the tracker watches two faces");
    }

    // ------------------------------------------------------------------
    // The candidate carries its id, so that the application can talk about
    // it after another candidate was dropped.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        const Decision first = enroller.observe(viewAt(kSamePerson), 1000);
        const Decision second = enroller.observe(viewAt(kOtherPerson), 2000);

        check(first.candidate != second.candidate,
              "every candidate has an id of its own");

        const oc::FaceCandidate* candidate =
            enroller.candidate(first.candidate);
        check(candidate != nullptr, "the candidate can be looked up by its id");
        if (candidate != nullptr) {
            check(candidate->id == first.candidate,
                  "the candidate knows its own id");
        }
    }

    // ------------------------------------------------------------------
    // The application asks the tracker who owns a face before it offers
    // it, so that the "probably somebody who is enrolled already" case is
    // told apart from a face that nobody knows.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        const Decision first = enroller.observe(viewAt(kSamePerson), 1000);
        const Decision second = enroller.observe(viewAt(kOtherPerson), 2000);

        const oc::FaceCandidate* owner =
            enroller.closest(embeddingAt(kSamePicture), 0.35f);
        check(owner != nullptr, "the owner of a face is found");
        if (owner != nullptr) {
            check(owner->id == first.candidate,
                  "the closest candidate owns the face");
        }

        const oc::FaceCandidate* other =
            enroller.closest(embeddingAt(kOtherPerson), 0.35f);
        check(other != nullptr, "the owner of the other face is found");
        if (other != nullptr) {
            check(other->id == second.candidate,
                  "the second candidate owns the other face");
        }

        // 45 degrees is 0.29 away from both of them.
        check(enroller.closest(embeddingAt(45.0), 0.001f) == nullptr,
              "a face that is too far from every candidate has no owner");
    }

    // ------------------------------------------------------------------
    // A candidate holds at most as many views as it was asked for: the
    // views it has are the resources of the person, and a candidate that
    // is not stored (a store that could not be written) does not grow
    // without a bound.
    // ------------------------------------------------------------------
    {
        AutoEnroller enroller(testOptions());

        enroller.observe(viewAt(kSamePerson), 1000);
        enroller.observe(viewAt(kAnotherPose), 2000);
        const Decision third = enroller.observe(viewAt(kThirdPose), 3000);
        check(third.ready, "the candidate is ready when the target is met");

        const Decision fourth = enroller.observe(viewAt(30.0), 4000);
        check(fourth.kind == Decision::Kind::ViewRejected,
              "a view beyond the target is rejected");
        check(fourth.reason == Decision::Reason::EnoughViews,
              "a view beyond the target is rejected by the limit");
        check(fourth.views == 3, "the candidate still holds three views");
    }

    if (failures == 0) {
        std::cout << "auto_enroller_test: all checks passed.\n";
        return 0;
    }
    std::cerr << "auto_enroller_test: " << failures << " check(s) failed.\n";
    return 1;
}
