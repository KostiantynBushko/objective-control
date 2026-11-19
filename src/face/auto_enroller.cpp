// The tracker of the faces that are not in the database yet.
//
// The tracker is deliberately free of any knowledge about the camera, the
// detector or the store: it takes embeddings and pictures and gives
// answers. The application does the rest -- it matches the face against
// the database first, offers the faces it does not know to the tracker,
// and writes the candidates that are ready to the store.

#include "objective-control/face/auto_enroller.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>

namespace oc {

namespace {

/// The distance of a candidate that holds no view at all.
constexpr float kNoDistance = std::numeric_limits<float>::max();

} // namespace

// ---------------------------------------------------------------------------
// FaceCandidate
// ---------------------------------------------------------------------------

float FaceCandidate::confidence() const {
    // The confidence is the largest distance between two of the views:
    // the two views that disagree the most decide how sure the
    // application is that they show the same person.
    float worst = 0.0f;
    for (std::size_t i = 0; i < views.size(); ++i) {
        for (std::size_t j = i + 1; j < views.size(); ++j) {
            worst = std::max(
                worst, views[i].embedding.cosineDistance(views[j].embedding));
        }
    }
    return worst;
}

float FaceCandidate::closestDistance(const fd::Embedding& embedding) const {
    if (views.empty()) {
        return kNoDistance;
    }
    float best = kNoDistance;
    for (const FaceView& view : views) {
        best = std::min(best, view.embedding.cosineDistance(embedding));
    }
    return best;
}

float FaceCandidate::largestDistance(const fd::Embedding& embedding) const {
    float worst = 0.0f;
    for (const FaceView& view : views) {
        worst = std::max(worst, view.embedding.cosineDistance(embedding));
    }
    return worst;
}

// ---------------------------------------------------------------------------
// The samples of a person that was enrolled automatically
// ---------------------------------------------------------------------------

std::string autoEnrolledMetadata(int views, float confidence) {
    // A confidence of 0.2134 is noise: the number says how sure the
    // application was, and three digits are enough for that.
    std::ostringstream out;
    out << "{\"auto_enrolled\": true, \"views\": " << views
        << ", \"confidence\": " << std::setprecision(3) << confidence << '}';
    return out.str();
}

std::vector<fd::FaceEntry> makeEnrolmentEntries(const FaceCandidate& candidate,
                                                 int person_id) {
    const std::string metadata =
        autoEnrolledMetadata(static_cast<int>(candidate.views.size()),
                             candidate.confidence());

    std::vector<fd::FaceEntry> entries;
    entries.reserve(candidate.views.size());
    for (const FaceView& view : candidate.views) {
        fd::FaceEntry entry;
        entry.person_id = person_id;
        entry.embedding = view.embedding;
        entry.info.metadata = metadata;
        entry.info.photo = view.photo;
        entries.push_back(entry);
    }
    return entries;
}

// ---------------------------------------------------------------------------
// AutoEnroller
// ---------------------------------------------------------------------------

AutoEnroller::AutoEnroller() : AutoEnroller(Options()) {}

AutoEnroller::AutoEnroller(Options options) : options_(options) {
    // The knobs are clamped, so that a nonsensical value cannot make the
    // tracker behave in a surprising way.
    options_.target_samples = std::max(1, options_.target_samples);
    options_.max_candidates = std::max(1, options_.max_candidates);
    options_.min_size = std::max(0, options_.min_size);
    options_.min_score = std::max(0.0f, options_.min_score);
    options_.consistency = std::max(0.0f, options_.consistency);
    options_.diversity = std::max(0.0f, options_.diversity);
}

bool AutoEnroller::isReady(const FaceCandidate& candidate) const {
    if (static_cast<int>(candidate.views.size()) < options_.target_samples) {
        return false;
    }
    return candidate.confidence() <= options_.consistency;
}

AutoEnroller::Decision AutoEnroller::observe(const FaceView& view,
                                              std::uint64_t now_us) {
    Decision decision;

    // ---------------- The quality gates ----------------
    // A view that does not pass them is not stored: it costs one embedding
    // and nothing else.
    if (!view.embedding.valid) {
        decision.kind = Decision::Kind::ViewRejected;
        decision.reason = Decision::Reason::InvalidEmbedding;
        return decision;
    }

    if (options_.min_size > 0 && view.width < options_.min_size) {
        decision.kind = Decision::Kind::ViewRejected;
        decision.reason = Decision::Reason::TooSmall;
        return decision;
    }

    if (options_.min_score > 0.0f && view.detection_score < options_.min_score) {
        decision.kind = Decision::Kind::ViewRejected;
        decision.reason = Decision::Reason::ScoreTooLow;
        return decision;
    }

    // ---------------- Which candidate does the face belong to? ----------------
    // The candidate whose closest view wins takes the detection.
    Entry* owner = nullptr;
    float closest = kNoDistance;
    for (const std::unique_ptr<Entry>& entry : candidates_) {
        const float distance =
            entry->candidate.closestDistance(view.embedding);
        if (distance > options_.consistency) {
            continue;
        }
        if (owner == nullptr || distance < closest) {
            owner = entry.get();
            closest = distance;
        }
    }

    if (owner != nullptr) {
        decision.candidate = owner->id;
        decision.data = &owner->candidate;

        // A candidate holds at most as many views as it was asked for: the
        // views it has are the resources of the person, and a candidate
        // that is not stored (a store that could not be written) does not
        // grow without a bound.
        if (static_cast<int>(owner->candidate.views.size()) >=
            options_.target_samples) {
            decision.kind = Decision::Kind::ViewRejected;
            decision.reason = Decision::Reason::EnoughViews;
            decision.views = static_cast<int>(owner->candidate.views.size());
            decision.confidence = owner->candidate.confidence();
            return decision;
        }

        // The duplicate gate: two views that are almost the same picture
        // are not two resources. Only one of them is kept.
        if (closest < options_.diversity) {
            decision.kind = Decision::Kind::ViewRejected;
            decision.reason = Decision::Reason::Duplicate;
            decision.views = static_cast<int>(owner->candidate.views.size());
            decision.confidence = owner->candidate.confidence();
            return decision;
        }

        // The identity gate: a candidate has to stay the same person. A
        // face that is far away from all of its views is not one of them.
        if (owner->candidate.largestDistance(view.embedding) >
            options_.consistency) {
            decision.kind = Decision::Kind::ViewRejected;
            decision.reason = Decision::Reason::Inconsistent;
            decision.views = static_cast<int>(owner->candidate.views.size());
            decision.confidence = owner->candidate.confidence();
            return decision;
        }

        // The view belongs to the candidate.
        owner->candidate.views.push_back(view);
        owner->candidate.last_seen_us = now_us;

        decision.kind = Decision::Kind::ViewAccepted;
        decision.views = static_cast<int>(owner->candidate.views.size());
        decision.confidence = owner->candidate.confidence();
        decision.ready = isReady(owner->candidate);
        return decision;
    }

    // ---------------- Nobody wants the face: a new candidate ----------------
    if (static_cast<int>(candidates_.size()) >= options_.max_candidates) {
        decision.kind = Decision::Kind::ViewRejected;
        decision.reason = Decision::Reason::TooManyCandidates;
        return decision;
    }

    auto entry = std::make_unique<Entry>();
    entry->id = nextId_++;
    entry->candidate.id = entry->id;
    entry->candidate.views.push_back(view);
    entry->candidate.first_seen_us = now_us;
    entry->candidate.last_seen_us = now_us;

    decision.kind = Decision::Kind::CandidateStarted;
    decision.candidate = entry->id;
    decision.data = &entry->candidate;
    decision.views = 1;
    decision.confidence = 0.0f;
    // One view is enough when one view is all that is asked for.
    decision.ready = isReady(entry->candidate);

    candidates_.push_back(std::move(entry));
    return decision;
}

const FaceCandidate* AutoEnroller::closest(const fd::Embedding& embedding,
                                            float max_distance) const {
    const FaceCandidate* best = nullptr;
    float closest = kNoDistance;
    for (const std::unique_ptr<Entry>& entry : candidates_) {
        const float distance = entry->candidate.closestDistance(embedding);
        if (distance > max_distance) {
            continue;
        }
        if (best == nullptr || distance < closest) {
            best = &entry->candidate;
            closest = distance;
        }
    }
    return best;
}

std::vector<const FaceCandidate*> AutoEnroller::ready() const {
    std::vector<const FaceCandidate*> result;
    for (const std::unique_ptr<Entry>& entry : candidates_) {
        if (isReady(entry->candidate)) {
            result.push_back(&entry->candidate);
        }
    }
    return result;
}

std::vector<AutoEnroller::Decision> AutoEnroller::expire(
    std::uint64_t now_us) {
    std::vector<Decision> decisions;
    std::vector<int> discarded;

    for (const std::unique_ptr<Entry>& entry : candidates_) {
        const FaceCandidate& candidate = entry->candidate;
        const bool timedOut =
            now_us > candidate.last_seen_us + options_.timeout_us;
        const bool windowOver =
            now_us > candidate.first_seen_us + options_.window_us;

        if (!timedOut && !windowOver) {
            continue;
        }

        Decision decision;
        decision.candidate = entry->id;
        decision.views = static_cast<int>(candidate.views.size());
        decision.confidence = candidate.confidence();

        if (isReady(candidate)) {
            // The candidate holds enough views and they agree with each
            // other: it is ready, whatever the window says. The
            // application stores it and removes it.
            decision.kind = Decision::Kind::CandidateReady;
            decision.ready = true;
            decision.data = &candidate;
        } else if (timedOut) {
            // The face went away and did not come back: it is forgotten.
            decision.kind = Decision::Kind::CandidateDiscarded;
            decision.reason = Decision::Reason::TimedOut;
            discarded.push_back(entry->id);
        } else if (options_.strict || candidate.views.empty() ||
                   candidate.confidence() > options_.consistency) {
            // The window is over and the face is not worth keeping.
            decision.kind = Decision::Kind::CandidateDiscarded;
            decision.reason = Decision::Reason::WindowOver;
            discarded.push_back(entry->id);
        } else {
            // The window is over, but the candidate holds at least one good
            // view and the identity is confident: it is stored with the
            // views it has.
            decision.kind = Decision::Kind::CandidateReady;
            decision.ready = true;
            decision.data = &candidate;
        }

        decisions.push_back(decision);
    }

    // The candidates that were thrown away are of no use to anybody: they
    // are dropped here, so that the tracker does not hold on to them.
    for (const int id : discarded) {
        remove(id);
    }

    return decisions;
}

bool AutoEnroller::remove(int candidate) {
    const auto it = std::find_if(
        candidates_.begin(), candidates_.end(),
        [candidate](const std::unique_ptr<Entry>& entry) {
            return entry->id == candidate;
        });
    if (it == candidates_.end()) {
        return false;
    }
    candidates_.erase(it);
    return true;
}

const FaceCandidate* AutoEnroller::candidate(int id) const {
    for (const std::unique_ptr<Entry>& entry : candidates_) {
        if (entry->id == id) {
            return &entry->candidate;
        }
    }
    return nullptr;
}

} // namespace oc
