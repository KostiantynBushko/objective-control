#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "face-detection/face/recognizer.hpp"

namespace oc {

/// One view of a face: the embedding computed for it and the picture of it.
///
/// A view is what ends up in the database: one row of `face_enrollments`,
/// with the embedding, the picture of the face and the properties of the
/// person it belongs to. Two views of the same person differ in pose,
/// distance or lighting -- that is what makes several of them useful.
struct FaceView {
    /// The embedding of the face. It has to be valid (`fd::Embedding::valid`).
    fd::Embedding embedding;

    /// The picture of the face, as the bytes of an encoded image (a JPEG,
    /// for instance). It is optional.
    std::vector<std::uint8_t> photo;

    /// The width of the crop the face was cut out of, in pixels.
    int width = 0;

    /// The height of the crop the face was cut out of, in pixels.
    int height = 0;

    /// The confidence of the detector that found the face.
    float detection_score = 0.0f;

    /// When the face was seen, in microseconds.
    std::uint64_t timestamp_us = 0;
};

/// A face the application is watching, before it is written to the store.
///
/// A candidate lives in memory only. It is thrown away when it times out,
/// and it is turned into a person when it is confident enough.
struct FaceCandidate {
    /// The views that were collected for this face, oldest first.
    std::vector<FaceView> views;

    /// When the face was seen for the first time, in microseconds.
    std::uint64_t first_seen_us = 0;

    /// When the face was seen for the last time, in microseconds.
    std::uint64_t last_seen_us = 0;

    /// How sure the application is that the views belong to one person.
    ///
    /// It is the **largest** cosine distance between two of the views: the
    /// two views that disagree the most decide how sure the application is.
    /// A candidate with a single view has a confidence of 0 by definition.
    [[nodiscard]] float confidence() const;

    /// The smallest cosine distance between @p embedding and the views.
    ///
    /// Returns a very large number when the candidate holds no view.
    [[nodiscard]] float closestDistance(const fd::Embedding& embedding) const;

    /// The largest cosine distance between @p embedding and the views.
    ///
    /// Returns 0 when the candidate holds no view.
    [[nodiscard]] float largestDistance(const fd::Embedding& embedding) const;
};

/// Watches the faces that are not in the database yet.
///
/// The tracker knows nothing about the camera, the detector or the store: it
/// takes embeddings and pictures and gives answers. That is what makes it
/// testable without a camera, a model or a database.
///
/// A face that is not in the database yet is kept here for a while. Every
/// detection that belongs to it adds a view to it, and the views are
/// checked against each other: the face has to stay the same person
/// (`consistency`) and the views have to be different pictures
/// (`diversity`). When the candidate holds enough views and they agree with
/// each other, it is **ready** and the application writes it to the store.
///
/// The tracker never writes anything itself. The application asks which
/// candidates are ready, writes their views to the store and tells the
/// tracker that it is done with them.
class AutoEnroller {
public:
    /// The knobs of the tracker.
    struct Options {
        /// How many views are collected before a person is stored.
        int target_samples = 3;

        /// How long the views of a face are collected, in microseconds.
        ///
        /// When the window is over, a face that is confident enough is
        /// stored with the views it has (unless `strict` is set).
        std::uint64_t window_us = 20'000'000;

        /// How long a face may be out of sight before it is forgotten, in
        /// microseconds.
        std::uint64_t timeout_us = 10'000'000;

        /// The largest cosine distance between two views of the same face.
        ///
        /// A face whose views are further apart than this is not the same
        /// face. It is also the distance at which a detection still belongs
        /// to a candidate.
        float consistency = 0.35f;

        /// The smallest cosine distance between two views of the same face.
        ///
        /// Two views that are closer than this are the same picture, and
        /// only one of them is kept.
        float diversity = 0.08f;

        /// The smallest face that is worth keeping, in pixels. The gate is
        /// measured on the crop, which is the picture that is stored.
        int min_size = 64;

        /// The smallest detector score a face may have to be kept.
        float min_score = 0.6f;

        /// How many faces are watched at the same time. It keeps the memory
        /// of the tracker bounded.
        int max_candidates = 8;

        /// Throw a face away instead of storing it with the views it has
        /// when the collection window is over.
        bool strict = false;
    };

    /// What the tracker decided about a detection or a candidate.
    struct Decision {
        /// The kind of the decision.
        enum class Kind {
            None,               ///< Nothing happened.
            ViewAccepted,       ///< The view was added to a candidate.
            ViewRejected,       ///< The view was not kept.
            CandidateStarted,   ///< A new candidate was started.
            CandidateReady,     ///< The candidate is ready to be stored.
            CandidateDiscarded, ///< The candidate was thrown away.
        };

        /// Why a view was rejected, or why a candidate was thrown away.
        enum class Reason {
            None,             ///< No reason; the decision succeeded.
            InvalidEmbedding, ///< The embedding could not be used.
            TooSmall,         ///< The face was smaller than `min_size`.
            ScoreTooLow,      ///< The detector was not sure enough.
            Duplicate,        ///< The view was the same picture as another one.
            Inconsistent,     ///< The view was not the same person.
            TooManyCandidates, ///< Too many faces are being watched at once.
            WindowOver,       ///< The collection window is over.
            TimedOut,         ///< The face was out of sight for too long.
        };

        /// What happened.
        Kind kind = Kind::None;

        /// Why it happened.
        Reason reason = Reason::None;

        /// The id of the candidate the decision is about, or -1.
        int candidate = -1;

        /// How many views the candidate holds now.
        int views = 0;

        /// How confident the candidate is.
        float confidence = 1.0f;

        /// True when the candidate is ready to be written to the store.
        bool ready = false;

        /// The candidate the decision is about.
        ///
        /// It points into the tracker and stays valid until the candidate
        /// is removed with remove(). It is null when the decision is not
        /// about a candidate -- a discarded one is dropped by the tracker,
        /// so there is nothing left to point at.
        const FaceCandidate* data = nullptr;
    };

    /// Creates a tracker with the default options.
    AutoEnroller();

    /// Creates a tracker with the given options.
    explicit AutoEnroller(Options options);

    /// The options the tracker runs with.
    [[nodiscard]] const Options& options() const { return options_; }

    /// Offers a detection to the tracker.
    ///
    /// @p view is the face that was seen, @p now_us is the time it was seen
    /// at. The returned decision says what happened to it.
    Decision observe(const FaceView& view, std::uint64_t now_us);

    /// The candidates that are ready to be written to the store.
    ///
    /// The pointers point into the tracker and stay valid until the
    /// candidate is removed.
    [[nodiscard]] std::vector<const FaceCandidate*> ready() const;

    /// Reports the candidates that ran out of time.
    ///
    /// A candidate that was not seen for `timeout_us` is forgotten. A
    /// candidate whose collection window is over is reported as ready --
    /// it is stored with the views it has -- unless `strict` is set, in
    /// which case it is thrown away.
    ///
    /// A candidate that is ready is reported by ready() as well, and it
    /// stays in the tracker until the application removes it: a store that
    /// could not be written is therefore retried with the next frame. A
    /// candidate that was thrown away is dropped by the tracker itself.
    std::vector<Decision> expire(std::uint64_t now_us);

    /// Forgets the candidate with the given id.
    ///
    /// The application calls this once it has stored (or thrown away) the
    /// views of a candidate. A candidate that is not removed stays in the
    /// tracker and is reported again by ready().
    ///
    /// Returns false when the candidate is unknown.
    bool remove(int candidate);

    /// How many faces are being watched.
    [[nodiscard]] std::size_t candidateCount() const {
        return candidates_.size();
    }

    /// The candidate with the given id, or nullptr.
    [[nodiscard]] const FaceCandidate* candidate(int id) const;

private:
    /// A candidate and the bookkeeping around it.
    struct Entry {
        /// The id of the candidate. It is stable for as long as the
        /// candidate is watched.
        int id = -1;

        /// The candidate itself.
        FaceCandidate candidate;
    };

    /// True when the candidate holds enough views and they agree.
    [[nodiscard]] bool isReady(const FaceCandidate& candidate) const;

    Options options_;
    // The candidates are held one by one, so that the address of a
    // candidate does not change when another one is started or dropped.
    std::vector<std::unique_ptr<Entry>> candidates_;
    int nextId_ = 1;
};

} // namespace oc
